/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_sql_export.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

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

// Unique temp names per writer, so concurrent exports never collide.
std::atomic<uint64_t> temp_counter{0};

bool valid_option_char(char value)
{
  const unsigned char byte = static_cast<unsigned char>(value);
  return byte >= 1 && byte <= 127 && value != '\r' && value != '\n';
}

// delimiter/quote/escape are printable distinct separators. quote may equal
// escape, which is the standard RFC 4180 doubling form and the default here.
bool valid_options(const CsvWriteOptions &options)
{
  return valid_option_char(options.delimiter) && valid_option_char(options.quote)
      && valid_option_char(options.escape) && options.delimiter != options.quote
      && options.delimiter != options.escape && options.null_text.size() < 1024
      && !options.line_terminator.empty() && options.line_terminator.size() < 8;
}

// Reject empty, "." and ".." components before any filesystem access, so the
// descriptor-relative traversal below can never be steered upwards.
bool valid_relative_path(const char *relative)
{
  const char *part = relative;
  while (*part) {
    const char *slash = std::strchr(part, '/');
    const size_t length = slash ? static_cast<size_t>(slash - part) : std::strlen(part);
    if (length == 0 || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.')) {
      return false;
    }
    if (!slash) break;
    part = slash + 1;
  }
  return true;
}

// Resolve no symlinks while opening any component, including the secure root.
// The directory descriptors pin the traversal against rename/symlink races.
// Mirrors open_beneath() in file_scan_utils.cpp: that file is owned by the
// scanner, so the export writer keeps its own copy of the identical traversal.
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

} // namespace

CsvWriter::CsvWriter() = default;

CsvWriter::~CsvWriter()
{
  // Never publish a half-written export: the target is renamed into place by
  // commit() only, so anything still open here is discarded.
  abort();
}

void CsvWriter::reset()
{
  abort();
  write_failed_ = false;
  committed_ = false;
  header_written_ = false;
  row_columns_ = 0;
  row_count_ = 0;
  used_ = 0;
  options_ = CsvWriteOptions();
  target_path_[0] = root_relative_[0] = relative_dir_[0] = base_name_[0] = temp_name_[0] = '\0';
  null_text_[0] = line_terminator_[0] = error_[0] = '\0';
}

void CsvWriter::close_directory_fds()
{
  if (dir_fd_ >= 0) { ::close(dir_fd_); dir_fd_ = -1; }
  if (root_fd_ >= 0) { ::close(root_fd_); root_fd_ = -1; }
}

void CsvWriter::abort()
{
  if (temp_fd_ >= 0) { ::close(temp_fd_); temp_fd_ = -1; }
  // Only a name this writer created is removed. The target name itself is never
  // unlinked here: another actor may have created it after open().
  if (temp_created_ && dir_fd_ >= 0) { ::unlinkat(dir_fd_, temp_name_, 0); }
  temp_created_ = false;
  close_directory_fds();
  used_ = 0;
}

Status CsvWriter::fail(Status status, const char *message)
{
  // Only an I/O failure poisons the stream; usage errors stay recoverable so a
  // caller can correct the row and keep appending.
  write_failed_ = status == Status::Io;
  const int code = errno;
  if (status == Status::Io && code != 0) {
    std::snprintf(error_, sizeof(error_), "%.200s: %.240s: %s",
        target_path_[0] ? target_path_ : "(no target)", message, std::strerror(code));
  } else {
    std::snprintf(error_, sizeof(error_), "%.200s: %.280s",
        target_path_[0] ? target_path_ : "(no target)", message);
  }
  return status;
}

Status CsvWriter::open(const char *path, const char *secure_root, const CsvWriteOptions &options)
{
  reset();
  if (!options.enabled) return fail(Status::Disabled, "enable_file_sql is disabled");
  if (!secure_root || !*secure_root || equal_ci(secure_root, "null")) {
    return fail(Status::Denied, "secure_file_priv is not configured");
  }
  if (!valid_options(options)) return fail(Status::InvalidOption, "invalid CSV export options");
  if (!path || !*path) return fail(Status::Denied, "export path is empty");
  const size_t path_length = std::strlen(path);
  if (path_length >= PATH_BYTES) return fail(Status::Denied, "export path is too long");
  char root[PATH_BYTES];
  if (!realpath(secure_root, root)) return fail(Status::Denied, "cannot resolve secure_file_priv");
  // The caller-facing form of the root keeps its own spelling so that lexical
  // prefix checks match the path handed in; only the traversal uses the
  // canonical form, always with O_NOFOLLOW from the root descriptor.
  char given_root[PATH_BYTES];
  std::snprintf(given_root, sizeof(given_root), "%s", secure_root);
  size_t given_length = std::strlen(given_root);
  while (given_length > 1 && given_root[given_length - 1] == '/') given_root[--given_length] = '\0';
  const char *relative = path;
  if (path[0] == '/') {
    if (given_length == 1) {
      relative = path + 1;
    } else {
      if (path_length <= given_length || std::strncmp(path, given_root, given_length) != 0
          || path[given_length] != '/') {
        return fail(Status::Denied, "export path is outside secure_file_priv");
      }
      relative = path + given_length + 1;
    }
  }
  if (!*relative) return fail(Status::Denied, "export path must name a file");
  if (!valid_relative_path(relative)) return fail(Status::Denied, "export path has an invalid component");
  const size_t root_length = std::strlen(root);
  if (root_length + 1 + std::strlen(relative) >= sizeof(target_path_)) {
    return fail(Status::Denied, "export path is too long");
  }
  std::snprintf(target_path_, sizeof(target_path_), "%s/%s", root_length == 1 ? "" : root, relative);

  char relative_copy[PATH_BYTES];
  std::snprintf(relative_copy, sizeof(relative_copy), "%s", relative);
  char *base = std::strrchr(relative_copy, '/');
  const char *directory = "";
  if (base) {
    *base = '\0';
    directory = relative_copy;
    ++base;
  } else {
    base = relative_copy;
  }
  if (std::strlen(base) >= NAME_BYTES) return fail(Status::Denied, "export target name is too long");
  std::snprintf(base_name_, sizeof(base_name_), "%s", base);
  std::snprintf(relative_dir_, sizeof(relative_dir_), "%s", directory);
  std::snprintf(root_relative_, sizeof(root_relative_), "%s", root_length == 1 ? "" : root + 1);

  const int system_root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (system_root < 0) return fail(Status::Io, "cannot open filesystem root");
  root_fd_ = root_length == 1 ? fcntl(system_root, F_DUPFD_CLOEXEC, 0)
                              : open_beneath(system_root, root + 1, true);
  ::close(system_root);
  if (root_fd_ < 0) {
    abort();
    return fail(Status::Denied, "secure directory changed during open");
  }
  dir_fd_ = *directory ? open_beneath(root_fd_, directory, true) : fcntl(root_fd_, F_DUPFD_CLOEXEC, 0);
  if (dir_fd_ < 0) {
    abort();
    return fail(Status::Denied, "export directory changed during open");
  }

  Status status = check_target_available();
  if (status == Status::Ok) status = create_temp_file();
  if (status != Status::Ok) {
    abort();
    return status;
  }
  options_ = options;
  if (!options.null_text.empty()) {
    std::memcpy(null_text_, options.null_text.data(), options.null_text.size());
  }
  null_text_[options.null_text.size()] = '\0';
  options_.null_text = std::string_view(null_text_, options.null_text.size());
  std::memcpy(line_terminator_, options.line_terminator.data(), options.line_terminator.size());
  line_terminator_[options.line_terminator.size()] = '\0';
  options_.line_terminator = std::string_view(line_terminator_, options.line_terminator.size());
  return Status::Ok;
}

Status CsvWriter::check_target_available()
{
  // Classify an existing entry early so a symlink reads as a denial and a
  // regular file or directory as a fail-if-exists conflict. This is advisory
  // only: no name is claimed here, because reserving the target by creating an
  // empty placeholder opens a window in which another process can replace that
  // placeholder, and a later renameat() would then destroy its data. commit()
  // enforces the same rule atomically with linkat(2), which never replaces.
  struct stat existing = {};
  if (fstatat(dir_fd_, base_name_, &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    if (S_ISLNK(existing.st_mode)) return fail(Status::Denied, "export target is a symbolic link");
    return fail(Status::Changed, "export target already exists");
  }
  if (errno != ENOENT) return fail(Status::Io, "cannot inspect export target");
  return Status::Ok;
}

Status CsvWriter::verify_directory_unchanged()
{
  // The target directory is pinned by descriptor, so a directory renamed out of
  // secure_file_priv after open() would otherwise still receive the export. The
  // directory is re-resolved from the configured root and compared by identity
  // before publication. A short window remains between this check and linkat(2)
  // because POSIX offers no move-proof handle; the check turns a silent escape
  // into a reported failure.
  struct stat pinned = {};
  if (fstat(dir_fd_, &pinned) != 0) return fail(Status::Changed, "export directory changed during export");
  const int system_root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (system_root < 0) return fail(Status::Io, "cannot open filesystem root");
  const int fresh_root = *root_relative_ ? open_beneath(system_root, root_relative_, true)
                                         : fcntl(system_root, F_DUPFD_CLOEXEC, 0);
  ::close(system_root);
  if (fresh_root < 0) return fail(Status::Changed, "export directory changed during export");
  const int fresh_dir = *relative_dir_ ? open_beneath(fresh_root, relative_dir_, true)
                                       : fcntl(fresh_root, F_DUPFD_CLOEXEC, 0);
  ::close(fresh_root);
  if (fresh_dir < 0) return fail(Status::Changed, "export directory changed during export");
  struct stat fresh = {};
  const int stat_rc = fstat(fresh_dir, &fresh);
  ::close(fresh_dir);
  if (stat_rc != 0 || pinned.st_dev != fresh.st_dev || pinned.st_ino != fresh.st_ino) {
    return fail(Status::Changed, "export directory changed during export");
  }
  return Status::Ok;
}

Status CsvWriter::create_temp_file()
{
  char suffix[64];
  for (int attempt = 0; attempt < 16; ++attempt) {
    const uint64_t counter = temp_counter.fetch_add(1) + 1;
    const int suffix_length = std::snprintf(suffix, sizeof(suffix), ".seekdb-tmp.%ld.%llu",
        static_cast<long>(::getpid()), static_cast<unsigned long long>(counter));
    if (suffix_length <= 0 || static_cast<size_t>(suffix_length) >= sizeof(suffix)) {
      return fail(Status::Io, "cannot build export temp name");
    }
    const size_t available = sizeof(temp_name_) - 2 - static_cast<size_t>(suffix_length);
    const size_t keep = std::min(std::strlen(base_name_), available);
    const int length = std::snprintf(temp_name_, sizeof(temp_name_), ".%.*s%s",
        static_cast<int>(keep), base_name_, suffix);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(temp_name_)) {
      return fail(Status::Io, "export temp name is too long");
    }
    const int fd = ::openat(dir_fd_, temp_name_, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      temp_fd_ = fd;
      temp_created_ = true;
      return Status::Ok;
    }
    if (errno != EEXIST) return fail(Status::Io, "cannot create export temp file");
  }
  return fail(Status::Io, "cannot create export temp file");
}

Status CsvWriter::flush_buffer()
{
  size_t written = 0;
  while (written < used_) {
    const ssize_t n = ::write(temp_fd_, buffer_ + written, used_ - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      return fail(Status::Io, "cannot write export data");
    }
  }
  used_ = 0;
  return Status::Ok;
}

Status CsvWriter::emit(const char *data, size_t size)
{
  while (size > 0) {
    if (used_ == sizeof(buffer_)) {
      const Status status = flush_buffer();
      if (status != Status::Ok) return status;
    }
    const size_t take = std::min(size, sizeof(buffer_) - used_);
    std::memcpy(buffer_ + used_, data, take);
    used_ += take;
    data += take;
    size -= take;
  }
  return Status::Ok;
}

Status CsvWriter::emit(char value)
{
  return emit(&value, 1);
}

// Non-NULL empty strings are quoted so they stay distinguishable from NULL,
// whose null_text is written raw. Every occurrence of quote and escape inside a
// quoted field is prefixed with the escape character, which reduces to the
// standard doubling when escape == quote.
Status CsvWriter::emit_field(std::string_view value, bool is_null)
{
  if (is_null) return emit(options_.null_text.data(), options_.null_text.size());
  bool needs_quote = options_.force_quote || value.empty() || value == options_.null_text;
  if (!needs_quote) {
    for (const char c : value) {
      if (c == options_.delimiter || c == options_.quote || c == options_.escape || c == '\r'
          || c == '\n') {
        needs_quote = true;
        break;
      }
    }
  }
  if (!needs_quote) return emit(value.data(), value.size());
  Status status = emit(options_.quote);
  for (size_t i = 0; status == Status::Ok && i < value.size(); ++i) {
    const char c = value[i];
    if (c == options_.quote || c == options_.escape) status = emit(options_.escape);
    if (status == Status::Ok) status = emit(c);
  }
  return status == Status::Ok ? emit(options_.quote) : status;
}

Status CsvWriter::emit_values(const std::string_view *values, const bool *is_null, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      const Status status = emit(options_.delimiter);
      if (status != Status::Ok) return status;
    }
    const Status status = emit_field(values[i], is_null ? is_null[i] : false);
    if (status != Status::Ok) return status;
  }
  return emit(options_.line_terminator.data(), options_.line_terminator.size());
}

Status CsvWriter::write_header(const std::string_view *names, size_t count)
{
  if (write_failed_) return Status::Io;
  if (!is_open()) return fail(Status::InvalidOption, "export writer is not open");
  if (!options_.header) return fail(Status::InvalidOption, "a header is disabled for this export");
  if (header_written_) return fail(Status::InvalidOption, "the export header is already written");
  if (!names || count == 0 || count > MAX_HEADER_COLUMNS) {
    return fail(Status::InvalidOption, "invalid export header column count");
  }
  for (size_t i = 0; i < count; ++i) {
    if (names[i].size() > MAX_FIELD_BYTES) return fail(Status::ResourceLimit, "export header field is too large");
  }
  const Status status = emit_values(names, nullptr, count);
  if (status != Status::Ok) return status;
  header_written_ = true;
  row_columns_ = count;
  return Status::Ok;
}

Status CsvWriter::write_row(const std::string_view *values, const bool *is_null, size_t count)
{
  if (write_failed_) return Status::Io;
  if (!is_open()) return fail(Status::InvalidOption, "export writer is not open");
  if (options_.header && !header_written_) {
    return fail(Status::InvalidOption, "the export header must be written before rows");
  }
  if (!values || count == 0 || count > MAX_HEADER_COLUMNS) {
    return fail(Status::InvalidOption, "invalid export row column count");
  }
  if (header_written_ && count != row_columns_) {
    return fail(Status::InvalidOption, "export row column count differs from the header");
  }
  for (size_t i = 0; i < count; ++i) {
    if (values[i].size() > MAX_FIELD_BYTES) return fail(Status::ResourceLimit, "export field is too large");
  }
  const Status status = emit_values(values, is_null, count);
  if (status != Status::Ok) return status;
  ++row_count_;
  return Status::Ok;
}

Status CsvWriter::commit()
{
  if (write_failed_) return Status::Io;
  if (!is_open()) return fail(Status::InvalidOption, "export writer is not open");
  if (options_.header && !header_written_) {
    return fail(Status::InvalidOption, "the export header must be written before commit");
  }
  Status status = flush_buffer();
  if (status == Status::Ok && ::fsync(temp_fd_) != 0) {
    status = fail(Status::Io, "cannot sync export data");
  }
  const int fd = temp_fd_;
  temp_fd_ = -1;
  if (::close(fd) != 0 && status == Status::Ok) status = fail(Status::Io, "cannot close export data");
  if (status != Status::Ok) {
    abort();
    return status;
  }
  // Publish without replacing: linkat(2) creates the target name only when it is
  // still free, so a target created by another actor in the meantime keeps its
  // bytes and the export fails instead of clobbering it.
  status = verify_directory_unchanged();
  if (status != Status::Ok) {
    abort();
    return status;
  }
  if (::linkat(dir_fd_, temp_name_, dir_fd_, base_name_, 0) != 0) {
    if (errno == EEXIST) {
      status = fail(Status::Changed, "export target already exists");
    } else {
      status = fail(Status::Io, "cannot publish export file");
    }
    abort();
    return status;
  }
  temp_created_ = false;         // the published link owns the data now
  ::unlinkat(dir_fd_, temp_name_, 0);
  ::fsync(dir_fd_);              // best effort; directory fsync is optional
  committed_ = true;
  close_directory_fds();
  return Status::Ok;
}

} // namespace oceanbase::sql::file_sql
}} // namespace oceanbase::sql
