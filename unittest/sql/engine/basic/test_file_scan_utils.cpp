/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_utils.h"
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace oceanbase::sql::file_sql;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)
struct Fixtures {
  std::string root;
  std::vector<std::string> files;
  Fixtures() {
    char name[] = "/tmp/seekdb-file-scan-XXXXXX";
    char *p = mkdtemp(name);
    if (!p) std::abort();
    root = p;
  }
  std::string put(const std::string &name, const std::string &contents) {
    std::string path = root + "/" + name;
    std::ofstream stream(path, std::ios::binary); stream << contents;
    if (!stream.good()) std::abort();
    files.push_back(path); return path;
  }
  ~Fixtures() {
    // Only remove paths created in this unique fixture directory.
    if (root.find("/tmp/seekdb-file-scan-") != 0 || root.size() != 28) std::abort();
    for (const auto &p : files) {
      if (p.find(root + "/") != 0) std::abort();
      unlink(p.c_str());
    }
    rmdir(root.c_str());
  }
};
void test_types() {
  CHECK(infer_type("9223372036854775807") == Type::BigInt);
  CHECK(infer_type("-9223372036854775808") == Type::BigInt);
  CHECK(infer_type("9223372036854775808") == Type::Varchar);
  CHECK(infer_type("-9223372036854775809") == Type::Varchar);
  CHECK(infer_type("1e3") == Type::Double);
  CHECK(infer_type("1e9999") == Type::Varchar);
  CHECK(infer_type("nan") == Type::Varchar);
  CHECK(infer_type("TRUE") == Type::Boolean);
  CHECK(infer_type("2024-02-29") == Type::Date);
  CHECK(infer_type("2023-02-29") == Type::Varchar);
  CHECK(infer_type("2026-04-31") == Type::Varchar);
  CHECK(infer_type("2026-10-26 23:59:59.123456") == Type::DateTime);
  CHECK(infer_type("2026-10-26 24:00:00") == Type::Varchar);
  CHECK(widen(Type::Null, Type::BigInt) == Type::BigInt);
  CHECK(widen(Type::Double, Type::BigInt) == Type::Double);
  CHECK(widen(Type::Date, Type::DateTime) == Type::DateTime);
  CHECK(widen(Type::Boolean, Type::BigInt) == Type::Varchar);
}
void test_csv() {
  Fixtures f;
  const auto path = f.put("data.csv", "\xef\xbb\xbfid,name,note,id,nothing\r\n1,世界,\"a,\r\nb\",10,\r\n2,\"\",\"say \"\"hi\"\"\",20,NULL\n");
  CsvOptions o; o.enabled = true;
  auto reader = std::make_unique<CsvReader>();
  const auto opened = reader->open(path.c_str(), f.root.c_str(), o);
  CHECK(opened == Status::Ok);
  if (opened != Status::Ok) return;
  CHECK(reader->infer() == Status::Ok);
  CHECK(reader->column_count() == 5);
  CHECK(std::string(reader->column(3).name) == "id__2");
  CHECK(reader->column(0).type == Type::BigInt);
  CHECK(reader->column(4).type == Type::Null);
  CHECK(reader->column(4).nullable);
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->value(1) == "世界");
  CHECK(reader->value(2) == "a,\r\nb");
  CHECK(reader->is_null(4));
  CHECK(reader->next() == Status::Ok);
  CHECK(reader->value(1).empty() && !reader->is_null(1));
  CHECK(reader->value(2) == "say \"hi\"");
  CHECK(reader->next() == Status::End);
  CHECK(reader->rescan() == Status::Ok);
  CHECK(reader->next() == Status::Ok && reader->value(0) == "1");
}
void test_security_and_mutation() {
  Fixtures f, outside;
  auto path = f.put("data.csv", "id\n1\n");
  auto escape = outside.put("secret.csv", "id\n2\n");
  CsvOptions o;
  auto r = std::make_unique<CsvReader>();
  CHECK(r->open(path.c_str(), f.root.c_str(), o) == Status::Disabled);
  o.enabled = true;
  CHECK(r->open(path.c_str(), "", o) == Status::Denied);
  CHECK(r->open(escape.c_str(), f.root.c_str(), o) == Status::Denied);
  std::string link = f.root + "/link.csv";
  CHECK(symlink(escape.c_str(), link.c_str()) == 0); f.files.push_back(link);
  CHECK(r->open(link.c_str(), f.root.c_str(), o) == Status::Denied);
  CHECK(r->open(f.root.c_str(), f.root.c_str(), o) == Status::Denied);
  if (r->open(path.c_str(), f.root.c_str(), o) == Status::Ok) {
    CHECK(r->infer() == Status::Ok);
    std::ofstream(path, std::ios::app) << "3\n";
    CHECK(r->next() == Status::Changed);
    CHECK(r->rescan() == Status::Changed);
  } else CHECK(false);
}
void test_malformed_and_options() {
  Fixtures f; CsvOptions o; o.enabled = true;
  auto r = std::make_unique<CsvReader>();
  for (const auto &text : {"a,b\n1\n", "a\n\"unterminated", "a\n\"x\"junk\n", "a\nx\"y\n"}) {
    auto path = f.put("bad.csv", text);
    CHECK(r->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
    CHECK(r->infer() == Status::Malformed);
    CHECK(std::strstr(r->error(), "row") != nullptr);
  }
  auto path = f.put("custom.csv", "1;'a\\'b';nil\n2;ok;''\n");
  o.header = false; o.delimiter = ';'; o.quote = '\''; o.escape = '\\'; o.null_text = "nil";
  CHECK(r->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(r->infer() == Status::Ok);
  CHECK(r->next() == Status::Ok);
  CHECK(r->value(1) == "a'b" && r->is_null(2));
  CHECK(r->next() == Status::Ok && !r->is_null(2));
  o.sample_size = 10;
  CHECK(r->open(path.c_str(), f.root.c_str(), o) == Status::Unsupported);
  o.sample_size = -1; o.max_record_bytes = 3;
  CHECK(r->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(r->infer() == Status::ResourceLimit);
}
void test_descriptor_lifetime_and_replacement() {
  Fixtures f; CsvOptions o; o.enabled = true;
  auto path = f.put("original.csv", "a\n1\n");
  auto replacement = f.put("replacement.csv", "a\n2\n");
  int before[256];
  for (int fd = 0; fd < 256; ++fd) before[fd] = fcntl(fd, F_GETFD);
  auto r = std::make_unique<CsvReader>();
  CHECK(r->open(path.c_str(), "/", o) == Status::Ok);
  for (int fd = 0; fd < 256; ++fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (before[fd] < 0 && flags >= 0) CHECK((flags & FD_CLOEXEC) != 0);
  }
  CHECK(r->infer() == Status::Ok);
  // Both rename endpoints are files created in this fixture's unique directory.
  CHECK(path.find(f.root + "/") == 0 && replacement.find(f.root + "/") == 0);
  CHECK(std::rename(replacement.c_str(), path.c_str()) == 0);
  CHECK(r->next() == Status::Changed);
  r.reset();
  for (int fd = 0; fd < 256; ++fd) if (before[fd] < 0) CHECK(fcntl(fd, F_GETFD) < 0);
}
void test_streaming_and_unicode_limits() {
  Fixtures f; CsvOptions o; o.enabled = true;
  auto r = std::make_unique<CsvReader>();
  auto large = f.put("large.csv", "id,note\n");
  {
    std::ofstream stream(large, std::ios::app);
    for (int i = 0; i < 100000; ++i) stream << i << "," << std::string(256, 'x') << "\n";
  }
  // Total input exceeds the row workspace cap; inference must not materialize it.
  CHECK(r->open(large.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(r->infer() == Status::Ok);
  size_t count = 0;
  while (r->next() == Status::Ok) ++count;
  CHECK(count == 100000);
  CHECK(r->rescan() == Status::Ok);
  CHECK(r->next() == Status::Ok && r->value(0) == "0");
  for (auto bad : {std::string("v\n\xc0\xaf\n"), std::string("v\n\xed\xa0\x80\n"), std::string("v\n\0x\n", 6)}) {
    auto path = f.put("utf8.csv", bad);
    CHECK(r->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
    CHECK(r->infer() == Status::Malformed);
  }
  auto header = f.put("headers.csv", "id,ID,id__2\n1,2,3\n");
  CHECK(r->open(header.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(r->infer() == Status::Ok);
  CHECK(std::string(r->column(1).name) == "ID__2");
  CHECK(std::string(r->column(2).name) == "id__2__2");
  std::string row;
  for (size_t i = 0; i < CsvReader::MAX_COLUMNS; ++i) row += "c,";
  auto wide = f.put("wide.csv", row + "extra\n");
  CHECK(r->open(wide.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(r->infer() == Status::ResourceLimit);
}
void test_resolver_executor_schema_handoff() {
  Fixtures f;
  const auto path = f.put("handoff.csv", "id,label\n1,first\n2,second\n");
  CsvOptions o; o.enabled = true;
  auto resolver = std::make_unique<CsvReader>();
  CHECK(resolver->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(resolver->infer() == Status::Ok);
  const auto identity = resolver->identity();
  std::vector<CsvReader::Column> columns;
  for (size_t i = 0; i < resolver->column_count(); ++i) columns.push_back(resolver->column(i));
  resolver.reset(); // execution cannot depend on resolver-owned memory or descriptors
  auto executor = std::make_unique<CsvReader>();
  CHECK(executor->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(executor->bind_schema(columns.data(), columns.size(), identity) == Status::Ok);
  CHECK(executor->next() == Status::Ok && executor->value(1) == "first");
  CHECK(executor->next() == Status::Ok && executor->value(1) == "second");
  CHECK(executor->next() == Status::End);
  CHECK(executor->rescan() == Status::Ok);
  CHECK(executor->next() == Status::Ok && executor->value(0) == "1");
  executor->close();
  // Same path and same-length contents must not make a stale planned schema valid.
  f.put("handoff.csv", "id,label\n9,third\n8,fourth\n");
  CHECK(executor->open(path.c_str(), f.root.c_str(), o) == Status::Ok);
  CHECK(executor->bind_schema(columns.data(), columns.size(), identity) == Status::Changed);
  CHECK(executor->next() == Status::Changed);
}
int main() {
  test_resolver_executor_schema_handoff();
  test_types(); test_csv(); test_security_and_mutation(); test_malformed_and_options();
  test_streaming_and_unicode_limits();
  test_descriptor_lifetime_and_replacement();
  if (failures) std::cerr << failures << " checks failed\n";
  else std::cout << "file_scan_utils: all checks passed\n";
  return failures ? 1 : 0;
}
