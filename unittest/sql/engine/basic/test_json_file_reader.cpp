/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/json_file_reader.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace oceanbase::sql::file_sql;

static int checks = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)

namespace {

// Unique fixture directory under /tmp; every file it creates is removed again so
// a failing run never litters the machine.
struct Fixtures {
  std::string root;
  std::vector<std::string> files;
  Fixtures()
  {
    char name[] = "/tmp/seekdb-json-reader-XXXXXX";
    char *made = mkdtemp(name);
    if (made == nullptr) std::abort();
    root = made;
  }
  std::string put(const std::string &name, const std::string &contents)
  {
    const std::string path = root + "/" + name;
    std::ofstream stream(path, std::ios::binary);
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.close();
    if (!stream) std::abort();
    files.push_back(path);
    return path;
  }
  std::string link(const std::string &name, const std::string &target)
  {
    const std::string path = root + "/" + name;
    if (symlink(target.c_str(), path.c_str()) != 0) std::abort();
    files.push_back(path);
    return path;
  }
  ~Fixtures()
  {
    static const std::string prefix = "/tmp/seekdb-json-reader-";
    // Only ever delete inside this unique fixture directory.
    if (root.compare(0, prefix.size(), prefix) != 0) std::abort();
    for (const std::string &path : files) {
      if (path.compare(0, root.size() + 1, root + "/") != 0) std::abort();
      unlink(path.c_str());
    }
    rmdir(root.c_str());
  }
};

const char *const BASIC_JSONL =
    "{\"id\":1,\"name\":\"Alice\",\"score\":98.5,\"active\":true}\n"
    "{\"id\":2,\"name\":\"Bob\",\"active\":false,\"day\":\"2026-08-02\"}\n"
    "{\"id\":3,\"name\":\"Chen\",\"score\":90,\"active\":true,\"note\":\"ok\"}\n";

bool contains(const char *text, const char *needle) { return std::strstr(text, needle) != nullptr; }

std::unique_ptr<JsonlReader> open_reader(const std::string &path, const std::string &root,
                                         const JsonOptions &options = JsonOptions{true, -1, 16 * 1024 * 1024})
{
  auto reader = std::make_unique<JsonlReader>();
  const Status status = reader->open(path.c_str(), root.c_str(), options);
  CHECK(status == Status::Ok);
  return reader;
}

// Canonical spelling of a fixture path; the reader reports errors against it.
std::string canonical(const std::string &path)
{
  char resolved[PATH_MAX] = {};
  if (realpath(path.c_str(), resolved) == nullptr) std::abort();
  return resolved;
}

// The recorded public fixture must drive exactly the documented schema/values.
void test_public_basic_fixture()
{
  Fixtures f;
  const std::string path = f.put("basic.jsonl", BASIC_JSONL);
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 6);
  if (reader->column_count() == 6) {
    const char *names[] = {"id", "name", "score", "active", "day", "note"};
    const Type types[] = {Type::BigInt, Type::Varchar, Type::Double, Type::Boolean, Type::Date, Type::Varchar};
    const bool nullable[] = {false, false, true, false, true, true};
    for (size_t i = 0; i < 6; ++i) {
      CHECK(std::string(reader->column(i).name) == names[i]);
      CHECK(reader->column(i).type == types[i]);
      CHECK(reader->column(i).nullable == nullable[i]);
    }
    CHECK(reader->column(0).max_bytes == 1);
    CHECK(reader->column(1).max_bytes == 5);
    CHECK(reader->column(2).max_bytes == 4);
    CHECK(reader->column(3).max_bytes == 5);
    CHECK(reader->column(4).max_bytes == 10);
    CHECK(reader->column(5).max_bytes == 2);
  }
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->row_number() == 1);
  CHECK(reader->value(0) == "1");
  CHECK(reader->value(1) == "Alice");
  CHECK(reader->value(2) == "98.5");
  CHECK(reader->value(3) == "true");
  CHECK(reader->is_null(4));
  CHECK(reader->is_null(5));
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->row_number() == 2);
  CHECK(reader->value(0) == "2");
  CHECK(reader->value(1) == "Bob");
  CHECK(reader->is_null(2));
  CHECK(reader->value(3) == "false");
  CHECK(reader->value(4) == "2026-08-02");
  CHECK(reader->is_null(5));
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->row_number() == 3);
  // `score` widened to DOUBLE but keeps the JSON spelling of an integer literal.
  CHECK(reader->column(2).type == Type::Double);
  CHECK(reader->value(2) == "90");
  CHECK(reader->value(3) == "true");
  CHECK(reader->is_null(4));
  CHECK(reader->value(5) == "ok");
  CHECK(reader->next() == Status::End);
  CHECK(reader->next() == Status::End);
  CHECK(reader->rescan() == Status::Ok);
  CHECK(reader->next() == Status::Ok && reader->row_number() == 1);
  CHECK(reader->value(1) == "Alice");
}

void test_nested_values_rejected()
{
  Fixtures f;
  struct Case { const char *text; const char *message; };
  const Case cases[] = {
    {"{\"a\":{\"b\":1}}\n", "nested JSON object is not supported"},
    {"{\"a\":[1,2]}\n", "nested JSON array is not supported"},
    {"{\"a\":[]}\n", "nested JSON array is not supported"},
    {"{\"a\":{}}\n", "nested JSON object is not supported"},
  };
  for (const Case &item : cases) {
    const std::string path = f.put("nested.jsonl", item.text);
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Malformed);
    CHECK(contains(reader->error(), item.message));
    CHECK(contains(reader->error(), canonical(path).c_str()));
    CHECK(contains(reader->error(), "row 1, line 1"));
  }
  // A nested value on a later line reports that line, not the first one.
  const std::string path = f.put("later.jsonl", "{\"a\":1}\n\n{\"a\":{\"b\":2}}\n");
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Malformed);
  CHECK(contains(reader->error(), "row 2, line 3"));
  CHECK(contains(reader->error(), "nested JSON object is not supported"));
}

void test_escapes()
{
  Fixtures f;
  auto value_of = [&f](const std::string &contents, Status *status) -> std::string {
    const std::string path = f.put("escape.jsonl", contents);
    JsonOptions options;
    options.enabled = true;
    auto reader = std::make_unique<JsonlReader>();
    std::string value;
    *status = reader->open(path.c_str(), f.root.c_str(), options);
    if (*status == Status::Ok) *status = reader->infer();
    if (*status == Status::Ok) *status = reader->next();
    if (*status == Status::Ok) value.assign(reader->value(0));
    return value;
  };
  Status status = Status::Ok;
  CHECK(value_of("{\"s\":\"q\\\"b\\\\s\\/b\\b\\f\\n\\r\\t\"}\n", &status) == "q\"b\\s/b\b\f\n\r\t");
  CHECK(status == Status::Ok);
  CHECK(value_of("{\"s\":\"\\u00e9\"}\n", &status) == "\xc3\xa9");
  CHECK(status == Status::Ok);
  CHECK(value_of("{\"s\":\"\\u4e16\\u754c\"}\n", &status) == "\xe4\xb8\x96\xe7\x95\x8c");
  CHECK(status == Status::Ok);
  // Surrogate pair for U+1F600.
  CHECK(value_of("{\"s\":\"\\ud83d\\ude00\"}\n", &status) == "\xf0\x9f\x98\x80");
  CHECK(status == Status::Ok);
  CHECK(value_of("{\"s\":\"plain\"}\n", &status) == "plain");
  CHECK(status == Status::Ok);
  // Escaped keys are decoded before they are matched against the schema.
  {
    const std::string path = f.put("key.jsonl", "{\"a\\u0062\":1}\n{\"ab\":2}\n");
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Ok);
    CHECK(reader->column_count() == 1);
    CHECK(reader->next() == Status::Ok && reader->value(0) == "1");
    CHECK(reader->next() == Status::Ok && reader->value(0) == "2");
  }

  const char *const rejected[] = {
    "{\"s\":\"\\q\"}\n",         // unknown escape
    "{\"s\":\"\\u12\"}\n",       // short \u escape
    "{\"s\":\"\\u12g4\"}\n",     // non-hex \u escape
    "{\"s\":\"\\ud83d\"}\n",     // lone high surrogate
    "{\"s\":\"\\udc00\"}\n",     // lone low surrogate
    "{\"s\":\"\\ud83d\\u0041\"}\n", // high surrogate followed by a non-surrogate
    "{\"s\":\"\\u0000\"}\n",     // NUL
    "{\"s\":\"unterminated\n",   // missing quote
    "{\"s\":\"trailing\\\"\n",   // escaped quote then end of line
  };
  for (const char *contents : rejected) {
    const std::string path = f.put("bad.jsonl", contents);
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Malformed);
    CHECK(contains(reader->error(), "row 1, line 1"));
  }

  // Raw control bytes and invalid UTF-8 never reach the payload.
  std::string control = "{\"s\":\"a";
  control += '\x01';
  control += "\"}\n";
  std::string bad_utf8 = "{\"s\":\"";
  bad_utf8 += '\xc0';
  bad_utf8 += '\xaf';
  bad_utf8 += "\"}\n";
  std::string bad_key = "{\"a";
  bad_key += '\xff';
  bad_key += "\":1}\n";
  for (const std::string &contents : {control, bad_utf8, bad_key}) {
    const std::string path = f.put("bytes.jsonl", contents);
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Malformed);
  }
}

void test_duplicate_keys_last_wins()
{
  Fixtures f;
  auto read_one = [&f](const std::string &contents, const std::string &field) -> std::string {
    const std::string path = f.put("dup.jsonl", contents);
    auto reader = open_reader(path, f.root);
    if (reader->infer() != Status::Ok) return "<infer failed>";
    size_t index = 0;
    while (index < reader->column_count() && std::string(reader->column(index).name) != field) ++index;
    if (index == reader->column_count()) return "<missing column>";
    if (reader->next() != Status::Ok) return "<next failed>";
    return reader->is_null(index) ? std::string("<null>") : std::string(reader->value(index));
  };
  CHECK(read_one("{\"a\":1,\"a\":2}\n", "a") == "2");
  CHECK(read_one("{\"a\":2,\"a\":null}\n", "a") == "<null>");
  CHECK(read_one("{\"a\":null,\"a\":3}\n", "a") == "3");
  CHECK(read_one("{\"a\":true,\"a\":\"x\"}\n", "a") == "x");

  // The final occurrence also decides the type contributed to the union schema.
  {
    const std::string path = f.put("dup_type.jsonl", "{\"a\":\"x\",\"a\":2.5}\n");
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Ok);
    CHECK(reader->column_count() == 1);
    CHECK(reader->column(0).type == Type::Double);
    CHECK(reader->next() == Status::Ok && reader->value(0) == "2.5");
  }
}

void test_blank_lines_and_bom()
{
  Fixtures f;
  const std::string path = f.put("bom.jsonl",
      "\xef\xbb\xbf{\"a\":1}\n"
      "\n"
      "   \n"
      "\t\r\n"
      "{\"a\":2}\n"
      "\n");
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 1);
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->row_number() == 1);
  CHECK(reader->value(0) == "1");
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->row_number() == 2);
  CHECK(reader->value(0) == "2");
  CHECK(reader->next() == Status::End);

  // CRLF records behave like LF records.
  const std::string crlf = f.put("crlf.jsonl", "{\"a\":1}\r\n{\"a\":2}\r\n");
  auto crlf_reader = open_reader(crlf, f.root);
  CHECK(crlf_reader->infer() == Status::Ok);
  CHECK(crlf_reader->next() == Status::Ok && crlf_reader->value(0) == "1");
  CHECK(crlf_reader->next() == Status::Ok && crlf_reader->value(0) == "2");
  CHECK(crlf_reader->next() == Status::End);

  // A file with no records at all is not a valid table.
  const std::string blank = f.put("blank.jsonl", "\n \n\t\r\n");
  auto blank_reader = open_reader(blank, f.root);
  CHECK(blank_reader->infer() == Status::Malformed);
}

void test_missing_versus_explicit_null()
{
  Fixtures f;
  const std::string path = f.put("nulls.jsonl", "{\"a\":1,\"b\":null}\n{\"a\":2}\n");
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 2);
  CHECK(reader->column(1).type == Type::Null);
  CHECK(reader->column(1).nullable);
  CHECK(!reader->column(0).nullable);
  CHECK(reader->next() == Status::Ok);
  CHECK(!reader->is_null(0) && reader->is_null(1));
  CHECK(reader->next() == Status::Ok);
  CHECK(!reader->is_null(0) && reader->is_null(1));
  CHECK(reader->next() == Status::End);

  // An empty object still produces a row; every column is NULL for it.
  const std::string empty_object = f.put("empty_object.jsonl", "{}\n{\"a\":1}\n");
  auto empty_reader = open_reader(empty_object, f.root);
  CHECK(empty_reader->infer() == Status::Ok);
  CHECK(empty_reader->column_count() == 1);
  CHECK(empty_reader->column(0).nullable);
  CHECK(empty_reader->next() == Status::Ok && empty_reader->is_null(0));
  CHECK(empty_reader->next() == Status::Ok && empty_reader->value(0) == "1");
}

void test_number_typing()
{
  Fixtures f;
  struct Case { const std::string value; Type type; };
  const Case cases[] = {
    {"0", Type::BigInt},
    {"-0", Type::BigInt},
    {"1", Type::BigInt},
    {"-9223372036854775808", Type::BigInt},
    {"9223372036854775807", Type::BigInt},
    {"9223372036854775808", Type::Double},   // int64 overflow falls back to DOUBLE
    {"-9223372036854775809", Type::Double},
    {"1.0", Type::Double},
    {"1e3", Type::Double},
    {"1E3", Type::Double},
    {"-2.5e-3", Type::Double},
    {"1e999", Type::Varchar},                // not representable: keeps its text
  };
  for (const Case &item : cases) {
    const std::string path = f.put("number.jsonl", "{\"v\":" + item.value + "}\n");
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Ok);
    if (reader->column_count() != 1) continue;
    CHECK(reader->column(0).type == item.type);
    CHECK(reader->next() == Status::Ok);
    CHECK(reader->value(0) == item.value);
  }

  // Malformed numbers are rejected instead of being treated as text.
  const char *const rejected[] = {"{\"v\":01}\n", "{\"v\":+1}\n", "{\"v\":.5}\n", "{\"v\":5.}\n",
                                  "{\"v\":1e}\n", "{\"v\":-}\n", "{\"v\":1.2.3}\n"};
  for (const char *contents : rejected) {
    const std::string path = f.put("bad_number.jsonl", contents);
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Malformed);
  }
}

void test_string_typing()
{
  Fixtures f;
  const std::string path = f.put("strings.jsonl",
      "{\"a\":\"12\",\"b\":\"2026-08-02\",\"c\":\"2026-08-02 10:11:12\","
      "\"d\":\"true\",\"e\":\"1e3\",\"f\":\"\",\"g\":\"2023-02-29\"}\n");
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 7);
  // Only the two temporal spellings leave VARCHAR; a numeric-looking or
  // boolean-looking string keeps its text type.
  const Type types[] = {Type::Varchar, Type::Date, Type::DateTime, Type::Varchar,
                        Type::Varchar, Type::Varchar, Type::Varchar};
  for (size_t i = 0; i < reader->column_count(); ++i) CHECK(reader->column(i).type == types[i]);
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->value(0) == "12");
  CHECK(reader->value(4) == "1e3");

  // DATE and DATETIME widen to DATETIME across lines.
  const std::string wide = f.put("date.jsonl",
      "{\"d\":\"2026-08-02\"}\n{\"d\":\"2026-08-03 00:00:00\"}\n");
  auto wide_reader = open_reader(wide, f.root);
  CHECK(wide_reader->infer() == Status::Ok);
  CHECK(wide_reader->column(0).type == Type::DateTime);
}

void test_case_insensitive_union()
{
  Fixtures f;
  const std::string path = f.put("case.jsonl", "{\"Name\":\"a\"}\n{\"NAME\":\"b\"}\n");
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 1);
  CHECK(std::string(reader->column(0).name) == "Name"); // first spelling wins
  CHECK(reader->next() == Status::Ok && reader->value(0) == "a");
  CHECK(reader->next() == Status::Ok && reader->value(0) == "b");

  // Duplicate keys differing only in case stay one column; the last one wins.
  const std::string duplicate = f.put("case_dup.jsonl", "{\"Name\":\"a\",\"NAME\":\"b\"}\n");
  auto duplicate_reader = open_reader(duplicate, f.root);
  CHECK(duplicate_reader->infer() == Status::Ok);
  CHECK(duplicate_reader->column_count() == 1);
  CHECK(duplicate_reader->next() == Status::Ok && duplicate_reader->value(0) == "b");

  const std::string widened = f.put("case_widen.jsonl", "{\"Score\":1}\n{\"score\":2.5}\n");
  auto widened_reader = open_reader(widened, f.root);
  CHECK(widened_reader->infer() == Status::Ok);
  CHECK(widened_reader->column_count() == 1);
  CHECK(widened_reader->column(0).type == Type::Double);
}

void test_syntax_errors()
{
  Fixtures f;
  const char *const rejected[] = {
    "1\n",                            // not an object
    "[1,2]\n",                        // array record
    "\"text\"\n",                     // string record
    "{\"a\":1} trailing\n",           // trailing garbage
    "{\"a\":1\n",                     // unterminated object
    "{\"a\":1 \"b\":2}\n",            // missing comma
    "{\"a\":1,}\n",                   // trailing comma
    "{\"a\" 1}\n",                    // missing colon
    "{\"a\":}\n",                     // missing value
    "{a:1}\n",                        // unquoted key
    "{,}\n",                          // leading comma
    "{\"a\":1}}\n",                   // extra brace
  };
  for (const char *contents : rejected) {
    const std::string path = f.put("syntax.jsonl", contents);
    auto reader = open_reader(path, f.root);
    CHECK(reader->infer() == Status::Malformed);
    CHECK(contains(reader->error(), canonical(path).c_str()));
  }
}

void test_rescan_repeats_rows()
{
  Fixtures f;
  const std::string path = f.put("rescan.jsonl", BASIC_JSONL);
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  size_t rows = 0;
  while (reader->next() == Status::Ok) ++rows;
  CHECK(rows == 3);
  CHECK(reader->next() == Status::End);
  CHECK(reader->rescan() == Status::Ok);
  CHECK(reader->row_number() == 0);
  rows = 0;
  while (reader->next() == Status::Ok) ++rows;
  CHECK(rows == 3);
  CHECK(reader->rescan() == Status::Ok);
  CHECK(reader->next() == Status::Ok && reader->value(0) == "1");
}

void test_bind_schema_handoff()
{
  Fixtures f;
  const std::string path = f.put("handoff.jsonl", BASIC_JSONL);
  std::vector<JsonlReader::Column> columns;
  FileIdentity identity;
  {
    auto resolver = open_reader(path, f.root);
    CHECK(resolver->infer() == Status::Ok);
    identity = resolver->identity();
    for (size_t i = 0; i < resolver->column_count(); ++i) columns.push_back(resolver->column(i));
    CHECK(columns.size() == 6);
  } // execution must not depend on resolver-owned state
  auto executor = std::make_unique<JsonlReader>();
  CHECK(executor->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(executor->bind_schema(columns.data(), columns.size(), identity) == Status::Ok);
  CHECK(executor->column_count() == 6);
  CHECK(executor->next() == Status::Ok && executor->value(1) == "Alice");
  CHECK(executor->next() == Status::Ok && executor->is_null(2));
  CHECK(executor->next() == Status::Ok && executor->value(2) == "90");
  CHECK(executor->next() == Status::End);
  CHECK(executor->rescan() == Status::Ok);
  CHECK(executor->next() == Status::Ok && executor->value(0) == "1");

  // A planned schema that omits a key present in the file is a mismatch.
  std::vector<JsonlReader::Column> narrowed(columns.begin(), columns.begin() + 1);
  auto mismatch = std::make_unique<JsonlReader>();
  CHECK(mismatch->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(mismatch->bind_schema(narrowed.data(), narrowed.size(), identity) == Status::Ok);
  CHECK(mismatch->next() == Status::Malformed);
  CHECK(contains(mismatch->error(), "absent from the planned schema"));

  // Structurally invalid planned schemas are rejected outright.
  std::vector<JsonlReader::Column> invalid(columns.begin(), columns.begin() + 2);
  invalid[0].name[0] = 0;
  auto bad_name = std::make_unique<JsonlReader>();
  CHECK(bad_name->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(bad_name->bind_schema(invalid.data(), invalid.size(), identity) == Status::InvalidOption);
  invalid = std::vector<JsonlReader::Column>(columns.begin(), columns.begin() + 2);
  invalid[1].type = static_cast<Type>(99);
  auto bad_type = std::make_unique<JsonlReader>();
  CHECK(bad_type->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(bad_type->bind_schema(invalid.data(), invalid.size(), identity) == Status::InvalidOption);
  // Two planned columns that differ only in case would be ambiguous.
  invalid = std::vector<JsonlReader::Column>(columns.begin(), columns.begin() + 2);
  std::strcpy(invalid[1].name, "ID");
  auto duplicate_name = std::make_unique<JsonlReader>();
  CHECK(duplicate_name->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(duplicate_name->bind_schema(invalid.data(), invalid.size(), identity) == Status::InvalidOption);
  // No schema at all is invalid too.
  auto no_columns = std::make_unique<JsonlReader>();
  CHECK(no_columns->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(no_columns->bind_schema(columns.data(), 0, identity) == Status::InvalidOption);
}

void test_identity_mismatch_and_rewrite()
{
  Fixtures f;
  const std::string path = f.put("grow.jsonl", "{\"a\":1}\n");
  auto reader = open_reader(path, f.root);
  CHECK(reader->infer() == Status::Ok);
  {
    std::ofstream stream(path, std::ios::app | std::ios::binary);
    stream << "{\"a\":2}\n";
  }
  CHECK(reader->next() == Status::Changed);
  CHECK(reader->rescan() == Status::Changed);

  // Same-size replacement detected through a different inode.
  const std::string original = f.put("same_size.jsonl", "{\"a\":1}\n");
  const std::string replacement = f.put("same_size_new.jsonl", "{\"a\":2}\n");
  auto stale = std::make_unique<JsonlReader>();
  CHECK(stale->open(original.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(stale->infer() == Status::Ok);
  const FileIdentity identity = stale->identity();
  std::vector<JsonlReader::Column> columns{stale->column(0)};
  CHECK(original.compare(0, f.root.size() + 1, f.root + "/") == 0);
  CHECK(replacement.compare(0, f.root.size() + 1, f.root + "/") == 0);
  CHECK(std::rename(replacement.c_str(), original.c_str()) == 0);
  auto planned = std::make_unique<JsonlReader>();
  CHECK(planned->open(original.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(planned->bind_schema(columns.data(), columns.size(), identity) == Status::Changed);
  CHECK(planned->next() == Status::Changed);
}

void test_secure_open_failures()
{
  Fixtures f;
  Fixtures outside;
  const std::string path = f.put("secure.jsonl", "{\"a\":1}\n");
  const std::string outside_path = outside.put("secret.jsonl", "{\"a\":2}\n");
  JsonOptions options;
  auto reader = std::make_unique<JsonlReader>();

  CHECK(reader->open(path.c_str(), f.root.c_str(), options) == Status::Disabled);
  options.enabled = true;
  CHECK(reader->open(path.c_str(), "", options) == Status::Denied);
  CHECK(reader->open(path.c_str(), "null", options) == Status::Denied);
  CHECK(reader->open(path.c_str(), "NULL", options) == Status::Denied);
  CHECK(reader->open(path.c_str(), nullptr, options) == Status::Denied);
  CHECK(reader->open(nullptr, f.root.c_str(), options) == Status::Denied);
  CHECK(reader->open(outside_path.c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(reader->open(f.root.c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(reader->open((f.root + "/missing.jsonl").c_str(), f.root.c_str(), options) == Status::Denied);
  const std::string link = f.link("escape.jsonl", outside_path);
  CHECK(reader->open(link.c_str(), f.root.c_str(), options) == Status::Denied);
  options.sample_size = 0;
  CHECK(reader->open(path.c_str(), f.root.c_str(), options) == Status::Unsupported);
  options.sample_size = -1;
  options.max_record_bytes = 0;
  CHECK(reader->open(path.c_str(), f.root.c_str(), options) == Status::InvalidOption);
  options.max_record_bytes = 16 * 1024 * 1024 + 1;
  CHECK(reader->open(path.c_str(), f.root.c_str(), options) == Status::InvalidOption);
  options.max_record_bytes = 16 * 1024 * 1024;
  CHECK(reader->open(path.c_str(), f.root.c_str(), options) == Status::Ok);
}

void test_empty_and_limits()
{
  Fixtures f;
  const std::string empty = f.put("empty.jsonl", "");
  auto reader = std::make_unique<JsonlReader>();
  CHECK(reader->open(empty.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Malformed);

  // A record larger than max_record_bytes fails instead of growing without bound.
  std::string big = "{\"a\":\"";
  big.append(200, 'x');
  big += "\"}\n";
  const std::string big_path = f.put("big.jsonl", big);
  CHECK(reader->open(big_path.c_str(), f.root.c_str(), JsonOptions{true, -1, 64}) == Status::Ok);
  CHECK(reader->infer() == Status::ResourceLimit);
  CHECK(contains(reader->error(), "row 1, line 1"));

  // A record of exactly max_record_bytes is still accepted.
  const std::string exact = f.put("exact.jsonl", "{\"a\":1}\n");
  CHECK(reader->open(exact.c_str(), f.root.c_str(), JsonOptions{true, -1, 7}) == Status::Ok);
  CHECK(reader->infer() == Status::Ok);

  // Keys longer than MAX_NAME_BYTES and empty keys are refused.
  const std::string long_key = f.put("long_key.jsonl",
      "{\"" + std::string(JsonlReader::MAX_NAME_BYTES + 1, 'k') + "\":1}\n");
  CHECK(reader->open(long_key.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::ResourceLimit);
  const std::string max_key = f.put("max_key.jsonl",
      "{\"" + std::string(JsonlReader::MAX_NAME_BYTES, 'k') + "\":1}\n");
  CHECK(reader->open(max_key.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::Ok);
  const std::string empty_key = f.put("empty_key.jsonl", "{\"\":1}\n");
  CHECK(reader->open(empty_key.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::ResourceLimit);
}

std::string wide_record(size_t keys, size_t rows)
{
  std::string text;
  text.reserve(rows * keys * 8 + 16);
  for (size_t r = 0; r < rows; ++r) {
    text += '{';
    for (size_t k = 0; k < keys; ++k) {
      if (k != 0) text += ',';
      text += "\"k";
      text += std::to_string(k);
      text += "\":1";
    }
    text += "}\n";
  }
  return text;
}

void test_column_limit()
{
  Fixtures f;
  auto reader = std::make_unique<JsonlReader>();
  const std::string at_limit = f.put("at_limit.jsonl", wide_record(JsonlReader::MAX_COLUMNS, 1));
  CHECK(reader->open(at_limit.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == JsonlReader::MAX_COLUMNS);
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->next() == Status::End);
  const std::string over_limit = f.put("over_limit.jsonl", wide_record(JsonlReader::MAX_COLUMNS + 1, 1));
  CHECK(reader->open(over_limit.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::ResourceLimit);
}

void test_streaming_large_input()
{
  Fixtures f;
  // Total input is several times the per-record cap: only one line is ever held.
  std::string text;
  const size_t rows = 20000;
  const std::string padding(200, 'p');
  text.reserve(rows * 240);
  for (size_t i = 0; i < rows; ++i) {
    text += "{\"i\":" + std::to_string(i) + ",\"pad\":\"" + padding + "\"}\n";
  }
  const std::string path = f.put("large.jsonl", text);
  const size_t cap = 1024 * 1024;
  CHECK(text.size() > 3 * cap);
  auto reader = std::make_unique<JsonlReader>();
  CHECK(reader->open(path.c_str(), f.root.c_str(), JsonOptions{true, -1, cap}) == Status::Ok);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 2);
  size_t seen = 0;
  while (reader->next() == Status::Ok) {
    if (seen == 0) CHECK(reader->value(0) == "0");
    ++seen;
  }
  CHECK(seen == rows);
  CHECK(reader->rescan() == Status::Ok);
  CHECK(reader->next() == Status::Ok && reader->value(0) == "0");
}

void test_reader_reuse_and_lifetime()
{
  Fixtures f;
  const std::string first = f.put("first.jsonl", "{\"a\":1}\n");
  const std::string second = f.put("second.jsonl", "{\"b\":\"x\"}\n");
  auto reader = std::make_unique<JsonlReader>();
  CHECK(reader->open(first.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->next() == Status::Ok);
  // Re-opening the same object starts from a clean state.
  CHECK(reader->open(second.c_str(), f.root.c_str(), JsonOptions{true, -1, 16 * 1024 * 1024}) == Status::Ok);
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 1);
  CHECK(std::string(reader->column(0).name) == "b");
  CHECK(reader->next() == Status::Ok && reader->value(0) == "x");
  reader->close();
  CHECK(reader->column_count() == 0);
  CHECK(reader->next() == Status::InvalidOption);
  // Calls before open() fail instead of touching unopened resources.
  auto fresh = std::make_unique<JsonlReader>();
  CHECK(fresh->infer() == Status::InvalidOption);
  CHECK(fresh->rescan() == Status::InvalidOption);
  CHECK(fresh->next() == Status::InvalidOption);
  CHECK(fresh->identity() == FileIdentity{});
}

} // namespace

int main()
{
  test_public_basic_fixture();
  test_nested_values_rejected();
  test_escapes();
  test_duplicate_keys_last_wins();
  test_blank_lines_and_bom();
  test_missing_versus_explicit_null();
  test_number_typing();
  test_string_typing();
  test_case_insensitive_union();
  test_syntax_errors();
  test_rescan_repeats_rows();
  test_bind_schema_handoff();
  test_identity_mismatch_and_rewrite();
  test_secure_open_failures();
  test_empty_and_limits();
  test_column_limit();
  test_streaming_large_input();
  test_reader_reuse_and_lifetime();
  if (failures != 0) std::cerr << failures << " of " << checks << " checks failed\n";
  else std::cout << "json_file_reader: " << checks << " checks passed\n";
  return failures == 0 ? 0 : 1;
}
