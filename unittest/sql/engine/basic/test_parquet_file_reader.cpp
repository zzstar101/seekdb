/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Standalone unit test for ParquetReader.
//
// No Parquet writer is available in this build environment, so the fixtures are
// generated in-test by the minimal encoder below (Thrift compact protocol page
// headers and footer, PLAIN / dictionary value pages, RLE and BIT_PACKED levels,
// DATA_PAGE and DATA_PAGE_V2, optional literal-only Snappy). The writer is part
// of the test only: the reader never depends on it.
#include "sql/engine/basic/parquet_file_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace oceanbase::sql::file_sql;

namespace {

int failures = 0;

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #expr ")\n";     \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

const char *status_name(Status status)
{
  switch (status) {
    case Status::Ok: return "Ok";
    case Status::End: return "End";
    case Status::Disabled: return "Disabled";
    case Status::InvalidOption: return "InvalidOption";
    case Status::Denied: return "Denied";
    case Status::Io: return "Io";
    case Status::Changed: return "Changed";
    case Status::Malformed: return "Malformed";
    case Status::ResourceLimit: return "ResourceLimit";
    case Status::Unsupported: return "Unsupported";
  }
  return "?";
}

void expect_status(const char *what, Status got, Status want)
{
  if (got != want) {
    std::cerr << what << ": expected " << status_name(want) << ", got "
              << status_name(got) << "\n";
    ++failures;
  }
}

// ---------------------------------------------------------------------------
// Temporary files
// ---------------------------------------------------------------------------
struct Fixtures {
  std::string root;
  std::vector<std::string> files;

  Fixtures()
  {
    char name[] = "/tmp/seekdb-parquet-test-XXXXXX";
    char *dir = mkdtemp(name);
    if (dir == nullptr) {
      std::cerr << "mkdtemp failed\n";
      std::abort();
    }
    root = dir;
  }
  ~Fixtures()
  {
    for (const std::string &file : files) {
      ::unlink(file.c_str());
    }
    ::rmdir(root.c_str());
  }
  Fixtures(const Fixtures &) = delete;
  Fixtures &operator=(const Fixtures &) = delete;

  std::string put(const std::string &name, const std::string &bytes)
  {
    const std::string path = root + "/" + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    files.push_back(path);
    return path;
  }
  std::string join(const std::string &name) const { return root + "/" + name; }
};

void write_file(const std::string &path, const std::string &bytes)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string base64_decode(std::string_view text)
{
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  };
  std::string out;
  uint32_t bits = 0;
  int count = 0;
  for (char c : text) {
    const int digit = value(c);
    if (digit < 0) continue;
    bits = (bits << 6) | static_cast<uint32_t>(digit);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(static_cast<char>((bits >> count) & 0xff));
    }
  }
  return out;
}

// `data/simple.parquet.b64` from the public file_sql suite: a 461-byte
// arrow-rs file with one OPTIONAL INT32 column named c1 holding 0..6, using
// Snappy, RLE levels and one row group.
const char kSimpleParquetBase64[] =
    "UEFSMRUGFTwVQFwVDhUAFQ4VABUEFQARHDYAKAQGAAAAGAQAAAAAAAAAA38cbAAAAAABAAAAAgAAAAMAAAAEAAAABQAAAAYA"
    "AAAmlAEcFQIZJQYAGRgCYzEVAhYOFogBFowBJgg8NgAoBAYAAAAYBAAAAAAAAAAVBBksSARyb290FQIAFQIlAhgCYzEAFg4Z"
    "HBkcJpQBHBUCGSUGABkYAmMxFQIWDhaIARaMASYIPDYAKAQGAAAAGAQAAAAAAAAAFowBFg4AGRwYDEFSUk9XOnNjaGVtYRi8"
    "AS8vLy8vNFFBQUFBUUFBQUFBQUFLQUE0QURBQUxBQVFBQ2dBQUFCUUFBQUFBQUFBQkJBQUtBQXdBQUFBSUFBUUFDZ0FBQUFn"
    "QUFBQUlBQUFBQUFBQUFBRUFBQUFVQUFBQUVBQVVBQkFBRGdBUEFBUUFBQUFJQUJBQUFBQVlBQUFBSUFBQUFBQUFBUUljQUFB"
    "QUNBQU1BQVFBQ3dBSUFBQUFJQUFBQUFBQUFBRUFBQUFBQWdBQUFHTXhBQUE9ABgsQXJyb3cyIC0gTmF0aXZlIFJ1c3QgaW1w"
    "bGVtZW50YXRpb24gb2YgQXJyb3cATgEAAFBBUjE=";

// ---------------------------------------------------------------------------
// Thrift compact protocol writer (footer and page headers only)
// ---------------------------------------------------------------------------
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

// Parquet physical types.
enum Physical {
  P_BOOLEAN = 0,
  P_INT32 = 1,
  P_INT64 = 2,
  P_INT96 = 3,
  P_FLOAT = 4,
  P_DOUBLE = 5,
  P_BYTE_ARRAY = 6,
  P_FLBA = 7,
};

// Parquet converted types used by the tests.
enum Converted {
  C_NONE = -1,
  C_UTF8 = 0,
  C_DATE = 6,
  C_TIME_MILLIS = 7,
  C_TIME_MICROS = 8,
  C_TIMESTAMP_MILLIS = 9,
  C_TIMESTAMP_MICROS = 10,
};

enum Encoding {
  E_PLAIN = 0,
  E_PLAIN_DICTIONARY = 2,
  E_RLE = 3,
  E_BIT_PACKED = 4,
  E_RLE_DICTIONARY = 8,
};

struct Tw {
  std::string out;

  void b(uint8_t v) { out.push_back(static_cast<char>(v)); }
  void varint(uint64_t v)
  {
    while (v >= 0x80) {
      b(static_cast<uint8_t>((v & 0x7f) | 0x80));
      v >>= 7;
    }
    b(static_cast<uint8_t>(v));
  }
  void zz(int64_t v)
  {
    varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63));
  }
  void field(int &last, int id, int type)
  {
    const int delta = id - last;
    if (delta > 0 && delta < 16) {
      b(static_cast<uint8_t>((delta << 4) | type));
    } else {
      b(static_cast<uint8_t>(type));
      zz(id);
    }
    last = id;
  }
  void i32f(int &last, int id, int32_t v)
  {
    field(last, id, CT_I32);
    zz(v);
  }
  void i64f(int &last, int id, int64_t v)
  {
    field(last, id, CT_I64);
    zz(v);
  }
  void i8f(int &last, int id, int8_t v)
  {
    field(last, id, CT_BYTE);
    b(static_cast<uint8_t>(v));
  }
  void boolf(int &last, int id, bool v) { field(last, id, v ? CT_TRUE : CT_FALSE); }
  void binf(int &last, int id, std::string_view v)
  {
    field(last, id, CT_BINARY);
    binary(v);
  }
  void binary(std::string_view v)
  {
    varint(v.size());
    out.append(v.data(), v.size());
  }
  void structf(int &last, int id, const std::string &body)
  {
    field(last, id, CT_STRUCT);
    out += body;
  }
  void list_header(int element_type, size_t count)
  {
    if (count < 15) {
      b(static_cast<uint8_t>((count << 4) | static_cast<size_t>(element_type)));
    } else {
      b(static_cast<uint8_t>(0xf0 | element_type));
      varint(count);
    }
  }
  void stop() { b(CT_STOP); }
};

void append_le32(std::string &out, uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>(static_cast<uint8_t>(value >> (8 * i))));
  }
}

void append_le64(std::string &out, uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(static_cast<uint8_t>(value >> (8 * i))));
  }
}

// ---------------------------------------------------------------------------
// Literal-only Snappy compressor: valid for any input, never references itself.
// ---------------------------------------------------------------------------
std::string snappy_literal(const std::string &input)
{
  std::string out;
  uint64_t length = input.size();
  while (length >= 0x80) {
    out.push_back(static_cast<char>(static_cast<uint8_t>((length & 0x7f) | 0x80)));
    length >>= 7;
  }
  out.push_back(static_cast<char>(static_cast<uint8_t>(length)));
  size_t pos = 0;
  while (pos < input.size()) {
    const size_t len = std::min<size_t>(input.size() - pos, 4096);
    const uint64_t minus_one = len - 1;
    if (minus_one < 60) {
      out.push_back(static_cast<char>(static_cast<uint8_t>(minus_one << 2)));
    } else {
      int bytes = 1;
      while (bytes < 4 && (minus_one >> (8 * bytes)) != 0) {
        ++bytes;
      }
      out.push_back(static_cast<char>(static_cast<uint8_t>((59 + bytes) << 2)));
      for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>(static_cast<uint8_t>(minus_one >> (8 * i))));
      }
    }
    out.append(input, pos, len);
    pos += len;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Value and level encoders
// ---------------------------------------------------------------------------
struct V {
  bool is_null = false;
  int64_t i = 0;
  int32_t julian = 0;
  double d = 0;
  std::string s;
};

V vnull()
{
  V v;
  v.is_null = true;
  return v;
}
V vint(int64_t value)
{
  V v;
  v.i = value;
  return v;
}
V vdbl(double value)
{
  V v;
  v.d = value;
  return v;
}
V vstr(std::string value)
{
  V v;
  v.s = std::move(value);
  return v;
}
V vbool(bool value) { return vint(value ? 1 : 0); }
V vint96(int64_t nanos, int32_t julian)
{
  V v;
  v.i = nanos;
  v.julian = julian;
  return v;
}

struct Col {
  std::string name;
  int physical = P_INT64;
  int converted = C_NONE;
  bool optional = true;
  int type_length = 0;
  std::vector<V> values;
};

struct Gen {
  bool v2 = false;
  int codec = 0;  // 0 = UNCOMPRESSED, 1 = SNAPPY
  bool dictionary = false;
  size_t rows_per_page = 0;   // 0 = one page per chunk
  size_t rows_per_group = 0;  // 0 = one row group
  int level_encoding = E_RLE;
  bool nested = false;
  int32_t num_values_delta = 0;
  std::string created_by = "seekdb-parquet-test";
};

std::string plain_value(const Col &col, const V &value)
{
  std::string out;
  switch (col.physical) {
    case P_INT32:
      append_le32(out, static_cast<uint32_t>(static_cast<int32_t>(value.i)));
      break;
    case P_INT64:
      append_le64(out, static_cast<uint64_t>(value.i));
      break;
    case P_INT96:
      append_le64(out, static_cast<uint64_t>(value.i));
      append_le32(out, static_cast<uint32_t>(value.julian));
      break;
    case P_FLOAT: {
      const float f = static_cast<float>(value.d);
      uint32_t bits = 0;
      std::memcpy(&bits, &f, sizeof(bits));
      append_le32(out, bits);
      break;
    }
    case P_DOUBLE: {
      uint64_t bits = 0;
      std::memcpy(&bits, &value.d, sizeof(bits));
      append_le64(out, bits);
      break;
    }
    case P_FLBA:
      out.append(value.s);
      break;
    case P_BYTE_ARRAY:
      append_le32(out, static_cast<uint32_t>(value.s.size()));
      out += value.s;
      break;
    default:
      break;
  }
  return out;
}

// Bit-packs `count` values (at most 8) taking `width` bits each, LSB first.
std::string pack_group(const std::vector<uint32_t> &values, size_t start, size_t count, int width)
{
  std::string out(static_cast<size_t>(width), '\0');
  for (size_t i = 0; i < count; ++i) {
    for (int bit = 0; bit < width; ++bit) {
      if (((values[start + i] >> bit) & 1u) != 0) {
        const size_t absolute = i * static_cast<size_t>(width) + static_cast<size_t>(bit);
        out[absolute / 8] = static_cast<char>(
            static_cast<uint8_t>(out[absolute / 8]) | static_cast<uint8_t>(1u << (absolute % 8)));
      }
    }
  }
  return out;
}

std::string bitpacked_plain(const std::vector<uint32_t> &values, int width)
{
  std::string out;
  for (size_t i = 0; i < values.size(); i += 8) {
    out += pack_group(values, i, std::min<size_t>(8, values.size() - i), width);
  }
  return out;
}

// RLE / bit-packed hybrid: RLE runs for runs of at least eight equal values,
// single bit-packed groups otherwise.
std::string hybrid_encode(const std::vector<uint32_t> &values, int width)
{
  std::string out;
  std::vector<uint32_t> pending;
  // A bit-packed run always covers a whole number of eight-value groups, so a
  // group is emitted only when it is full; the tail is padded to eight values,
  // which is legal because it is the last group of the stream.
  auto flush_full = [&]() {
    while (pending.size() >= 8) {
      const std::vector<uint32_t> group(pending.begin(), pending.begin() + 8);
      Tw w;
      w.varint((uint64_t(1) << 1) | 1u);
      out += w.out;
      out += pack_group(group, 0, 8, width);
      pending.erase(pending.begin(), pending.begin() + 8);
    }
  };
  size_t i = 0;
  while (i < values.size()) {
    size_t j = i;
    while (j < values.size() && values[j] == values[i]) {
      ++j;
    }
    const size_t run = j - i;
    if (run >= 8 && pending.empty()) {
      Tw w;
      w.varint(uint64_t(run) << 1);
      const size_t bytes = (static_cast<size_t>(width) + 7) / 8;
      for (size_t k = 0; k < bytes; ++k) {
        w.b(static_cast<uint8_t>(values[i] >> (8 * k)));
      }
      out += w.out;
    } else {
      for (size_t k = i; k < j; ++k) {
        pending.push_back(values[k]);
        flush_full();
      }
    }
    i = j;
  }
  if (!pending.empty()) {
    std::vector<uint32_t> group = pending;
    group.resize(8, 0);
    Tw w;
    w.varint((uint64_t(1) << 1) | 1u);
    out += w.out;
    out += pack_group(group, 0, 8, width);
  }
  return out;
}

std::string pack_bits(const std::vector<uint8_t> &bits)
{
  std::string out((bits.size() + 7) / 8, '\0');
  for (size_t i = 0; i < bits.size(); ++i) {
    if (bits[i] != 0) {
      out[i / 8] = static_cast<char>(static_cast<uint8_t>(out[i / 8]) |
                                     static_cast<uint8_t>(1u << (i % 8)));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Footer structures
// ---------------------------------------------------------------------------
std::string element_root(size_t children)
{
  Tw w;
  int last = 0;
  w.binf(last, 4, "schema");
  w.i32f(last, 5, static_cast<int32_t>(children));
  w.stop();
  return w.out;
}

std::string element_group(int repetition, const std::string &name, size_t children)
{
  Tw w;
  int last = 0;
  w.i32f(last, 3, repetition);
  w.binf(last, 4, name);
  w.i32f(last, 5, static_cast<int32_t>(children));
  w.stop();
  return w.out;
}

std::string element_leaf(const Col &col)
{
  Tw w;
  int last = 0;
  w.i32f(last, 1, col.physical);
  if (col.physical == P_FLBA) {
    w.i32f(last, 2, col.type_length);
  }
  w.i32f(last, 3, col.optional ? 1 : 0);
  w.binf(last, 4, col.name);
  if (col.converted != C_NONE) {
    w.i32f(last, 6, col.converted);
  }
  w.stop();
  return w.out;
}

std::string statistics_body(int64_t null_count)
{
  Tw w;
  int last = 0;
  w.i64f(last, 3, null_count);
  w.stop();
  return w.out;
}

std::string column_metadata_body(const Col &col, const std::vector<int> &encodings, int codec,
                                 int64_t num_values, int64_t uncompressed, int64_t compressed,
                                 int64_t data_offset, int64_t dictionary_offset, int64_t nulls)
{
  Tw w;
  int last = 0;
  w.i32f(last, 1, col.physical);
  w.field(last, 2, CT_LIST);
  w.list_header(CT_I32, encodings.size());
  for (int encoding : encodings) {
    w.zz(encoding);
  }
  w.field(last, 3, CT_LIST);
  w.list_header(CT_BINARY, 1);
  w.binary(col.name);
  w.i32f(last, 4, codec);
  w.i64f(last, 5, num_values);
  w.i64f(last, 6, uncompressed);
  w.i64f(last, 7, compressed);
  w.i64f(last, 9, data_offset);
  if (dictionary_offset >= 0) {
    w.i64f(last, 11, dictionary_offset);
  }
  w.structf(last, 12, statistics_body(nulls));
  w.stop();
  return w.out;
}

std::string column_chunk_body(int64_t file_offset, const std::string &metadata)
{
  Tw w;
  int last = 0;
  w.i64f(last, 2, file_offset);
  w.structf(last, 3, metadata);
  w.stop();
  return w.out;
}

std::string row_group_body(const std::vector<std::string> &chunks, int64_t total_bytes, int64_t num_rows)
{
  Tw w;
  int last = 0;
  w.field(last, 1, CT_LIST);
  w.list_header(CT_STRUCT, chunks.size());
  for (const std::string &chunk : chunks) {
    w.out += chunk;
  }
  w.i64f(last, 2, total_bytes);
  w.i64f(last, 3, num_rows);
  w.stop();
  return w.out;
}

std::string file_metadata_body(const std::vector<std::string> &schema, int64_t num_rows,
                               const std::vector<std::string> &row_groups,
                               const std::string &created_by)
{
  Tw w;
  int last = 0;
  w.i32f(last, 1, 2);
  w.field(last, 2, CT_LIST);
  w.list_header(CT_STRUCT, schema.size());
  for (const std::string &element : schema) {
    w.out += element;
  }
  w.i64f(last, 3, num_rows);
  w.field(last, 4, CT_LIST);
  w.list_header(CT_STRUCT, row_groups.size());
  for (const std::string &group : row_groups) {
    w.out += group;
  }
  if (!created_by.empty()) {
    w.binf(last, 6, created_by);
  }
  w.stop();
  return w.out;
}

// ---------------------------------------------------------------------------
// Page headers
// ---------------------------------------------------------------------------
std::string page_header_v1(int32_t num_values, int32_t encoding, int32_t definition_encoding,
                           int32_t repetition_encoding, int32_t uncompressed, int32_t compressed)
{
  Tw inner;
  int il = 0;
  inner.i32f(il, 1, num_values);
  inner.i32f(il, 2, encoding);
  inner.i32f(il, 3, definition_encoding);
  inner.i32f(il, 4, repetition_encoding);
  inner.stop();
  Tw w;
  int last = 0;
  w.i32f(last, 1, 0);
  w.i32f(last, 2, uncompressed);
  w.i32f(last, 3, compressed);
  w.structf(last, 5, inner.out);
  w.stop();
  return w.out;
}

std::string page_header_v2(int32_t num_values, int32_t num_nulls, int32_t num_rows,
                           int32_t encoding, int32_t definition_length, int32_t repetition_length,
                           bool compressed_flag, int32_t uncompressed, int32_t compressed)
{
  Tw inner;
  int il = 0;
  inner.i32f(il, 1, num_values);
  inner.i32f(il, 2, num_nulls);
  inner.i32f(il, 3, num_rows);
  inner.i32f(il, 4, encoding);
  inner.i32f(il, 5, definition_length);
  inner.i32f(il, 6, repetition_length);
  inner.boolf(il, 7, compressed_flag);
  inner.stop();
  Tw w;
  int last = 0;
  w.i32f(last, 1, 3);
  w.i32f(last, 2, uncompressed);
  w.i32f(last, 3, compressed);
  w.structf(last, 8, inner.out);
  w.stop();
  return w.out;
}

std::string page_header_dictionary(int32_t num_values, int32_t encoding, int32_t uncompressed,
                                   int32_t compressed)
{
  Tw inner;
  int il = 0;
  inner.i32f(il, 1, num_values);
  inner.i32f(il, 2, encoding);
  inner.stop();
  Tw w;
  int last = 0;
  w.i32f(last, 1, 2);
  w.i32f(last, 2, uncompressed);
  w.i32f(last, 3, compressed);
  w.structf(last, 7, inner.out);
  w.stop();
  return w.out;
}

// ---------------------------------------------------------------------------
// File generator
// ---------------------------------------------------------------------------
struct ChunkOut {
  std::string bytes;
  int64_t dictionary_offset = -1;
  int64_t data_offset = -1;
  int64_t num_values = 0;
  int64_t uncompressed = 0;
  int64_t compressed = 0;
  int64_t nulls = 0;
  std::vector<int> encodings;
};

std::string compress(const std::string &payload, int codec)
{
  return codec == 1 ? snappy_literal(payload) : payload;
}

ChunkOut build_chunk(const Col &col, size_t begin, size_t end, const Gen &gen)
{
  ChunkOut out;
  const bool v2 = gen.v2;
  const size_t rows = end - begin;
  for (size_t row = begin; row < end; ++row) {
    if (col.values[row].is_null) {
      ++out.nulls;
    }
  }

  // Per-chunk dictionary over the non-null values, in first-appearance order.
  std::vector<std::string> dictionary;
  std::vector<size_t> dictionary_index(rows, 0);
  if (gen.dictionary) {
    for (size_t row = begin; row < end; ++row) {
      const V &value = col.values[row];
      if (value.is_null) {
        continue;
      }
      const std::string raw = plain_value(col, value);
      size_t index = dictionary.size();
      for (size_t k = 0; k < dictionary.size(); ++k) {
        if (dictionary[k] == raw) {
          index = k;
          break;
        }
      }
      if (index == dictionary.size()) {
        dictionary.push_back(raw);
      }
      dictionary_index[row - begin] = index;
    }
  }
  if (gen.dictionary) {
    out.encodings.push_back(v2 ? E_RLE_DICTIONARY : E_PLAIN_DICTIONARY);
  }
  out.encodings.push_back(E_PLAIN);
  if (col.optional) {
    out.encodings.push_back(gen.level_encoding);
  }

  if (gen.dictionary) {
    std::string payload;
    for (const std::string &entry : dictionary) {
      payload += entry;
    }
    const std::string stored = compress(payload, gen.codec);
    out.dictionary_offset = static_cast<int64_t>(out.bytes.size());
    out.bytes += page_header_dictionary(static_cast<int32_t>(dictionary.size()), E_PLAIN,
                                        static_cast<int32_t>(payload.size()),
                                        static_cast<int32_t>(stored.size()));
    out.bytes += stored;
    out.uncompressed += static_cast<int64_t>(payload.size());
    out.compressed += static_cast<int64_t>(stored.size());
  }

  const size_t per_page = gen.rows_per_page == 0 ? rows : std::min(gen.rows_per_page, rows);
  for (size_t start = 0; start < rows; start += per_page) {
    const size_t count = std::min(per_page, rows - start);
    std::vector<uint32_t> levels;
    std::vector<uint32_t> indices;
    std::vector<uint8_t> boolean_bits;
    std::string plain_bytes;
    int32_t nulls_in_page = 0;
    for (size_t k = 0; k < count; ++k) {
      const V &value = col.values[begin + start + k];
      if (col.optional) {
        levels.push_back(value.is_null ? 0u : 1u);
      }
      if (value.is_null) {
        ++nulls_in_page;
        continue;
      }
      if (gen.dictionary) {
        indices.push_back(static_cast<uint32_t>(dictionary_index[start + k]));
      } else if (col.physical == P_BOOLEAN) {
        boolean_bits.push_back(static_cast<uint8_t>(value.i != 0));
      } else {
        plain_bytes += plain_value(col, value);
      }
    }

    std::string values;
    int32_t encoding = E_PLAIN;
    if (gen.dictionary) {
      encoding = v2 ? E_RLE_DICTIONARY : E_PLAIN_DICTIONARY;
      int width = 1;
      while (width < 32 && (static_cast<size_t>(1) << width) < dictionary.size()) {
        ++width;
      }
      values.push_back(static_cast<char>(static_cast<uint8_t>(width)));
      values += hybrid_encode(indices, width);
    } else if (col.physical == P_BOOLEAN) {
      values = pack_bits(boolean_bits);
    } else {
      values = plain_bytes;
    }

    std::string level_bytes;
    if (col.optional) {
      if (v2) {
        level_bytes = hybrid_encode(levels, 1);
      } else if (gen.level_encoding == E_BIT_PACKED) {
        level_bytes = bitpacked_plain(levels, 1);
      } else {
        const std::string encoded = hybrid_encode(levels, 1);
        append_le32(level_bytes, static_cast<uint32_t>(encoded.size()));
        level_bytes += encoded;
      }
    }

    if (out.data_offset < 0) {
      out.data_offset = static_cast<int64_t>(out.bytes.size());
    }
    if (v2) {
      const std::string stored = compress(values, gen.codec);
      const std::string payload = level_bytes + stored;
      const int32_t uncompressed =
          static_cast<int32_t>(level_bytes.size() + values.size());
      out.bytes += page_header_v2(static_cast<int32_t>(count), nulls_in_page,
                                  static_cast<int32_t>(count), encoding,
                                  static_cast<int32_t>(level_bytes.size()), 0, gen.codec == 1,
                                  uncompressed, static_cast<int32_t>(payload.size()));
      out.bytes += payload;
      out.uncompressed += uncompressed;
      out.compressed += static_cast<int64_t>(payload.size());
    } else {
      const std::string payload = level_bytes + values;
      const std::string stored = compress(payload, gen.codec);
      out.bytes += page_header_v1(static_cast<int32_t>(count), encoding, gen.level_encoding, E_RLE,
                                  static_cast<int32_t>(payload.size()),
                                  static_cast<int32_t>(stored.size()));
      out.bytes += stored;
      out.uncompressed += static_cast<int64_t>(payload.size());
      out.compressed += static_cast<int64_t>(stored.size());
    }
  }
  out.num_values = static_cast<int64_t>(rows) + gen.num_values_delta;
  return out;
}

std::string build_parquet(const std::vector<Col> &cols, const Gen &gen)
{
  std::string out = "PAR1";
  std::vector<std::string> schema;
  if (gen.nested) {
    schema.push_back(element_root(1));
    schema.push_back(element_group(1, "g", 2));
    Col leaf_a;
    leaf_a.name = "a";
    Col leaf_b;
    leaf_b.name = "b";
    schema.push_back(element_leaf(leaf_a));
    schema.push_back(element_leaf(leaf_b));
    const std::string metadata = file_metadata_body(schema, 0, {}, gen.created_by);
    out += metadata;
    append_le32(out, static_cast<uint32_t>(metadata.size()));
    out += "PAR1";
    return out;
  }

  size_t rows = 0;
  for (const Col &col : cols) {
    rows = std::max(rows, col.values.size());
  }
  schema.push_back(element_root(cols.size()));
  for (const Col &col : cols) {
    schema.push_back(element_leaf(col));
  }

  std::vector<std::string> row_groups;
  const size_t group_rows = gen.rows_per_group == 0 ? rows : gen.rows_per_group;
  for (size_t begin = 0; begin < rows; begin += group_rows) {
    const size_t end = std::min(rows, begin + group_rows);
    std::vector<std::string> chunks;
    int64_t total_bytes = 0;
    for (const Col &col : cols) {
      const ChunkOut chunk = build_chunk(col, begin, end, gen);
      const int64_t base = static_cast<int64_t>(out.size());
      const int64_t dictionary_offset =
          chunk.dictionary_offset >= 0 ? base + chunk.dictionary_offset : -1;
      const int64_t data_offset = base + chunk.data_offset;
      out += chunk.bytes;
      const std::string metadata =
          column_metadata_body(col, chunk.encodings, gen.codec, chunk.num_values,
                               chunk.uncompressed, chunk.compressed, data_offset,
                               dictionary_offset, chunk.nulls);
      chunks.push_back(column_chunk_body(dictionary_offset >= 0 ? dictionary_offset : data_offset,
                                         metadata));
      total_bytes += static_cast<int64_t>(chunk.bytes.size());
    }
    row_groups.push_back(row_group_body(chunks, total_bytes, static_cast<int64_t>(end - begin)));
  }

  const std::string metadata = file_metadata_body(schema, static_cast<int64_t>(rows), row_groups,
                                                  gen.created_by);
  out += metadata;
  append_le32(out, static_cast<uint32_t>(metadata.size()));
  out += "PAR1";
  return out;
}

// ---------------------------------------------------------------------------
// Reader helpers
// ---------------------------------------------------------------------------
struct Opened {
  std::unique_ptr<ParquetReader> reader;
  Status status = Status::Ok;
};

Opened open_reader(const std::string &path, const std::string &secure_root,
                   size_t max_page_bytes = 0)
{
  Opened opened;
  ParquetOptions options;
  options.enabled = true;
  if (max_page_bytes > 0) {
    options.max_page_bytes = max_page_bytes;
  }
  opened.reader.reset(new ParquetReader());
  opened.status = opened.reader->open(path.c_str(), secure_root.c_str(), options);
  if (opened.status == Status::Ok) {
    opened.status = opened.reader->infer();
  }
  return opened;
}

std::string current_row(ParquetReader &reader)
{
  std::string text;
  for (size_t i = 0; i < reader.column_count(); ++i) {
    if (i != 0) {
      text += '|';
    }
    if (reader.is_null(i)) {
      text += "NULL";
    } else {
      text += std::string(reader.value(i));
    }
  }
  return text;
}

std::vector<std::string> scan_all(ParquetReader &reader)
{
  std::vector<std::string> rows;
  for (;;) {
    const Status status = reader.next();
    if (status == Status::End) {
      break;
    }
    if (status != Status::Ok) {
      std::cerr << "scan failed: " << status_name(status) << " (" << reader.error() << ")\n";
      ++failures;
      break;
    }
    rows.push_back(current_row(reader));
  }
  return rows;
}

void expect_rows(const char *what, const std::vector<std::string> &actual,
                 const std::vector<std::string> &expected)
{
  if (actual == expected) {
    return;
  }
  ++failures;
  std::cerr << what << ": row mismatch\n  expected:";
  for (const std::string &row : expected) {
    std::cerr << " [" << row << "]";
  }
  std::cerr << "\n  actual:  ";
  for (const std::string &row : actual) {
    std::cerr << " [" << row << "]";
  }
  std::cerr << "\n";
}

std::vector<size_t> indexes(std::initializer_list<size_t> list) { return std::vector<size_t>(list); }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
void test_arrow_rs_fixture()
{
  Fixtures fixtures;
  const std::string path = fixtures.put("simple.parquet", base64_decode(kSimpleParquetBase64));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("fixture open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  ParquetReader &reader = *opened.reader;
  CHECK(reader.column_count() == 1);
  CHECK(std::string(reader.column(0).name) == "c1");
  CHECK(reader.column(0).type == Type::BigInt);
  CHECK(reader.column(0).nullable);
  CHECK(reader.projected(0));
  expect_rows("fixture rows", scan_all(reader), {"0", "1", "2", "3", "4", "5", "6"});
  CHECK(reader.row_number() == 7);
  // A second scan from the same reader re-reads the same rows.
  expect_status("fixture rescan", reader.rescan(), Status::Ok);
  expect_rows("fixture rows after rescan", scan_all(reader), {"0", "1", "2", "3", "4", "5", "6"});

  // The planner infers in one reader and the executor binds that schema later.
  const FileIdentity identity = reader.identity();
  ParquetReader::Column planned[1];
  std::memcpy(planned, &reader.column(0), sizeof(ParquetReader::Column));
  ParquetReader executor;
  ParquetOptions options;
  options.enabled = true;
  expect_status("bind open", executor.open(path.c_str(), fixtures.root.c_str(), options),
                Status::Ok);
  expect_status("bind schema", executor.bind_schema(planned, 1, identity), Status::Ok);
  expect_rows("bound rows", scan_all(executor), {"0", "1", "2", "3", "4", "5", "6"});
}

void test_primitive_types_v1()
{
  Fixtures fixtures;
  Col ints;
  ints.name = "i64";
  ints.physical = P_INT64;
  ints.optional = false;
  ints.values = {vint(1), vint(-2), vint(3), vint(4), vint(5)};

  Col doubles;
  doubles.name = "dbl";
  doubles.physical = P_DOUBLE;
  doubles.optional = true;
  doubles.values = {vdbl(1.5), vnull(), vdbl(-0.25), vdbl(90.0), vdbl(1e10)};

  Col flags;
  flags.name = "flag";
  flags.physical = P_BOOLEAN;
  flags.optional = false;
  flags.values = {vbool(true), vbool(false), vbool(true), vbool(true), vbool(false)};

  Col texts;
  texts.name = "txt";
  texts.physical = P_BYTE_ARRAY;
  texts.converted = C_UTF8;
  texts.values = {vstr("alpha"), vstr(""), vnull(), vstr("delta"), vstr("e")};

  Col fixed;
  fixed.name = "fx";
  fixed.physical = P_FLBA;
  fixed.type_length = 2;
  fixed.values = {vstr("ab"), vstr("cd"), vstr("ef"), vstr("gh"), vstr("ij")};

  Col days;
  days.name = "day";
  days.physical = P_INT32;
  days.converted = C_DATE;
  days.values = {vint(0), vint(19934), vnull(), vint(-1), vint(1)};

  Col stamps;
  stamps.name = "ts";
  stamps.physical = P_INT64;
  stamps.converted = C_TIMESTAMP_MILLIS;
  stamps.values = {vint(0), vint(1754131200123), vnull(), vint(-1000), vint(1000)};

  Gen gen;
  gen.rows_per_page = 2;
  std::vector<Col> cols = {ints, doubles, flags, texts, fixed, days, stamps};
  const std::string path = fixtures.put("types.parquet", build_parquet(cols, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("types open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  ParquetReader &reader = *opened.reader;
  CHECK(reader.column_count() == 7);
  CHECK(!reader.column(0).nullable);
  CHECK(reader.column(1).nullable);
  CHECK(reader.column(0).type == Type::BigInt);
  CHECK(reader.column(1).type == Type::Double);
  CHECK(reader.column(2).type == Type::Boolean);
  CHECK(reader.column(3).type == Type::Varchar);
  CHECK(reader.column(4).type == Type::Varchar);
  CHECK(reader.column(5).type == Type::Date);
  CHECK(reader.column(6).type == Type::DateTime);
  expect_rows("types rows", scan_all(reader),
              {"1|1.5|true|alpha|ab|1970-01-01|1970-01-01 00:00:00",
               "-2|NULL|false||cd|2024-07-30|2025-08-02 10:40:00.123",
               "3|-0.25|true|NULL|ef|NULL|NULL",
               "4|90|true|delta|gh|1969-12-31|1969-12-31 23:59:59",
               "5|10000000000|false|e|ij|1970-01-02|1970-01-01 00:00:01"});
}

void test_v2_snappy()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT32;
  values.values = {vint(7), vnull(), vint(9), vint(9), vnull(), vint(-4), vint(0)};
  Col labels;
  labels.name = "label";
  labels.physical = P_BYTE_ARRAY;
  labels.converted = C_UTF8;
  labels.values = {vstr("aa"), vstr("bb"), vnull(), vstr("cc"), vstr("dd"), vstr("ee"), vstr("ff")};

  Gen gen;
  gen.v2 = true;
  gen.codec = 1;
  gen.rows_per_page = 3;
  const std::string path = fixtures.put("v2.parquet", build_parquet({values, labels}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("v2 open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  expect_rows("v2 rows", scan_all(*opened.reader),
              {"7|aa", "NULL|bb", "9|NULL", "9|cc", "NULL|dd", "-4|ee", "0|ff"});
}

void test_dictionary()
{
  Fixtures fixtures;
  Col repeated;
  repeated.name = "kind";
  repeated.physical = P_BYTE_ARRAY;
  repeated.converted = C_UTF8;
  for (int i = 0; i < 20; ++i) {
    repeated.values.push_back(vstr(i % 3 == 0 ? "one" : (i % 3 == 1 ? "two" : "three")));
  }
  Col numbers;
  numbers.name = "n";
  numbers.physical = P_INT32;
  numbers.values = {vint(1), vnull(), vint(1), vint(1), vint(2), vint(2), vint(2), vint(2),
                    vint(2), vint(2), vnull(), vnull(), vnull(), vnull(), vnull(), vnull(),
                    vnull(), vnull(), vint(3), vint(1)};
  for (size_t i = 0; i < 20; ++i) {
    if (numbers.values[i].is_null) {
      repeated.values[i] = vnull();
    }
  }

  Gen gen;
  gen.dictionary = true;
  gen.codec = 1;
  gen.rows_per_page = 7;
  const std::string path = fixtures.put("dict.parquet", build_parquet({repeated, numbers}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("dict open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  const std::vector<std::string> rows = scan_all(*opened.reader);
  CHECK(rows.size() == 20);
  if (rows.size() == 20) {
    CHECK(rows[0] == "one|1");
    CHECK(rows[1] == "NULL|NULL");
    CHECK(rows[2] == "three|1");
    CHECK(rows[3] == "one|1");
    CHECK(rows[4] == "two|2");
    CHECK(rows[18] == "one|3");
    CHECK(rows[19] == "two|1");
  }

  Gen v2;
  v2.v2 = true;
  v2.dictionary = true;
  v2.codec = 1;
  const std::string v2_path = fixtures.put("dict_v2.parquet", build_parquet({repeated, numbers}, v2));
  Opened v2_opened = open_reader(v2_path, fixtures.root);
  expect_status("dict v2 open", v2_opened.status, Status::Ok);
  if (v2_opened.status == Status::Ok) {
    expect_rows("dict v2 rows", scan_all(*v2_opened.reader), rows);
  }
}

void test_multiple_row_groups()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT64;
  for (int i = 0; i < 7; ++i) {
    values.values.push_back(vint(i));
  }
  Gen gen;
  gen.rows_per_group = 2;
  gen.rows_per_page = 1;
  const std::string path = fixtures.put("groups.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("groups open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  expect_rows("groups rows", scan_all(*opened.reader), {"0", "1", "2", "3", "4", "5", "6"});
}

void test_projection()
{
  Fixtures fixtures;
  Col a;
  a.name = "a";
  a.physical = P_INT64;
  a.values = {vint(1), vint(2)};
  Col b;
  b.name = "b";
  b.physical = P_INT64;
  b.values = {vint(10), vint(20)};
  Col c;
  c.name = "c";
  c.physical = P_BYTE_ARRAY;
  c.converted = C_UTF8;
  c.values = {vstr("x"), vstr("y")};
  Col d;
  d.name = "d";
  d.physical = P_INT32;
  d.values = {vint(100), vnull()};

  Gen gen;
  gen.codec = 1;
  const std::string path = fixtures.put("projection.parquet", build_parquet({a, b, c, d}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("projection open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  ParquetReader &reader = *opened.reader;
  const std::vector<size_t> chosen = indexes({0, 2});
  reader.set_projection(chosen.data(), chosen.size());
  CHECK(reader.projected(0));
  CHECK(!reader.projected(1));
  CHECK(reader.projected(2));
  CHECK(!reader.projected(3));
  expect_rows("projected rows", scan_all(reader), {"1|NULL|x|NULL", "2|NULL|y|NULL"});

  // Projecting every column again yields the full rows.
  expect_status("projection rescan", reader.rescan(), Status::Ok);
  std::vector<size_t> all;
  for (size_t i = 0; i < reader.column_count(); ++i) {
    all.push_back(i);
  }
  reader.set_projection(all.data(), all.size());
  expect_rows("all columns", scan_all(reader), {"1|10|x|100", "2|20|y|NULL"});

  // A bad projection is rejected before it can produce rows.
  expect_status("projection rescan 2", reader.rescan(), Status::Ok);
  const std::vector<size_t> bad = indexes({7});
  reader.set_projection(bad.data(), bad.size());
  expect_status("bad projection", reader.next(), Status::InvalidOption);
  const std::vector<size_t> empty;
  ParquetReader fresh;
  ParquetOptions options;
  options.enabled = true;
  expect_status("projection open 2", fresh.open(path.c_str(), fixtures.root.c_str(), options),
                Status::Ok);
  fresh.set_projection(empty.data(), 0);
  expect_status("projection without columns", fresh.next(), Status::InvalidOption);
}

void test_bit_packed_levels()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT32;
  values.values = {vint(1), vnull(), vint(3), vnull(), vnull(), vint(6)};
  Gen gen;
  gen.level_encoding = E_BIT_PACKED;
  const std::string path = fixtures.put("bitpacked.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("bitpacked open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  expect_rows("bitpacked rows", scan_all(*opened.reader), {"1", "NULL", "3", "NULL", "NULL", "6"});
}

void test_int96_and_time()
{
  Fixtures fixtures;
  Col stamp;
  stamp.name = "stamp";
  stamp.physical = P_INT96;
  stamp.values = {vint96(0, 2440588), vint96(3723456000000LL, 2440589), vnull()};
  Col clock;
  clock.name = "clock";
  clock.physical = P_INT32;
  clock.converted = C_TIME_MILLIS;
  clock.values = {vint(0), vint(3723000), vint(86399999)};
  Col micro_clock;
  micro_clock.name = "micro_clock";
  micro_clock.physical = P_INT64;
  micro_clock.converted = C_TIME_MICROS;
  micro_clock.values = {vint(0), vint(3723000000LL), vint(86399999999LL)};

  Gen gen;
  const std::string path =
      fixtures.put("int96.parquet", build_parquet({stamp, clock, micro_clock}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("int96 open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  CHECK(opened.reader->column(0).type == Type::DateTime);
  CHECK(opened.reader->column(1).type == Type::Varchar);
  CHECK(opened.reader->column(2).type == Type::Varchar);
  expect_rows("int96 rows", scan_all(*opened.reader),
              {"1970-01-01 00:00:00|00:00:00|00:00:00",
               "1970-01-02 01:02:03.456|01:02:03|01:02:03",
               "NULL|23:59:59.999|23:59:59.999999"});
}

void test_float_and_nan()
{
  Fixtures fixtures;
  Col values;
  values.name = "f";
  values.physical = P_FLOAT;
  values.values = {vdbl(0.5), vdbl(-2.0)};
  Gen gen;
  const std::string path = fixtures.put("float.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("float open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  CHECK(opened.reader->column(0).type == Type::Double);
  expect_rows("float rows", scan_all(*opened.reader), {"0.5", "-2"});
}

void test_unsupported_and_corrupt()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT32;
  values.values = {vint(1), vint(2)};

  // Nested schema.
  {
    Gen gen;
    gen.nested = true;
    const std::string path = fixtures.put("nested.parquet", build_parquet({values}, gen));
    Opened opened = open_reader(path, fixtures.root);
    expect_status("nested schema", opened.status, Status::Unsupported);
  }
  // Unsupported codec.
  {
    Gen gen;
    gen.codec = 2;  // GZIP
    const std::string path = fixtures.put("gzip.parquet", build_parquet({values}, gen));
    Opened opened = open_reader(path, fixtures.root);
    expect_status("gzip codec", opened.status, Status::Unsupported);
  }
  // Truncated file.
  {
    Gen gen;
    std::string bytes = build_parquet({values}, gen);
    bytes.resize(bytes.size() - 20);
    const std::string path = fixtures.put("truncated.parquet", bytes);
    Opened opened = open_reader(path, fixtures.root);
    expect_status("truncated file", opened.status, Status::Malformed);
  }
  // Not a Parquet file.
  {
    const std::string path = fixtures.put("text.parquet", "not a parquet file at all\n");
    Opened opened = open_reader(path, fixtures.root);
    expect_status("bad magic", opened.status, Status::Malformed);
  }
  // Empty file.
  {
    const std::string path = fixtures.put("empty.parquet", "");
    Opened opened = open_reader(path, fixtures.root);
    expect_status("empty file", opened.status, Status::Malformed);
  }
  // Footer length larger than the file.
  {
    Gen gen;
    std::string bytes = build_parquet({values}, gen);
    const size_t at = bytes.size() - 8;
    bytes[at] = static_cast<char>(0xff);
    bytes[at + 1] = static_cast<char>(0xff);
    bytes[at + 2] = static_cast<char>(0xff);
    bytes[at + 3] = static_cast<char>(0x7f);
    const std::string path = fixtures.put("bad_footer.parquet", bytes);
    Opened opened = open_reader(path, fixtures.root);
    expect_status("bad footer length", opened.status, Status::Malformed);
  }
  // A chunk that claims more values than its pages hold.
  {
    Gen gen;
    gen.num_values_delta = 3;
    const std::string path = fixtures.put("short.parquet", build_parquet({values}, gen));
    Opened opened = open_reader(path, fixtures.root);
    if (opened.status == Status::Ok) {
      expect_status("short chunk", opened.reader->next(), Status::Malformed);
    } else {
      expect_status("short chunk open", opened.status, Status::Ok);
    }
  }
  // Page larger than the configured limit.
  {
    Gen gen;
    gen.codec = 1;
    const std::string path = fixtures.put("large.parquet", build_parquet({values}, gen));
    Opened opened = open_reader(path, fixtures.root, 8);
    if (opened.status == Status::Ok) {
      expect_status("page limit", opened.reader->next(), Status::ResourceLimit);
    } else {
      expect_status("page limit open", opened.status, Status::Ok);
    }
  }
  // A file with no columns cannot back a table function.
  {
    Gen gen;
    const std::string path = fixtures.put("nocols.parquet", build_parquet({}, gen));
    Opened opened = open_reader(path, fixtures.root);
    expect_status("no columns", opened.status, Status::Malformed);
  }
}

void test_security()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT32;
  values.values = {vint(1)};
  Gen gen;
  const std::string path = fixtures.put("secure.parquet", build_parquet({values}, gen));

  // Disabled option.
  {
    ParquetReader reader;
    ParquetOptions options;
    expect_status("disabled", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::Disabled);
  }
  // Empty and "null" secure roots fail closed.
  {
    ParquetReader reader;
    ParquetOptions options;
    options.enabled = true;
    expect_status("empty root", reader.open(path.c_str(), "", options), Status::Denied);
    expect_status("null root", reader.open(path.c_str(), "null", options), Status::Denied);
  }
  // A path outside the secure root is denied.
  {
    const std::string outside = fixtures.put("../escape.parquet", build_parquet({values}, gen));
    const std::string escaped = outside + "/../../../.." + outside;
    ParquetReader reader;
    ParquetOptions options;
    options.enabled = true;
    expect_status("outside root", reader.open(escaped.c_str(), fixtures.root.c_str(), options),
                  Status::Denied);
  }
  // A directory is not a file.
  {
    ParquetReader reader;
    ParquetOptions options;
    options.enabled = true;
    expect_status("directory", reader.open(fixtures.root.c_str(), "/", options), Status::Denied);
  }
  // An invalid option fails before touching the file.
  {
    ParquetReader reader;
    ParquetOptions options;
    options.enabled = true;
    options.sample_size = 10;
    expect_status("sample size", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::Unsupported);
    options.sample_size = -1;
    options.max_page_bytes = 0;
    expect_status("page limit option", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::InvalidOption);
  }
}

void test_changed_file()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT64;
  values.values = {vint(1), vint(2)};
  Gen gen;
  gen.rows_per_group = 1;
  const std::string path = fixtures.put("changed.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("changed open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  ParquetReader &reader = *opened.reader;
  expect_status("first row", reader.next(), Status::Ok);
  // Rewrite the file behind the reader: the second row group must not be read
  // from a file that no longer matches the inferred identity.
  write_file(path, build_parquet({values}, gen) + std::string(64, 'x'));
  expect_status("changed row group", reader.next(), Status::Changed);
}

void test_bind_schema_mismatch()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT64;
  values.values = {vint(1)};
  Gen gen;
  const std::string path = fixtures.put("bind.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("bind open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  ParquetReader::Column planned = opened.reader->column(0);
  const FileIdentity identity = opened.reader->identity();

  ParquetOptions options;
  options.enabled = true;

  {
    ParquetReader reader;
    expect_status("bind open reader", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::Ok);
    ParquetReader::Column wrong_name = planned;
    std::snprintf(wrong_name.name, sizeof(wrong_name.name), "%s", "other");
    expect_status("bind wrong name", reader.bind_schema(&wrong_name, 1, identity), Status::Malformed);
  }
  {
    ParquetReader reader;
    expect_status("bind open reader 2", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::Ok);
    ParquetReader::Column wrong_type = planned;
    wrong_type.type = Type::Varchar;
    expect_status("bind wrong type", reader.bind_schema(&wrong_type, 1, identity), Status::Malformed);
  }
  {
    ParquetReader reader;
    expect_status("bind open reader 3", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::Ok);
    expect_status("bind wrong count", reader.bind_schema(&planned, 2, identity),
                  Status::InvalidOption);
  }
  {
    ParquetReader reader;
    expect_status("bind open reader 4", reader.open(path.c_str(), fixtures.root.c_str(), options),
                  Status::Ok);
    FileIdentity stale = identity;
    stale.size += 1;
    expect_status("bind stale identity", reader.bind_schema(&planned, 1, stale), Status::Changed);
  }
}

void test_empty_rows()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT32;
  Gen gen;
  const std::string path = fixtures.put("zero.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("zero rows open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  expect_status("zero rows", opened.reader->next(), Status::End);
  CHECK(opened.reader->column_count() == 1);
}

void test_projection_after_rows()
{
  Fixtures fixtures;
  Col values;
  values.name = "v";
  values.physical = P_INT64;
  values.values = {vint(1), vint(2)};
  Gen gen;
  const std::string path = fixtures.put("late.parquet", build_parquet({values}, gen));
  Opened opened = open_reader(path, fixtures.root);
  expect_status("late open", opened.status, Status::Ok);
  if (opened.status != Status::Ok) {
    return;
  }
  ParquetReader &reader = *opened.reader;
  expect_status("late first row", reader.next(), Status::Ok);
  const std::vector<size_t> all = indexes({0});
  reader.set_projection(all.data(), all.size());
  expect_status("late projection", reader.next(), Status::InvalidOption);
}

void test_memory_budget()
{
  Fixtures fixtures;
  ParquetOptions options;
  options.enabled = true;
  options.max_memory_bytes = 256 * 1024;
  Col col;
  col.name = "a";
  col.physical = P_INT64;
  col.optional = false;
  for (int i = 0; i < 8192; ++i) col.values.push_back(vint(i));
  std::vector<Col> cols(4, col);
  for (size_t i = 0; i < cols.size(); ++i) cols[i].name = "c" + std::to_string(i);
  const std::string pages = fixtures.put("budget-pages.parquet", build_parquet(cols, Gen()));
  ParquetReader reader;
  expect_status("budget pages open", reader.open(pages.c_str(), fixtures.root.c_str(), options), Status::Ok);
  expect_status("budget pages infer", reader.infer(), Status::Ok);
  expect_status("aggregate pages", reader.next(), Status::ResourceLimit);
  expect_status("budget error sticky", reader.next(), Status::ResourceLimit);
  CHECK(std::string(reader.error()).find("max_memory_bytes") != std::string::npos);

  // Projection must not reserve pages for unreferenced columns; reopen releases
  // all old allocations and a rescan must reuse, not cumulatively spend, budget.
  expect_status("budget reopen", reader.open(pages.c_str(), fixtures.root.c_str(), options), Status::Ok);
  expect_status("budget reopen infer", reader.infer(), Status::Ok);
  const size_t only = 0;
  reader.set_projection(&only, 1);
  for (int pass = 0; pass < 3; ++pass) {
    expect_status("budget rescan", reader.rescan(), Status::Ok);
    for (int i = 0; i < 8192; ++i) {
      if (reader.next() != Status::Ok) { CHECK(false); break; }
    }
    expect_status("budget projected end", reader.next(), Status::End);
  }

  Gen dict;
  dict.dictionary = true;
  const std::string dictionary = fixtures.put("budget-dictionary.parquet", build_parquet({col}, dict));
  expect_status("budget dictionary open", reader.open(dictionary.c_str(), fixtures.root.c_str(), options), Status::Ok);
  expect_status("budget dictionary infer", reader.infer(), Status::Ok);
  expect_status("dictionary cells budget", reader.next(), Status::ResourceLimit);

  // A tiny footer can advertise a huge list before providing its first item.
  Tw footer;
  int last = 0;
  footer.i32f(last, 1, 2);
  footer.field(last, 4, CT_LIST);
  footer.list_header(CT_STRUCT, 1u << 20);
  footer.stop();
  std::string bytes = "PAR1" + footer.out;
  append_le32(bytes, static_cast<uint32_t>(footer.out.size()));
  bytes += "PAR1";
  const std::string metadata = fixtures.put("budget-metadata.parquet", bytes);
  expect_status("metadata allocation budget", reader.open(metadata.c_str(), fixtures.root.c_str(), options), Status::ResourceLimit);

  options.max_memory_bytes = 0;
  expect_status("zero memory option", reader.open(pages.c_str(), fixtures.root.c_str(), options), Status::InvalidOption);
  options.max_memory_bytes = 1;
  expect_status("footer bytes budget", reader.open(pages.c_str(), fixtures.root.c_str(), options), Status::ResourceLimit);

  // Two large dictionary strings fit their page limit individually, but the
  // retained rendered dictionaries and row text must share the reader budget.
  Col strings;
  strings.name = "strings";
  strings.physical = P_BYTE_ARRAY;
  strings.optional = false;
  strings.values = {vstr(std::string(60000, 'x')), vstr(std::string(60000, 'y'))};
  const std::string text_path = fixtures.put("budget-text.parquet", build_parquet({strings}, dict));
  options.max_memory_bytes = 300 * 1024;
  expect_status("dictionary text open", reader.open(text_path.c_str(), fixtures.root.c_str(), options), Status::Ok);
  expect_status("dictionary text infer", reader.infer(), Status::Ok);
  expect_status("dictionary text budget", reader.next(), Status::ResourceLimit);

  // Repeated row groups retain capacities, rather than spending budget afresh.
  Gen groups;
  groups.rows_per_group = 64;
  groups.rows_per_page = 16;
  const std::string grouped = fixtures.put("budget-groups.parquet", build_parquet({col}, groups));
  options.max_memory_bytes = 256 * 1024;
  expect_status("budget groups open", reader.open(grouped.c_str(), fixtures.root.c_str(), options), Status::Ok);
  expect_status("budget groups infer", reader.infer(), Status::Ok);
  for (int i = 0; i < 8192; ++i) {
    if (reader.next() != Status::Ok) { CHECK(false); break; }
  }
  expect_status("budget groups end", reader.next(), Status::End);
}

} // namespace

int main()
{
  test_arrow_rs_fixture();
  test_primitive_types_v1();
  test_v2_snappy();
  test_dictionary();
  test_multiple_row_groups();
  test_projection();
  test_bit_packed_levels();
  test_int96_and_time();
  test_float_and_nan();
  test_unsupported_and_corrupt();
  test_security();
  test_changed_file();
  test_bind_schema_mismatch();
  test_empty_rows();
  test_projection_after_rows();
  test_memory_budget();
  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "parquet_file_reader: all checks passed\n";
  return 0;
}
