/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "file_scan_utils.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oceanbase { namespace sql { namespace file_sql {

// Streaming, fail-if-exists CSV export sink. Output is written to a unique
// sibling temporary file with a fixed 64 KiB buffer and published by
// linkat(2) (a no-replace primitive) only after fsync(), so a failed or
// abandoned export never leaves a partial file and a target that appears
// concurrently is never overwritten.
struct CsvWriteOptions {
  bool enabled = true;               // SQL layer maps enable_file_sql onto this
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string_view null_text = "NULL";
  std::string_view line_terminator = "\n";
  bool force_quote = false;
};

class CsvWriter {
public:
  static constexpr size_t MAX_FIELD_BYTES = 16 * 1024 * 1024;
  static constexpr size_t MAX_HEADER_COLUMNS = 1024;
  CsvWriter();
  ~CsvWriter();                                    // abort() when not committed
  CsvWriter(const CsvWriter &) = delete;
  CsvWriter &operator=(const CsvWriter &) = delete;
  Status open(const char *path, const char *secure_root, const CsvWriteOptions &options);
  Status write_header(const std::string_view *names, size_t count);
  Status write_row(const std::string_view *values, const bool *is_null, size_t count);
  Status commit();                                  // flush + fsync + no-replace publish
  void abort();                                     // idempotent; removes only owned temp files
  bool is_open() const { return temp_fd_ >= 0; }
  bool is_committed() const { return committed_; }
  const char *error() const { return error_; }
  const char *target_path() const { return target_path_; }
  uint64_t row_count() const { return row_count_; }

private:
  static constexpr size_t BUFFER_BYTES = 64 * 1024;
  static constexpr size_t PATH_BYTES = 4096;
  static constexpr size_t NAME_BYTES = 256;
  static constexpr size_t NULL_TEXT_BYTES = 1024;
  static constexpr size_t TERMINATOR_BYTES = 8;
  Status fail(Status status, const char *message);
  Status emit(const char *data, size_t size);
  Status emit(char value);
  Status emit_field(std::string_view value, bool is_null);
  Status emit_values(const std::string_view *values, const bool *is_null, size_t count);
  Status flush_buffer();
  Status check_target_available();
  Status create_temp_file();
  Status verify_directory_unchanged();
  void reset();
  void close_directory_fds();
  int root_fd_ = -1;
  int dir_fd_ = -1;
  int temp_fd_ = -1;
  bool temp_created_ = false;
  bool committed_ = false;
  bool header_written_ = false;
  size_t row_columns_ = 0;
  uint64_t row_count_ = 0;
  bool write_failed_ = false;
  CsvWriteOptions options_;
  char target_path_[PATH_BYTES] = {};
  char root_relative_[PATH_BYTES] = {};
  char relative_dir_[PATH_BYTES] = {};
  char base_name_[NAME_BYTES] = {};
  char temp_name_[NAME_BYTES] = {};
  char null_text_[NULL_TEXT_BYTES] = {};
  char line_terminator_[TERMINATOR_BYTES] = {};
  char error_[512] = {};
  char buffer_[BUFFER_BYTES] = {};
  size_t used_ = 0;
};

} // namespace oceanbase::sql::file_sql
}} // namespace oceanbase::sql
