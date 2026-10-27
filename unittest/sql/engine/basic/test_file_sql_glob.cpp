/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_sql_glob.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace oceanbase::sql::file_sql;

static int failures = 0;

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)
#define CHECK_MATCH(p, c, expected) do { \
  const bool actual_ = glob_match((p), (c)); \
  if (actual_ != (expected)) { \
    std::cerr << __LINE__ << ": glob_match(\"" << (p) << "\", \"" << (c) << "\") == " << actual_ \
              << ", expected " << (expected) << "\n"; \
    ++failures; \
  } \
} while (0)

// Every fixture lives in one unique mkdtemp directory and is removed again, so a
// failing assertion can never touch files that this test did not create.
struct Fixture {
  std::string root;
  std::vector<std::string> owned;
  Fixture()
  {
    char name[] = "/tmp/seekdb-glob-XXXXXX";
    char *created = mkdtemp(name);
    if (!created) std::abort();
    root = created;
  }
  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;
  void own(const std::string &path) { owned.push_back(path); }
  std::string dir(const std::string &relative)
  {
    std::string path = root + "/" + relative;
    if (mkdir(path.c_str(), 0700) != 0) std::abort();
    owned.push_back(path);
    return path;
  }
  std::string put(const std::string &relative)
  {
    std::string path = root + "/" + relative;
    std::ofstream stream(path, std::ios::binary);
    stream << "payload";
    stream.close();
    if (!stream) std::abort();
    owned.push_back(path);
    return path;
  }
  std::string link(const std::string &relative, const std::string &target)
  {
    std::string path = root + "/" + relative;
    if (symlink(target.c_str(), path.c_str()) != 0) std::abort();
    owned.push_back(path);
    return path;
  }
  std::string canonical() const
  {
    char buffer[4096];
    if (!realpath(root.c_str(), buffer)) std::abort();
    return buffer;
  }
  // Result paths are always built from the canonical secure root.
  std::string at(const std::string &relative) const { return canonical() + "/" + relative; }
  ~Fixture()
  {
    if (root.find("/tmp/seekdb-glob-") != 0 || root.size() != 23) std::abort();
    // Files first, then the directories deepest-first, so nothing is left behind
    // even when a check above failed.
    for (const auto &path : owned) {
      if (path.find(root + "/") != 0) std::abort();
      unlink(path.c_str());
    }
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
      rmdir(it->c_str());
    }
    if (rmdir(root.c_str()) != 0) std::abort();
  }
};

static std::vector<std::string> listed(const GlobResult &result)
{
  std::vector<std::string> files;
  for (size_t i = 0; i < result.count(); ++i) files.push_back(result.file(i));
  return files;
}

// Helper so a braced path list can be used inside CHECK(): the extra parentheses
// keep the commas away from the preprocessor.
static bool files_are(const GlobResult &result, const std::vector<std::string> &expected)
{
  return listed(result) == expected;
}

static size_t open_descriptors()
{
  size_t open = 0;
  for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++open;
  return open;
}

void test_match_stars_and_questions()
{
  CHECK_MATCH("*.csv", "a.csv", true);
  CHECK_MATCH("*.csv", "a.csvx", false);
  CHECK_MATCH("*.csv", "csv", false);
  CHECK_MATCH("*", "abc", true);
  CHECK_MATCH("*", "", true);
  // '*' never crosses a path separator.
  CHECK_MATCH("*", "a/b", false);
  CHECK_MATCH("a/*/c", "a/b/c", true);
  CHECK_MATCH("a/*/c", "a/b/x/c", false);
  CHECK_MATCH("a*b", "ab", true);
  CHECK_MATCH("a*b", "axxb", true);
  CHECK_MATCH("a*b", "a/b", false);
  CHECK_MATCH("**/a", "b/a", true);
  CHECK_MATCH("?*", "a", true);
  CHECK_MATCH("?*", "", false);
  // '?' is exactly one non-'/' character.
  CHECK_MATCH("?", "a", true);
  CHECK_MATCH("?", "ab", false);
  CHECK_MATCH("?", "/", false);
  CHECK_MATCH("a?c", "abc", true);
  CHECK_MATCH("a?c", "a/c", false);
  CHECK_MATCH("a?c", "ac", false);
  // '.' and '+' are ordinary literal characters.
  CHECK_MATCH("a.c", "abc", false);
  CHECK_MATCH("a.c", "a.c", true);
  CHECK_MATCH("a+c", "a+c", true);
  CHECK_MATCH("a(c)", "a(c)", true);
  // Traversal cannot be smuggled in through the pattern language.
  CHECK_MATCH("a/../b", "a/../b", true);
  CHECK_MATCH("*", "..", true);
}

void test_match_classes()
{
  CHECK_MATCH("[abc]x", "bx", true);
  CHECK_MATCH("[abc]x", "dx", false);
  CHECK_MATCH("[a-c]x", "bx", true);
  CHECK_MATCH("[a-c]x", "dx", false);
  CHECK_MATCH("[a-cx-z]y", "yy", true);
  CHECK_MATCH("[a-cx-z]y", "wy", false);
  CHECK_MATCH("[!a]x", "bx", true);
  CHECK_MATCH("[!a]x", "ax", false);
  CHECK_MATCH("[^a]x", "bx", true);
  CHECK_MATCH("[^a]x", "ax", false);
  // A leading ']' is a literal member.
  CHECK_MATCH("[]]", "]", true);
  CHECK_MATCH("[]a]", "a", true);
  CHECK_MATCH("[]a]", "b", false);
  // A trailing '-' is a literal member.
  CHECK_MATCH("[a-]", "-", true);
  CHECK_MATCH("[a-]", "a", true);
  CHECK_MATCH("[a-]", "b", false);
  // Classes are single-character tokens and never consume '/'.
  CHECK_MATCH("[a-c]", "b", true);
  CHECK_MATCH("[!a]", "/", false);
  // Malformed classes degrade deterministically instead of reading out of bounds.
  CHECK_MATCH("[", "[", true);
  CHECK_MATCH("[a-", "[a-", true);
  CHECK_MATCH("[!", "[!", true);
  CHECK_MATCH("[z-a]", "b", false);
  CHECK_MATCH("[z-a]", "z", false);
  CHECK_MATCH("[z-a]", "[z-a]", false);
}

void test_match_escapes_and_globstar()
{
  CHECK_MATCH("\\*", "*", true);
  CHECK_MATCH("\\*", "a", false);
  CHECK_MATCH("a\\?b", "a?b", true);
  CHECK_MATCH("a\\?b", "axb", false);
  CHECK_MATCH("a\\[b", "a[b", true);
  CHECK_MATCH("\\[ab\\]", "[ab]", true);
  CHECK_MATCH("\\\\", "\\", true);
  // A trailing escape stays literal and stays in bounds.
  CHECK_MATCH("a\\", "a\\", true);
  CHECK_MATCH("a\\", "a", false);
  // '**' is a run including '/'.
  CHECK_MATCH("**", "a/b/c", true);
  CHECK_MATCH("**", "", true);
  CHECK_MATCH("a/**", "a/b/c", true);
  CHECK_MATCH("a**b", "a/x/y/b", true);
  CHECK_MATCH("**/*.csv", "a/b/c.csv", true);
  CHECK_MATCH("**/*.csv", "c.csv", true);
  CHECK_MATCH("**/*.csv", "a/b/c.txt", false);
  // '/**/' also matches a single '/'.
  CHECK_MATCH("a/**/b", "a/b", true);
  CHECK_MATCH("a/**/b", "a/x/y/b", true);
  CHECK_MATCH("a/**/b", "a/xb", false);
  CHECK_MATCH("a/**/b", "ab", false);
  CHECK_MATCH("/**/b", "/b", true);
  // Dotfiles are ordinary names; glob_match has no shell-style dot suppression.
  CHECK_MATCH("*", ".hidden", true);
  CHECK_MATCH(".*", ".hidden", true);
  CHECK_MATCH("*", ".", true);
  // Empty pattern and empty candidate.
  CHECK_MATCH("", "", true);
  CHECK_MATCH("", "a", false);
  CHECK_MATCH("a", "", false);
  CHECK_MATCH("**/**", "a/b", true);
}

void test_disabled_and_denials()
{
  Fixture f, outside;
  f.put("a.csv");
  outside.put("secret.csv");
  GlobOptions options;
  GlobResult result;

  options.enabled = false;
  CHECK(glob_files("*", f.root.c_str(), options, result) == Status::Disabled);
  CHECK(std::strlen(result.error()) > 0);
  CHECK(result.count() == 0);
  options.enabled = true;

  CHECK(glob_files("*", "", options, result) == Status::Denied);
  CHECK(std::strstr(result.error(), "secure_file_priv is not configured") != nullptr);
  CHECK(glob_files("*", nullptr, options, result) == Status::Denied);
  CHECK(std::strstr(result.error(), "secure_file_priv is not configured") != nullptr);
  CHECK(glob_files(nullptr, f.root.c_str(), options, result) == Status::Denied);
  CHECK(glob_files("", f.root.c_str(), options, result) == Status::Denied);

  CHECK(glob_files("../a.csv", f.root.c_str(), options, result) == Status::Denied);
  CHECK(glob_files("a/../b", f.root.c_str(), options, result) == Status::Denied);
  CHECK(glob_files("**/../*", f.root.c_str(), options, result) == Status::Denied);
  CHECK(glob_files("/tmp/../etc/*", f.root.c_str(), options, result) == Status::Denied);
  CHECK(glob_files("a//b", f.root.c_str(), options, result) == Status::Denied);

  const std::string outside_pattern = outside.canonical() + "/*";
  CHECK(glob_files(outside_pattern.c_str(), f.root.c_str(), options, result) == Status::Denied);
  CHECK(std::strlen(result.error()) > 0);
  CHECK(result.count() == 0);
  // Textually adjacent but not beneath the root.
  CHECK(glob_files((f.root + "x/*").c_str(), f.root.c_str(), options, result) == Status::Denied);
  CHECK(glob_files("/etc/*", f.root.c_str(), options, result) == Status::Denied);
  // A missing secure root is never resolved into something permissive.
  const std::string missing = f.root + "/missing";
  CHECK(glob_files("*", missing.c_str(), options, result) == Status::Denied);

  // Pattern byte bound.
  const std::string oversized(5000, 'a');
  CHECK(glob_files(oversized.c_str(), f.root.c_str(), options, result) == Status::Denied);
  CHECK(std::strstr(result.error(), "max_pattern_bytes") != nullptr);
  GlobOptions small;
  small.max_pattern_bytes = 8;
  CHECK(glob_files("12345678", f.root.c_str(), small, result) == Status::Denied);
  CHECK(glob_files("1234567", f.root.c_str(), small, result) == Status::Ok);

  // The pattern is C-string bounded: a NUL terminates it and is never read past.
  const char embedded[] = "a.csv\0ignored";
  CHECK(glob_files(embedded, f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 1);
  CHECK((files_are(result, {f.at("a.csv")})));
}

void test_symlink_escape()
{
  Fixture f, outside;
  outside.put("secret.csv");
  outside.dir("inner");
  outside.put("inner/deep.csv");
  f.link("escape.csv", outside.at("secret.csv"));
  f.link("escape_dir", outside.canonical());
  f.dir("real");
  f.put("real/inside.csv");
  f.link("alias", f.root + "/real");

  GlobOptions options;
  GlobResult result;

  // Every entry is a symlink: nothing is returned, nothing is an error.
  CHECK(glob_files("*", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  CHECK(glob_files("escape.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  CHECK(glob_files("escape.csv", outside.canonical().c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  // A symlinked directory is never descended into, even when it points outside.
  CHECK(glob_files("escape_dir/secret.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  CHECK(glob_files("escape_dir/**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  CHECK(glob_files("escape_dir/*", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  // Recursive traversal never crosses the alias either.
  CHECK(glob_files("**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("real/inside.csv")})));
}

void test_traversal_ordering_and_bounds()
{
  Fixture f;
  f.dir("a");
  f.dir("a/b");
  f.dir("a/b/c");
  f.dir("z");
  f.put("top.csv");
  f.put("a/one.csv");
  f.put("a/b/two.csv");
  f.put("a/b/c/three.csv");
  f.put("note.txt");
  f.put("a/b/notes.txt");
  f.put(".hidden.csv");
  f.put("z/zed.csv");

  GlobOptions options;
  GlobResult result;

  CHECK(glob_files("**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  std::vector<std::string> expected{f.at(".hidden.csv"), f.at("a/b/c/three.csv"), f.at("a/b/two.csv"),
                                    f.at("a/one.csv"), f.at("top.csv"), f.at("z/zed.csv")};
  std::sort(expected.begin(), expected.end());
  CHECK(listed(result) == expected);
  CHECK(result.count() == expected.size());
  CHECK(std::strlen(result.error()) == 0);
  CHECK(!result.truncated());

  // Deterministic: a second identical call returns the identical order.
  CHECK(glob_files("**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(listed(result) == expected);
  auto observed = listed(result);
  CHECK(std::is_sorted(observed.begin(), observed.end()));

  // Index accessors are bounded.
  CHECK(result.file(0) != nullptr);
  CHECK(result.file(result.count()) == nullptr);
  CHECK(result.file(result.count() + 100) == nullptr);
  CHECK(result.file_view(result.count()).empty());
  CHECK(result.file_view(0) == std::string_view(expected[0]));
  result.clear();
  CHECK(result.count() == 0);
  CHECK(result.file(0) == nullptr);
  CHECK(result.file_view(0).empty());
  CHECK(std::strlen(result.error()) == 0);
  CHECK(!result.truncated());

  // Overlapping globstars collapse to one deterministic entry per file.
  CHECK(glob_files("**/**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(listed(result) == expected);

  // Directories are never returned.
  CHECK(glob_files("*", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at(".hidden.csv"), f.at("note.txt"), f.at("top.csv")})));
  CHECK(glob_files("a/b/*", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("a/b/notes.txt"), f.at("a/b/two.csv")})));

  // Non-regular files are excluded; no file content is ever opened.
  const std::string fifo = f.root + "/pipe.csv";
  CHECK(mkfifo(fifo.c_str(), 0600) == 0);
  f.own(fifo);
  CHECK(glob_files("*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at(".hidden.csv"), f.at("top.csv")})));

  // A pattern without wildcards returns exactly that file, and no-match is not an error.
  CHECK(glob_files("a/b/two.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("a/b/two.csv")})));
  CHECK(glob_files("a/b/missing.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  CHECK(glob_files("*.nomatch", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);
  CHECK(glob_files("no/such/directory/*", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);

  // Absolute patterns are accepted in both spellings of the secure root.
  const std::string given = f.root + "/a/*.csv";
  CHECK(glob_files(given.c_str(), f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("a/one.csv")})));
  const std::string canonical_pattern = f.canonical() + "/a/b/*.csv";
  CHECK(glob_files(canonical_pattern.c_str(), f.canonical().c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("a/b/two.csv")})));
  // Redundant separators and '.' components stay inside the root.
  CHECK(glob_files("./top.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("top.csv")})));
  CHECK(glob_files("a/", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 0);

  // '**' as the final component lists every regular file beneath.
  CHECK(glob_files("a/**", f.root.c_str(), options, result) == Status::Ok);
  std::vector<std::string> under_a{"a/b/c/three.csv", "a/b/notes.txt", "a/b/two.csv", "a/one.csv"};
  std::vector<std::string> under_a_expected;
  for (const auto &relative : under_a) under_a_expected.push_back(f.at(relative));
  std::sort(under_a_expected.begin(), under_a_expected.end());
  CHECK(listed(result) == under_a_expected);
  std::vector<std::string> all{".hidden.csv", "a/b/c/three.csv", "a/b/notes.txt", "a/b/two.csv",
                               "a/one.csv", "note.txt", "top.csv", "z/zed.csv"};
  std::vector<std::string> all_expected;
  for (const auto &relative : all) all_expected.push_back(f.at(relative));
  std::sort(all_expected.begin(), all_expected.end());
  CHECK(glob_files("**", f.root.c_str(), options, result) == Status::Ok);
  CHECK(listed(result) == all_expected);

  // A long chain of globstar components is collapsed into one traversal step, so
  // it returns exactly what "**/*.csv" returns instead of multiplying the walk.
  std::string hostile;
  for (int repeat = 0; repeat < 64; ++repeat) hostile += "**/";
  hostile += "*.csv";
  CHECK(glob_files(hostile.c_str(), f.root.c_str(), options, result) == Status::Ok);
  CHECK(listed(result) == expected);
}

void test_resource_limits()
{
  Fixture f;
  f.put("one.csv");
  f.put("two.csv");
  f.put("three.csv");
  GlobOptions options;
  GlobResult result;

  options.max_entries = 2;
  CHECK(glob_files("*.csv", f.root.c_str(), options, result) == Status::ResourceLimit);
  CHECK(std::strstr(result.error(), "max_entries") != nullptr);
  CHECK(result.count() <= 2);
  CHECK(!result.truncated());
  options.max_entries = 3;
  CHECK(glob_files("*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 3);

  Fixture deep;
  deep.dir("a");
  deep.dir("a/b");
  deep.dir("a/b/c");
  deep.put("a/b/c/deep.csv");
  GlobOptions shallow;
  shallow.max_depth = 1;
  CHECK(glob_files("**/*.csv", deep.root.c_str(), shallow, result) == Status::ResourceLimit);
  CHECK(std::strstr(result.error(), "max_depth") != nullptr);
  shallow.max_depth = 3;
  CHECK(glob_files("**/*.csv", deep.root.c_str(), shallow, result) == Status::Ok);
  CHECK((files_are(result, {deep.at("a/b/c/deep.csv")})));
}

void test_descriptor_hygiene()
{
  Fixture f;
  f.dir("a");
  f.put("a/x.csv");
  f.put("y.csv");
  GlobOptions options;
  GlobResult result;

  int before[256];
  for (int fd = 0; fd < 256; ++fd) before[fd] = fcntl(fd, F_GETFD);

  CHECK(glob_files("**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == 2);
  CHECK(glob_files("missing/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(glob_files("../escape", f.root.c_str(), options, result) == Status::Denied);
  GlobOptions tight;
  tight.max_entries = 1;
  CHECK(glob_files("**/*.csv", f.root.c_str(), tight, result) == Status::ResourceLimit);
  GlobOptions shallow;
  shallow.max_depth = 0;
  CHECK(glob_files("**/*.csv", f.root.c_str(), shallow, result) == Status::ResourceLimit);

  for (int fd = 0; fd < 256; ++fd) CHECK((before[fd] < 0) == (fcntl(fd, F_GETFD) < 0));
  CHECK(open_descriptors() == static_cast<size_t>(std::count_if(before, before + 256,
      [](int flags) { return flags >= 0; })));

  // Concurrent removal between calls must not leave stale state or descriptors.
  unlink((f.root + "/y.csv").c_str());
  CHECK(glob_files("**/*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK((files_are(result, {f.at("a/x.csv")})));
}

void test_directory_refill_keeps_descriptor_alive()
{
  Fixture f;
  // macOS may eagerly buffer a small directory in fdopendir(). A large one
  // forces readdir() to refill and exposes premature descriptor closure.
  const size_t count = 4096;
  for (size_t i = 0; i < count; ++i) {
    f.put(std::string(120, 'x') + std::to_string(i) + ".csv");
  }
  GlobOptions options;
  options.max_entries = count + 1;
  GlobResult result;
  const size_t descriptors = open_descriptors();
  CHECK(glob_files("*.csv", f.root.c_str(), options, result) == Status::Ok);
  CHECK(result.count() == count);
  CHECK(open_descriptors() == descriptors);
}

int main()
{
  test_match_stars_and_questions();
  test_match_classes();
  test_match_escapes_and_globstar();
  test_disabled_and_denials();
  test_symlink_escape();
  test_traversal_ordering_and_bounds();
  test_resource_limits();
  test_descriptor_hygiene();
  test_directory_refill_keeps_descriptor_alive();
  if (failures) std::cerr << failures << " checks failed\n";
  else std::cout << "file_sql_glob: all checks passed\n";
  return failures ? 1 : 0;
}
