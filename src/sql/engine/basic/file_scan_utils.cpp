/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_scan_utils.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
bool equal_ci(std::string_view a, std::string_view b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char c = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + ('a' - 'A') : a[i];
    const char d = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + ('a' - 'A') : b[i];
    if (c != d) return false;
  }
  return true;
}
int decimal(std::string_view text, size_t offset, size_t count)
{
  int n = 0;
  if (offset + count > text.size()) return -1;
  for (size_t i = offset; i < offset + count; ++i) {
    if (text[i] < '0' || text[i] > '9') return -1;
    n = n * 10 + text[i] - '0';
  }
  return n;
}
bool valid_date(std::string_view t)
{
  if (t.size() < 10 || t[4] != '-' || t[7] != '-') return false;
  const int y = decimal(t, 0, 4), m = decimal(t, 5, 2), d = decimal(t, 8, 2);
  const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return y > 0 && m >= 1 && m <= 12 && d >= 1
      && d <= days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
#ifndef _WIN32
bool valid_utf8(std::string_view t)
{
  for (size_t i = 0; i < t.size();) {
    const unsigned char c = t[i++];
    if (c == 0) return false;
    if (c < 0x80) continue;
    unsigned n = 0, code = 0, minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) { n = 1; code = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; code = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; code = c & 7; minimum = 0x10000; }
    else return false;
    if (i + n > t.size()) return false;
    while (n--) {
      const unsigned char next = t[i++];
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}
bool same_file(const struct stat &a, const struct stat &b)
{
#ifdef __APPLE__
  const auto am = a.st_mtimespec, bm = b.st_mtimespec;
  const auto ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
  const auto am = a.st_mtim, bm = b.st_mtim;
  const auto ac = a.st_ctim, bc = b.st_ctim;
#endif
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
      && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec
      && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}
// Resolve no symlinks while opening any component, including the secure root.
// The directory descriptors pin the traversal against rename/symlink races.
int open_beneath(int base, const char *relative, bool directory)
{
  char path[4096];
  if (!relative || !*relative || std::strlen(relative) >= sizeof(path)) return -1;
  std::strcpy(path, relative);
  int fd = fcntl(base, F_DUPFD_CLOEXEC, 0);
  char *part = path;
  while (fd >= 0) {
    char *slash = std::strchr(part, '/');
    if (slash) *slash = '\0';
    if (!*part || std::strcmp(part, ".") == 0 || std::strcmp(part, "..") == 0) {
      ::close(fd); return -1;
    }
    int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    if (slash || directory) flags |= O_DIRECTORY;
    const int next = openat(fd, part, flags);
    ::close(fd); fd = next;
    if (!slash) break;
    part = slash + 1;
  }
  return fd;
}
#endif
}

Type infer_type(std::string_view text)
{
  if (equal_ci(text, "true") || equal_ci(text, "false")) return Type::Boolean;
  if (valid_date(text)) {
    if (text.size() == 10) return Type::Date;
    if (text.size() >= 19 && (text[10] == ' ' || text[10] == 'T') && text[13] == ':' && text[16] == ':'
        && decimal(text, 11, 2) >= 0 && decimal(text, 11, 2) <= 23
        && decimal(text, 14, 2) >= 0 && decimal(text, 14, 2) <= 59
        && decimal(text, 17, 2) >= 0 && decimal(text, 17, 2) <= 59) {
      if (text.size() == 19 || (text.size() >= 21 && text.size() <= 26 && text[19] == '.'
          && decimal(text, 20, text.size() - 20) >= 0)) return Type::DateTime;
    }
  }
  if (text.empty()) return Type::Varchar;
  auto number = text;
  if (number.front() == '+') number.remove_prefix(1);
  if (number.empty()) return Type::Varchar;
  bool integer = true;
  for (size_t i = number.front() == '-' ? 1 : 0; i < number.size(); ++i) {
    integer = integer && number[i] >= '0' && number[i] <= '9';
  }
  if (integer) {
    int64_t value = 0;
    auto result = std::from_chars(number.data(), number.data() + number.size(), value);
    return result.ec == std::errc() && result.ptr == number.data() + number.size() ? Type::BigInt : Type::Varchar;
  }
  double value = 0;
  auto result = std::from_chars(number.data(), number.data() + number.size(), value);
  return result.ec == std::errc() && result.ptr == number.data() + number.size() && std::isfinite(value)
      ? Type::Double : Type::Varchar;
}
Type widen(Type a, Type b)
{
  if (a == b || b == Type::Null) return a;
  if (a == Type::Null) return b;
  if ((a == Type::BigInt && b == Type::Double) || (a == Type::Double && b == Type::BigInt)) return Type::Double;
  if ((a == Type::Date && b == Type::DateTime) || (a == Type::DateTime && b == Type::Date)) return Type::DateTime;
  return Type::Varchar;
}
const char *type_name(Type type)
{
  switch (type) {
    case Type::Null: return "NULL";
    case Type::BigInt: return "BIGINT";
    case Type::Double: return "DOUBLE";
    case Type::Boolean: return "BOOLEAN";
    case Type::Date: return "DATE";
    case Type::DateTime: return "DATETIME";
    default: return "VARCHAR";
  }
}
#ifndef _WIN32
CsvReader::~CsvReader() { close(); }
void CsvReader::close()
{
  if (file_) std::fclose(file_);
  if (root_fd_ >= 0) ::close(root_fd_);
  std::free(buffer_);
  file_ = nullptr; root_fd_ = -1; buffer_ = nullptr; capacity_ = used_ = 0;
  field_count_ = column_count_ = 0; row_number_ = 0; line_number_ = 1;
  inferred_ = false; status_ = Status::Ok; error_[0] = path_[0] = relative_path_[0] = 0;
}
Status CsvReader::fail(Status status, const char *message)
{
  status_ = status;
  std::snprintf(error_, sizeof(error_), "%.240s: row %llu, line %llu: %s", path_,
      static_cast<unsigned long long>(row_number_), static_cast<unsigned long long>(line_number_), message);
  return status;
}
Status CsvReader::open(const char *path, const char *secure_root, const CsvOptions &options)
{
  close();
  if (!options.enabled) return fail(Status::Disabled, "enable_file_sql is disabled");
  if (!path || !secure_root || !*secure_root || equal_ci(secure_root, "null")) return fail(Status::Denied, "secure_file_priv denies access");
  if (options.sample_size != -1) return fail(Status::Unsupported, "only sample_size=-1 is supported");
  const auto bad_char = [](char c) { return c <= 0 || c == '\r' || c == '\n' || static_cast<unsigned char>(c) >= 128; };
  if (bad_char(options.delimiter) || bad_char(options.quote) || bad_char(options.escape)
      || options.delimiter == options.quote || options.delimiter == options.escape
      || options.max_record_bytes == 0 || options.max_record_bytes > 16 * 1024 * 1024
      || options.null_text.size() >= sizeof(null_text_)) return fail(Status::InvalidOption, "invalid CSV options");
  char root[4096], canonical[4096];
  if (std::strlen(path) >= sizeof(path_) || std::strlen(secure_root) >= sizeof(root)) return fail(Status::ResourceLimit, "path too long");
  std::strcpy(path_, path);
  if (!realpath(secure_root, root) || !realpath(path, canonical)) return fail(Status::Denied, "cannot resolve secure path");
  const size_t len = std::strlen(root);
  if (std::strncmp(root, canonical, len) != 0 || (len > 1 && canonical[len] != '/')) return fail(Status::Denied, "path is outside secure_file_priv");
  const char *relative = canonical + len + (len > 1 ? 1 : 0);
  if (!*relative) return fail(Status::Denied, "input must be a regular file");
  std::strcpy(relative_path_, relative);
  const int system_root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (system_root < 0) return fail(Status::Io, "cannot open filesystem root");
  root_fd_ = len == 1 ? fcntl(system_root, F_DUPFD_CLOEXEC, 0) : open_beneath(system_root, root + 1, true);
  ::close(system_root);
  if (root_fd_ < 0) return fail(Status::Denied, "secure directory changed during open");
  const int fd = open_beneath(root_fd_, relative_path_, false);
  if (fd < 0) return fail(Status::Denied, "input path changed during open");
  if (fstat(fd, &snapshot_) != 0 || !S_ISREG(snapshot_.st_mode)) {
    ::close(fd); return fail(Status::Denied, "input must be a regular file");
  }
  file_ = fdopen(fd, "rb");
  if (!file_) { ::close(fd); return fail(Status::Io, "cannot open input stream"); }
  options_ = options;
  if (!options.null_text.empty()) std::memcpy(null_text_, options.null_text.data(), options.null_text.size());
  null_text_[options.null_text.size()] = 0;
  options_.null_text = std::string_view(null_text_, options.null_text.size());
  return rewind_file();
}
FileIdentity CsvReader::identity() const
{
#ifdef __APPLE__
  const auto modified = snapshot_.st_mtimespec, changed = snapshot_.st_ctimespec;
#else
  const auto modified = snapshot_.st_mtim, changed = snapshot_.st_ctim;
#endif
  return {static_cast<uint64_t>(snapshot_.st_dev), static_cast<uint64_t>(snapshot_.st_ino), snapshot_.st_size,
      modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec};
}
Status CsvReader::bind_schema(const Column *columns, size_t count, const FileIdentity &expected)
{
  if (check_unchanged() != Status::Ok) return status_;
  if (!(identity() == expected)) return fail(Status::Changed, "input changed since schema resolution");
  if (!columns || count == 0 || count > MAX_COLUMNS) return fail(Status::InvalidOption, "invalid planned schema");
  for (size_t i = 0; i < count; ++i) {
    if (!std::memchr(columns[i].name, 0, sizeof(columns[i].name)) || !columns[i].name[0]
        || columns[i].type < Type::Null || columns[i].type > Type::DateTime
        || columns[i].max_bytes > options_.max_record_bytes) return fail(Status::InvalidOption, "invalid planned column");
  }
  for (size_t i = 0; i < count; ++i) columns_[i] = columns[i];
  column_count_ = count;
  inferred_ = true;
  const Status result = rescan();
  if (result == Status::Ok && options_.header && field_count_ != count) {
    return fail(Status::Malformed, "CSV header differs from planned schema");
  }
  return result;
}
Status CsvReader::check_unchanged()
{
  if (status_ != Status::Ok) return status_;
  if (!file_) return fail(Status::InvalidOption, "reader is not open");
  struct stat fd_stat, path_stat;
  if (fstat(fileno(file_), &fd_stat) != 0 || fstatat(root_fd_, relative_path_, &path_stat, AT_SYMLINK_NOFOLLOW) != 0
      || !same_file(snapshot_, fd_stat) || !same_file(snapshot_, path_stat)) return fail(Status::Changed, "input file changed");
  return Status::Ok;
}
Status CsvReader::rewind_file()
{
  if (check_unchanged() != Status::Ok) return status_;
  if (fseeko(file_, 0, SEEK_SET) != 0) return fail(Status::Io, "cannot rewind input");
  row_number_ = 0; line_number_ = 1; used_ = field_count_ = 0;
  unsigned char bom[3];
  const size_t n = std::fread(bom, 1, sizeof(bom), file_);
  if (std::ferror(file_)) return fail(Status::Io, "input read failed");
  if (n != 3 || bom[0] != 0xef || bom[1] != 0xbb || bom[2] != 0xbf) {
    if (fseeko(file_, 0, SEEK_SET) != 0) return fail(Status::Io, "cannot rewind input");
  }
  return Status::Ok;
}
int CsvReader::get()
{
  const int c = std::fgetc(file_);
  if (c == '\n') ++line_number_;
  return c;
}
bool CsvReader::append(char c)
{
  if (used_ >= options_.max_record_bytes) { fail(Status::ResourceLimit, "CSV record exceeds byte limit"); return false; }
  if (used_ == capacity_) {
    const size_t size = std::min(options_.max_record_bytes, std::max<size_t>(4096, capacity_ * 2));
    char *grown = static_cast<char *>(std::realloc(buffer_, size));
    if (!grown) { fail(Status::ResourceLimit, "cannot allocate row workspace"); return false; }
    buffer_ = grown; capacity_ = size;
  }
  buffer_[used_++] = c;
  return true;
}
bool CsvReader::finish_cell(size_t offset, bool quoted)
{
  if (field_count_ >= MAX_COLUMNS) { fail(Status::ResourceLimit, "too many CSV columns"); return false; }
  const std::string_view text(buffer_ ? buffer_ + offset : "", used_ - offset);
  if (!valid_utf8(text)) { fail(Status::Malformed, "invalid UTF-8 or NUL in CSV field"); return false; }
  cells_[field_count_++] = {offset, text.size(), !quoted && ((options_.empty_is_null && text.empty()) || text == options_.null_text)};
  return true;
}
Status CsvReader::read_record()
{
  if (check_unchanged() != Status::Ok) return status_;
  used_ = field_count_ = 0;
  int c = get();
  if (c == EOF) return std::ferror(file_) ? fail(Status::Io, "input read failed") : Status::End;
  ++row_number_;
  while (true) {
    const size_t start = used_;
    const bool quoted = c == options_.quote;
    if (quoted) {
      while (true) {
        c = get();
        if (c == EOF) return fail(std::ferror(file_) ? Status::Io : Status::Malformed, "unterminated quoted CSV field");
        if (c == options_.quote) {
          c = get();
          if (options_.escape == options_.quote && c == options_.quote) {
            if (!append(static_cast<char>(c))) return status_;
          } else break;
        } else if (options_.escape != options_.quote && c == options_.escape) {
          c = get();
          if (c != options_.quote && c != options_.escape) return fail(Status::Malformed, "invalid CSV escape");
          if (!append(static_cast<char>(c))) return status_;
        } else if (!append(static_cast<char>(c))) return status_;
      }
      if (c != EOF && c != options_.delimiter && c != '\r' && c != '\n') return fail(Status::Malformed, "characters after closing quote");
    } else {
      while (c != EOF && c != options_.delimiter && c != '\r' && c != '\n') {
        if (c == options_.quote) return fail(Status::Malformed, "quote in unquoted CSV field");
        if (!append(static_cast<char>(c))) return status_;
        c = get();
      }
    }
    if (!finish_cell(start, quoted)) return status_;
    if (c != options_.delimiter) {
      if (c == '\r') {
        c = get();
        if (c != '\n') return fail(Status::Malformed, "bare CR record separator");
      }
      if (std::ferror(file_)) return fail(Status::Io, "input read failed");
      return check_unchanged();
    }
    c = get();
  }
}
std::string_view CsvReader::value(size_t index) const
{
  if (index >= field_count_) return {};
  return {buffer_ ? buffer_ + cells_[index].offset : "", cells_[index].length};
}
Status CsvReader::infer()
{
  if (rewind_file() != Status::Ok) return status_;
  Status s = read_record();
  if (s == Status::End) return fail(Status::Malformed, "empty CSV has no column schema");
  if (s != Status::Ok) return s;
  column_count_ = field_count_;
  for (size_t i = 0; i < column_count_; ++i) {
    columns_[i] = Column{};
    char base[MAX_NAME_BYTES + 1];
    const auto name = value(i);
    if (options_.header && !name.empty()) {
      if (name.size() > MAX_NAME_BYTES) return fail(Status::ResourceLimit, "CSV column name too long");
      std::memcpy(base, name.data(), name.size()); base[name.size()] = 0;
    } else std::snprintf(base, sizeof(base), "column%zu", i);
    std::strcpy(columns_[i].name, base);
    size_t suffix = 2;
    for (size_t j = 0; j < i;) {
      if (equal_ci(columns_[i].name, columns_[j].name)) {
        const int n = std::snprintf(columns_[i].name, sizeof(columns_[i].name), "%s__%zu", base, suffix++);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(columns_[i].name)) return fail(Status::ResourceLimit, "deduplicated column name too long");
        j = 0;
      } else ++j;
    }
  }
  if (options_.header) s = read_record();
  uint64_t rows = 0;
  while (s == Status::Ok) {
    if (field_count_ != column_count_) return fail(Status::Malformed, "CSV field count differs from schema");
    ++rows;
    for (size_t i = 0; i < column_count_; ++i) {
      columns_[i].nullable |= cells_[i].is_null;
      columns_[i].max_bytes = std::max(columns_[i].max_bytes, cells_[i].length);
      if (!cells_[i].is_null) columns_[i].type = widen(columns_[i].type, infer_type(value(i)));
    }
    s = read_record();
  }
  if (s != Status::End) return s;
  if (rows == 0) for (size_t i = 0; i < column_count_; ++i) columns_[i].nullable = true;
  inferred_ = true;
  return rescan();
}
Status CsvReader::rescan()
{
  if (rewind_file() != Status::Ok) return status_;
  if (!inferred_) return fail(Status::InvalidOption, "infer schema before rescan");
  return options_.header ? read_record() : Status::Ok;
}
Status CsvReader::next()
{
  if (status_ != Status::Ok) return status_;
  if (!inferred_) return fail(Status::InvalidOption, "infer schema before scanning");
  Status s = read_record();
  if (s == Status::Ok && field_count_ != column_count_) s = fail(Status::Malformed, "CSV field count differs from schema");
  return s;
}
#else
// Fail closed on platforms without descriptor-relative no-follow traversal.
// Normal SQL must continue to build and run when file SQL is unavailable.
CsvReader::~CsvReader() = default;
Status CsvReader::open(const char *, const char *, const CsvOptions &options)
{
  std::snprintf(error_, sizeof(error_), "secure local file scanning is unavailable on this platform");
  return options.enabled ? Status::Unsupported : Status::Disabled;
}
Status CsvReader::infer() { return Status::Unsupported; }
FileIdentity CsvReader::identity() const { return {}; }
Status CsvReader::bind_schema(const Column *, size_t, const FileIdentity &) { return Status::Unsupported; }
Status CsvReader::next() { return Status::Unsupported; }
Status CsvReader::rescan() { return Status::Unsupported; }
void CsvReader::close() {}
std::string_view CsvReader::value(size_t) const { return {}; }
#endif
}}} // namespace oceanbase::sql::file_sql
