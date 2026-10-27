/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/parquet_file_reader.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql {

namespace {

// A page header is parsed through a windowed read: the window only has to hold
// the largest legal page header, never the page payload.
constexpr size_t PAGE_HEADER_WINDOW = 64u << 10;
constexpr size_t MAX_FOOTER_BYTES = 32u << 20;
constexpr size_t MAX_PAGE_BYTES = 256u << 20;
constexpr size_t MAX_LIST_ITEMS = 1u << 20;
constexpr size_t VARCHAR_BYTES = 65535;
constexpr size_t TIME_TEXT_BYTES = 18;
constexpr int MAX_THRIFT_DEPTH = 16;
constexpr int64_t SECONDS_PER_DAY = 86400;

// Every variable-sized allocation goes through this reader-local budget. Check
// the full new allocation while the old capacity is still charged, so vector
// growth cannot temporarily evade the limit. clear() deliberately keeps charges.
struct MemoryBudget {
  size_t limit = 0;
  size_t used = 0;
  bool exhausted = false;

  template <typename T>
  bool reserve(std::vector<T> &values, size_t count)
  {
    if (count <= values.capacity()) return true;
    if (count > (limit - used) / sizeof(T)) {
      exhausted = true;
      return false;
    }
    const size_t previous = values.capacity();
    values.reserve(count);
    used += (values.capacity() - previous) * sizeof(T);
    return true;
  }
  template <typename T>
  bool resize(std::vector<T> &values, size_t count)
  {
    if (!reserve(values, count)) return false;
    values.resize(count);
    return true;
  }
};

// Vector-backed text avoids unaccounted std::string growth/temporary strings.
struct BudgetText {
  MemoryBudget *budget = nullptr;
  std::vector<char> bytes;
  size_t size() const { return bytes.size(); }
  const char *data() const { return bytes.empty() ? "" : bytes.data(); }
  void clear() { bytes.clear(); }
  void append(const char *data, size_t count)
  {
    if (count > budget->limit - bytes.size()) {
      budget->exhausted = true;
      return;
    }
    const size_t needed = bytes.size() + count;
    if (needed > bytes.capacity()) {
      // Geometric growth keeps large dictionaries linear-time, but fall back
      // to the exact request when the spare capacity would exceed the budget.
      size_t capacity = needed;
      if (bytes.capacity() <= (budget->limit - budget->used) / 2) {
        capacity = std::max(needed, bytes.capacity() * 2);
      }
      if (!budget->reserve(bytes, capacity)) return;
    }
    bytes.insert(bytes.end(), data, data + count);
  }
  void append(const char *data) { append(data, std::strlen(data)); }
};

enum CompactType {
  CT_STOP = 0,
  CT_TRUE = 1,
  CT_FALSE = 2,
  CT_BYTE = 3,
  CT_I16 = 4,
  CT_I32 = 5,
  CT_I64 = 6,
  CT_DOUBLE = 7,
  CT_BINARY = 8,
  CT_LIST = 9,
  CT_SET = 10,
  CT_MAP = 11,
  CT_STRUCT = 12,
};

enum FieldResult { FIELD_VALUE = 0, FIELD_STOP = 1, FIELD_ERROR = 2 };

enum PhysicalType {
  P_BOOLEAN = 0,
  P_INT32 = 1,
  P_INT64 = 2,
  P_INT96 = 3,
  P_FLOAT = 4,
  P_DOUBLE = 5,
  P_BYTE_ARRAY = 6,
  P_FLBA = 7,
};

enum PageType {
  PT_DATA = 0,
  PT_INDEX = 1,
  PT_DICTIONARY = 2,
  PT_DATA_V2 = 3,
};

enum CodecType { CODEC_UNCOMPRESSED = 0, CODEC_SNAPPY = 1 };

enum EncodingType {
  ENC_PLAIN = 0,
  ENC_PLAIN_DICTIONARY = 2,
  ENC_RLE = 3,
  ENC_BIT_PACKED = 4,
  ENC_RLE_DICTIONARY = 8,
};

enum RepetitionType { REP_REQUIRED = 0, REP_OPTIONAL = 1, REP_REPEATED = 2 };

// Converted types and logical types are normalized into one enumeration, so the
// value decoder switches exactly once.
enum LogicalType {
  LOGIC_NONE = 0,
  LOGIC_STRING,
  LOGIC_ENUM,
  LOGIC_DATE,
  LOGIC_TIME_MILLIS,
  LOGIC_TIME_MICROS,
  LOGIC_TIME_NANOS,
  LOGIC_TIMESTAMP_MILLIS,
  LOGIC_TIMESTAMP_MICROS,
  LOGIC_TIMESTAMP_NANOS,
  LOGIC_DECIMAL,
  LOGIC_INT_8,
  LOGIC_INT_16,
  LOGIC_INT_32,
  LOGIC_INT_64,
  LOGIC_UINT_8,
  LOGIC_UINT_16,
  LOGIC_UINT_32,
  LOGIC_UINT_64,
  LOGIC_JSON,
  LOGIC_BSON,
  LOGIC_UNKNOWN,
};

inline uint32_t read_le32(const uint8_t *data)
{
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
      | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

inline uint64_t read_le64(const uint8_t *data)
{
  return static_cast<uint64_t>(read_le32(data))
      | (static_cast<uint64_t>(read_le32(data + 4)) << 32);
}

// ---------------------------------------------------------------------------
// Thrift compact protocol reader
// ---------------------------------------------------------------------------
struct CompactReader {
  const uint8_t *data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  MemoryBudget *budget = nullptr;

  bool byte(uint8_t &out)
  {
    if (pos >= size) {
      return false;
    }
    out = data[pos++];
    return true;
  }
  bool varint(uint64_t &out)
  {
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      uint8_t value = 0;
      if (!byte(value)) {
        return false;
      }
      if (shift == 63 && (value & 0x7e) != 0) {
        return false;
      }
      out |= static_cast<uint64_t>(value & 0x7f) << shift;
      if ((value & 0x80) == 0) {
        return true;
      }
    }
    return false;
  }
  bool zigzag(int64_t &out)
  {
    uint64_t raw = 0;
    if (!varint(raw)) {
      return false;
    }
    out = static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
    return true;
  }
  bool int64(int64_t &out) { return zigzag(out); }
  bool int32(int32_t &out)
  {
    int64_t value = 0;
    if (!zigzag(value) || value < INT32_MIN || value > INT32_MAX) {
      return false;
    }
    out = static_cast<int32_t>(value);
    return true;
  }
  bool fixed64(uint64_t &out)
  {
    if (size - pos < 8) {
      return false;
    }
    out = read_le64(data + pos);
    pos += 8;
    return true;
  }
  bool binary(std::string_view &out)
  {
    uint64_t length = 0;
    if (!varint(length) || length > size - pos) {
      return false;
    }
    out = std::string_view(reinterpret_cast<const char *>(data + pos), static_cast<size_t>(length));
    pos += static_cast<size_t>(length);
    return true;
  }
  bool list_header(uint8_t &element_type, size_t &count)
  {
    uint8_t value = 0;
    if (!byte(value)) {
      return false;
    }
    element_type = static_cast<uint8_t>(value & 0x0f);
    uint64_t items = (value >> 4) & 0x0f;
    if (items == 15 && !varint(items)) {
      return false;
    }
    if (items > MAX_LIST_ITEMS) {
      return false;
    }
    count = static_cast<size_t>(items);
    return true;
  }
  FieldResult field(int16_t &last, int16_t &id, uint8_t &type)
  {
    uint8_t value = 0;
    if (!byte(value)) {
      return FIELD_ERROR;
    }
    type = static_cast<uint8_t>(value & 0x0f);
    if (type == CT_STOP) {
      return FIELD_STOP;
    }
    if (type > CT_STRUCT) {
      return FIELD_ERROR;
    }
    const int delta = (value >> 4) & 0x0f;
    if (delta == 0) {
      int64_t explicit_id = 0;
      if (!zigzag(explicit_id) || explicit_id < INT16_MIN || explicit_id > INT16_MAX) {
        return FIELD_ERROR;
      }
      id = static_cast<int16_t>(explicit_id);
    } else {
      id = static_cast<int16_t>(last + delta);
    }
    last = id;
    return FIELD_VALUE;
  }
  bool skip(uint8_t type, int depth)
  {
    if (depth > MAX_THRIFT_DEPTH) {
      return false;
    }
    switch (type) {
      case CT_TRUE:
      case CT_FALSE:
        return true;
      case CT_BYTE: {
        uint8_t value = 0;
        return byte(value);
      }
      case CT_I16:
      case CT_I32:
      case CT_I64: {
        int64_t value = 0;
        return zigzag(value);
      }
      case CT_DOUBLE: {
        uint64_t value = 0;
        return fixed64(value);
      }
      case CT_BINARY: {
        std::string_view value;
        return binary(value);
      }
      case CT_LIST:
      case CT_SET: {
        uint8_t element = 0;
        size_t count = 0;
        if (!list_header(element, count)) {
          return false;
        }
        for (size_t i = 0; i < count; ++i) {
          if (!skip(element, depth + 1)) {
            return false;
          }
        }
        return true;
      }
      case CT_MAP: {
        uint64_t count = 0;
        if (!varint(count) || count > MAX_LIST_ITEMS) {
          return false;
        }
        if (count == 0) {
          return true;
        }
        uint8_t types = 0;
        if (!byte(types)) {
          return false;
        }
        for (uint64_t i = 0; i < count; ++i) {
          if (!skip(static_cast<uint8_t>((types >> 4) & 0x0f), depth + 1)
              || !skip(static_cast<uint8_t>(types & 0x0f), depth + 1)) {
            return false;
          }
        }
        return true;
      }
      case CT_STRUCT: {
        int16_t last = 0;
        int16_t id = 0;
        uint8_t nested = 0;
        for (;;) {
          const FieldResult result = field(last, id, nested);
          if (result == FIELD_STOP) {
            return true;
          }
          if (result == FIELD_ERROR || !skip(nested, depth + 1)) {
            return false;
          }
        }
      }
      default:
        return false;
    }
  }
};

bool consume_empty_struct(CompactReader &reader)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  return reader.field(last, id, type) == FIELD_STOP;
}

// ---------------------------------------------------------------------------
// Footer metadata
// ---------------------------------------------------------------------------
struct SchemaElement {
  int32_t physical = -1;
  int32_t type_length = 0;
  int32_t repetition = -1;
  int32_t num_children = 0;
  int32_t logical = LOGIC_NONE;
  int32_t scale = 0;
  std::string_view name;
};

struct ColumnChunkMeta {
  std::string_view path;
  int32_t physical = -1;
  int32_t codec = -1;
  int64_t num_values = -1;
  int64_t data_page_offset = -1;
  int64_t dictionary_page_offset = -1;
  bool has_metadata = false;
};

struct RowGroupMeta {
  std::vector<ColumnChunkMeta> chunks;
  int64_t num_rows = -1;
};

struct FileMeta {
  int32_t version = 0;
  int64_t num_rows = -1;
  std::vector<SchemaElement> schema;
  std::vector<RowGroupMeta> row_groups;
};

struct PageHeader {
  int32_t type = -1;
  int32_t uncompressed_size = -1;
  int32_t compressed_size = -1;
  int32_t num_values = 0;
  int32_t encoding = -1;
  int32_t definition_encoding = -1;
  int32_t repetition_encoding = -1;
  int32_t definition_length = 0;
  int32_t repetition_length = 0;
  int32_t num_nulls = 0;
  int32_t num_rows = 0;
  bool is_compressed = true;
};

bool parse_time_unit(CompactReader &reader, int32_t &unit)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return unit != 0;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    if (type == CT_STRUCT) {
      if (!consume_empty_struct(reader)) {
        return false;
      }
      unit = id;
    } else if (!reader.skip(type, 1)) {
      return false;
    }
  }
}

bool parse_time_type(CompactReader &reader, int32_t &unit)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return unit != 0;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    if (id == 2) {
      if (type != CT_STRUCT || !parse_time_unit(reader, unit)) {
        return false;
      }
    } else if (!reader.skip(type, 2)) {
      return false;
    }
  }
}

bool parse_decimal_type(CompactReader &reader, int32_t &scale)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    if (id == 1) {
      if (type != CT_I32 || !reader.int32(scale)) {
        return false;
      }
    } else if (!reader.skip(type, 2)) {
      return false;
    }
  }
}

bool parse_logical_type(CompactReader &reader, SchemaElement &element)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        element.logical = LOGIC_STRING;
        if (type == CT_STRUCT && !consume_empty_struct(reader)) {
          return false;
        }
        break;
      case 4:
        element.logical = LOGIC_ENUM;
        if (type == CT_STRUCT && !consume_empty_struct(reader)) {
          return false;
        }
        break;
      case 5: {
        int32_t scale = 0;
        if (type != CT_STRUCT || !parse_decimal_type(reader, scale)) {
          return false;
        }
        element.logical = LOGIC_DECIMAL;
        element.scale = scale;
        break;
      }
      case 6:
        element.logical = LOGIC_DATE;
        if (type == CT_STRUCT && !consume_empty_struct(reader)) {
          return false;
        }
        break;
      case 7: {
        int32_t unit = 0;
        if (type != CT_STRUCT || !parse_time_type(reader, unit)) {
          return false;
        }
        element.logical =
            unit == 2 ? LOGIC_TIME_MICROS : (unit == 3 ? LOGIC_TIME_NANOS : LOGIC_TIME_MILLIS);
        break;
      }
      case 8: {
        int32_t unit = 0;
        if (type != CT_STRUCT || !parse_time_type(reader, unit)) {
          return false;
        }
        element.logical = unit == 2 ? LOGIC_TIMESTAMP_MICROS
                                   : (unit == 3 ? LOGIC_TIMESTAMP_NANOS : LOGIC_TIMESTAMP_MILLIS);
        break;
      }
      case 10:
        element.logical = LOGIC_INT_64;
        if (!reader.skip(type, 2)) {
          return false;
        }
        break;
      case 11:
        element.logical = LOGIC_UNKNOWN;
        if (type == CT_STRUCT && !consume_empty_struct(reader)) {
          return false;
        }
        break;
      case 12:
        element.logical = LOGIC_JSON;
        if (type == CT_STRUCT && !consume_empty_struct(reader)) {
          return false;
        }
        break;
      case 13:
        element.logical = LOGIC_BSON;
        if (type == CT_STRUCT && !consume_empty_struct(reader)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type, 2)) {
          return false;
        }
        break;
    }
  }
}

bool parse_schema_element(CompactReader &reader, SchemaElement &element)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  int32_t converted = -1;
  bool has_logical = false;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      if (element.physical < 0 && converted < 0 && element.num_children <= 0
          && element.name.empty()) {
        return false;
      }
      if (!has_logical) {
        switch (converted) {
          case 0:
          case 4:
          case 19:
          case 20:
            element.logical = LOGIC_STRING;
            break;
          case 5:
            element.logical = LOGIC_DECIMAL;
            break;
          case 6:
            element.logical = LOGIC_DATE;
            break;
          case 7:
            element.logical = LOGIC_TIME_MILLIS;
            break;
          case 8:
            element.logical = LOGIC_TIME_MICROS;
            break;
          case 9:
            element.logical = LOGIC_TIMESTAMP_MILLIS;
            break;
          case 10:
            element.logical = LOGIC_TIMESTAMP_MICROS;
            break;
          case 11:
            element.logical = LOGIC_UINT_8;
            break;
          case 12:
            element.logical = LOGIC_UINT_16;
            break;
          case 13:
            element.logical = LOGIC_UINT_32;
            break;
          case 14:
            element.logical = LOGIC_UINT_64;
            break;
          case 15:
            element.logical = LOGIC_INT_8;
            break;
          case 16:
            element.logical = LOGIC_INT_16;
            break;
          case 17:
            element.logical = LOGIC_INT_32;
            break;
          case 18:
            element.logical = LOGIC_INT_64;
            break;
          default:
            break;
        }
      }
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        if (type != CT_I32 || !reader.int32(element.physical)) {
          return false;
        }
        break;
      case 2:
        if (type != CT_I32 || !reader.int32(element.type_length)) {
          return false;
        }
        break;
      case 3:
        if (type != CT_I32 || !reader.int32(element.repetition)) {
          return false;
        }
        break;
      case 4: {
        std::string_view name;
        if (type != CT_BINARY || !reader.binary(name)
            || name.size() > ParquetReader::MAX_NAME_BYTES) {
          return false;
        }
        element.name = name;
        break;
      }
      case 5:
        if (type != CT_I32 || !reader.int32(element.num_children)) {
          return false;
        }
        break;
      case 6:
        if (type != CT_I32 || !reader.int32(converted)) {
          return false;
        }
        break;
      case 10:
        if (type != CT_STRUCT || !parse_logical_type(reader, element)) {
          return false;
        }
        has_logical = true;
        break;
      default:
        if (!reader.skip(type, 1)) {
          return false;
        }
        break;
    }
  }
}

bool parse_column_metadata(CompactReader &reader, ColumnChunkMeta &chunk)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        if (type != CT_I32 || !reader.int32(chunk.physical)) {
          return false;
        }
        break;
      case 3: {
        if (type != CT_LIST) {
          return false;
        }
        uint8_t element = 0;
        size_t count = 0;
        if (!reader.list_header(element, count) || count == 0) {
          return false;
        }
        for (size_t i = 0; i < count; ++i) {
          std::string_view part;
          if (!reader.binary(part)) {
            return false;
          }
          if (i + 1 == count) {
            chunk.path = part;
          }
        }
        break;
      }
      case 4:
        if (type != CT_I32 || !reader.int32(chunk.codec)) {
          return false;
        }
        break;
      case 5:
        if (type != CT_I64 || !reader.int64(chunk.num_values)) {
          return false;
        }
        break;
      case 9:
        if (type != CT_I64 || !reader.int64(chunk.data_page_offset)) {
          return false;
        }
        break;
      case 11:
        if (type != CT_I64 || !reader.int64(chunk.dictionary_page_offset)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type, 2)) {
          return false;
        }
        break;
    }
  }
}

bool parse_column_chunk(CompactReader &reader, ColumnChunkMeta &chunk)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    if (id == 3) {
      if (type != CT_STRUCT || !parse_column_metadata(reader, chunk)) {
        return false;
      }
      chunk.has_metadata = true;
    } else if (!reader.skip(type, 2)) {
      return false;
    }
  }
}

bool parse_row_group(CompactReader &reader, RowGroupMeta &group)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1: {
        if (type != CT_LIST) {
          return false;
        }
        uint8_t element = 0;
        size_t count = 0;
        if (!reader.list_header(element, count) || element != CT_STRUCT) {
          return false;
        }
        if (!reader.budget->resize(group.chunks, count)) return false;
        for (size_t i = 0; i < count; ++i) {
          if (!parse_column_chunk(reader, group.chunks[i])) {
            return false;
          }
        }
        break;
      }
      case 3:
        if (type != CT_I64 || !reader.int64(group.num_rows)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type, 2)) {
          return false;
        }
        break;
    }
  }
}

bool parse_file_metadata(CompactReader &reader, FileMeta &meta)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        if (type != CT_I32 || !reader.int32(meta.version)) {
          return false;
        }
        break;
      case 2: {
        if (type != CT_LIST) {
          return false;
        }
        uint8_t element = 0;
        size_t count = 0;
        if (!reader.list_header(element, count) || element != CT_STRUCT) {
          return false;
        }
        if (!reader.budget->resize(meta.schema, count)) return false;
        for (size_t i = 0; i < count; ++i) {
          if (!parse_schema_element(reader, meta.schema[i])) {
            return false;
          }
        }
        break;
      }
      case 3:
        if (type != CT_I64 || !reader.int64(meta.num_rows)) {
          return false;
        }
        break;
      case 4: {
        if (type != CT_LIST) {
          return false;
        }
        uint8_t element = 0;
        size_t count = 0;
        if (!reader.list_header(element, count) || element != CT_STRUCT) {
          return false;
        }
        if (!reader.budget->resize(meta.row_groups, count)) return false;
        for (size_t i = 0; i < count; ++i) {
          if (!parse_row_group(reader, meta.row_groups[i])) {
            return false;
          }
        }
        break;
      }
      default:
        if (!reader.skip(type, 1)) {
          return false;
        }
        break;
    }
  }
}

bool parse_data_page_header(CompactReader &reader, PageHeader &header)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        if (type != CT_I32 || !reader.int32(header.num_values)) {
          return false;
        }
        break;
      case 2:
        if (type != CT_I32 || !reader.int32(header.encoding)) {
          return false;
        }
        break;
      case 3:
        if (type != CT_I32 || !reader.int32(header.definition_encoding)) {
          return false;
        }
        break;
      case 4:
        if (type != CT_I32 || !reader.int32(header.repetition_encoding)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type, 3)) {
          return false;
        }
        break;
    }
  }
}

bool parse_dictionary_page_header(CompactReader &reader, PageHeader &header)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    if (id == 1) {
      if (type != CT_I32 || !reader.int32(header.num_values)) {
        return false;
      }
    } else if (id == 2) {
      if (type != CT_I32 || !reader.int32(header.encoding)) {
        return false;
      }
    } else if (!reader.skip(type, 3)) {
      return false;
    }
  }
}

bool parse_data_page_header_v2(CompactReader &reader, PageHeader &header)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        if (type != CT_I32 || !reader.int32(header.num_values)) {
          return false;
        }
        break;
      case 2:
        if (type != CT_I32 || !reader.int32(header.num_nulls)) {
          return false;
        }
        break;
      case 3:
        if (type != CT_I32 || !reader.int32(header.num_rows)) {
          return false;
        }
        break;
      case 4:
        if (type != CT_I32 || !reader.int32(header.encoding)) {
          return false;
        }
        break;
      case 5:
        if (type != CT_I32 || !reader.int32(header.definition_length)) {
          return false;
        }
        break;
      case 6:
        if (type != CT_I32 || !reader.int32(header.repetition_length)) {
          return false;
        }
        break;
      case 7:
        if (type == CT_TRUE) {
          header.is_compressed = true;
        } else if (type == CT_FALSE) {
          header.is_compressed = false;
        } else {
          return false;
        }
        break;
      default:
        if (!reader.skip(type, 3)) {
          return false;
        }
        break;
    }
  }
}

bool parse_page_header(CompactReader &reader, PageHeader &header)
{
  int16_t last = 0;
  int16_t id = 0;
  uint8_t type = 0;
  for (;;) {
    const FieldResult result = reader.field(last, id, type);
    if (result == FIELD_STOP) {
      return true;
    }
    if (result == FIELD_ERROR) {
      return false;
    }
    switch (id) {
      case 1:
        if (type != CT_I32 || !reader.int32(header.type)) {
          return false;
        }
        break;
      case 2:
        if (type != CT_I32 || !reader.int32(header.uncompressed_size)) {
          return false;
        }
        break;
      case 3:
        if (type != CT_I32 || !reader.int32(header.compressed_size)) {
          return false;
        }
        break;
      case 5:
        if (type != CT_STRUCT || !parse_data_page_header(reader, header)) {
          return false;
        }
        break;
      case 7:
        if (type != CT_STRUCT || !parse_dictionary_page_header(reader, header)) {
          return false;
        }
        break;
      case 8:
        if (type != CT_STRUCT || !parse_data_page_header_v2(reader, header)) {
          return false;
        }
        break;
      default:
        if (!reader.skip(type, 1)) {
          return false;
        }
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// Snappy: raw block format, no framing
// ---------------------------------------------------------------------------
bool snappy_uncompress(const char *input, size_t input_size, char *output, size_t output_size)
{
  const uint8_t *in = reinterpret_cast<const uint8_t *>(input);
  size_t pos = 0;
  uint64_t length = 0;
  int shift = 0;
  for (;;) {
    if (pos >= input_size || shift >= 64) {
      return false;
    }
    const uint8_t value = in[pos++];
    length |= static_cast<uint64_t>(value & 0x7f) << shift;
    if ((value & 0x80) == 0) {
      break;
    }
    shift += 7;
  }
  if (length != output_size) {
    return false;
  }
  uint8_t *out = reinterpret_cast<uint8_t *>(output);
  size_t written = 0;
  while (pos < input_size) {
    const uint8_t tag = in[pos++];
    const int type = tag & 0x03;
    if (type == 0) {
      size_t run = tag >> 2;
      if (run >= 60) {
        const size_t extra = run - 59;
        if (extra > 4 || input_size - pos < extra) {
          return false;
        }
        run = 0;
        for (size_t i = 0; i < extra; ++i) {
          run |= static_cast<size_t>(in[pos + i]) << (8 * i);
        }
        pos += extra;
        run += 1;
      } else {
        run += 1;
      }
      if (run > input_size - pos || run > output_size - written) {
        return false;
      }
      std::memcpy(out + written, in + pos, run);
      pos += run;
      written += run;
    } else {
      size_t run = 0;
      size_t offset = 0;
      if (type == 1) {
        if (pos >= input_size) {
          return false;
        }
        run = 4 + ((tag >> 2) & 0x07);
        offset = (static_cast<size_t>(tag & 0xe0) << 3) | in[pos];
        pos += 1;
      } else if (type == 2) {
        if (input_size - pos < 2) {
          return false;
        }
        run = (tag >> 2) + 1;
        offset = static_cast<size_t>(in[pos]) | (static_cast<size_t>(in[pos + 1]) << 8);
        pos += 2;
      } else {
        if (input_size - pos < 4) {
          return false;
        }
        run = (tag >> 2) + 1;
        offset = static_cast<size_t>(read_le32(in + pos));
        pos += 4;
      }
      if (offset == 0 || offset > written || run > output_size - written) {
        return false;
      }
      for (size_t i = 0; i < run; ++i) {
        out[written] = out[written - offset];
        written += 1;
      }
    }
  }
  return written == output_size;
}

// ---------------------------------------------------------------------------
// Level and dictionary index decoders
// ---------------------------------------------------------------------------
bool unpack_bits(const uint8_t *data, size_t size, size_t index, uint32_t width, uint32_t &out)
{
  if (width == 0) {
    out = 0;
    return true;
  }
  const size_t bit = index * static_cast<size_t>(width);
  const size_t byte = bit / 8;
  const size_t shift = bit % 8;
  const size_t need = (shift + static_cast<size_t>(width) + 7) / 8;
  if (byte + need > size) {
    return false;
  }
  uint64_t acc = 0;
  for (size_t i = 0; i < need; ++i) {
    acc |= static_cast<uint64_t>(data[byte + i]) << (8 * i);
  }
  const uint64_t mask = (static_cast<uint64_t>(1) << width) - 1;
  out = static_cast<uint32_t>((acc >> shift) & mask);
  return true;
}

// RLE / bit-packing hybrid. An RLE run is exposed as a (value, count) pair and a
// bit-packed run is decoded in place, so paging never materializes a per-page
// level or index array.
struct HybridDecoder {
  const uint8_t *data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  uint32_t width = 0;
  uint64_t run_value = 0;
  uint64_t run_left = 0;
  const uint8_t *packed = nullptr;
  size_t packed_size = 0;
  size_t packed_values = 0;
  size_t packed_read = 0;

  void reset(const uint8_t *bytes, size_t bytes_size, uint32_t bit_width)
  {
    data = bytes;
    size = bytes_size;
    pos = 0;
    width = bit_width;
    run_value = 0;
    run_left = 0;
    packed = nullptr;
    packed_size = 0;
    packed_values = 0;
    packed_read = 0;
  }
  bool next(uint32_t &out)
  {
    for (;;) {
      if (run_left > 0) {
        out = static_cast<uint32_t>(run_value);
        --run_left;
        return true;
      }
      if (packed_read < packed_values) {
        if (!unpack_bits(packed, packed_size, packed_read, width, out)) {
          return false;
        }
        ++packed_read;
        return true;
      }
      if (pos >= size) {
        return false;
      }
      uint64_t header = 0;
      int shift = 0;
      for (;;) {
        if (pos >= size || shift >= 64) {
          return false;
        }
        const uint8_t value = data[pos++];
        header |= static_cast<uint64_t>(value & 0x7f) << shift;
        if ((value & 0x80) == 0) {
          break;
        }
        shift += 7;
      }
      if ((header & 1) == 0) {
        const size_t bytes = (static_cast<size_t>(width) + 7) / 8;
        if (bytes > size - pos) {
          return false;
        }
        run_value = 0;
        for (size_t i = 0; i < bytes; ++i) {
          run_value |= static_cast<uint64_t>(data[pos + i]) << (8 * i);
        }
        pos += bytes;
        run_left = header >> 1;
      } else {
        const uint64_t groups = header >> 1;
        const uint64_t bytes = groups * static_cast<uint64_t>(width);
        if (bytes > static_cast<uint64_t>(size - pos)) {
          return false;
        }
        packed = data + pos;
        packed_size = static_cast<size_t>(bytes);
        packed_values = static_cast<size_t>(groups) * 8;
        packed_read = 0;
        pos += static_cast<size_t>(bytes);
      }
    }
  }
};

// Definition levels of one page. The deprecated BIT_PACKED encoding stores the
// levels inline without a length prefix; RLE stores a 4-byte length prefix.
struct LevelDecoder {
  enum Mode { NONE = 0, RLE = 1, BIT_PACKED = 2 };
  Mode mode = NONE;
  HybridDecoder hybrid;
  const uint8_t *bits = nullptr;
  size_t bits_size = 0;
  size_t bits_values = 0;
  size_t bits_read = 0;

  void reset()
  {
    mode = NONE;
    hybrid.reset(nullptr, 0, 0);
    bits = nullptr;
    bits_size = 0;
    bits_values = 0;
    bits_read = 0;
  }
  bool next(uint32_t &out)
  {
    if (mode == RLE) {
      return hybrid.next(out);
    }
    if (mode == BIT_PACKED) {
      if (bits_read >= bits_values) {
        return false;
      }
      const bool ok = unpack_bits(bits, bits_size, bits_read, 1, out);
      ++bits_read;
      return ok;
    }
    out = 0;
    return true;
  }
};

// ---------------------------------------------------------------------------
// Text rendering: every value is exposed as text, so the executor keeps one
// conversion path shared with the CSV and JSONL readers.
// ---------------------------------------------------------------------------
void append_int(BudgetText &out, int64_t value)
{
  char buffer[24];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc()) {
    out.append(buffer, static_cast<size_t>(result.ptr - buffer));
  }
}

void append_uint(BudgetText &out, uint64_t value)
{
  char buffer[24];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec == std::errc()) {
    out.append(buffer, static_cast<size_t>(result.ptr - buffer));
  }
}

void append_double(BudgetText &out, double value)
{
  // %.17g round-trips through the executor's floating point parser, which is
  // what keeps DOUBLE and FLOAT columns exact.
  char buffer[48];
  const int digits = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (digits > 0) {
    const size_t length = static_cast<size_t>(digits) < sizeof(buffer) ? static_cast<size_t>(digits)
                                                                      : sizeof(buffer) - 1;
    out.append(buffer, length);
  }
}

struct CivilDate {
  int64_t year = 1970;
  int32_t month = 1;
  int32_t day = 1;
};

CivilDate civil_from_days(int64_t days)
{
  const int64_t shifted = days + 719468;
  const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const int64_t day_of_era = shifted - era * 146097;
  const int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const int64_t year = year_of_era + era * 400;
  const int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const int64_t month_prime = (5 * day_of_year + 2) / 153;
  CivilDate date;
  date.day = static_cast<int32_t>(day_of_year - (153 * month_prime + 2) / 5 + 1);
  date.month = static_cast<int32_t>(month_prime + (month_prime < 10 ? 3 : -9));
  date.year = year + (date.month <= 2 ? 1 : 0);
  return date;
}

void append_date(BudgetText &out, int64_t days)
{
  const CivilDate date = civil_from_days(days);
  char buffer[48];
  const int digits = std::snprintf(buffer, sizeof(buffer), "%04lld-%02d-%02d",
                                   static_cast<long long>(date.year), date.month, date.day);
  if (digits > 0) {
    out.append(buffer);
  }
}

// Renders an integral count `value` of `units_per_second` as HH:MM:SS, with
// exactly `fraction_digits` decimals when the value is not a whole second.
void append_time(BudgetText &out, int64_t value, int64_t units_per_second, int fraction_digits)
{
  const int64_t seconds = value / units_per_second;
  const int64_t fraction = value % units_per_second;
  const long long hour = static_cast<long long>(seconds / 3600);
  const long long minute = static_cast<long long>((seconds / 60) % 60);
  const long long second = static_cast<long long>(seconds % 60);
  char buffer[64];
  int digits = 0;
  if (fraction == 0) {
    digits = std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", hour, minute, second);
  } else if (fraction_digits == 3) {
    digits = std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld", hour, minute,
                           second, static_cast<long long>(fraction));
  } else if (fraction_digits == 9) {
    digits = std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%09lld", hour, minute,
                           second, static_cast<long long>(fraction));
  } else {
    digits = std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%06lld", hour, minute,
                           second, static_cast<long long>(fraction));
  }
  if (digits > 0) {
    out.append(buffer);
  }
}

void append_datetime(BudgetText &out, int64_t days, int64_t seconds_of_day, int64_t micros)
{
  const CivilDate date = civil_from_days(days);
  char buffer[64];
  int digits = 0;
  const long long year = static_cast<long long>(date.year);
  const long long hour = static_cast<long long>(seconds_of_day / 3600);
  const long long minute = static_cast<long long>((seconds_of_day / 60) % 60);
  const long long second = static_cast<long long>(seconds_of_day % 60);
  if (micros == 0) {
    digits = std::snprintf(buffer, sizeof(buffer), "%04lld-%02d-%02d %02lld:%02lld:%02lld", year,
                           date.month, date.day, hour, minute, second);
  } else {
    digits = std::snprintf(buffer, sizeof(buffer), "%04lld-%02d-%02d %02lld:%02lld:%02lld.%06lld",
                           year, date.month, date.day, hour, minute, second,
                           static_cast<long long>(micros));
    // seekdb renders fractional seconds without trailing zeros.
    while (digits > 0 && buffer[digits - 1] == '0') {
      --digits;
    }
  }
  if (digits > 0) {
    out.append(buffer, static_cast<size_t>(digits));
  }
}

void append_timestamp(BudgetText &out, int64_t value, int64_t units_per_second)
{
  int64_t seconds = value / units_per_second;
  int64_t remainder = value % units_per_second;
  if (remainder < 0) {
    remainder += units_per_second;
    seconds -= 1;
  }
  int64_t micros = remainder;
  if (units_per_second == 1000000000LL) {
    micros = remainder / 1000;
  } else if (units_per_second == 1000LL) {
    micros = remainder * 1000;
  }
  int64_t days = seconds / SECONDS_PER_DAY;
  int64_t rest = seconds % SECONDS_PER_DAY;
  if (rest < 0) {
    rest += SECONDS_PER_DAY;
    days -= 1;
  }
  append_datetime(out, days, rest, micros);
}

void append_int96(BudgetText &out, int32_t julian, int64_t nanos)
{
  const int64_t days = static_cast<int64_t>(julian) - 2440588;
  const int64_t seconds = nanos / 1000000000LL;
  const int64_t micros = (nanos % 1000000000LL) / 1000;
  append_datetime(out, days, seconds, micros);
}

void append_decimal(BudgetText &out, int64_t value, int32_t scale)
{
  double divisor = 1;
  for (int32_t i = 0; i < scale && i < 18; ++i) {
    divisor *= 10;
  }
  append_double(out, static_cast<double>(value) / divisor);
}

// Per-column paging state: the rendered dictionary, one decoded page, and the
// level/index readers that walk it.
struct ColumnState {
  int64_t page_offset = 0;
  int64_t values_left = 0;
  size_t encoding = ENC_PLAIN;
  size_t values_offset = 0;
  size_t values_size = 0;
  size_t values_pos = 0;
  int32_t page_values = 0;
  int32_t page_values_read = 0;
  size_t defined_read = 0;
  std::vector<char> page;
  BudgetText dictionary;
  std::vector<ParquetReader::Cell> dictionary_cells;
  LevelDecoder levels;
  HybridDecoder indices;
};

} // namespace

// ---------------------------------------------------------------------------
// Reader implementation
// ---------------------------------------------------------------------------
struct ParquetReader::Impl {
  MemoryBudget memory;
  SecureFile file;
  ParquetOptions options;
  Status status = Status::Ok;
  Status pending = Status::Ok;
  char error[512] = {};
  bool opened = false;
  bool inferred = false;
  bool bound = false;
  bool rows_started = false;
  int64_t file_size = 0;
  int64_t file_rows = 0;
  FileIdentity identity;
  std::vector<SchemaElement> elements;
  std::vector<RowGroupMeta> groups;
  std::vector<Column> columns;
  size_t column_count = 0;
  std::vector<ColumnState> states;
  std::vector<Cell> cells;
  std::vector<size_t> projection;
  std::vector<uint8_t> projected;
  BudgetText text;
  size_t group = 0;
  size_t current_group = 0;
  int64_t rows_left = 0;
  uint64_t row_index = 0;
  std::vector<char> buffer;
  std::vector<char> window;
  std::vector<char> footer;

  Impl() { text.budget = &memory; }

  Status memory_failure()
  {
    return fail(Status::ResourceLimit, "Parquet reader exceeds max_memory_bytes");
  }

  Status fail(Status result, const char *message)
  {
    status = result;
    if (message != nullptr) {
      std::snprintf(error, sizeof(error), "%s", message);
    }
    return result;
  }

  static void reset_state(ColumnState &state)
  {
    state.page_offset = 0;
    state.values_left = 0;
    state.encoding = ENC_PLAIN;
    state.values_offset = 0;
    state.values_size = 0;
    state.values_pos = 0;
    state.page_values = 0;
    state.page_values_read = 0;
    state.defined_read = 0;
    state.page.clear();
    state.dictionary.clear();
    state.dictionary_cells.clear();
    state.levels.reset();
    state.indices.reset(nullptr, 0, 0);
  }

  Status read_exact(int64_t offset, size_t length, std::vector<char> &target)
  {
    if (!memory.resize(target, length)) return memory_failure();
    if (length == 0) {
      return Status::Ok;
    }
    size_t got = 0;
    const Status result = file.read_at(target.data(), length, offset, got);
    if (result == Status::Io) {
      return fail(Status::Io, file.error());
    }
    if (result != Status::Ok || got != length) {
      return fail(Status::Malformed, "file is truncated");
    }
    return Status::Ok;
  }

  Status describe(const SchemaElement &element, Column &column)
  {
    if (element.name.size() > MAX_NAME_BYTES) {
      return fail(Status::Malformed, "column name is too long");
    }
    std::memcpy(column.name, element.name.data(), element.name.size());
    column.name[element.name.size()] = '\0';
    column.nullable = element.repetition == REP_OPTIONAL;
    column.max_bytes = 0;
    switch (element.physical) {
      case P_BOOLEAN:
        column.type = Type::Boolean;
        break;
      case P_INT32:
        if (element.logical == LOGIC_DATE) {
          column.type = Type::Date;
        } else if (element.logical == LOGIC_TIME_MILLIS) {
          column.type = Type::Varchar;
          column.max_bytes = TIME_TEXT_BYTES;
        } else if (element.logical == LOGIC_DECIMAL) {
          column.type = Type::Double;
        } else {
          column.type = Type::BigInt;
        }
        break;
      case P_INT64:
        if (element.logical == LOGIC_TIMESTAMP_MILLIS || element.logical == LOGIC_TIMESTAMP_MICROS
            || element.logical == LOGIC_TIMESTAMP_NANOS) {
          column.type = Type::DateTime;
        } else if (element.logical == LOGIC_TIME_MICROS || element.logical == LOGIC_TIME_NANOS) {
          column.type = Type::Varchar;
          column.max_bytes = TIME_TEXT_BYTES;
        } else if (element.logical == LOGIC_DECIMAL) {
          column.type = Type::Double;
        } else {
          column.type = Type::BigInt;
        }
        break;
      case P_INT96:
        column.type = Type::DateTime;
        break;
      case P_FLOAT:
      case P_DOUBLE:
        column.type = Type::Double;
        break;
      case P_BYTE_ARRAY:
      case P_FLBA:
        if (element.logical == LOGIC_DECIMAL) {
          return fail(Status::Unsupported, "binary DECIMAL columns are not supported");
        }
        // A footer statistic cannot bound a value's length (the lexicographic
        // maximum is not the longest value), so the whole VARCHAR domain is
        // reserved and a longer value fails closed while decoding.
        column.type = Type::Varchar;
        column.max_bytes = VARCHAR_BYTES;
        break;
      default:
        return fail(Status::Unsupported, "unknown physical type");
    }
    return Status::Ok;
  }

  Status validate_schema(const FileMeta &meta)
  {
    if (meta.version < 1) {
      return fail(Status::Malformed, "file metadata has no version");
    }
    if (meta.schema.empty()) {
      return fail(Status::Malformed, "file metadata has no schema");
    }
    const SchemaElement &root = meta.schema[0];
    if (root.physical >= 0 || root.num_children <= 0) {
      return fail(Status::Malformed, "schema root is not a group");
    }
    if (static_cast<size_t>(root.num_children) > MAX_COLUMNS) {
      return fail(Status::ResourceLimit, "Parquet file has too many columns");
    }
    if (static_cast<size_t>(root.num_children) + 1 != meta.schema.size()) {
      return fail(Status::Unsupported, "nested Parquet schemas are not supported");
    }
    for (size_t i = 1; i < meta.schema.size(); ++i) {
      const SchemaElement &element = meta.schema[i];
      if (element.num_children > 0) {
        return fail(Status::Unsupported, "nested Parquet schemas are not supported");
      }
      if (element.repetition == REP_REPEATED) {
        return fail(Status::Unsupported, "repeated Parquet fields are not supported");
      }
      if (element.repetition != REP_REQUIRED && element.repetition != REP_OPTIONAL) {
        return fail(Status::Malformed, "schema element has no repetition type");
      }
      if (element.physical < P_BOOLEAN || element.physical > P_FLBA) {
        return fail(Status::Malformed, "schema element has no physical type");
      }
      if (element.name.empty()) {
        return fail(Status::Malformed, "schema element has no name");
      }
      if (!is_valid_utf8(element.name)) {
        return fail(Status::Malformed, "column name is not valid UTF-8");
      }
      if (element.physical == P_FLBA
          && (element.type_length <= 0 || element.type_length > 65536)) {
        return fail(Status::Malformed, "invalid fixed length byte array");
      }
    }
    return Status::Ok;
  }

  Status validate_groups()
  {
    if (file_rows < 0) {
      return fail(Status::Malformed, "file metadata has no row count");
    }
    int64_t total = 0;
    for (const RowGroupMeta &row_group : groups) {
      if (row_group.num_rows < 0) {
        return fail(Status::Malformed, "row group has no row count");
      }
      total += row_group.num_rows;
      if (row_group.chunks.size() != column_count) {
        return fail(Status::Malformed, "row group does not match the schema");
      }
      for (size_t i = 0; i < column_count; ++i) {
        const ColumnChunkMeta &chunk = row_group.chunks[i];
        const SchemaElement &element = elements[i];
        if (!chunk.has_metadata) {
          return fail(Status::Malformed, "column chunk has no metadata");
        }
        if (!chunk.path.empty() && chunk.path != element.name) {
          return fail(Status::Malformed, "column chunk path does not match the schema");
        }
        if (chunk.physical != element.physical) {
          return fail(Status::Malformed, "column chunk type does not match the schema");
        }
        if (chunk.codec != CODEC_UNCOMPRESSED && chunk.codec != CODEC_SNAPPY) {
          return fail(Status::Unsupported, "Parquet compression codec is not supported");
        }
        if (chunk.num_values < 0) {
          return fail(Status::Malformed, "column chunk has no value count");
        }
        if (row_group.num_rows > 0
            && (chunk.data_page_offset < 4 || chunk.data_page_offset >= file_size)) {
          return fail(Status::Malformed, "column chunk offset is outside the file");
        }
        if (chunk.dictionary_page_offset >= 0
            && (chunk.dictionary_page_offset < 4
                || chunk.dictionary_page_offset >= chunk.data_page_offset)) {
          return fail(Status::Malformed, "dictionary page offset is invalid");
        }
      }
    }
    if (total != file_rows) {
      return fail(Status::Malformed, "row groups do not add up to the file row count");
    }
    return Status::Ok;
  }

  Status parse_footer()
  {
    if (file_size < 12) {
      return fail(Status::Malformed, "file is too small to be Parquet");
    }
    char trailer[8];
    size_t got = 0;
    const Status trailer_status = file.read_at(trailer, sizeof(trailer), file_size - 8, got);
    if (trailer_status == Status::Io) {
      return fail(Status::Io, file.error());
    }
    if (trailer_status != Status::Ok || got != sizeof(trailer)) {
      return fail(Status::Malformed, "file is truncated");
    }
    if (std::memcmp(trailer + 4, "PAR1", 4) != 0) {
      return fail(Status::Malformed, "missing footer magic");
    }
    const uint32_t metadata_size = read_le32(reinterpret_cast<const uint8_t *>(trailer));
    if (metadata_size < 1 || static_cast<int64_t>(metadata_size) > file_size - 12) {
      return fail(Status::Malformed, "invalid footer length");
    }
    if (static_cast<size_t>(metadata_size) > MAX_FOOTER_BYTES) {
      return fail(Status::ResourceLimit, "footer metadata is too large");
    }
    char header[4];
    const Status header_status = file.read_at(header, sizeof(header), 0, got);
    if (header_status == Status::Io) {
      return fail(Status::Io, file.error());
    }
    if (header_status != Status::Ok || got != sizeof(header)
        || std::memcmp(header, "PAR1", 4) != 0) {
      return fail(Status::Malformed, "missing header magic");
    }
    const int64_t metadata_offset = file_size - 8 - static_cast<int64_t>(metadata_size);
    const Status read_status = read_exact(metadata_offset, metadata_size, footer);
    if (read_status != Status::Ok) {
      return read_status;
    }
    CompactReader reader;
    reader.data = reinterpret_cast<const uint8_t *>(footer.data());
    reader.size = footer.size();
    reader.budget = &memory;
    FileMeta meta;
    if (!parse_file_metadata(reader, meta)) {
      return memory.exhausted ? memory_failure()
                              : fail(Status::Malformed, "invalid file metadata");
    }
    file_rows = meta.num_rows;
    // Validate before the metadata is moved into the reader state: the schema
    // checks read the parsed footer, not the reader fields.
    const Status schema_status = validate_schema(meta);
    if (schema_status != Status::Ok) {
      return schema_status;
    }
    elements = std::move(meta.schema);
    // Drop the schema root: leaf elements are addressed by column index.
    elements.erase(elements.begin());
    groups = std::move(meta.row_groups);
    column_count = elements.size();
    return validate_groups();
  }

  void close_file()
  {
    file.close();
    opened = false;
    inferred = false;
    bound = false;
    rows_started = false;
    rows_left = 0;
    group = 0;
    current_group = 0;
    std::vector<SchemaElement>().swap(elements);
    std::vector<RowGroupMeta>().swap(groups);
    std::vector<Column>().swap(columns);
    std::vector<ColumnState>().swap(states);
    std::vector<Cell>().swap(cells);
    std::vector<size_t>().swap(projection);
    std::vector<uint8_t>().swap(projected);
    column_count = 0;
    file_size = 0;
    file_rows = 0;
    row_index = 0;
    std::vector<char>().swap(text.bytes);
    std::vector<char>().swap(footer);
    std::vector<char>().swap(buffer);
    std::vector<char>().swap(window);
    memory = MemoryBudget();
  }

  Status do_open(const char *path, const char *secure_root, const ParquetOptions &open_options)
  {
    close_file();
    status = Status::Ok;
    pending = Status::Ok;
    error[0] = '\0';
    if (!open_options.enabled) {
      return fail(Status::Disabled, "File SQL is not enabled");
    }
    if (open_options.sample_size != -1) {
      return fail(Status::Unsupported, "sample_size other than -1 is not supported");
    }
    if (open_options.max_page_bytes == 0 || open_options.max_page_bytes > MAX_PAGE_BYTES) {
      return fail(Status::InvalidOption, "max_page_bytes is out of range");
    }
    if (open_options.max_memory_bytes == 0) {
      return fail(Status::InvalidOption, "max_memory_bytes must be positive");
    }
    options = open_options;
    memory.limit = options.max_memory_bytes;
    const Status open_status = file.open(path, secure_root);
    if (open_status != Status::Ok) {
      return fail(open_status, file.error());
    }
    identity = file.identity();
    file_size = file.size();
    const Status footer_status = parse_footer();
    if (footer_status != Status::Ok) {
      close_file();
      return footer_status;
    }
    if (!memory.resize(columns, column_count)
        || !memory.resize(states, column_count)
        || !memory.resize(cells, column_count)
        || !memory.reserve(projection, column_count)
        || !memory.resize(projected, column_count)) {
      close_file();
      return memory_failure();
    }
    for (size_t i = 0; i < column_count; ++i) {
      const Status describe_status = describe(elements[i], columns[i]);
      if (describe_status != Status::Ok) {
        close_file();
        return describe_status;
      }
    }

    for (size_t i = 0; i < column_count; ++i) {
      states[i].dictionary.budget = &memory;
      reset_state(states[i]);
    }
    cells.assign(column_count, Cell());
    projection.clear();
    for (size_t i = 0; i < column_count; ++i) {
      projection.push_back(i);
    }
    projected.assign(column_count, 1);
    opened = true;
    return Status::Ok;
  }

  Status require_ready()
  {
    if (!opened) {
      return fail(Status::InvalidOption, "Parquet file is not open");
    }
    if (!inferred && !bound) {
      return fail(Status::InvalidOption, "schema was not inferred or bound");
    }
    return Status::Ok;
  }

  Status do_infer()
  {
    if (!opened) {
      return fail(Status::InvalidOption, "Parquet file is not open");
    }
    inferred = true;
    return do_rescan();
  }

  Status do_bind(const Column *planned, size_t count, const FileIdentity &expected)
  {
    if (!opened) {
      return fail(Status::InvalidOption, "Parquet file is not open");
    }
    if (planned == nullptr || count == 0) {
      return fail(Status::InvalidOption, "planned schema is empty");
    }
    if (count != column_count) {
      return fail(Status::InvalidOption, "planned schema column count does not match the file");
    }
    if (!(expected == identity)) {
      return fail(Status::Changed, "Parquet file changed since schema inference");
    }
    const Status unchanged = file.check_unchanged();
    if (unchanged != Status::Ok) {
      return fail(unchanged, "Parquet file changed since schema inference");
    }
    for (size_t i = 0; i < count; ++i) {
      if (!equal_ascii_ci(columns[i].name, planned[i].name)) {
        return fail(Status::Malformed, "planned column name does not match the file schema");
      }
      if (columns[i].type != planned[i].type || columns[i].nullable != planned[i].nullable) {
        return fail(Status::Malformed, "planned column type does not match the file schema");
      }
    }
    bound = true;
    return do_rescan();
  }

  Status do_rescan()
  {
    if (!opened) {
      return fail(Status::InvalidOption, "Parquet file is not open");
    }
    group = 0;
    current_group = 0;
    rows_left = 0;
    row_index = 0;
    rows_started = false;
    pending = Status::Ok;
    text.clear();
    for (size_t i = 0; i < states.size(); ++i) {
      states[i].dictionary.budget = &memory;
      reset_state(states[i]);
    }
    for (size_t i = 0; i < cells.size(); ++i) {
      cells[i] = Cell();
    }
    return Status::Ok;
  }

  Status start_group(size_t index)
  {
    const Status unchanged = file.check_unchanged();
    if (unchanged != Status::Ok) {
      return fail(unchanged, "Parquet file changed while it was being read");
    }
    const RowGroupMeta &row_group = groups[index];
    if (row_group.chunks.size() != column_count) {
      return fail(Status::Malformed, "row group does not match the schema");
    }
    current_group = index;
    for (size_t i = 0; i < column_count; ++i) {
      ColumnState &state = states[i];
      reset_state(state);
      const ColumnChunkMeta &chunk = row_group.chunks[i];
      if (chunk.num_values != row_group.num_rows) {
        return fail(Status::Malformed, "column chunk value count does not match the row group");
      }
      state.values_left = chunk.num_values;
      state.page_offset = chunk.dictionary_page_offset >= 0 ? chunk.dictionary_page_offset
                                                           : chunk.data_page_offset;
    }
    rows_left = row_group.num_rows;
    return Status::Ok;
  }

  Status decode_plain(const SchemaElement &element, const uint8_t *data, size_t size, size_t &pos,
                      BudgetText &out)
  {
    switch (element.physical) {
      case P_BOOLEAN: {
        if (pos / 8 >= size) {
          return fail(Status::Malformed, "boolean values are truncated");
        }
        const bool set = ((data[pos / 8] >> (pos % 8)) & 1u) != 0;
        pos += 1;
        out.append(set ? "true" : "false");
        return Status::Ok;
      }
      case P_INT32: {
        if (size - pos < 4) {
          return fail(Status::Malformed, "int32 values are truncated");
        }
        const int32_t value = static_cast<int32_t>(read_le32(data + pos));
        pos += 4;
        append_int32(out, element, value);
        return Status::Ok;
      }
      case P_INT64: {
        if (size - pos < 8) {
          return fail(Status::Malformed, "int64 values are truncated");
        }
        const int64_t value = static_cast<int64_t>(read_le64(data + pos));
        pos += 8;
        append_int64(out, element, value);
        return Status::Ok;
      }
      case P_INT96: {
        if (size - pos < 12) {
          return fail(Status::Malformed, "int96 values are truncated");
        }
        const int64_t nanos = static_cast<int64_t>(read_le64(data + pos));
        const int32_t julian = static_cast<int32_t>(read_le32(data + pos + 8));
        pos += 12;
        if (julian < 0 || nanos < 0 || nanos >= SECONDS_PER_DAY * 1000000000LL) {
          return fail(Status::Malformed, "int96 timestamp is out of range");
        }
        append_int96(out, julian, nanos);
        return Status::Ok;
      }
      case P_FLOAT: {
        if (size - pos < 4) {
          return fail(Status::Malformed, "float values are truncated");
        }
        const uint32_t bits = read_le32(data + pos);
        pos += 4;
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        append_double(out, static_cast<double>(value));
        return Status::Ok;
      }
      case P_DOUBLE: {
        if (size - pos < 8) {
          return fail(Status::Malformed, "double values are truncated");
        }
        const uint64_t bits = read_le64(data + pos);
        pos += 8;
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        append_double(out, value);
        return Status::Ok;
      }
      case P_BYTE_ARRAY: {
        if (size - pos < 4) {
          return fail(Status::Malformed, "byte array is truncated");
        }
        const uint32_t length = read_le32(data + pos);
        pos += 4;
        if (static_cast<size_t>(length) > size - pos) {
          return fail(Status::Malformed, "byte array is truncated");
        }
        out.append(reinterpret_cast<const char *>(data + pos), length);
        pos += length;
        return Status::Ok;
      }
      case P_FLBA: {
        const size_t length = static_cast<size_t>(element.type_length);
        if (length > size - pos) {
          return fail(Status::Malformed, "fixed length value is truncated");
        }
        out.append(reinterpret_cast<const char *>(data + pos), length);
        pos += length;
        return Status::Ok;
      }
      default:
        return fail(Status::Unsupported, "unknown physical type");
    }
  }

  void append_int32(BudgetText &out, const SchemaElement &element, int32_t value)
  {
    switch (element.logical) {
      case LOGIC_DATE:
        append_date(out, value);
        break;
      case LOGIC_TIME_MILLIS:
        append_time(out, value, 1000, 3);
        break;
      case LOGIC_DECIMAL:
        append_decimal(out, value, element.scale);
        break;
      case LOGIC_UINT_8:
      case LOGIC_UINT_16:
      case LOGIC_UINT_32:
        append_uint(out, static_cast<uint32_t>(value));
        break;
      default:
        append_int(out, value);
        break;
    }
  }

  void append_int64(BudgetText &out, const SchemaElement &element, int64_t value)
  {
    switch (element.logical) {
      case LOGIC_TIMESTAMP_MILLIS:
        append_timestamp(out, value, 1000);
        break;
      case LOGIC_TIMESTAMP_MICROS:
        append_timestamp(out, value, 1000000);
        break;
      case LOGIC_TIMESTAMP_NANOS:
        append_timestamp(out, value, 1000000000);
        break;
      case LOGIC_TIME_MICROS:
        append_time(out, value, 1000000, 6);
        break;
      case LOGIC_TIME_NANOS:
        append_time(out, value, 1000000000, 9);
        break;
      case LOGIC_DECIMAL:
        append_decimal(out, value, element.scale);
        break;
      case LOGIC_UINT_64:
        append_uint(out, static_cast<uint64_t>(value));
        break;
      default:
        append_int(out, value);
        break;
    }
  }

  Status load_dictionary(ColumnState &state, const SchemaElement &element,
                         const ColumnChunkMeta &chunk, const PageHeader &header, int64_t data_offset)
  {
    if (element.physical == P_BOOLEAN) {
      return fail(Status::Malformed, "boolean columns cannot have a dictionary");
    }
    const size_t compressed = static_cast<size_t>(header.compressed_size);
    const Status read_status = read_exact(data_offset, compressed, buffer);
    if (read_status != Status::Ok) {
      return read_status;
    }
    const char *values = buffer.data();
    size_t values_size = compressed;
    if (chunk.codec == CODEC_SNAPPY) {
      const size_t uncompressed = static_cast<size_t>(header.uncompressed_size);
      if (!memory.resize(window, uncompressed)) return memory_failure();
      if (!snappy_uncompress(buffer.data(), compressed, window.data(), uncompressed)) {
        return fail(Status::Malformed, "snappy dictionary page is corrupt");
      }
      values = window.data();
      values_size = uncompressed;
    }
    if (header.num_values < 0) {
      return fail(Status::Malformed, "dictionary page has no value count");
    }
    const size_t count = static_cast<size_t>(header.num_values);
    state.dictionary.clear();
    state.dictionary_cells.clear();
    if (!memory.reserve(state.dictionary_cells, count)) return memory_failure();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(values);
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
      Cell cell;
      cell.is_null = false;
      cell.offset = state.dictionary.size();
      const Status decode_status = decode_plain(element, data, values_size, pos, state.dictionary);
      if (decode_status != Status::Ok) {
        return decode_status;
      }
      if (memory.exhausted) return memory_failure();
      cell.length = state.dictionary.size() - cell.offset;
      state.dictionary_cells.push_back(cell);
    }
    return Status::Ok;
  }

  Status load_data_page(ColumnState &state, const SchemaElement &element,
                        const ColumnChunkMeta &chunk, const PageHeader &header, int64_t data_offset,
                        bool version2)
  {
    const size_t compressed = static_cast<size_t>(header.compressed_size);
    const size_t uncompressed = static_cast<size_t>(header.uncompressed_size);
    Status read_status = read_exact(data_offset, compressed, buffer);
    if (read_status != Status::Ok) {
      return read_status;
    }
    if (!memory.resize(state.page, uncompressed)) return memory_failure();
    size_t values_offset = 0;
    size_t values_size = 0;
    if (version2) {
      const size_t repetition_length = static_cast<size_t>(header.repetition_length);
      const size_t definition_length = static_cast<size_t>(header.definition_length);
      if (repetition_length != 0) {
        return fail(Status::Unsupported, "repeated Parquet fields are not supported");
      }
      if (definition_length > uncompressed || definition_length > compressed) {
        return fail(Status::Malformed, "page level lengths are inconsistent");
      }
      if (definition_length > 0) {
        std::memcpy(state.page.data(), buffer.data(), definition_length);
        state.levels.reset();
        state.levels.mode = LevelDecoder::RLE;
        state.levels.hybrid.reset(reinterpret_cast<const uint8_t *>(state.page.data()),
                                  definition_length, 1);
      }
      const size_t raw_values = compressed - definition_length;
      const size_t plain_values = uncompressed - definition_length;
      if (!header.is_compressed || chunk.codec == CODEC_UNCOMPRESSED) {
        if (raw_values != plain_values) {
          return fail(Status::Malformed, "page sizes are inconsistent");
        }
        if (plain_values > 0) {
          std::memcpy(state.page.data() + definition_length, buffer.data() + definition_length,
                      plain_values);
        }
      } else if (chunk.codec == CODEC_SNAPPY) {
        if (plain_values > 0 && !snappy_uncompress(buffer.data() + definition_length, raw_values,
                                                   state.page.data() + definition_length,
                                                   plain_values)) {
          return fail(Status::Malformed, "snappy page is corrupt");
        }
      } else {
        return fail(Status::Unsupported, "Parquet compression codec is not supported");
      }
      values_offset = definition_length;
      values_size = plain_values;
    } else {
      if (chunk.codec == CODEC_SNAPPY) {
        if (uncompressed == 0
            || !snappy_uncompress(buffer.data(), compressed, state.page.data(), uncompressed)) {
          return fail(Status::Malformed, "snappy page is corrupt");
        }
      } else if (chunk.codec == CODEC_UNCOMPRESSED) {
        if (compressed != uncompressed) {
          return fail(Status::Malformed, "page sizes are inconsistent");
        }
        if (compressed > 0) {
          std::memcpy(state.page.data(), buffer.data(), compressed);
        }
      } else {
        return fail(Status::Unsupported, "Parquet compression codec is not supported");
      }
      if (element.repetition == REP_OPTIONAL) {
        if (header.definition_encoding == ENC_RLE) {
          if (uncompressed < 4) {
            return fail(Status::Malformed, "definition levels are truncated");
          }
          const uint32_t length = read_le32(reinterpret_cast<const uint8_t *>(state.page.data()));
          if (static_cast<size_t>(length) > uncompressed - 4) {
            return fail(Status::Malformed, "definition levels are truncated");
          }
          state.levels.reset();
          state.levels.mode = LevelDecoder::RLE;
          state.levels.hybrid.reset(reinterpret_cast<const uint8_t *>(state.page.data()) + 4,
                                    length, 1);
          values_offset = 4 + static_cast<size_t>(length);
        } else if (header.definition_encoding == ENC_BIT_PACKED) {
          const size_t length = (static_cast<size_t>(header.num_values) + 7) / 8;
          if (length > uncompressed) {
            return fail(Status::Malformed, "definition levels are truncated");
          }
          state.levels.reset();
          state.levels.mode = LevelDecoder::BIT_PACKED;
          state.levels.bits = reinterpret_cast<const uint8_t *>(state.page.data());
          state.levels.bits_size = length;
          state.levels.bits_values = static_cast<size_t>(header.num_values);
          state.levels.bits_read = 0;
          values_offset = length;
        } else {
          return fail(Status::Unsupported, "definition level encoding is not supported");
        }
      }
      values_size = uncompressed - values_offset;
    }
    if (header.num_values < 0 || static_cast<int64_t>(header.num_values) > state.values_left) {
      return fail(Status::Malformed, "page holds more values than its column chunk");
    }
    state.values_left -= header.num_values;
    state.values_offset = values_offset;
    state.values_size = values_size;
    state.values_pos = 0;
    state.defined_read = 0;
    state.page_values = header.num_values;
    state.page_values_read = 0;
    state.encoding = ENC_PLAIN;
    const int32_t encoding = header.encoding;
    if (encoding == ENC_PLAIN) {
      state.encoding = ENC_PLAIN;
    } else if (encoding == ENC_PLAIN_DICTIONARY || encoding == ENC_RLE_DICTIONARY) {
      if (values_size < 1) {
        return fail(Status::Malformed, "dictionary indices are truncated");
      }
      const uint32_t width = static_cast<uint8_t>(state.page[values_offset]);
      if (width > 32) {
        return fail(Status::Malformed, "dictionary index width is invalid");
      }
      state.encoding = ENC_RLE_DICTIONARY;
      state.indices.reset(reinterpret_cast<const uint8_t *>(state.page.data()) + values_offset + 1,
                          values_size - 1, width);
    } else {
      return fail(Status::Unsupported, "page encoding is not supported");
    }
    return Status::Ok;
  }

  Status load_page(ColumnState &state, size_t index)
  {
    const ColumnChunkMeta &chunk = groups[current_group].chunks[index];
    for (;;) {
      if (state.values_left <= 0) {
        return Status::End;
      }
      if (state.page_offset < 4 || state.page_offset >= file_size) {
        return fail(Status::Malformed, "page offset is outside the file");
      }
      const int64_t remaining = file_size - state.page_offset;
      const size_t window_size =
          static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(PAGE_HEADER_WINDOW), remaining));
      if (!memory.resize(window, window_size)) return memory_failure();
      size_t got = 0;
      const Status header_status = file.read_at(window.data(), window_size, state.page_offset, got);
      if (header_status == Status::Io) {
        return fail(Status::Io, file.error());
      }
      if (header_status != Status::Ok || got == 0) {
        return fail(Status::Malformed, "page header is truncated");
      }
      CompactReader reader;
      reader.data = reinterpret_cast<const uint8_t *>(window.data());
      reader.size = got;
      PageHeader header;
      if (!parse_page_header(reader, header)) {
        return fail(Status::Malformed, "invalid page header");
      }
      const int64_t data_offset = state.page_offset + static_cast<int64_t>(reader.pos);
      if (header.compressed_size < 0 || header.uncompressed_size < 0) {
        return fail(Status::Malformed, "page size is invalid");
      }
      if (static_cast<size_t>(header.compressed_size) > options.max_page_bytes
          || static_cast<size_t>(header.uncompressed_size) > options.max_page_bytes) {
        return fail(Status::ResourceLimit, "page exceeds max_page_bytes");
      }
      if (static_cast<int64_t>(header.compressed_size) > file_size - data_offset) {
        return fail(Status::Malformed, "page payload is outside the file");
      }
      state.page_offset = data_offset + static_cast<int64_t>(header.compressed_size);
      if (header.type == PT_DICTIONARY) {
        const Status dictionary_status =
            load_dictionary(state, elements[index], chunk, header, data_offset);
        if (dictionary_status != Status::Ok) {
          return dictionary_status;
        }
        continue;
      }
      if (header.type == PT_INDEX) {
        continue;
      }
      if (header.type == PT_DATA) {
        return load_data_page(state, elements[index], chunk, header, data_offset, false);
      }
      if (header.type == PT_DATA_V2) {
        return load_data_page(state, elements[index], chunk, header, data_offset, true);
      }
      return fail(Status::Malformed, "unknown page type");
    }
  }

  Status read_cell(size_t index, Cell &cell)
  {
    ColumnState &state = states[index];
    const SchemaElement &element = elements[index];
    const Column &column = columns[index];
    for (;;) {
      if (state.page_values_read < state.page_values) {
        break;
      }
      const Status page_status = load_page(state, index);
      if (page_status != Status::Ok) {
        return page_status == Status::End
                   ? fail(Status::Malformed, "column chunk is shorter than its row group")
                   : page_status;
      }
    }
    bool present = true;
    if (element.repetition == REP_OPTIONAL) {
      uint32_t level = 0;
      if (!state.levels.next(level)) {
        return fail(Status::Malformed, "definition levels are truncated");
      }
      present = level != 0;
    }
    ++state.page_values_read;
    cell.is_null = true;
    cell.offset = text.size();
    cell.length = 0;
    if (!present) {
      return Status::Ok;
    }
    const uint8_t *data = reinterpret_cast<const uint8_t *>(state.page.data())
                          + state.values_offset;
    size_t pos = element.physical == P_BOOLEAN ? state.defined_read : state.values_pos;
    Status decode_status = Status::Ok;
    if (state.encoding == ENC_RLE_DICTIONARY) {
      uint32_t dictionary_index = 0;
      if (!state.indices.next(dictionary_index)) {
        return fail(Status::Malformed, "dictionary indices are truncated");
      }
      if (static_cast<size_t>(dictionary_index) >= state.dictionary_cells.size()) {
        return fail(Status::Malformed, "dictionary index is out of range");
      }
      const Cell &entry = state.dictionary_cells[static_cast<size_t>(dictionary_index)];
      text.append(state.dictionary.data() + entry.offset, entry.length);
    } else {
      decode_status = decode_plain(element, data, state.values_size, pos, text);
      if (decode_status == Status::Ok) {
        state.values_pos = pos;
      }
    }
    if (decode_status != Status::Ok) {
      return decode_status;
    }
    if (memory.exhausted) return memory_failure();
    ++state.defined_read;
    cell.is_null = false;
    cell.length = text.size() - cell.offset;
    if (column.type == Type::Varchar
        && !is_valid_utf8(std::string_view(text.data() + cell.offset, cell.length))) {
      return fail(Status::Malformed, "string value is not valid UTF-8");
    }
    return Status::Ok;
  }

  Status do_next()
  {
    if (status != Status::Ok) {
      return status;
    }
    if (pending != Status::Ok) {
      return pending;
    }
    const Status guard = require_ready();
    if (guard != Status::Ok) {
      return guard;
    }
    for (;;) {
      if (rows_left > 0) {
        break;
      }
      if (group >= groups.size()) {
        return Status::End;
      }
      const Status group_status = start_group(group);
      if (group_status != Status::Ok) {
        return group_status;
      }
      ++group;
      if (rows_left == 0) {
        continue;
      }
    }
    rows_started = true;
    ++row_index;
    text.clear();
    for (size_t i = 0; i < cells.size(); ++i) {
      cells[i] = Cell();
      cells[i].is_null = true;
    }
    for (size_t i = 0; i < projection.size(); ++i) {
      const size_t index = projection[i];
      const Status cell_status = read_cell(index, cells[index]);
      if (cell_status != Status::Ok) {
        return cell_status;
      }
    }
    --rows_left;
    return Status::Ok;
  }
};

ParquetReader::ParquetReader() : impl_(new Impl()) {}

ParquetReader::~ParquetReader() = default;

Status ParquetReader::open(const char *path, const char *secure_root, const ParquetOptions &options)
{
  return impl_->do_open(path, secure_root, options);
}

Status ParquetReader::infer() { return impl_->do_infer(); }

FileIdentity ParquetReader::identity() const { return impl_->identity; }

Status ParquetReader::bind_schema(const Column *columns, size_t count,
                                  const FileIdentity &expected)
{
  return impl_->do_bind(columns, count, expected);
}

void ParquetReader::set_projection(const size_t *columns, size_t count)
{
  if (impl_->rows_started) {
    impl_->pending =
        impl_->fail(Status::InvalidOption, "projection must be set before the first row");
    return;
  }
  if (count == 0) {
    if (impl_->column_count == 0) {
      impl_->pending = Status::Ok;
      return;
    }
    impl_->pending = impl_->fail(Status::InvalidOption, "projection is empty");
    return;
  }
  if (count > impl_->column_count) {
    impl_->pending = impl_->fail(Status::InvalidOption, "too many projection columns");
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    if (columns == nullptr || columns[i] >= impl_->column_count) {
      impl_->pending = impl_->fail(Status::InvalidOption, "projection column is out of range");
      return;
    }
    for (size_t k = 0; k < i; ++k) {
      if (columns[k] == columns[i]) {
        impl_->pending = impl_->fail(Status::InvalidOption, "projection column is duplicated");
        return;
      }
    }
  }
  impl_->projection.assign(columns, columns + count);
  impl_->projected.assign(impl_->column_count, 0);
  for (size_t i = 0; i < impl_->projection.size(); ++i) {
    impl_->projected[impl_->projection[i]] = 1;
  }
  impl_->pending = Status::Ok;
}

Status ParquetReader::next() { return impl_->do_next(); }

Status ParquetReader::rescan() { return impl_->do_rescan(); }

void ParquetReader::close() { impl_->close_file(); }

size_t ParquetReader::column_count() const { return impl_->column_count; }

const ParquetReader::Column &ParquetReader::column(size_t index) const
{
  static const Column empty;
  if (index >= impl_->columns.size()) {
    return empty;
  }
  return impl_->columns[index];
}

bool ParquetReader::projected(size_t index) const
{
  return index < impl_->projected.size() && impl_->projected[index] != 0;
}

std::string_view ParquetReader::value(size_t index) const
{
  if (index >= impl_->cells.size()) {
    return std::string_view();
  }
  const Cell &cell = impl_->cells[index];
  if (cell.is_null) {
    return std::string_view();
  }
  return std::string_view(impl_->text.data() + cell.offset, cell.length);
}

bool ParquetReader::is_null(size_t index) const
{
  return index >= impl_->cells.size() || impl_->cells[index].is_null;
}

const char *ParquetReader::error() const { return impl_->error; }

uint64_t ParquetReader::row_number() const { return impl_->row_index; }

}}} // namespace oceanbase::sql::file_sql
