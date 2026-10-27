/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/engine/basic/file_scan_utils.h"
#include "sql/engine/basic/json_file_reader.h"
#include "sql/engine/basic/parquet_file_reader.h"
#include "sql/engine/basic/file_sql_glob.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_obj_type.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase { namespace sql { namespace file_sql {

struct FileColumn {
  OB_UNIS_VERSION(1);
public:
  common::ObString name_;
  int64_t type_ = static_cast<int64_t>(Type::Null);
  int64_t max_bytes_ = 0;
  bool nullable_ = true;
  common::ObObjType sql_type() const;
};

// The format only decides how execution reopens the same bytes; the schema and
// the identity captured during resolution are what the plan carries.
enum class FileFormat : int64_t { Csv = 0, Json = 1, Parquet = 2, Glob = 3 };

// A bounded schema, not data: the constant expression carries this definition
// through native rewrites and code generation. Execution revalidates both the
// current security policy and the identity captured during schema inference.
struct FileScanDefinition {
  OB_UNIS_VERSION(1);
public:
  int infer_csv(common::ObIAllocator &allocator, const common::ObString &path,
                const char *secure_root, const CsvOptions &options);
  int infer_json(common::ObIAllocator &allocator, const common::ObString &path,
                 const char *secure_root, const JsonOptions &options);
  int infer_parquet(common::ObIAllocator &allocator, const common::ObString &path,
                    const char *secure_root, const ParquetOptions &options);
  int infer_glob(common::ObIAllocator &allocator, const common::ObString &path,
                 const char *secure_root);
  int encode(common::ObIAllocator &allocator, common::ObString &encoded) const;
  int decode(const common::ObString &encoded);
  int open_csv(common::ObIAllocator &allocator, CsvReader &reader, const char *secure_root) const;
  int open_json(common::ObIAllocator &allocator, JsonlReader &reader, const char *secure_root) const;
  int open_parquet(common::ObIAllocator &allocator, ParquetReader &reader, const char *secure_root) const;
  common::ObString path_;
  common::ObString null_text_;
  bool header_ = true;
  bool empty_is_null_ = true;
  int64_t delimiter_ = ',';
  int64_t quote_ = '"';
  int64_t escape_ = '"';
  int64_t format_ = static_cast<int64_t>(FileFormat::Csv);
  common::ObSEArray<int64_t, 7> identity_;
  common::ObSEArray<FileColumn, 16> columns_;
};

// One executor-facing view over the format-specific readers. They share a member
// shape, so the executor keeps a single conversion path for every format; only
// the format that can prune columns (Parquet) overrides set_projection().
class FileScanReader {
public:
  FileScanReader() = default;
  ~FileScanReader();
  int open(const FileScanDefinition &definition, common::ObIAllocator &allocator,
           const char *secure_root);
  Status next();
  Status rescan();
  void close();
  int64_t column_count() const;
  bool is_null(int64_t column) const;
  std::string_view value(int64_t column) const;
  Type column_type(int64_t column) const;
  const char *error() const;
  void set_projection(const int64_t *columns, int64_t count);
private:
  FileFormat format_ = FileFormat::Csv;
  // Exactly one of these is non-null; the pointer that is set names the format.
  CsvReader *csv_ = nullptr;
  JsonlReader *json_ = nullptr;
  ParquetReader *parquet_ = nullptr;
  GlobResult *glob_ = nullptr;
  size_t glob_position_ = 0;
};

int sql_error(Status status, const char *message);

}}} // namespace oceanbase::sql::file_sql
