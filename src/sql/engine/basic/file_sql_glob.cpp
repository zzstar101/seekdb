/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "file_sql_glob.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <utility>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {

size_t GlobResult::count() const { return entries_.size(); }

const char *GlobResult::file(size_t index) const
{
  return index < entries_.size() ? entries_[index].c_str() : nullptr;
}

std::string_view GlobResult::file_view(size_t index) const
{
  return index < entries_.size() ? std::string_view(entries_[index]) : std::string_view();
}

// Exceeding a bound is reported as Status::ResourceLimit, so a result is either
// complete or absent; it is never a silently truncated answer.
bool GlobResult::truncated() const { return false; }

// Never null: an empty string means "no error".
const char *GlobResult::error() const { return error_.c_str(); }

void GlobResult::clear()
{
  entries_.clear();
  error_.clear();
}

namespace {

struct ClassRange {
  unsigned char low;
  unsigned char high;
};

struct ClassSpec {
  bool negate = false;
  std::vector<ClassRange> ranges;
};

enum class TokenKind { Literal, AnyOne, Star, GlobStar, GlobStarSlash, Class, Never };

struct Token {
  TokenKind kind = TokenKind::Literal;
  unsigned char literal = 0;
  ClassSpec spec;
};

bool class_contains(const ClassSpec &spec, unsigned char value)
{
  for (const ClassRange &range : spec.ranges) {
    if (value >= range.low && value <= range.high) return true;
  }
  return false;
}

// Parse "[...]" starting at pattern[offset] into `token`. Returns the offset just
// past the closing ']', or 0 when the class is unterminated, in which case the
// caller treats '[' as an ordinary literal.
size_t parse_class(std::string_view pattern, size_t offset, Token &token)
{
  size_t pos = offset + 1;
  ClassSpec spec;
  if (pos < pattern.size() && (pattern[pos] == '!' || pattern[pos] == '^')) {
    spec.negate = true;
    ++pos;
  }
  // A ']' right after '[' or right after the negation marker is a literal member.
  if (pos < pattern.size() && pattern[pos] == ']') {
    spec.ranges.push_back({static_cast<unsigned char>(']'), static_cast<unsigned char>(']')});
    ++pos;
  }
  bool closed = false;
  bool malformed = false;
  while (pos < pattern.size()) {
    if (pattern[pos] == ']') {
      closed = true;
      ++pos;
      break;
    }
    unsigned char low = static_cast<unsigned char>(pattern[pos]);
    size_t consumed = 1;
    if (pattern[pos] == '\\' && pos + 1 < pattern.size()) {
      low = static_cast<unsigned char>(pattern[pos + 1]);
      consumed = 2;
    }
    pos += consumed;
    // '-' separates a range only when a member follows it.
    if (pos < pattern.size() && pattern[pos] == '-' && pos + 1 < pattern.size() && pattern[pos + 1] != ']') {
      unsigned char high = static_cast<unsigned char>(pattern[pos + 1]);
      size_t width = 2;
      if (pattern[pos + 1] == '\\' && pos + 2 < pattern.size()) {
        high = static_cast<unsigned char>(pattern[pos + 2]);
        width = 3;
      }
      if (low > high) {
        // A reversed range is malformed. Matching nothing is deterministic and
        // keeps the parser from ever reading outside the pattern.
        malformed = true;
      } else {
        spec.ranges.push_back({low, high});
      }
      pos += width;
    } else {
      spec.ranges.push_back({low, low});
    }
  }
  if (!closed) return 0;
  if (malformed) {
    token.kind = TokenKind::Never;
    token.spec = ClassSpec();
  } else {
    token.kind = TokenKind::Class;
    token.spec = spec;
  }
  return pos;
}

std::vector<Token> tokenize(std::string_view pattern)
{
  std::vector<Token> tokens;
  tokens.reserve(pattern.size());
  size_t pos = 0;
  while (pos < pattern.size()) {
    const char current = pattern[pos];
    if (current == '\\') {
      Token token;
      token.kind = TokenKind::Literal;
      // A trailing escape stays literal instead of reading one byte past the end.
      token.literal = static_cast<unsigned char>(pos + 1 < pattern.size() ? pattern[pos + 1] : '\\');
      pos += pos + 1 < pattern.size() ? 2 : 1;
      tokens.push_back(token);
    } else if (current == '*') {
      if (pos + 1 < pattern.size() && pattern[pos + 1] == '*') {
        const bool component_start = pos == 0 || pattern[pos - 1] == '/';
        if (component_start && pos + 2 < pattern.size() && pattern[pos + 2] == '/') {
          // A "**/" path component also matches zero components, which is what
          // makes "/**/" collapse to a single '/'.
          Token token;
          token.kind = TokenKind::GlobStarSlash;
          tokens.push_back(token);
          pos += 3;
        } else {
          Token token;
          token.kind = TokenKind::GlobStar;
          tokens.push_back(token);
          pos += 2;
        }
      } else {
        Token token;
        token.kind = TokenKind::Star;
        tokens.push_back(token);
        ++pos;
      }
    } else if (current == '?') {
      Token token;
      token.kind = TokenKind::AnyOne;
      tokens.push_back(token);
      ++pos;
    } else if (current == '[') {
      Token token;
      const size_t next = parse_class(pattern, pos, token);
      if (next == 0) {
        Token literal;
        literal.kind = TokenKind::Literal;
        literal.literal = static_cast<unsigned char>('[');
        tokens.push_back(literal);
        ++pos;
      } else {
        tokens.push_back(token);
        pos = next;
      }
    } else {
      Token token;
      token.kind = TokenKind::Literal;
      token.literal = static_cast<unsigned char>(current);
      tokens.push_back(token);
      ++pos;
    }
  }
  return tokens;
}

// Backtracking matcher with memoization, so a pathological pattern cannot force
// exponential work. When the memo table would be too large it is simply not
// used; the match result is unaffected.
class MatchState {
 public:
  MatchState(const std::vector<Token> &tokens, std::string_view candidate)
      : tokens_(tokens), candidate_(candidate)
  {
    const size_t width = candidate.size() + 1;
    if (tokens.size() + 1 <= kMaxMemoCells / width) {
      memo_.assign((tokens.size() + 1) * width, 0);
      width_ = width;
    }
  }

  bool run() { return match(0, 0); }

 private:
  static const size_t kMaxMemoCells = 1u << 20;

  bool match(size_t index, size_t offset)
  {
    if (index == tokens_.size()) return offset == candidate_.size();
    const size_t slot = width_ == 0 ? 0 : index * width_ + offset;
    if (width_ != 0 && memo_[slot] != 0) return memo_[slot] == 2;
    const Token &token = tokens_[index];
    bool result = false;
    switch (token.kind) {
      case TokenKind::Never:
        result = false;
        break;
      case TokenKind::Literal:
        result = offset < candidate_.size()
            && static_cast<unsigned char>(candidate_[offset]) == token.literal
            && match(index + 1, offset + 1);
        break;
      case TokenKind::AnyOne:
        result = offset < candidate_.size() && candidate_[offset] != '/' && match(index + 1, offset + 1);
        break;
      case TokenKind::Class:
        if (offset < candidate_.size() && candidate_[offset] != '/') {
          const bool contains = class_contains(token.spec, static_cast<unsigned char>(candidate_[offset]));
          result = contains != token.spec.negate && match(index + 1, offset + 1);
        }
        break;
      case TokenKind::Star:
        result = match(index + 1, offset);
        for (size_t next = offset; !result && next < candidate_.size() && candidate_[next] != '/'; ++next) {
          result = match(index + 1, next + 1);
        }
        break;
      case TokenKind::GlobStar:
        for (size_t next = offset; !result && next <= candidate_.size(); ++next) {
          result = match(index + 1, next);
        }
        break;
      case TokenKind::GlobStarSlash:
        // Zero components, or any run that ends with a '/'.
        result = match(index + 1, offset);
        for (size_t next = offset; !result && next < candidate_.size(); ++next) {
          if (candidate_[next] == '/') result = match(index + 1, next + 1);
        }
        break;
    }
    if (width_ != 0) memo_[slot] = static_cast<signed char>(result ? 2 : 1);
    return result;
  }

  const std::vector<Token> &tokens_;
  std::string_view candidate_;
  std::vector<signed char> memo_;
  size_t width_ = 0;
};

} // namespace

bool glob_match(std::string_view pattern, std::string_view candidate)
{
  const std::vector<Token> tokens = tokenize(pattern);
  return MatchState(tokens, candidate).run();
}

#ifndef _WIN32

namespace {

constexpr size_t kMaxPathBytes = 4096;

bool equal_ci(const char *left, const char *right)
{
  if (left == nullptr || right == nullptr) return false;
  for (; *left != '\0' && *right != '\0'; ++left, ++right) {
    const char a = *left >= 'A' && *left <= 'Z' ? static_cast<char>(*left + ('a' - 'A')) : *left;
    const char b = *right >= 'A' && *right <= 'Z' ? static_cast<char>(*right + ('a' - 'A')) : *right;
    if (a != b) return false;
  }
  return *left == '\0' && *right == '\0';
}

bool starts_with(const std::string &text, const std::string &prefix)
{
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// A descriptor is closed by the destructor, including on every error path.
struct OwnedFd {
  OwnedFd() = default;
  OwnedFd(const OwnedFd &) = delete;
  OwnedFd &operator=(const OwnedFd &) = delete;
  ~OwnedFd() { reset(); }
  void reset()
  {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  int fd = -1;
};

int duplicate_cloexec(int descriptor)
{
  return descriptor < 0 ? -1 : fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
}

// The same security reasoning as file_scan_utils.cpp open_beneath(): no component
// is ever resolved through a symlink, and every step is descriptor-relative, so a
// concurrent rename or symlink swap cannot redirect the traversal. Only
// directories are opened here, so O_DIRECTORY is always set and no file content
// is ever opened.
int open_directory_beneath(int base, const char *relative)
{
  char path[kMaxPathBytes];
  if (relative == nullptr || *relative == '\0' || std::strlen(relative) >= sizeof(path)) return -1;
  std::memcpy(path, relative, std::strlen(relative) + 1);
  int fd = duplicate_cloexec(base);
  char *part = path;
  while (fd >= 0) {
    char *slash = std::strchr(part, '/');
    if (slash != nullptr) *slash = '\0';
    if (*part == '\0' || std::strcmp(part, ".") == 0 || std::strcmp(part, "..") == 0) {
      ::close(fd);
      return -1;
    }
    const int next = ::openat(fd, part, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY | O_NONBLOCK);
    ::close(fd);
    fd = next;
    if (slash == nullptr) break;
    part = slash + 1;
  }
  return fd;
}

// Offset of the first wildcard that is not escaped by a backslash.
size_t first_wildcard(const std::string &text)
{
  bool escaped = false;
  for (size_t index = 0; index < text.size(); ++index) {
    const char current = text[index];
    if (escaped) {
      escaped = false;
    } else if (current == '\\') {
      escaped = true;
    } else if (current == '*' || current == '?' || current == '[') {
      return index;
    }
  }
  return std::string::npos;
}

// Map an absolute pattern onto a path relative to the canonical secure root. Only
// the leading wildcard-free directory prefix is canonicalized; everything after it
// is matched against real directory entries, so a wildcard can never encode an
// escape from the root.
Status absolute_to_relative(const std::string &text, const std::string &given_root,
                            const std::string &canonical_root, std::string &relative, std::string &error)
{
  const size_t wildcard = first_wildcard(text);
  const std::string prefix = wildcard == std::string::npos ? text : text.substr(0, wildcard);
  const size_t separator = prefix.rfind('/');
  std::string directory = separator == std::string::npos ? std::string("/") : prefix.substr(0, separator);
  while (directory.size() > 1 && directory.back() == '/') directory.pop_back();
  if (directory.empty()) directory = "/";
  if (directory.size() >= kMaxPathBytes) {
    error = "glob pattern is too long";
    return Status::Denied;
  }
  const std::string tail = text.substr(separator == std::string::npos ? 0 : separator);
  std::string normalized_root = given_root;
  while (normalized_root.size() > 1 && normalized_root.back() == '/') normalized_root.pop_back();

  // Fast path: the literal prefix is spelled exactly like a known spelling of the
  // secure root, so no filesystem resolution is needed.
  if (directory == canonical_root || directory == normalized_root) {
    relative = tail;
  } else {
    char resolved[kMaxPathBytes];
    if (realpath(directory.c_str(), resolved) == nullptr) {
      error = "cannot resolve the glob pattern directory";
      return Status::Denied;
    }
    const std::string canonical_directory(resolved);
    if (canonical_root == "/") {
      relative = canonical_directory.substr(1) + tail;
    } else if (canonical_directory == canonical_root) {
      relative = tail;
    } else if (starts_with(canonical_directory, canonical_root + "/")) {
      relative = canonical_directory.substr(canonical_root.size() + 1) + tail;
    } else {
      error = "glob pattern is outside secure_file_priv";
      return Status::Denied;
    }
  }
  while (!relative.empty() && relative[0] == '/') relative.erase(0, 1);
  while (!relative.empty() && relative.back() == '/') relative.pop_back();
  return Status::Ok;
}

// One bounded pass over the directory tree beneath the secure root.
struct Walker {
  Walker(const GlobOptions &options, const std::vector<std::string> &components)
      : options_(options), components_(components)
  {}
  Status fail(Status status, const std::string &message)
  {
    status_ = status;
    error_ = message;
    return status;
  }
  // A complete result set is required, so overflow is an error and never a
  // silent truncation.
  bool add(const std::string &path)
  {
    if (entries_.size() >= options_.max_entries) {
      fail(Status::ResourceLimit, "glob matched more than max_entries entries");
      return false;
    }
    entries_.push_back(path);
    return true;
  }
  Status walk(int dir_fd, size_t index, const std::string &prefix, size_t depth);

  const GlobOptions &options_;
  const std::vector<std::string> &components_;
  std::vector<std::string> entries_;
  std::string error_;
  Status status_ = Status::Ok;
  // One (directory, component index) expansion per depth. A repeat visit can only
  // re-enumerate files that are already collected or re-raise the same bound
  // error, so skipping it keeps a pattern with many "**" components from
  // multiplying the traversal instead of merely bounding it.
  std::map<std::pair<std::string, size_t>, size_t> expanded_;
};

Status Walker::walk(int dir_fd, size_t index, const std::string &prefix, size_t depth)
{
  if (status_ != Status::Ok) return status_;
  if (index >= components_.size()) return Status::Ok;
  const std::string &component = components_[index];
  const bool last = index + 1 == components_.size();
  const bool globstar = component == "**";
  // Shell-style expansion requires a literal dot at the start of each hidden
  // path component. In particular, '**' must not descend into hidden folders.
  // Apply this in the filesystem walker, not the general string matcher.
  const bool explicit_dot = component[0] == '.'
      || (component.size() > 1 && component[0] == '\\' && component[1] == '.');
  {
    auto found = expanded_.find(std::make_pair(prefix, index));
    if (found == expanded_.end()) {
      expanded_.insert(std::make_pair(std::make_pair(prefix, index), depth));
    } else if (found->second <= depth) {
      return Status::Ok; // already expanded with at least as much depth budget
    } else {
      found->second = depth;
    }
  }
  {
    OwnedFd scan;
    // A fresh file description is required: a dup() would share the directory
    // offset, so a second scan of the same descriptor would start already at the
    // end. "." cannot be a symlink, and the enclosing descriptor already pins the
    // directory, so no component resolution happens here.
    scan.fd = ::openat(dir_fd, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NONBLOCK);
    if (scan.fd < 0) return fail(Status::Io, "cannot read a directory");
    DIR *directory = ::fdopendir(scan.fd);
    if (directory == nullptr) return fail(Status::Io, "cannot read a directory");
    scan.fd = -1; // Transfer ownership; closedir(), not reset(), must close it.
    struct DirectoryCloser {
      ~DirectoryCloser()
      {
        if (directory != nullptr) ::closedir(directory);
      }
      DIR *directory;
    } closer{directory};
    while (true) {
      errno = 0;
      const struct dirent *entry = ::readdir(directory);
      const int read_error = errno;
      if (entry == nullptr) {
        if (read_error != 0) return fail(Status::Io, "cannot read a directory entry");
        break;
      }
      const char *name = entry->d_name;
      // "." and ".." are never candidates, whatever the pattern says.
      if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
      if (name[0] == '.' && !explicit_dot) continue;
      if (!glob_match(component, name)) continue;
      struct stat metadata;
      // No symlink is ever followed; an entry that raced away is simply skipped.
      if (::fstatat(dir_fd, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) continue;
      if (S_ISLNK(metadata.st_mode)) continue;
      if (last) {
        if (S_ISREG(metadata.st_mode) && !add(prefix + name)) return status_;
      } else if (S_ISDIR(metadata.st_mode)) {
        OwnedFd child;
        child.fd = ::openat(dir_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY | O_NONBLOCK);
        if (child.fd < 0) continue; // changed concurrently: skip this branch only
        const size_t next_depth = depth + (globstar ? 1 : 0);
        if (next_depth > options_.max_depth) {
          return fail(Status::ResourceLimit, "glob exceeded max_depth directory levels");
        }
        const Status walked = walk(child.fd, index + (globstar ? 0 : 1), prefix + name + "/", next_depth);
        if (walked != Status::Ok) return walked;
      }
    }
  }
  // "**" also matches zero directory levels, which is what makes "a/**/b" match
  // "a/b".
  if (globstar) return walk(dir_fd, index + 1, prefix, depth);
  return Status::Ok;
}

} // namespace

Status glob_files(const char *pattern, const char *secure_root, const GlobOptions &options, GlobResult &result)
{
  result.clear();
  if (!options.enabled) {
    result.error_ = "file glob is disabled by enable_file_sql";
    return Status::Disabled;
  }
  if (pattern == nullptr || pattern[0] == '\0') {
    result.error_ = "glob pattern is empty";
    return Status::Denied;
  }
  if (secure_root == nullptr || secure_root[0] == '\0' || equal_ci(secure_root, "null")) {
    result.error_ = "secure_file_priv is not configured";
    return Status::Denied;
  }
  // The pattern arrives as a C string, so an embedded NUL is unobservable here;
  // the length bound is enforced before any parsing happens.
  const size_t pattern_bytes = std::strlen(pattern);
  if (pattern_bytes >= options.max_pattern_bytes) {
    result.error_ = "glob pattern exceeds max_pattern_bytes";
    return Status::Denied;
  }
  if (std::strlen(secure_root) >= kMaxPathBytes) {
    result.error_ = "secure_file_priv path is too long";
    return Status::Denied;
  }

  char canonical[kMaxPathBytes];
  if (::realpath(secure_root, canonical) == nullptr) {
    result.error_ = "secure_file_priv is not resolvable";
    return Status::Denied;
  }
  struct stat root_metadata;
  if (::stat(canonical, &root_metadata) != 0 || !S_ISDIR(root_metadata.st_mode)) {
    result.error_ = "secure_file_priv is not a directory";
    return Status::Denied;
  }
  const std::string canonical_root(canonical);

  // Trailing separators are dropped so "dir/" and "dir" mean the same thing.
  size_t usable = pattern_bytes;
  while (usable > 1 && pattern[usable - 1] == '/') --usable;
  const std::string text(pattern, usable);

  std::string relative;
  if (text[0] == '/') {
    std::string error;
    const Status resolved = absolute_to_relative(text, std::string(secure_root), canonical_root, relative, error);
    if (resolved != Status::Ok) {
      result.error_ = error;
      return resolved;
    }
  } else {
    relative = text;
  }

  std::vector<std::string> components;
  size_t start = 0;
  while (start <= relative.size()) {
    const size_t slash = relative.find('/', start);
    const size_t end = slash == std::string::npos ? relative.size() : slash;
    const std::string component = relative.substr(start, end - start);
    if (component.empty()) {
      result.error_ = "glob pattern has an empty path component";
      return Status::Denied;
    }
    if (component == "..") {
      result.error_ = "glob pattern must not contain '..'";
      return Status::Denied;
    }
    if (component != ".") {
      // "**/**" consumes directory levels exactly like "**", so collapsing
      // adjacent globstars keeps a long pattern from multiplying the work the
      // walker has to do without changing which files match.
      if (!(component == "**" && !components.empty() && components.back() == "**")) {
        components.push_back(component);
      }
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  // The pattern names the secure root itself, which is a directory, so no regular
  // file can match it.
  if (components.empty()) return Status::Ok;
  // A trailing "**" means "every regular file at or below", the same shape as
  // "**/*", so the walker needs no special case for it.
  if (components.back() == "**") components.push_back("*");

  OwnedFd root;
  {
    const int system_root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (system_root < 0) {
      result.error_ = "cannot open the filesystem root";
      return Status::Io;
    }
    root.fd = canonical_root == "/" ? duplicate_cloexec(system_root)
                                    : open_directory_beneath(system_root, canonical_root.c_str() + 1);
    ::close(system_root);
  }
  if (root.fd < 0) {
    result.error_ = "secure_file_priv changed while opening it";
    return Status::Denied;
  }

  Walker walker(options, components);
  const std::string prefix = canonical_root == "/" ? std::string("/") : canonical_root + "/";
  const Status walked = walker.walk(root.fd, 0, prefix, 0);
  root.reset();
  if (walked != Status::Ok) {
    result.error_ = walker.error_;
    return walked;
  }
  std::sort(walker.entries_.begin(), walker.entries_.end());
  walker.entries_.erase(std::unique(walker.entries_.begin(), walker.entries_.end()), walker.entries_.end());
  result.entries_ = std::move(walker.entries_);
  return Status::Ok;
}

#else

// Fail closed on platforms without descriptor-relative no-follow traversal.
Status glob_files(const char *pattern, const char *secure_root, const GlobOptions &options, GlobResult &result)
{
  result.clear();
  (void)pattern;
  (void)secure_root;
  if (!options.enabled) {
    result.error_ = "file glob is disabled by enable_file_sql";
    return Status::Disabled;
  }
  result.error_ = "secure local file globbing is unavailable on this platform";
  return Status::Unsupported;
}

#endif

}}} // namespace oceanbase::sql::file_sql
