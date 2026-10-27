/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_sql_export.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <memory>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace oceanbase::sql::file_sql;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)

namespace {

std::string read_file(const std::string &path)
{
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) return std::string("<unreadable>");
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool path_exists(const std::string &path) { return 0 == ::access(path.c_str(), F_OK); }

std::string canonical(const std::string &path)
{
  char buffer[PATH_MAX];
  if (!realpath(path.c_str(), buffer)) return std::string();
  return std::string(buffer);
}

// Every entry currently in a directory; used to prove no temp/target residue is left.
std::vector<std::string> list_dir(const std::string &path)
{
  std::vector<std::string> entries;
  DIR *dir = opendir(path.c_str());
  if (!dir) return entries;
  while (struct dirent *entry = readdir(dir)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    entries.push_back(entry->d_name);
  }
  closedir(dir);
  return entries;
}

CsvOptions reader_options(bool header, char delimiter = ',', char quote = '"', char escape = '"',
                          const char *null_text = "NULL")
{
  CsvOptions options;
  options.enabled = true;
  options.header = header;
  options.delimiter = delimiter;
  options.quote = quote;
  options.escape = escape;
  options.null_text = null_text;
  return options;
}

} // namespace

struct Fixtures {
  std::string root;
  Fixtures()
  {
    char name[] = "/tmp/seekdb-file-export-XXXXXX";
    char *created = mkdtemp(name);
    if (!created) std::abort();
    root = created;
  }
  std::string path(const std::string &name) const { return root + "/" + name; }
  std::string put(const std::string &name, const std::string &contents) const
  {
    std::string target = path(name);
    std::ofstream stream(target, std::ios::binary);
    stream << contents;
    if (!stream.good()) std::abort();
    return target;
  }
  ~Fixtures()
  {
    // Only touch paths inside this unique mkdtemp directory.
    if (root.find("/tmp/seekdb-file-export-") != 0) std::abort();
    for (const auto &entry : list_dir(root)) {
      const std::string full = root + "/" + entry;
      if (full.find(root + "/") != 0) std::abort();
      if (0 != ::unlink(full.c_str())) ::rmdir(full.c_str());
    }
    rmdir(root.c_str());
  }
};

void test_default_options_commit_exact_bytes()
{
  Fixtures f;
  CsvWriteOptions options;
  CsvWriter writer;
  CHECK(writer.open(f.path("out.csv").c_str(), f.root.c_str(), options) == Status::Ok);
  CHECK(writer.is_open());
  CHECK(!writer.is_committed());
  CHECK(std::string(writer.target_path()) == canonical(f.root) + "/out.csv");
  const std::string_view names[] = {"id", "name", "note"};
  CHECK(writer.write_header(names, 3) == Status::Ok);
  const std::string_view row1[] = {"1", "alice", ""};
  const bool null1[] = {false, false, true};
  CHECK(writer.write_row(row1, null1, 3) == Status::Ok);
  const std::string_view row2[] = {"2", "", ""};
  const bool null2[] = {false, false, true};
  CHECK(writer.write_row(row2, null2, 3) == Status::Ok);
  CHECK(writer.row_count() == 2);
  CHECK(writer.commit() == Status::Ok);
  CHECK(writer.is_committed());
  CHECK(!writer.is_open());
  CHECK(writer.error()[0] == '\0');
  CHECK(read_file(f.path("out.csv")) == std::string("id,name,note\n1,alice,NULL\n2,\"\",NULL\n"));
  CHECK(list_dir(f.root) == std::vector<std::string>{"out.csv"});
}

void test_custom_options_round_trip()
{
  Fixtures f;
  CsvWriteOptions options;
  options.header = false;
  options.delimiter = ';';
  options.quote = '\'';
  options.escape = '\\';
  options.null_text = "NIL";
  options.line_terminator = "\r\n";
  CsvWriter writer;
  CHECK(writer.open(f.path("custom.csv").c_str(), f.root.c_str(), options) == Status::Ok);
  const std::string_view row1[] = {"a;b", "it's", "", ""};
  const bool null1[] = {false, false, false, true};
  CHECK(writer.write_row(row1, null1, 4) == Status::Ok);
  const std::string_view row2[] = {"NIL", "x\\y", "line\nbreak", ""};
  const bool null2[] = {false, false, false, true};
  CHECK(writer.write_row(row2, null2, 4) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);
  const std::string expected = std::string("'a;b';'it\\'s';'';NIL\r\n")
                             + std::string("'NIL';'x\\\\y';'line\nbreak';NIL\r\n");
  CHECK(read_file(f.path("custom.csv")) == expected);

  CsvOptions read_options = reader_options(false, ';', '\'', '\\', "NIL");
  CsvReader reader;
  CHECK(reader.open(f.path("custom.csv").c_str(), f.root.c_str(), read_options) == Status::Ok);
  CHECK(reader.infer() == Status::Ok);
  CHECK(reader.column_count() == 4);
  CHECK(reader.next() == Status::Ok);
  CHECK(reader.value(0) == "a;b");
  CHECK(reader.value(1) == "it's");
  CHECK(!reader.is_null(2) && reader.value(2).empty());
  CHECK(reader.is_null(3));
  CHECK(reader.next() == Status::Ok);
  CHECK(!reader.is_null(0) && reader.value(0) == "NIL");
  CHECK(reader.value(1) == "x\\y");
  CHECK(reader.value(2) == "line\nbreak");
  CHECK(reader.is_null(3));
  CHECK(reader.next() == Status::End);
  reader.close();
}

void test_null_versus_empty_round_trip()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("nulls.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"empty_string", "null_value"};
  CHECK(writer.write_header(names, 2) == Status::Ok);
  const std::string_view row[] = {"", ""};
  const bool nulls[] = {false, true};
  CHECK(writer.write_row(row, nulls, 2) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);
  CHECK(read_file(f.path("nulls.csv")) == std::string("empty_string,null_value\n\"\",NULL\n"));

  CsvReader reader;
  CHECK(reader.open(f.path("nulls.csv").c_str(), f.root.c_str(), reader_options(true)) == Status::Ok);
  CHECK(reader.infer() == Status::Ok);
  CHECK(reader.next() == Status::Ok);
  CHECK(!reader.is_null(0));
  CHECK(reader.value(0).empty());
  CHECK(reader.is_null(1));
  CHECK(std::string(reader.column(0).name) == "empty_string");
  CHECK(reader.column(0).type == Type::Varchar);
  CHECK(reader.column(1).type == Type::Null);
}

void test_special_characters_round_trip()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("special.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"comma", "quote", "cr", "lf", "utf8", "plain"};
  CHECK(writer.write_header(names, 6) == Status::Ok);
  const std::string_view row[] = {"a,b", "say \"hi\"", "cr\rvalue", "lf\nvalue", "世界", "plain"};
  const bool nulls[] = {false, false, false, false, false, false};
  CHECK(writer.write_row(row, nulls, 6) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);
  CHECK(read_file(f.path("special.csv")) == std::string(
      "comma,quote,cr,lf,utf8,plain\n"
      "\"a,b\",\"say \"\"hi\"\"\",\"cr\rvalue\",\"lf\nvalue\",世界,plain\n"));

  CsvReader reader;
  CHECK(reader.open(f.path("special.csv").c_str(), f.root.c_str(), reader_options(true)) == Status::Ok);
  CHECK(reader.infer() == Status::Ok);
  CHECK(reader.next() == Status::Ok);
  CHECK(reader.value(0) == "a,b");
  CHECK(reader.value(1) == "say \"hi\"");
  CHECK(reader.value(2) == "cr\rvalue");
  CHECK(reader.value(3) == "lf\nvalue");
  CHECK(reader.value(4) == "世界");
  CHECK(reader.value(5) == "plain");
  CHECK(reader.next() == Status::End);
}

void test_force_quote()
{
  Fixtures f;
  CsvWriteOptions options;
  options.force_quote = true;
  options.line_terminator = "\r\n";
  CsvWriter writer;
  CHECK(writer.open(f.path("forced.csv").c_str(), f.root.c_str(), options) == Status::Ok);
  const std::string_view names[] = {"a", "b", "c"};
  CHECK(writer.write_header(names, 3) == Status::Ok);
  const std::string_view row[] = {"plain", "", "1"};
  const bool nulls[] = {false, true, false};
  CHECK(writer.write_row(row, nulls, 3) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);
  CHECK(read_file(f.path("forced.csv")) == std::string("\"a\",\"b\",\"c\"\r\n\"plain\",NULL,\"1\"\r\n"));
}

void test_existing_target_is_preserved()
{
  Fixtures f;
  const std::string original = "original contents\n";
  f.put("taken.csv", original);
  CsvWriter writer;
  CHECK(writer.open(f.path("taken.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Changed);
  CHECK(!writer.is_open());
  CHECK(std::string(writer.error()).find("export target already exists") != std::string::npos);
  CHECK(read_file(f.path("taken.csv")) == original);
  CHECK(list_dir(f.root) == std::vector<std::string>{"taken.csv"});

  CHECK(::mkdir(f.path("taken_dir").c_str(), 0700) == 0);
  CsvWriter directory_writer;
  CHECK(directory_writer.open(f.path("taken_dir").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Changed);
  CHECK(std::string(directory_writer.error()).find("export target already exists") != std::string::npos);
  CHECK(list_dir(f.path("taken_dir")).empty());
}

void test_abort_and_destructor_leave_no_residue()
{
  {
    Fixtures f;
    CsvWriter writer;
    CHECK(writer.open(f.path("aborted.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
    const std::string_view names[] = {"a", "b"};
    CHECK(writer.write_header(names, 2) == Status::Ok);
    const std::string_view row[] = {"1", "2"};
    const bool nulls[] = {false, false};
    CHECK(writer.write_row(row, nulls, 2) == Status::Ok);
    const auto mid_write = list_dir(f.root);
    // Only the writer's own temp file exists while the export is in flight: the
    // target name is never claimed before publication, so there is no empty
    // placeholder another process could swap for its own file.
    CHECK(mid_write.size() == 1);
    CHECK(mid_write.size() == 1 && mid_write[0].find(".aborted.csv.seekdb-tmp.") == 0);
    CHECK(!path_exists(f.path("aborted.csv")));
    writer.abort();
    CHECK(list_dir(f.root).empty());
    writer.abort();  // idempotent
    CHECK(list_dir(f.root).empty());
    const std::string_view late[] = {"1", "2"};
    CHECK(writer.write_row(late, nulls, 2) == Status::InvalidOption);
    CHECK(!writer.is_open());
  }
  {
    Fixtures f;
    const std::string_view names[] = {"a", "b"};
    const std::string_view row[] = {"1", "2"};
    const bool nulls[] = {false, false};
    {
      CsvWriter writer;
      CHECK(writer.open(f.path("dropped.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
      CHECK(writer.write_header(names, 2) == Status::Ok);
      CHECK(writer.write_row(row, nulls, 2) == Status::Ok);
    }
    CHECK(list_dir(f.root).empty());
    CHECK(!path_exists(f.path("dropped.csv")));
  }
  {
    Fixtures f;
    const std::string_view names[] = {"a", "b"};
    const std::string_view row[] = {"1", "2"};
    const bool nulls[] = {false, false};
    {
      CsvWriter writer;
      CHECK(writer.open(f.path("kept.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
      CHECK(writer.write_header(names, 2) == Status::Ok);
      CHECK(writer.write_row(row, nulls, 2) == Status::Ok);
      CHECK(writer.commit() == Status::Ok);
      writer.abort();  // abort after commit must not remove the published file
    }
    CHECK(list_dir(f.root) == std::vector<std::string>{"kept.csv"});
    CHECK(read_file(f.path("kept.csv")) == std::string("a,b\n1,2\n"));
  }
}

void test_streaming_large_output()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("big.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"id", "payload"};
  CHECK(writer.write_header(names, 2) == Status::Ok);
  std::string expected = "id,payload\n";
  const size_t rows = 6000;
  for (size_t i = 0; i < rows; ++i) {
    const std::string id = std::to_string(i);
    const std::string payload(80, static_cast<char>('a' + (i % 26)));
    const std::string_view row[] = {id, payload};
    const bool nulls[] = {false, false};
    CHECK(writer.write_row(row, nulls, 2) == Status::Ok);
    expected += id + "," + payload + "\n";
  }
  CHECK(writer.row_count() == rows);
  CHECK(writer.commit() == Status::Ok);
  CHECK(expected.size() > 4 * 64 * 1024);  // exercises several buffer flushes
  CHECK(read_file(f.path("big.csv")) == expected);
}

void test_read_back_through_csv_reader()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("types.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"id", "name", "score", "created", "updated", "flag", "note"};
  CHECK(writer.write_header(names, 7) == Status::Ok);
  const std::string_view row1[] = {"1", "alice", "3.5", "2026-01-02", "2026-01-02 03:04:05", "true", ""};
  const bool null1[] = {false, false, false, false, false, false, true};
  CHECK(writer.write_row(row1, null1, 7) == Status::Ok);
  const std::string_view row2[] = {"2", "", "4", "2026-01-03", "2026-01-03 04:05:06", "false", "x,y"};
  const bool null2[] = {false, false, false, false, false, false, false};
  CHECK(writer.write_row(row2, null2, 7) == Status::Ok);
  const std::string_view row3[] = {"3", "世界", "5.25", "2026-01-04", "2026-01-04 05:06:07", "false", "NULL"};
  const bool null3[] = {false, false, false, false, false, false, false};
  CHECK(writer.write_row(row3, null3, 7) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);

  CsvReader reader;
  CHECK(reader.open(f.path("types.csv").c_str(), f.root.c_str(), reader_options(true)) == Status::Ok);
  CHECK(reader.infer() == Status::Ok);
  CHECK(reader.column_count() == 7);
  CHECK(std::string(reader.column(0).name) == "id");
  CHECK(reader.column(0).type == Type::BigInt);
  CHECK(reader.column(1).type == Type::Varchar);
  CHECK(reader.column(2).type == Type::Double);
  CHECK(reader.column(3).type == Type::Date);
  CHECK(reader.column(4).type == Type::DateTime);
  CHECK(reader.column(5).type == Type::Boolean);
  CHECK(reader.column(6).type == Type::Varchar);
  CHECK(reader.column(6).nullable);
  CHECK(reader.next() == Status::Ok);
  CHECK(reader.value(0) == "1");
  CHECK(reader.value(1) == "alice");
  CHECK(reader.value(2) == "3.5");
  CHECK(reader.is_null(6));
  CHECK(reader.next() == Status::Ok);
  CHECK(!reader.is_null(1) && reader.value(1).empty());
  CHECK(reader.value(2) == "4");
  CHECK(reader.value(6) == "x,y");
  CHECK(reader.next() == Status::Ok);
  CHECK(reader.value(1) == "世界");
  // A non-NULL value equal to the null token must stay a value on read-back.
  CHECK(!reader.is_null(6));
  CHECK(reader.value(6) == "NULL");
  CHECK(reader.next() == Status::End);
  CHECK(reader.row_number() == 4);
}

void test_security_denials()
{
  Fixtures f;
  CsvWriteOptions options;
  const std::string target = f.path("denied.csv");

  CsvWriter writer;
  CHECK(writer.open(target.c_str(), nullptr, options) == Status::Denied);
  CHECK(std::string(writer.error()).find("secure_file_priv is not configured") != std::string::npos);
  CHECK(writer.open(target.c_str(), "", options) == Status::Denied);
  CHECK(std::string(writer.error()).find("secure_file_priv is not configured") != std::string::npos);
  CHECK(writer.open(target.c_str(), "NULL", options) == Status::Denied);

  CHECK(writer.open(f.path("../escape.csv").c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(writer.open((f.root + "/sub/../escape.csv").c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(writer.open("/etc/seekdb-export-denied.csv", f.root.c_str(), options) == Status::Denied);
  CHECK(writer.open("/", f.root.c_str(), options) == Status::Denied);
  CHECK(writer.open((f.root + "/").c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(writer.open(nullptr, f.root.c_str(), options) == Status::Denied);

  // A symlink anywhere on the path is refused, even when it points inside the root.
  CHECK(::symlink(f.root.c_str(), f.path("root_link").c_str()) == 0);
  CHECK(writer.open((f.path("root_link") + "/via_link.csv").c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(::symlink("/etc", f.path("etc_link").c_str()) == 0);
  CHECK(writer.open((f.path("etc_link") + "/via_link.csv").c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(::symlink(f.path("real.csv").c_str(), f.path("final_link.csv").c_str()) == 0);
  CHECK(writer.open(f.path("final_link.csv").c_str(), f.root.c_str(), options) == Status::Denied);
  CHECK(!path_exists(f.path("real.csv")));
  CHECK(list_dir(f.root).size() == 3);

  // A path too long is refused before any filesystem access.
  std::string too_long = f.root + "/";
  while (too_long.size() < 4096) too_long.push_back('a');
  CHECK(writer.open(too_long.c_str(), f.root.c_str(), options) == Status::Denied);

  // An embedded NUL cannot smuggle a path: the C string ends at the NUL byte.
  std::string nul_path = f.path("nul.csv");
  nul_path.push_back('\0');
  nul_path += "../../etc/passwd";
  CsvWriter nul_writer;
  CHECK(nul_writer.open(nul_path.c_str(), f.root.c_str(), options) == Status::Ok);
  CHECK(std::string(nul_writer.target_path()) == canonical(f.root) + "/nul.csv");
  const std::string_view nul_header[] = {"a"};
  CHECK(nul_writer.write_header(nul_header, 1) == Status::Ok);
  const std::string_view nul_row[] = {"1"};
  const bool nul_nulls[] = {false};
  CHECK(nul_writer.write_row(nul_row, nul_nulls, 1) == Status::Ok);
  CHECK(nul_writer.commit() == Status::Ok);
  CHECK(read_file(f.path("nul.csv")) == std::string("a\n1\n"));
}

void test_invalid_options()
{
  Fixtures f;
  const std::string target = f.path("options.csv");
  CsvWriter writer;
  CsvWriteOptions options;
  options.delimiter = '\0';
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::InvalidOption);

  options = CsvWriteOptions();
  options.delimiter = '\n';
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::InvalidOption);

  options = CsvWriteOptions();
  options.quote = '\r';
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::InvalidOption);

  options = CsvWriteOptions();
  options.escape = static_cast<char>(128);
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::InvalidOption);

  options = CsvWriteOptions();
  options.delimiter = options.quote;
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::InvalidOption);

  options = CsvWriteOptions();
  options.delimiter = options.escape;
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::InvalidOption);

  options = CsvWriteOptions();
  options.quote = options.escape;
  options.delimiter = ';';
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::Ok);  // doubling is legal
  writer.abort();
  CHECK(list_dir(f.root).empty());

  options = CsvWriteOptions();
  options.enabled = false;
  CHECK(writer.open(target.c_str(), f.root.c_str(), options) == Status::Disabled);
  CHECK(list_dir(f.root).empty());
}

void test_invalid_usage()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.commit() == Status::InvalidOption);  // never opened
  CHECK(writer.open(f.path("usage.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"a", "b"};
  const std::string_view row[] = {"1", "2"};
  const bool nulls[] = {false, false};
  const std::string_view row_short[] = {"1"};
  CHECK(writer.write_row(row, nulls, 2) == Status::InvalidOption);  // header required first
  CHECK(writer.commit() == Status::InvalidOption);                  // header still required at commit
  CHECK(writer.write_header(nullptr, 2) == Status::InvalidOption);
  CHECK(writer.write_header(names, 0) == Status::InvalidOption);
  std::vector<std::string_view> too_many(CsvWriter::MAX_HEADER_COLUMNS + 1, "c");
  CHECK(writer.write_header(too_many.data(), too_many.size()) == Status::InvalidOption);
  CHECK(writer.write_header(names, 2) == Status::Ok);
  CHECK(writer.write_header(names, 2) == Status::InvalidOption);
  CHECK(writer.write_row(row_short, nulls, 1) == Status::InvalidOption);
  CHECK(writer.commit() == Status::Ok);
  CHECK(writer.commit() == Status::InvalidOption);
  CHECK(writer.write_row(row, nulls, 2) == Status::InvalidOption);
  CHECK(writer.write_header(names, 2) == Status::InvalidOption);
  CHECK(read_file(f.path("usage.csv")) == std::string("a,b\n"));

  CsvWriteOptions no_header_options;
  no_header_options.header = false;
  CsvWriter no_header;
  CHECK(no_header.open(f.path("noheader.csv").c_str(), f.root.c_str(), no_header_options) == Status::Ok);
  CHECK(no_header.write_header(names, 2) == Status::InvalidOption);
  CHECK(no_header.write_row(row, nulls, 2) == Status::Ok);
  CHECK(no_header.commit() == Status::Ok);
  CHECK(read_file(f.path("noheader.csv")) == std::string("1,2\n"));
}

void test_oversized_field()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("big_field.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"payload"};
  CHECK(writer.write_header(names, 1) == Status::Ok);
  const std::string huge(CsvWriter::MAX_FIELD_BYTES + 1, 'x');
  const std::string_view row[] = {huge};
  const bool nulls[] = {false};
  CHECK(writer.write_row(row, nulls, 1) == Status::ResourceLimit);
  const std::string biggest(CsvWriter::MAX_FIELD_BYTES, 'y');
  const std::string_view ok_row[] = {biggest};
  CHECK(writer.write_row(ok_row, nulls, 1) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);
  const std::string big_expected = std::string("payload\n") + biggest + "\n";
  CHECK(read_file(f.path("big_field.csv")) == big_expected);
}

void test_io_failures()
{
  {
    // A directory that denies creation: open() cannot reserve the target.
    Fixtures f;
    const std::string locked = f.path("locked");
    CHECK(::mkdir(locked.c_str(), 0700) == 0);
    CHECK(::chmod(locked.c_str(), 0500) == 0);
    CsvWriter writer;
    const Status status = writer.open((locked + "/out.csv").c_str(), f.root.c_str(), CsvWriteOptions());
    CHECK(status == Status::Io);
    CHECK(!writer.is_open());
    CHECK(::chmod(locked.c_str(), 0700) == 0);
    CHECK(list_dir(locked).empty());
    CHECK(::rmdir(locked.c_str()) == 0);
  }
  {
    // A failing output stream: exceed RLIMIT_FSIZE so the flush reports Status::Io.
    Fixtures f;
    CsvWriter writer;
    CHECK(writer.open(f.path("io.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
    const std::string_view names[] = {"payload"};
    CHECK(writer.write_header(names, 1) == Status::Ok);
    struct rlimit previous = {};
    const bool have_limit = getrlimit(RLIMIT_FSIZE, &previous) == 0;
    struct sigaction ignore = {}, previous_action = {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    CHECK(sigaction(SIGXFSZ, &ignore, &previous_action) == 0);
    struct rlimit small = previous;
    small.rlim_cur = 4096;
    if (small.rlim_max != RLIM_INFINITY && small.rlim_max < small.rlim_cur) small.rlim_max = small.rlim_cur;
    CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
    const std::string payload(1024, 'x');
    const std::string_view row[] = {payload};
    const bool nulls[] = {false};
    Status status = Status::Ok;
    for (int i = 0; i < 512 && status == Status::Ok; ++i) status = writer.write_row(row, nulls, 1);
    CHECK(status == Status::Io);
    CHECK(writer.error()[0] != '\0');
    if (have_limit) CHECK(setrlimit(RLIMIT_FSIZE, &previous) == 0);
    CHECK(sigaction(SIGXFSZ, &previous_action, nullptr) == 0);
    writer.abort();
    CHECK(list_dir(f.root).empty());
  }
  {
    // A failing flush at commit(): everything stays buffered until commit(), so
    // the publish step is the one that reports Status::Io and must clean up.
    Fixtures f;
    CsvWriter writer;
    CHECK(writer.open(f.path("commit_io.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
    const std::string_view names[] = {"payload"};
    CHECK(writer.write_header(names, 1) == Status::Ok);
    struct rlimit previous = {};
    const bool have_limit = getrlimit(RLIMIT_FSIZE, &previous) == 0;
    struct sigaction ignore = {}, previous_action = {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    CHECK(sigaction(SIGXFSZ, &ignore, &previous_action) == 0);
    struct rlimit small = previous;
    small.rlim_cur = 4096;
    if (small.rlim_max != RLIM_INFINITY && small.rlim_max < small.rlim_cur) small.rlim_max = small.rlim_cur;
    CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
    const std::string payload(1024, 'x');
    const std::string_view row[] = {payload};
    const bool nulls[] = {false};
    for (int i = 0; i < 8; ++i) CHECK(writer.write_row(row, nulls, 1) == Status::Ok);
    CHECK(writer.commit() == Status::Io);
    CHECK(!writer.is_committed());
    CHECK(!writer.is_open());
    if (have_limit) CHECK(setrlimit(RLIMIT_FSIZE, &previous) == 0);
    CHECK(sigaction(SIGXFSZ, &previous_action, nullptr) == 0);
    CHECK(list_dir(f.root).empty());
    CHECK(!path_exists(f.path("commit_io.csv")));
  }
}

// Another actor creating the target name between open() and commit() must keep
// its bytes: publication is a no-replace link, so the export fails instead of
// overwriting data it does not own.
void test_concurrent_target_creation_is_preserved()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("raced.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"a"};
  CHECK(writer.write_header(names, 1) == Status::Ok);
  const std::string_view row[] = {"1"};
  const bool nulls[] = {false};
  CHECK(writer.write_row(row, nulls, 1) == Status::Ok);
  // The competing actor wins the name while the writer still holds its temp file.
  { std::ofstream other(f.path("raced.csv"), std::ios::binary); other << "OTHER\n"; }
  CHECK(writer.commit() == Status::Changed);
  CHECK(!writer.is_committed());
  CHECK(read_file(f.path("raced.csv")) == std::string("OTHER\n"));
  CHECK(list_dir(f.root) == std::vector<std::string>{"raced.csv"});
  CHECK(::unlink(f.path("raced.csv").c_str()) == 0);
}

// abort() removes only this writer's own temp file, never a target name it did
// not create.
void test_abort_preserves_concurrent_target()
{
  Fixtures f;
  CsvWriter writer;
  CHECK(writer.open(f.path("aborted.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  { std::ofstream other(f.path("aborted.csv"), std::ios::binary); other << "KEEP\n"; }
  writer.abort();
  CHECK(read_file(f.path("aborted.csv")) == std::string("KEEP\n"));
  CHECK(list_dir(f.root) == std::vector<std::string>{"aborted.csv"});
  CHECK(::unlink(f.path("aborted.csv").c_str()) == 0);
}

// A directory renamed out of secure_file_priv after open() must not receive the
// export: commit() re-resolves the target directory and fails when the pinned
// descriptor no longer matches.
void test_directory_moved_after_open_is_detected()
{
  Fixtures f;
  const std::string outside = f.root + "-outside";
  CHECK(::mkdir(f.path("moved").c_str(), 0700) == 0);
  CsvWriter writer;
  CHECK(writer.open(f.path("moved/out.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"a"};
  CHECK(writer.write_header(names, 1) == Status::Ok);
  const std::string_view row[] = {"1"};
  const bool nulls[] = {false};
  CHECK(writer.write_row(row, nulls, 1) == Status::Ok);
  CHECK(::rename(f.path("moved").c_str(), outside.c_str()) == 0);
  CHECK(writer.commit() == Status::Changed);
  CHECK(!writer.is_committed());
  CHECK(!path_exists(outside + "/out.csv"));
  // The temp file was removed through the pinned descriptor, so the moved
  // directory is empty and removable.
  CHECK(list_dir(outside).empty());
  CHECK(::rmdir(outside.c_str()) == 0);
}

void test_nested_directory_target()
{
  Fixtures f;
  CHECK(::mkdir(f.path("nested").c_str(), 0700) == 0);
  CsvWriter writer;
  CHECK(writer.open(f.path("nested/out.csv").c_str(), f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  const std::string_view names[] = {"a"};
  CHECK(writer.write_header(names, 1) == Status::Ok);
  const std::string_view row[] = {"1"};
  const bool nulls[] = {false};
  CHECK(writer.write_row(row, nulls, 1) == Status::Ok);
  CHECK(writer.commit() == Status::Ok);
  CHECK(read_file(f.path("nested/out.csv")) == std::string("a\n1\n"));
  CHECK(list_dir(f.path("nested")) == std::vector<std::string>{"out.csv"});
  CHECK(::unlink(f.path("nested/out.csv").c_str()) == 0);
  CHECK(::rmdir(f.path("nested").c_str()) == 0);

  CsvWriter relative_writer;
  CHECK(relative_writer.open("relative.csv", f.root.c_str(), CsvWriteOptions()) == Status::Ok);
  CHECK(std::string(relative_writer.target_path()) == canonical(f.root) + "/relative.csv");
  CHECK(relative_writer.write_header(names, 1) == Status::Ok);
  CHECK(relative_writer.write_row(row, nulls, 1) == Status::Ok);
  CHECK(relative_writer.commit() == Status::Ok);
  CHECK(read_file(f.path("relative.csv")) == std::string("a\n1\n"));
}

void test_filesystem_root_boundary()
{
  Fixtures f;
  CsvWriter writer;
  const std::string path = canonical(f.root) + "/root.csv";
  const Status status = writer.open(path.c_str(), "/", CsvWriteOptions());
  if (status != Status::Ok) std::cerr << writer.error() << '\n';
  CHECK(status == Status::Ok);
  if (status == Status::Ok) {
    const std::string_view names[] = {"id"};
    CHECK(writer.write_header(names, 1) == Status::Ok);
    CHECK(writer.commit() == Status::Ok);
    CHECK(read_file(path) == "id\n");
  }
}

int main()
{
  test_filesystem_root_boundary();
  test_default_options_commit_exact_bytes();
  test_custom_options_round_trip();
  test_null_versus_empty_round_trip();
  test_special_characters_round_trip();
  test_force_quote();
  test_existing_target_is_preserved();
  test_concurrent_target_creation_is_preserved();
  test_abort_preserves_concurrent_target();
  test_directory_moved_after_open_is_detected();
  test_abort_and_destructor_leave_no_residue();
  test_streaming_large_output();
  test_read_back_through_csv_reader();
  test_security_denials();
  test_invalid_options();
  test_invalid_usage();
  test_oversized_field();
  test_io_failures();
  test_nested_directory_target();
  if (failures != 0) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all file SQL export checks passed\n";
  return 0;
}
