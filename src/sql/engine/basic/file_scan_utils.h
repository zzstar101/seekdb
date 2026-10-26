/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <sys/stat.h>

namespace oceanbase { namespace sql { namespace file_sql {

enum class Status { Ok, End, Disabled, InvalidOption, Denied, Io, Changed, Malformed, ResourceLimit, Unsupported };
enum class Type { Null, BigInt, Double, Boolean, Varchar, Date, DateTime };
Type infer_type(std::string_view text);
Type widen(Type left, Type right);
const char *type_name(Type type);

struct CsvOptions {
  bool enabled = false;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string_view null_text = "NULL";
  bool empty_is_null = true;
  int64_t sample_size = -1;
  size_t max_record_bytes = 16 * 1024 * 1024;
};

// Bounded row workspace. No file contents are retained across next() calls.
// Views are valid until the next next()/rescan()/close(). Not copyable.
class CsvReader {
public:
  static constexpr size_t MAX_COLUMNS = 1024;
  static constexpr size_t MAX_NAME_BYTES = 512;
  struct Column {
    char name[MAX_NAME_BYTES + 1] = {};
    Type type = Type::Null;
    bool nullable = false;
    size_t max_bytes = 0;
  };
  struct Cell { size_t offset = 0; size_t length = 0; bool is_null = false; };
  CsvReader() = default;
  ~CsvReader();
  CsvReader(const CsvReader &) = delete;
  CsvReader &operator=(const CsvReader &) = delete;
  Status open(const char *path, const char *secure_root, const CsvOptions &options);
  Status infer();
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
  Status fail(Status status, const char *message);
  Status check_unchanged();
  Status read_record();
  Status rewind_file();
  bool append(char value);
  bool finish_cell(size_t offset, bool quoted);
  int get();
  FILE *file_ = nullptr;
  int root_fd_ = -1;
  struct stat snapshot_ = {};
  char relative_path_[4096] = {};
  char path_[4096] = {};
  char error_[512] = {};
  char null_text_[1025] = {};
  CsvOptions options_;
  Status status_ = Status::Ok;
  char *buffer_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
  size_t field_count_ = 0;
  size_t column_count_ = 0;
  uint64_t row_number_ = 0;
  uint64_t line_number_ = 1;
  bool inferred_ = false;
  std::array<Column, MAX_COLUMNS> columns_;
  std::array<Cell, MAX_COLUMNS> cells_;
};

}}} // namespace oceanbase::sql::file_sql
