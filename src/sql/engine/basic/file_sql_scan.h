/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/engine/basic/file_scan_utils.h"
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

// A bounded schema, not data: the constant expression carries this definition
// through native rewrites and code generation. Execution revalidates both the
// current security policy and the identity captured during schema inference.
struct FileScanDefinition {
  OB_UNIS_VERSION(1);
public:
  int infer_csv(common::ObIAllocator &allocator, const common::ObString &path,
                const char *secure_root, const CsvOptions &options);
  int encode(common::ObIAllocator &allocator, common::ObString &encoded) const;
  int decode(const common::ObString &encoded);
  int open_csv(common::ObIAllocator &allocator, CsvReader &reader, const char *secure_root) const;
  common::ObString path_;
  common::ObString null_text_;
  bool header_ = true;
  bool empty_is_null_ = true;
  int64_t delimiter_ = ',';
  int64_t quote_ = '"';
  int64_t escape_ = '"';
  common::ObSEArray<int64_t, 7> identity_;
  common::ObSEArray<FileColumn, 16> columns_;
};

int sql_error(Status status, const char *message);

}}} // namespace oceanbase::sql::file_sql
