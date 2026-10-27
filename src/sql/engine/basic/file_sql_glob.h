/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "file_scan_utils.h"

namespace oceanbase { namespace sql { namespace file_sql {

// Bounds for one glob() expansion. The SQL layer maps enable_file_sql onto
// `enabled`; the remaining fields keep a single expansion from walking an
// unbounded filesystem, so an over-limit expansion fails instead of truncating.
struct GlobOptions {
  bool enabled = true;              // SQL layer maps enable_file_sql onto this
  size_t max_entries = 65536;       // metadata bound
  size_t max_pattern_bytes = 4096;
  size_t max_depth = 32;            // directory levels for **
};

// Result of one glob_files() call: sorted, deduplicated absolute paths of
// regular files. Names only; no file content is ever read.
class GlobResult {
 public:
  size_t count() const;
  const char *file(size_t index) const;           // nullptr when out of range
  std::string_view file_view(size_t index) const; // empty view when out of range
  bool truncated() const;                         // stays false: overflow is an error
  const char *error() const;
  void clear();
 private:
  friend Status glob_files(const char *, const char *, const GlobOptions &, GlobResult &);
  std::vector<std::string> entries_;   // sorted, deduplicated, absolute paths
  std::string error_;
};

// Expand a shell-style pattern into regular files beneath secure_root.
// A leading '.' in a file or directory name must be explicitly matched by a
// literal or escaped dot in that path component; wildcards do not match it.
// Mirrors the security model of file_scan_utils.cpp: descriptor-relative
// traversal that never follows a symlink, denies "..", NUL and oversized
// patterns, and returns a human-readable error through result.error().
Status glob_files(const char *pattern, const char *secure_root,
                  const GlobOptions &options, GlobResult &result);

// Filesystem-free pattern matcher used by glob_files() and by tests. The walker
// applies the leading-dot rule separately before calling this string matcher.
// '*' matches a run of non-'/' characters, '?' exactly one non-'/' character,
// "**" a run including '/', "/**/" also a single '/', "[...]" a character class
// (ranges, leading '!'/'^' negates, a leading ']' is literal) and '\' escapes
// the following character literally.
bool glob_match(std::string_view pattern, std::string_view candidate);

}}} // namespace oceanbase::sql::file_sql
