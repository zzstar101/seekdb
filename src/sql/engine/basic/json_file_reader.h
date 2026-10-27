/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/engine/basic/file_scan_utils.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace oceanbase { namespace sql { namespace file_sql {

struct JsonOptions {
  bool enabled = false;
  int64_t sample_size = -1;
  size_t max_record_bytes = 16 * 1024 * 1024;
};

// Streaming reader for newline-delimited JSON (JSONL/NDJSON).
//
// One flat JSON object per non-blank line; the schema is the union of all keys
// in first-appearance order, compared ASCII-case-insensitively. Values are
// exposed as text so the executor keeps one conversion path shared with CSV.
// Only the current line and its decoded values are retained: memory is bounded
// by max_record_bytes and by the column limits below, never by the file size.
// Views returned by value() stay valid until the next next()/rescan()/close().
class JsonlReader {
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

  JsonlReader() = default;
  ~JsonlReader();
  JsonlReader(const JsonlReader &) = delete;
  JsonlReader &operator=(const JsonlReader &) = delete;

  Status open(const char *path, const char *secure_root, const JsonOptions &options);
  // Scans the whole file once and retains only the derived schema.
  Status infer();
  FileIdentity identity() const;
  // Binds a planned schema inferred from the same file identity. Copies the
  // metadata, so the planner's memory and descriptors need not stay alive.
  Status bind_schema(const Column *columns, size_t count, const FileIdentity &expected);
  Status next();
  Status rescan();
  void close();
  size_t column_count() const { return column_count_; }
  const Column &column(size_t index) const { return columns_[index]; }
  std::string_view value(size_t index) const;
  bool is_null(size_t index) const { return cells_[index].is_null; }
  const char *error() const { return error_; }
  uint64_t row_number() const { return row_number_; }

private:
  // Input is streamed through this buffer; one line is assembled at a time.
  static constexpr size_t READ_CHUNK = 32 * 1024;

  Status fail(Status status, const char *message);
  Status fail_path(Status status, const char *message);
  Status check_unchanged();
  Status rewind();
  Status fill();
  Status get(char &c);
  Status read_line(bool &has_line);
  Status scan_row();
  Status parse_row();
  Status parse_value(size_t index);
  Status parse_number(size_t index);
  Status parse_string(std::string &out, size_t limit);
  Status parse_escape(std::string &out, size_t limit);
  Status parse_unicode_escape(std::string &out, size_t limit);
  Status resolve_column(size_t &index);
  void merge_row();
  void skip_whitespace() { while (pos_ < end_ && is_whitespace(line_[pos_])) ++pos_; }
  bool at(char c) const { return pos_ < end_ && line_[pos_] == c; }
  bool consume(char c);
  bool consume_literal(std::string_view literal);
  bool read_hex4(uint32_t &value);
  bool append(std::string &out, size_t limit, std::string_view text);
  bool append(std::string &out, size_t limit, char c);
  void assign(size_t index, size_t offset, Type type);
  void assign_null(size_t index);
  void reset_row();

  SecureFile file_;
  JsonOptions options_;
  Status status_ = Status::Ok;
  char path_[4096] = {};
  char error_[512] = {};
  std::array<Column, MAX_COLUMNS> columns_;
  std::array<Cell, MAX_COLUMNS> cells_;
  std::array<Type, MAX_COLUMNS> row_types_;
  size_t column_count_ = 0;
  uint64_t row_number_ = 0;
  uint64_t line_number_ = 0;
  uint64_t next_line_number_ = 1;
  bool inferred_ = false;
  bool inferring_ = false;
  char chunk_[READ_CHUNK] = {};
  size_t chunk_pos_ = 0;
  size_t chunk_end_ = 0;
  int64_t next_offset_ = 0;
  std::string line_;
  std::string text_;
  std::string name_;
  size_t pos_ = 0;
  size_t end_ = 0;

  static bool is_whitespace(char c)
  {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }
};

}}} // namespace oceanbase::sql::file_sql
