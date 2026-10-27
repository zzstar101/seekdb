/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/engine/basic/file_scan_utils.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace oceanbase { namespace sql { namespace file_sql {

struct ParquetOptions {
  bool enabled = false;
  int64_t sample_size = -1;
  // Upper bound for a single page payload (compressed or decompressed) and for
  // the footer metadata block; exceeding it is a resource-limit failure.
  size_t max_page_bytes = 64 * 1024 * 1024;
  // Aggregate dynamic-storage budget per reader, including footer/metadata,
  // all column pages/dictionaries, scratch buffers and rendered row text.
  // Retained capacity and overlapping old/new allocations during growth count;
  // fixed reader objects and allocator bookkeeping are excluded.
  size_t max_memory_bytes = 256 * 1024 * 1024;
};

// Streaming reader for Parquet files with a flat leaf schema.
//
// The footer is parsed once at open() and validated (magic, bounds, schema,
// row groups, codecs); row groups and pages are then read on demand through
// positional reads, so memory is bounded by the file identity plus one page per
// referenced column, never by the file size. Values are exposed as text so the
// executor keeps one conversion path shared with CSV and JSONL.
//
// Supported: OPTIONAL/REQUIRED primitives (BOOLEAN, INT32, INT64, INT96, FLOAT,
// DOUBLE, BYTE_ARRAY, FIXED_LEN_BYTE_ARRAY), DATE/TIME/TIMESTAMP/STRING logical
// and converted types, PLAIN and dictionary page encodings, RLE and BIT_PACKED
// levels, DATA_PAGE and DATA_PAGE_V2, UNCOMPRESSED and SNAPPY, many row groups
// and many pages per chunk. Nested/repeated schemas and other codecs or
// encodings fail closed at open()/infer() or at the page that uses them.
//
// Views returned by value() stay valid until the next next()/rescan()/close().
class ParquetReader {
public:
  static constexpr size_t MAX_COLUMNS = 1024;
  static constexpr size_t MAX_NAME_BYTES = 512;
  struct Column {
    char name[MAX_NAME_BYTES + 1] = {};
    Type type = Type::Null;
    bool nullable = false;
    size_t max_bytes = 0;
  };
  struct Cell {
    size_t offset = 0;
    size_t length = 0;
    bool is_null = false;
  };

  ParquetReader();
  ~ParquetReader();
  ParquetReader(const ParquetReader &) = delete;
  ParquetReader &operator=(const ParquetReader &) = delete;

  Status open(const char *path, const char *secure_root, const ParquetOptions &options);
  // Derives the column types from the footer schema parsed by open().
  Status infer();
  FileIdentity identity() const;
  // Binds a planned schema inferred from the same file identity. Rejects a plan
  // that no longer matches the file's names, types or nullability.
  Status bind_schema(const Column *columns, size_t count, const FileIdentity &expected);
  // Restricts scanning to the listed column indexes; unset means all columns.
  // Columns that are not projected read as NULL, so callers must project every
  // column they inspect.
  void set_projection(const size_t *columns, size_t count);
  Status next();
  Status rescan();
  void close();
  size_t column_count() const;
  const Column &column(size_t index) const;
  bool projected(size_t index) const;
  std::string_view value(size_t index) const;
  bool is_null(size_t index) const;
  const char *error() const;
  uint64_t row_number() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}}} // namespace oceanbase::sql::file_sql
