/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/file_sql_scan.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include <cstring>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql {
using namespace common;
OB_SERIALIZE_MEMBER(FileColumn, name_, type_, max_bytes_, nullable_);
OB_SERIALIZE_MEMBER(FileScanDefinition, path_, null_text_, header_, empty_is_null_,
                    delimiter_, quote_, escape_, format_, identity_, columns_);

namespace {
// Data-shape failures are reported to the client with one stable wording shared
// by every format ("Incorrect arguments to file SQL"); the reader's precise
// reason stays in the server log so a rejection is still diagnosable.
int sql_reader_error(Status status, const char *detail)
{
  int ret = OB_SUCCESS;
  if (status == Status::Malformed || status == Status::InvalidOption
      || status == Status::ResourceLimit || status == Status::Io || status == Status::Changed) {
    LOG_WARN("file scan rejected its input", "status", static_cast<int>(status),
             "detail", detail == nullptr ? "" : detail);
    ret = sql_error(status, "file SQL");
  } else {
    ret = sql_error(status, detail);
  }
  return ret;
}

// Every format resolves its schema the same way: open once through the shared
// secure path, scan for the column set, and remember the file identity so
// execution can reject bytes that no longer match the planned schema.
template <typename Reader, typename Options>
int infer_definition(ObIAllocator &allocator, const ObString &path, const char *secure_root,
                     const Options &options, ObString &path_out,
                     ObSEArray<int64_t, 7> &identity, ObSEArray<FileColumn, 16> &columns)
{
  int ret = OB_SUCCESS;
  Reader *reader = nullptr;
  void *storage = nullptr;
  path_out.reset();
  identity.reset();
  columns.reset();
  if (path.empty() || path.length() >= 4096 || std::memchr(path.ptr(), 0, path.length()) != nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ob_write_string(allocator, path, path_out, true))) {
  } else if (OB_ISNULL(storage = allocator.alloc(sizeof(Reader)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    reader = new (storage) Reader();
    if (OB_FAIL(sql_reader_error(reader->open(path_out.ptr(), secure_root, options), reader->error()))) {
    } else if (OB_FAIL(sql_reader_error(reader->infer(), reader->error()))) {
    } else {
      const FileIdentity id = reader->identity();
      const int64_t values[] = {static_cast<int64_t>(id.device), static_cast<int64_t>(id.inode), id.size,
                               id.modified_seconds, id.modified_nanoseconds, id.changed_seconds, id.changed_nanoseconds};
      for (int64_t v : values) { if (OB_SUCC(ret)) { ret = identity.push_back(v); } }
      for (size_t i = 0; OB_SUCC(ret) && i < reader->column_count(); ++i) {
        const typename Reader::Column &source = reader->column(i);
        FileColumn col;
        col.type_ = static_cast<int64_t>(source.type);
        col.max_bytes_ = source.max_bytes;
        col.nullable_ = source.nullable;
        if (OB_FAIL(ob_write_string(allocator, ObString::make_string(source.name), col.name_))) {
        } else if (OB_FAIL(columns.push_back(col))) {
        }
      }
    }
    reader->~Reader();
    allocator.free(storage);
  }
  return ret;
}

// Reopens the pinned file and hands the planned schema to the reader, which
// validates it against the file it actually opened.
template <typename Reader, typename Options>
int open_planned(ObIAllocator &allocator, Reader &reader, const ObString &path,
                 const ObSEArray<int64_t, 7> &identity, const ObSEArray<FileColumn, 16> &columns,
                 const char *secure_root, const Options &options)
{
  int ret = OB_SUCCESS;
  ObString path_copy;
  typename Reader::Column *planned = nullptr;
  if (identity.count() != 7 || columns.empty() || columns.count() > static_cast<int64_t>(Reader::MAX_COLUMNS)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ob_write_string(allocator, path, path_copy, true))) {
  } else if (OB_FAIL(sql_reader_error(reader.open(path_copy.ptr(), secure_root, options), reader.error()))) {
  } else if (OB_ISNULL(planned = static_cast<typename Reader::Column *>(
                 allocator.alloc(sizeof(typename Reader::Column) * columns.count())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; i < columns.count(); ++i) {
      new (planned + i) typename Reader::Column();
      const FileColumn &src = columns.at(i);
      std::memcpy(planned[i].name, src.name_.ptr(), src.name_.length());
      planned[i].type = static_cast<Type>(src.type_);
      planned[i].max_bytes = src.max_bytes_;
      planned[i].nullable = src.nullable_;
    }
    FileIdentity id;
    id.device = static_cast<uint64_t>(identity.at(0));
    id.inode = static_cast<uint64_t>(identity.at(1));
    id.size = identity.at(2);
    id.modified_seconds = identity.at(3);
    id.modified_nanoseconds = identity.at(4);
    id.changed_seconds = identity.at(5);
    id.changed_nanoseconds = identity.at(6);
    ret = sql_reader_error(reader.bind_schema(planned, columns.count(), id), reader.error());
    allocator.free(planned);
  }
  if (OB_FAIL(ret)) {
    reader.close();
  }
  return ret;
}
} // namespace

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
      ret = OB_ERR_NO_PRIVILEGE; LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "secure_file_priv"); break;
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
  format_ = static_cast<int64_t>(FileFormat::Csv);
  if (OB_FAIL((infer_definition<CsvReader, CsvOptions>(allocator, path, secure_root, options,
                                                       path_, identity_, columns_)))) {
  } else if (OB_FAIL(ob_write_string(allocator, ObString(options.null_text.size(), options.null_text.data()), null_text_))) {
  } else {
    header_ = options.header;
    empty_is_null_ = options.empty_is_null;
    delimiter_ = options.delimiter;
    quote_ = options.quote;
    escape_ = options.escape;
  }
  return ret;
}

int FileScanDefinition::infer_json(ObIAllocator &allocator, const ObString &path,
                                   const char *secure_root, const JsonOptions &options)
{
  int ret = OB_SUCCESS;
  format_ = static_cast<int64_t>(FileFormat::Json);
  return infer_definition<JsonlReader, JsonOptions>(allocator, path, secure_root, options,
                                                    path_, identity_, columns_);
}

int FileScanDefinition::infer_parquet(ObIAllocator &allocator, const ObString &path,
                                      const char *secure_root, const ParquetOptions &options)
{
  int ret = OB_SUCCESS;
  format_ = static_cast<int64_t>(FileFormat::Parquet);
  return infer_definition<ParquetReader, ParquetOptions>(allocator, path, secure_root, options,
                                                         path_, identity_, columns_);
}

int FileScanDefinition::infer_glob(ObIAllocator &allocator, const ObString &path,
                                  const char *secure_root)
{
  int ret = OB_SUCCESS;
  ObString terminated_path;
  GlobResult result;
  GlobOptions options;
  format_ = static_cast<int64_t>(FileFormat::Glob);
  if (path.empty() || path.length() >= 4096 || std::memchr(path.ptr(), 0, path.length()) != nullptr) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ob_write_string(allocator, path, terminated_path, true))) {
  } else {
    const Status status = glob_files(terminated_path.ptr(), secure_root, options, result);
    ret = sql_error(status, result.error());
    OZ (ob_write_string(allocator, path, path_));
  }
  if (OB_SUCC(ret)) {
    identity_.reset();
    columns_.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < 7; ++i) {
      OZ (identity_.push_back(0));
    }
    FileColumn column;
    column.name_ = ObString::make_string("file");
    column.type_ = static_cast<int64_t>(Type::Varchar);
    column.max_bytes_ = 4096;
    column.nullable_ = false;
    OZ (columns_.push_back(column));
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
             || format_ < static_cast<int64_t>(FileFormat::Csv)
             || format_ > static_cast<int64_t>(FileFormat::Glob)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (static_cast<FileFormat>(format_) == FileFormat::Csv
             && (delimiter_ <= 0 || delimiter_ > 127 || quote_ <= 0 || quote_ > 127
                 || escape_ <= 0 || escape_ > 127)) {
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
  CsvOptions options;
  options.enabled = true;
  options.header = header_;
  options.empty_is_null = empty_is_null_;
  options.delimiter = delimiter_;
  options.quote = quote_;
  options.escape = escape_;
  options.null_text = std::string_view(null_text_.ptr(), null_text_.length());
  return open_planned<CsvReader, CsvOptions>(allocator, reader, path_, identity_, columns_, secure_root, options);
}

int FileScanDefinition::open_json(ObIAllocator &allocator, JsonlReader &reader, const char *secure_root) const
{
  JsonOptions options;
  options.enabled = true;
  return open_planned<JsonlReader, JsonOptions>(allocator, reader, path_, identity_, columns_, secure_root, options);
}

int FileScanDefinition::open_parquet(ObIAllocator &allocator, ParquetReader &reader,
                                     const char *secure_root) const
{
  ParquetOptions options;
  options.enabled = true;
  return open_planned<ParquetReader, ParquetOptions>(allocator, reader, path_, identity_, columns_,
                                                     secure_root, options);
}

FileScanReader::~FileScanReader() { close(); }

void FileScanReader::close()
{
  // The reader objects themselves live in the caller's arena allocator; only the
  // resources they own (streams, descriptors, buffers) need releasing here.
  if (csv_ != nullptr) { csv_->~CsvReader(); csv_ = nullptr; }
  if (json_ != nullptr) { json_->~JsonlReader(); json_ = nullptr; }
  if (parquet_ != nullptr) { parquet_->~ParquetReader(); parquet_ = nullptr; }
  if (glob_ != nullptr) { glob_->~GlobResult(); glob_ = nullptr; }
  glob_position_ = 0;
  format_ = FileFormat::Csv;
}

int FileScanReader::open(const FileScanDefinition &definition, ObIAllocator &allocator,
                         const char *secure_root)
{
  int ret = OB_SUCCESS;
  void *storage = nullptr;
  close();
  format_ = static_cast<FileFormat>(definition.format_);
  switch (format_) {
    case FileFormat::Csv:
      if (OB_ISNULL(storage = allocator.alloc(sizeof(CsvReader)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        csv_ = new (storage) CsvReader();
        ret = definition.open_csv(allocator, *csv_, secure_root);
      }
      break;
    case FileFormat::Json:
      if (OB_ISNULL(storage = allocator.alloc(sizeof(JsonlReader)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        json_ = new (storage) JsonlReader();
        ret = definition.open_json(allocator, *json_, secure_root);
      }
      break;
    case FileFormat::Parquet:
      if (OB_ISNULL(storage = allocator.alloc(sizeof(ParquetReader)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        parquet_ = new (storage) ParquetReader();
        ret = definition.open_parquet(allocator, *parquet_, secure_root);
      }
      break;
    case FileFormat::Glob: {
      ObString terminated_path;
      GlobOptions options;
      if (OB_ISNULL(storage = allocator.alloc(sizeof(GlobResult)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        glob_ = new (storage) GlobResult();
        if (OB_SUCC(ob_write_string(allocator, definition.path_, terminated_path, true))) {
          const Status status = glob_files(terminated_path.ptr(), secure_root, options, *glob_);
          ret = sql_error(status, glob_->error());
        } else {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        }
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
      break;
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

Status FileScanReader::next()
{
  if (csv_ != nullptr) return csv_->next();
  if (json_ != nullptr) return json_->next();
  if (parquet_ != nullptr) return parquet_->next();
  if (glob_ != nullptr) {
    if (glob_position_ >= glob_->count()) return Status::End;
    ++glob_position_;
    return Status::Ok;
  }
  return Status::InvalidOption;
}

Status FileScanReader::rescan()
{
  if (csv_ != nullptr) return csv_->rescan();
  if (json_ != nullptr) return json_->rescan();
  if (parquet_ != nullptr) return parquet_->rescan();
  if (glob_ != nullptr) { glob_position_ = 0; return Status::Ok; }
  return Status::InvalidOption;
}

int64_t FileScanReader::column_count() const
{
  if (csv_ != nullptr) return static_cast<int64_t>(csv_->column_count());
  if (json_ != nullptr) return static_cast<int64_t>(json_->column_count());
  if (parquet_ != nullptr) return static_cast<int64_t>(parquet_->column_count());
  if (glob_ != nullptr) return 1;
  return 0;
}

bool FileScanReader::is_null(int64_t column) const
{
  if (csv_ != nullptr) return csv_->is_null(static_cast<size_t>(column));
  if (json_ != nullptr) return json_->is_null(static_cast<size_t>(column));
  if (parquet_ != nullptr) return parquet_->is_null(static_cast<size_t>(column));
  if (glob_ != nullptr) return column != 0 || glob_position_ == 0;
  return true;
}

std::string_view FileScanReader::value(int64_t column) const
{
  if (csv_ != nullptr) return csv_->value(static_cast<size_t>(column));
  if (json_ != nullptr) return json_->value(static_cast<size_t>(column));
  if (parquet_ != nullptr) return parquet_->value(static_cast<size_t>(column));
  if (glob_ != nullptr && column == 0 && glob_position_ > 0) return glob_->file_view(glob_position_ - 1);
  return {};
}

Type FileScanReader::column_type(int64_t column) const
{
  if (csv_ != nullptr) return csv_->column(static_cast<size_t>(column)).type;
  if (json_ != nullptr) return json_->column(static_cast<size_t>(column)).type;
  if (parquet_ != nullptr) return parquet_->column(static_cast<size_t>(column)).type;
  if (glob_ != nullptr && column == 0) return Type::Varchar;
  return Type::Null;
}

const char *FileScanReader::error() const
{
  if (csv_ != nullptr) return csv_->error();
  if (json_ != nullptr) return json_->error();
  if (parquet_ != nullptr) return parquet_->error();
  if (glob_ != nullptr) return glob_->error();
  return "file scan reader is not open";
}

void FileScanReader::set_projection(const int64_t *columns, int64_t count)
{
  // A record-oriented format has to read every field of a record anyway, so only
  // the columnar format can turn a projection into work it does not do. Rejecting
  // is the reader's job: an unknown or repeated index there is an invalid plan.
  // An empty projection is not "no columns": it means the plan did not prune
  // anything, and the reader keeps reading every column.
  if (parquet_ == nullptr || columns == nullptr || count <= 0) {
    return;
  }
  std::vector<size_t> indexes;
  indexes.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    indexes.push_back(static_cast<size_t>(columns[i]));
  }
  parquet_->set_projection(indexes.data(), indexes.size());
}

}}} // namespace oceanbase::sql::file_sql
