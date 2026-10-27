/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/json_file_reader.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace oceanbase { namespace sql { namespace file_sql {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// JSON numbers carry no '+' and no leading zeros, so the token text is already
// the canonical spelling the executor will convert. A token that is an integer
// but does not fit int64 falls back to DOUBLE (a JSON number is still numeric);
// only a non-finite value such as 1e999 has to keep its text as a string.
Type type_of_number(std::string_view text, bool fractional)
{
  if (!fractional) {
    int64_t integer = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), integer);
    if (result.ec == std::errc() && result.ptr == text.data() + text.size()) return Type::BigInt;
  }
  double real = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), real);
  return result.ec == std::errc() && result.ptr == text.data() + text.size() && std::isfinite(real)
      ? Type::Double : Type::Varchar;
}

// A JSON string is never a number or a boolean: only the DATE/DATETIME
// spellings infer_type knows are promoted, everything else stays VARCHAR.
Type type_of_string(std::string_view text)
{
  const Type type = infer_type(text);
  return type == Type::Date || type == Type::DateTime ? type : Type::Varchar;
}

size_t utf8_encode(uint32_t code, char (&bytes)[4])
{
  if (code < 0x80) {
    bytes[0] = static_cast<char>(code);
    return 1;
  }
  if (code < 0x800) {
    bytes[0] = static_cast<char>(0xc0 | (code >> 6));
    bytes[1] = static_cast<char>(0x80 | (code & 0x3f));
    return 2;
  }
  if (code < 0x10000) {
    bytes[0] = static_cast<char>(0xe0 | (code >> 12));
    bytes[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    bytes[2] = static_cast<char>(0x80 | (code & 0x3f));
    return 3;
  }
  bytes[0] = static_cast<char>(0xf0 | (code >> 18));
  bytes[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3f));
  bytes[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3f));
  bytes[3] = static_cast<char>(0x80 | (code & 0x3f));
  return 4;
}

} // namespace

JsonlReader::~JsonlReader() { close(); }

Status JsonlReader::fail(Status status, const char *message)
{
  status_ = status;
  std::snprintf(error_, sizeof(error_), "%.240s: row %llu, line %llu: %s",
      path_[0] != 0 ? path_ : "input",
      static_cast<unsigned long long>(row_number_),
      static_cast<unsigned long long>(line_number_), message);
  return status;
}

Status JsonlReader::fail_path(Status status, const char *message)
{
  status_ = status;
  std::snprintf(error_, sizeof(error_), "%.240s: %s", path_[0] != 0 ? path_ : "input", message);
  return status;
}

void JsonlReader::close()
{
  file_.close();
  column_count_ = 0;
  row_number_ = 0;
  line_number_ = 0;
  next_line_number_ = 1;
  inferred_ = false;
  inferring_ = false;
  status_ = Status::Ok;
  chunk_pos_ = chunk_end_ = 0;
  next_offset_ = 0;
  pos_ = end_ = 0;
  line_.clear();
  text_.clear();
  name_.clear();
  path_[0] = error_[0] = 0;
}

Status JsonlReader::open(const char *path, const char *secure_root, const JsonOptions &options)
{
  close();
  if (path != nullptr) std::snprintf(path_, sizeof(path_), "%s", path);
  if (!options.enabled) return fail_path(Status::Disabled, "enable_file_sql is disabled");
  if (path == nullptr || secure_root == nullptr || !*secure_root || equal_ascii_ci(secure_root, "null")) {
    return fail_path(Status::Denied, "secure_file_priv denies access");
  }
  if (options.sample_size != -1) return fail_path(Status::Unsupported, "only sample_size=-1 is supported");
  if (options.max_record_bytes == 0 || options.max_record_bytes > 16 * 1024 * 1024) {
    return fail_path(Status::InvalidOption, "invalid JSON options");
  }
  options_ = options;
  const Status opened = file_.open(path, secure_root);
  if (opened != Status::Ok) {
    // SecureFile reports the containment decision; keep its message verbatim.
    std::snprintf(error_, sizeof(error_), "%s", file_.error());
    status_ = opened;
    return opened;
  }
  std::snprintf(path_, sizeof(path_), "%s", file_.path());
  if (file_.size() == 0) return fail_path(Status::Malformed, "empty newline-delimited JSON file");
  return rewind();
}

FileIdentity JsonlReader::identity() const { return file_.identity(); }

Status JsonlReader::check_unchanged()
{
  if (status_ != Status::Ok) return status_;
  const Status status = file_.check_unchanged();
  if (status == Status::Ok) return Status::Ok;
  return fail_path(status, status == Status::Changed ? "input file changed" : "input file is not open");
}

Status JsonlReader::fill()
{
  size_t read = 0;
  const Status status = file_.read_at(chunk_, sizeof(chunk_), next_offset_, read);
  if (status != Status::Ok && status != Status::End) return fail_path(Status::Io, "input read failed");
  chunk_pos_ = 0;
  chunk_end_ = read;
  next_offset_ += static_cast<int64_t>(read);
  return read == 0 ? Status::End : Status::Ok;
}

Status JsonlReader::get(char &c)
{
  if (chunk_pos_ == chunk_end_) {
    const Status status = fill();
    if (status != Status::Ok) return status;
  }
  c = chunk_[chunk_pos_++];
  return Status::Ok;
}

Status JsonlReader::rewind()
{
  const Status unchanged = check_unchanged();
  if (unchanged != Status::Ok) return unchanged;
  chunk_pos_ = chunk_end_ = 0;
  next_offset_ = 0;
  row_number_ = 0;
  line_number_ = 0;
  next_line_number_ = 1;
  pos_ = end_ = 0;
  line_.clear();
  text_.clear();
  name_.clear();
  // A single leading UTF-8 BOM belongs to the encoding, not to the first record.
  char bom[3] = {};
  size_t read = 0;
  const Status status = file_.read_at(bom, sizeof(bom), 0, read);
  if (status != Status::Ok && status != Status::End) return fail_path(Status::Io, "input read failed");
  if (read == sizeof(bom) && static_cast<unsigned char>(bom[0]) == 0xef
      && static_cast<unsigned char>(bom[1]) == 0xbb && static_cast<unsigned char>(bom[2]) == 0xbf) {
    next_offset_ = static_cast<int64_t>(sizeof(bom));
  }
  return Status::Ok;
}

Status JsonlReader::read_line(bool &has_line)
{
  line_.clear();
  has_line = false;
  line_number_ = next_line_number_;
  Status status = Status::Ok;
  char c = 0;
  while ((status = get(c)) == Status::Ok) {
    has_line = true;
    if (c == '\n') break;
    if (line_.size() >= options_.max_record_bytes) {
      // The record being read is the next row: count it so the message names it.
      ++row_number_;
      return fail(Status::ResourceLimit, "JSON record exceeds the byte limit");
    }
    line_.push_back(c);
  }
  if (status != Status::Ok && status != Status::End) return status;
  if (has_line) {
    ++next_line_number_;
    // The carriage return of a CRLF terminator is not part of the record.
    if (!line_.empty() && line_.back() == '\r') line_.pop_back();
  }
  return Status::Ok;
}

void JsonlReader::reset_row()
{
  for (size_t i = 0; i < column_count_; ++i) cells_[i] = Cell{0, 0, true};
}

void JsonlReader::assign(size_t index, size_t offset, Type type)
{
  cells_[index] = Cell{offset, text_.size() - offset, false};
  row_types_[index] = type;
}

void JsonlReader::assign_null(size_t index)
{
  cells_[index] = Cell{0, 0, true};
  row_types_[index] = Type::Null;
}

Status JsonlReader::scan_row()
{
  while (true) {
    bool has_line = false;
    const Status status = read_line(has_line);
    if (status != Status::Ok) return status;
    if (!has_line) return Status::End;
    pos_ = 0;
    end_ = line_.size();
    skip_whitespace();
    if (pos_ == end_) continue; // blank line: skipped and never becomes a row
    reset_row();
    text_.clear();
    ++row_number_;
    return parse_row();
  }
}

Status JsonlReader::parse_row()
{
  if (!consume('{')) return fail(Status::Malformed, "line must contain a JSON object");
  skip_whitespace();
  if (!consume('}')) {
    while (true) {
      skip_whitespace();
      if (!at('"')) return fail(Status::Malformed, "expected a JSON object key");
      name_.clear();
      Status status = parse_string(name_, MAX_NAME_BYTES);
      if (status == Status::ResourceLimit) {
        return fail(Status::ResourceLimit, "JSON object key exceeds the name limit");
      }
      if (status != Status::Ok) return status;
      if (name_.empty()) return fail(Status::ResourceLimit, "empty JSON object key");
      if (!is_valid_utf8(name_)) return fail(Status::Malformed, "invalid UTF-8 in a JSON object key");
      skip_whitespace();
      if (!consume(':')) return fail(Status::Malformed, "expected ':' after a JSON object key");
      skip_whitespace();
      size_t index = 0;
      if ((status = resolve_column(index)) != Status::Ok) return status;
      if ((status = parse_value(index)) != Status::Ok) return status;
      skip_whitespace();
      if (consume(',')) continue;
      if (consume('}')) break;
      return fail(Status::Malformed, "expected ',' or '}' in a JSON object");
    }
  }
  skip_whitespace();
  if (pos_ != end_) return fail(Status::Malformed, "trailing characters after a JSON object");
  return Status::Ok;
}

Status JsonlReader::resolve_column(size_t &index)
{
  for (size_t i = 0; i < column_count_; ++i) {
    if (equal_ascii_ci(std::string_view(columns_[i].name), name_)) {
      index = i;
      return Status::Ok;
    }
  }
  // check_unchanged() guarantees the same bytes that produced the bound schema,
  // so an unknown key can only come from a schema that does not describe it.
  if (!inferring_) return fail(Status::Malformed, "JSON object key is absent from the planned schema");
  if (column_count_ >= MAX_COLUMNS) return fail(Status::ResourceLimit, "too many JSON columns");
  Column &column = columns_[column_count_];
  column = Column{};
  // A key that appears only from the second row on is absent, hence NULL, in
  // every row above it, so the column is nullable regardless of its values.
  column.nullable = row_number_ > 1;
  std::memcpy(column.name, name_.data(), name_.size());
  column.name[name_.size()] = 0;
  index = column_count_++;
  return Status::Ok;
}

Status JsonlReader::parse_value(size_t index)
{
  if (pos_ == end_) return fail(Status::Malformed, "missing JSON value");
  const char c = line_[pos_];
  Status status = Status::Ok;
  if (c == '"') {
    const size_t offset = text_.size();
    if ((status = parse_string(text_, options_.max_record_bytes)) != Status::Ok) return status;
    const std::string_view decoded(text_.data() + offset, text_.size() - offset);
    if (!is_valid_utf8(decoded)) return fail(Status::Malformed, "invalid UTF-8 in a JSON string");
    assign(index, offset, type_of_string(decoded));
  } else if (c == '{') {
    return fail(Status::Malformed, "nested JSON object is not supported");
  } else if (c == '[') {
    return fail(Status::Malformed, "nested JSON array is not supported");
  } else if (c == 'n') {
    if (!consume_literal("null")) return fail(Status::Malformed, "invalid JSON value");
    assign_null(index);
  } else if (c == 't' || c == 'f') {
    const std::string_view literal = c == 't' ? std::string_view("true") : std::string_view("false");
    if (!consume_literal(literal)) return fail(Status::Malformed, "invalid JSON value");
    if (!append(text_, options_.max_record_bytes, literal)) return status_;
    assign(index, text_.size() - literal.size(), Type::Boolean);
  } else if (c == '-' || is_digit(c)) {
    status = parse_number(index);
  } else {
    return fail(Status::Malformed, "invalid JSON value");
  }
  return status;
}

Status JsonlReader::parse_number(size_t index)
{
  const size_t start = pos_;
  const bool negative = consume('-');
  if (negative && pos_ == end_) return fail(Status::Malformed, "invalid JSON number");
  if (at('0')) {
    ++pos_;
    // JSON forbids a leading zero, so "01" is malformed rather than two tokens.
    if (pos_ != end_ && is_digit(line_[pos_])) return fail(Status::Malformed, "invalid JSON number");
  } else if (pos_ != end_ && is_digit(line_[pos_])) {
    while (pos_ != end_ && is_digit(line_[pos_])) ++pos_;
  } else {
    return fail(Status::Malformed, "invalid JSON number");
  }
  bool fractional = false;
  if (consume('.')) {
    fractional = true;
    if (pos_ == end_ || !is_digit(line_[pos_])) return fail(Status::Malformed, "invalid JSON number");
    while (pos_ != end_ && is_digit(line_[pos_])) ++pos_;
  }
  if (at('e') || at('E')) {
    fractional = true;
    ++pos_;
    if (at('+') || at('-')) ++pos_;
    if (pos_ == end_ || !is_digit(line_[pos_])) return fail(Status::Malformed, "invalid JSON number");
    while (pos_ != end_ && is_digit(line_[pos_])) ++pos_;
  }
  const std::string_view token(line_.data() + start, pos_ - start);
  if (!append(text_, options_.max_record_bytes, token)) return status_;
  assign(index, text_.size() - token.size(), type_of_number(token, fractional));
  return Status::Ok;
}

Status JsonlReader::parse_string(std::string &out, size_t limit)
{
  if (!consume('"')) return fail(Status::Malformed, "expected a JSON string");
  while (true) {
    if (pos_ == end_) return fail(Status::Malformed, "unterminated JSON string");
    const char c = line_[pos_++];
    if (c == '"') return Status::Ok;
    if (c == '\\') {
      const Status status = parse_escape(out, limit);
      if (status != Status::Ok) return status;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      return fail(Status::Malformed, "raw control byte in a JSON string");
    } else if (!append(out, limit, c)) {
      return status_;
    }
  }
}

Status JsonlReader::parse_escape(std::string &out, size_t limit)
{
  if (pos_ == end_) return fail(Status::Malformed, "unterminated JSON escape");
  const char escape = line_[pos_++];
  char plain = 0;
  switch (escape) {
    case '"': plain = '"'; break;
    case '\\': plain = '\\'; break;
    case '/': plain = '/'; break;
    case 'b': plain = '\b'; break;
    case 'f': plain = '\f'; break;
    case 'n': plain = '\n'; break;
    case 'r': plain = '\r'; break;
    case 't': plain = '\t'; break;
    case 'u': return parse_unicode_escape(out, limit);
    default: return fail(Status::Malformed, "invalid JSON escape");
  }
  return append(out, limit, plain) ? Status::Ok : status_;
}

Status JsonlReader::parse_unicode_escape(std::string &out, size_t limit)
{
  uint32_t code = 0;
  if (!read_hex4(code)) return fail(Status::Malformed, "invalid \\u escape");
  if (code >= 0xd800 && code <= 0xdbff) {
    uint32_t low = 0;
    if (!consume('\\') || !consume('u') || !read_hex4(low)) {
      return fail(Status::Malformed, "lone high surrogate in a JSON string");
    }
    if (low < 0xdc00 || low > 0xdfff) return fail(Status::Malformed, "invalid surrogate pair in a JSON string");
    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
  } else if (code >= 0xdc00 && code <= 0xdfff) {
    return fail(Status::Malformed, "lone low surrogate in a JSON string");
  }
  if (code == 0) return fail(Status::Malformed, "NUL is not allowed in a JSON string");
  char bytes[4] = {};
  const size_t length = utf8_encode(code, bytes);
  return append(out, limit, std::string_view(bytes, length)) ? Status::Ok : status_;
}

bool JsonlReader::consume(char c)
{
  if (!at(c)) return false;
  ++pos_;
  return true;
}

bool JsonlReader::consume_literal(std::string_view literal)
{
  if (end_ - pos_ < literal.size()) return false;
  if (std::memcmp(line_.data() + pos_, literal.data(), literal.size()) != 0) return false;
  pos_ += literal.size();
  return true;
}

bool JsonlReader::read_hex4(uint32_t &value)
{
  value = 0;
  for (int i = 0; i < 4; ++i) {
    if (pos_ == end_) return false;
    const char c = line_[pos_++];
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
    else return false;
    value = (value << 4) | digit;
  }
  return true;
}

bool JsonlReader::append(std::string &out, size_t limit, std::string_view text)
{
  if (text.size() > limit || out.size() > limit - text.size()) {
    fail(Status::ResourceLimit, "JSON record exceeds the byte limit");
    return false;
  }
  out.append(text.data(), text.size());
  return true;
}

bool JsonlReader::append(std::string &out, size_t limit, char c)
{
  return append(out, limit, std::string_view(&c, 1));
}

void JsonlReader::merge_row()
{
  for (size_t i = 0; i < column_count_; ++i) {
    if (cells_[i].is_null) {
      // A missing key and an explicit null both make the column nullable.
      columns_[i].nullable = true;
    } else {
      columns_[i].type = widen(columns_[i].type, row_types_[i]);
      columns_[i].max_bytes = std::max(columns_[i].max_bytes, cells_[i].length);
    }
  }
}

Status JsonlReader::infer()
{
  column_count_ = 0;
  const Status rewound = rewind();
  if (rewound != Status::Ok) return rewound;
  inferring_ = true;
  bool has_row = false;
  Status status = Status::Ok;
  while ((status = scan_row()) == Status::Ok) {
    has_row = true;
    merge_row();
  }
  inferring_ = false;
  if (status != Status::End) return status;
  if (!has_row) return fail(Status::Malformed, "file contains no JSON objects");
  if (column_count_ == 0) return fail(Status::Malformed, "file contains no JSON object keys");
  inferred_ = true;
  return rescan();
}

Status JsonlReader::rescan()
{
  if (!inferred_) return fail(Status::InvalidOption, "infer the schema before rescannning");
  return rewind();
}

Status JsonlReader::next()
{
  // A sticky failure (including a rejected bind_schema) outranks a state check.
  const Status unchanged = check_unchanged();
  if (unchanged != Status::Ok) return unchanged;
  if (!inferred_) return fail(Status::InvalidOption, "infer the schema before scanning");
  return scan_row();
}

Status JsonlReader::bind_schema(const Column *columns, size_t count, const FileIdentity &expected)
{
  const Status unchanged = check_unchanged();
  if (unchanged != Status::Ok) return unchanged;
  if (!(identity() == expected)) return fail_path(Status::Changed, "input changed since schema resolution");
  if (columns == nullptr || count == 0 || count > MAX_COLUMNS) {
    return fail_path(Status::InvalidOption, "invalid planned schema");
  }
  for (size_t i = 0; i < count; ++i) {
    if (std::memchr(columns[i].name, 0, sizeof(columns[i].name)) == nullptr || columns[i].name[0] == 0
        || columns[i].type < Type::Null || columns[i].type > Type::DateTime
        || columns[i].max_bytes > options_.max_record_bytes) {
      return fail_path(Status::InvalidOption, "invalid planned column");
    }
    for (size_t j = 0; j < i; ++j) {
      if (equal_ascii_ci(columns[i].name, columns[j].name)) {
        return fail_path(Status::InvalidOption, "ambiguous planned column names");
      }
    }
  }
  for (size_t i = 0; i < count; ++i) columns_[i] = columns[i];
  column_count_ = count;
  inferred_ = true;
  return rescan();
}

std::string_view JsonlReader::value(size_t index) const
{
  if (index >= column_count_ || cells_[index].is_null) return {};
  return std::string_view(text_.data() + cells_[index].offset, cells_[index].length);
}

}}} // namespace oceanbase::sql::file_sql
