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
// Shared text helpers so every format reader validates and compares names the
// same way: UTF-8 (NUL-free) payloads and ASCII case-insensitive identifiers.
bool is_valid_utf8(std::string_view text);
bool equal_ascii_ci(std::string_view left, std::string_view right);

// Serializable file identity passed from schema resolution to execution.
// Nanosecond modification and change times also catch same-size rewrites.
struct FileIdentity {
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t size = 0;
  int64_t modified_seconds = 0;
  int64_t modified_nanoseconds = 0;
  int64_t changed_seconds = 0;
  int64_t changed_nanoseconds = 0;
  bool operator==(const FileIdentity &other) const {
    return device == other.device && inode == other.inode && size == other.size
        && modified_seconds == other.modified_seconds && modified_nanoseconds == other.modified_nanoseconds
        && changed_seconds == other.changed_seconds && changed_nanoseconds == other.changed_nanoseconds;
  }
};

// Descriptor-relative, no-symlink access to one regular file beneath a
// secure_file_priv root. Writers of format readers share this instead of
// re-deriving the containment policy, so every format fails closed identically.
// The snapshot taken at open() is what a planner serializes and an executor
// revalidates, which rejects a file replaced between resolution and execution.
class SecureFile {
public:
  SecureFile() = default;
  ~SecureFile();
  SecureFile(const SecureFile &) = delete;
  SecureFile &operator=(const SecureFile &) = delete;
  Status open(const char *path, const char *secure_root);
  // Detects replacement, truncation and same-size rewrites of the opened file.
  Status check_unchanged();
  FileIdentity identity() const;
  // Positional read used by formats with a footer or random-access pages.
  // Returns Ok when all requested bytes were read, End when the file ends
  // early, and reports how many bytes were actually available in read.
  Status read_at(void *buffer, size_t length, int64_t offset, size_t &read);
  int fd() const { return fd_; }
  int64_t size() const { return snapshot_.st_size; }
  const char *path() const { return path_; }
  const char *error() const { return error_; }
  void close();
private:
  Status fail(Status status, const char *message);
  int fd_ = -1;
  int root_fd_ = -1;
  struct stat snapshot_ = {};
  char path_[4096] = {};
  char relative_path_[4096] = {};
  char error_[512] = {};
};

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
  FileIdentity identity() const;
  // Bind only schema inferred from the same file identity and CSV options.
  // Copies metadata; the resolver and its descriptors need not remain alive.
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
