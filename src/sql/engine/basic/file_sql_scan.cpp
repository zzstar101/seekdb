/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_sql_scan.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include <cstring>

namespace oceanbase { namespace sql { namespace file_sql {
using namespace common;
OB_SERIALIZE_MEMBER(FileColumn, name_, type_, max_bytes_, nullable_);
OB_SERIALIZE_MEMBER(FileScanDefinition, path_, null_text_, header_, empty_is_null_,
                    delimiter_, quote_, escape_, identity_, columns_);

ObObjType FileColumn::sql_type() const
{
  switch (static_cast<Type>(type_)) {
    case Type::BigInt: return ObIntType;
    case Type::Double: return ObDoubleType;
    case Type::Boolean: return ObTinyIntType;
    case Type::Date: return ObDateType;
    case Type::DateTime: return ObDateTimeType;
    // An all-NULL column remains nullable and can be used in ordinary SQL.
    case Type::Null: case Type::Varchar: return ObVarcharType;
  }
  return ObMaxType;
}

int sql_error(Status status, const char *message)
{
  int ret = OB_SUCCESS;
  switch (status) {
    case Status::Ok: break;
    case Status::End: ret = OB_ITER_END; break;
    case Status::Disabled: case Status::Unsupported:
      ret = OB_NOT_SUPPORTED; LOG_USER_ERROR(OB_NOT_SUPPORTED, message); break;
    case Status::Denied:
      ret = OB_ERR_NO_PRIVILEGE; LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE); break;
    default:
      ret = OB_INVALID_ARGUMENT; LOG_USER_ERROR(OB_INVALID_ARGUMENT, message); break;
  }
  if (ret != OB_SUCCESS && ret != OB_ITER_END) {
    LOG_WARN("file scan failed", K(ret), K(message), K(static_cast<int>(status)));
  }
  return ret;
}

int FileScanDefinition::infer_csv(ObIAllocator &allocator, const ObString &path,
                                  const char *secure_root, const CsvOptions &options)
{
  int ret = OB_SUCCESS;
  CsvReader *reader = nullptr;
  void *storage = nullptr;
  if (path.empty() || path.length() >= 4096 || std::memchr(path.ptr(), 0, path.length()) != nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ob_write_string(allocator, path, path_, true))) {
  } else if (OB_FAIL(ob_write_string(allocator, ObString(options.null_text.size(), options.null_text.data()), null_text_))) {
  } else if (OB_ISNULL(storage = allocator.alloc(sizeof(CsvReader)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    reader = new (storage) CsvReader();
    if (OB_FAIL(sql_error(reader->open(path_.ptr(), secure_root, options), reader->error()))) {
    } else if (OB_FAIL(sql_error(reader->infer(), reader->error()))) {
    } else {
      header_ = options.header;
      empty_is_null_ = options.empty_is_null;
      delimiter_ = options.delimiter;
      quote_ = options.quote;
      escape_ = options.escape;
      const FileIdentity id = reader->identity();
      const int64_t values[] = {static_cast<int64_t>(id.device), static_cast<int64_t>(id.inode), id.size,
                               id.modified_seconds, id.modified_nanoseconds, id.changed_seconds, id.changed_nanoseconds};
      for (int64_t v : values) { if (OB_SUCC(ret)) { ret = identity_.push_back(v); } }
      for (size_t i = 0; OB_SUCC(ret) && i < reader->column_count(); ++i) {
        const CsvReader::Column &source = reader->column(i);
        FileColumn col;
        col.type_ = static_cast<int64_t>(source.type);
        col.max_bytes_ = source.max_bytes;
        col.nullable_ = source.nullable;
        if (OB_FAIL(ob_write_string(allocator, ObString::make_string(source.name), col.name_))) {
        } else if (OB_FAIL(columns_.push_back(col))) {
        }
      }
    }
    reader->~CsvReader();
    allocator.free(storage);
  }
  return ret;
}

int FileScanDefinition::encode(ObIAllocator &allocator, ObString &encoded) const
{
  int ret = OB_SUCCESS;
  const int64_t size = get_serialize_size();
  char *buffer = nullptr;
  int64_t pos = 0;
  if (size <= 0 || size > 2 * 1024 * 1024) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_ISNULL(buffer = static_cast<char *>(allocator.alloc(size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(serialize(buffer, size, pos))) {
  } else {
    encoded.assign_ptr(buffer, pos);
  }
  return ret;
}

int FileScanDefinition::decode(const ObString &encoded)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  if (encoded.empty() || encoded.length() > 2 * 1024 * 1024) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(deserialize(encoded.ptr(), encoded.length(), pos))) {
  } else if (pos != encoded.length() || identity_.count() != 7 || columns_.empty()
             || columns_.count() > CsvReader::MAX_COLUMNS || path_.empty() || path_.length() >= 4096
             || std::memchr(path_.ptr(), 0, path_.length()) != nullptr || null_text_.length() > 1024
             || delimiter_ <= 0 || delimiter_ > 127 || quote_ <= 0 || quote_ > 127
             || escape_ <= 0 || escape_ > 127) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    const FileColumn &col = columns_.at(i);
    if (col.name_.empty() || col.name_.length() > CsvReader::MAX_NAME_BYTES
        || std::memchr(col.name_.ptr(), 0, col.name_.length()) != nullptr
        || col.type_ < static_cast<int64_t>(Type::Null) || col.type_ > static_cast<int64_t>(Type::DateTime)
        || col.max_bytes_ < 0 || col.max_bytes_ > 16 * 1024 * 1024) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

int FileScanDefinition::open_csv(ObIAllocator &allocator, CsvReader &reader, const char *secure_root) const
{
  int ret = OB_SUCCESS;
  ObString path;
  CsvOptions options;
  options.enabled = true;
  options.header = header_;
  options.empty_is_null = empty_is_null_;
  options.delimiter = delimiter_;
  options.quote = quote_;
  options.escape = escape_;
  options.null_text = std::string_view(null_text_.ptr(), null_text_.length());
  CsvReader::Column *columns = nullptr;
  if (identity_.count() != 7 || columns_.empty() || columns_.count() > CsvReader::MAX_COLUMNS) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ob_write_string(allocator, path_, path, true))) {
  } else if (OB_FAIL(sql_error(reader.open(path.ptr(), secure_root, options), reader.error()))) {
  } else if (OB_ISNULL(columns = static_cast<CsvReader::Column *>(allocator.alloc(sizeof(*columns) * columns_.count())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; i < columns_.count(); ++i) {
      new (columns + i) CsvReader::Column();
      const FileColumn &src = columns_.at(i);
      std::memcpy(columns[i].name, src.name_.ptr(), src.name_.length());
      columns[i].type = static_cast<Type>(src.type_);
      columns[i].max_bytes = src.max_bytes_;
      columns[i].nullable = src.nullable_;
    }
    FileIdentity id;
    id.device = identity_.at(0); id.inode = identity_.at(1); id.size = identity_.at(2);
    id.modified_seconds = identity_.at(3); id.modified_nanoseconds = identity_.at(4);
    id.changed_seconds = identity_.at(5); id.changed_nanoseconds = identity_.at(6);
    ret = sql_error(reader.bind_schema(columns, columns_.count(), id), reader.error());
    allocator.free(columns);
  }
  if (OB_FAIL(ret)) reader.close();
  return ret;
}

}}} // namespace oceanbase::sql::file_sql
