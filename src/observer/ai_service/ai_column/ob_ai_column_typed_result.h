/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

namespace oceanbase { namespace observer { namespace ai_column {

// Validate the exact value before it becomes a READY queue payload. In particular,
// do not defer conversion errors to the atomic sibling-group UPDATE: one bad
// model response would otherwise prevent every successful sibling from publishing.
class AiColumnTypedResult
{
public:
  static const char *instruction(const std::string &spec)
  {
    if (spec == "json") return "Return only one valid JSON value, without markdown or explanation.";
    if (spec.compare(0, 4, "int:") == 0 || spec.compare(0, 5, "uint:") == 0)
      return "Return only one base-10 integer, without separators, markdown or explanation.";
    if (spec.compare(0, 7, "vector:") == 0)
      return "Return only a JSON array of finite numbers with the declared vector dimension, without markdown or explanation.";
    if (spec == "float" || spec == "double" || spec.compare(0, 8, "decimal:") == 0)
      return "Return only one finite base-10 number, without separators, markdown or explanation.";
    return "Return only the requested text, without markdown or explanation.";
  }

  static bool validate(const std::string &spec, const std::string &value)
  {
    if (value.size() > 16 * 1024 * 1024) return false;
    if (spec == "varchar") return true;
    const std::string text = trim(value);
    if (text.empty()) return false;
    if (spec == "json") return parse(text, 0, 0);
    if (spec.compare(0, 7, "vector:") == 0) {
      uint64_t dim = 0;
      return unsigned_value(spec.substr(7), 65535, dim) && dim > 0 && parse(text, 1, dim);
    }
    if (spec == "float" || spec == "double") return parse(text, spec == "float" ? 2 : 3, 0);
    const bool is_unsigned = spec.compare(0, 5, "uint:") == 0;
    if (is_unsigned || spec.compare(0, 4, "int:") == 0) {
      uint64_t bits = 0;
      if (!unsigned_value(spec.substr(is_unsigned ? 5 : 4), 64, bits) ||
          (bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64)) return false;
      const bool negative = text[0] == '-';
      if (negative && is_unsigned) return false;
      const uint64_t max = is_unsigned ? (UINT64_MAX >> (64 - bits)) :
          ((UINT64_MAX >> (65 - bits)) + (negative ? 1 : 0));
      uint64_t magnitude = 0;
      return unsigned_value(negative ? text.substr(1) : text, max, magnitude);
    }
    if (spec.compare(0, 8, "decimal:") == 0) {
      const size_t colon = spec.find(':', 8);
      uint64_t precision = 0, scale = 0;
      if (colon == std::string::npos || !unsigned_value(spec.substr(8, colon - 8), 65, precision) ||
          !unsigned_value(spec.substr(colon + 1), 30, scale) || precision == 0 || scale > precision) return false;
      size_t pos = text[0] == '-' ? 1 : 0, integral = 0, fractional = 0;
      bool point = false, digit = false, significant = false;
      for (; pos < text.size(); ++pos) {
        const char c = text[pos];
        if (c == '.' && !point) { point = true; continue; }
        if (c < '0' || c > '9') return false;
        digit = true;
        if (point) ++fractional;
        else { significant = significant || c != '0'; if (significant) ++integral; }
      }
      return digit && integral <= precision - scale && fractional <= scale;
    }
    return false;
  }

private:
  static std::string trim(const std::string &s)
  {
    const size_t first = s.find_first_not_of(" \t\n\r");
    return first == std::string::npos ? std::string() : s.substr(first, s.find_last_not_of(" \t\n\r") - first + 1);
  }
  static bool unsigned_value(const std::string &s, uint64_t max, uint64_t &out)
  {
    out = 0;
    if (s.empty()) return false;
    for (char c : s) {
      if (c < '0' || c > '9') return false;
      const uint64_t digit = c - '0';
      if (digit > max || out > (max - digit) / 10) return false;
      out = out * 10 + digit;
    }
    return true;
  }

  struct Handler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, Handler>
  {
    explicit Handler(int mode, uint64_t dimension)
      : mode_(mode), dimension_(dimension), depth_(0), count_(0), root_array_(false) {}
    bool Null() { return mode_ == 0; }
    bool Bool(bool) { return mode_ == 0; }
    bool String(const char *, rapidjson::SizeType, bool) { return mode_ == 0; }
    bool Int(int n) { return number(n); }
    bool Uint(unsigned n) { return number(n); }
    bool Int64(int64_t n) { return number(static_cast<double>(n)); }
    bool Uint64(uint64_t n) { return number(static_cast<double>(n)); }
    bool Double(double n) { return number(n); }
    bool number(double n)
    {
      if (!std::isfinite(n)) return false;
      if (mode_ == 0) return true;
      if (mode_ == 1 && (depth_ != 1 || !root_array_)) return false;
      if ((mode_ == 1 || mode_ == 2) && std::fabs(n) > std::numeric_limits<float>::max()) return false;
      ++count_;
      return mode_ != 1 || count_ <= dimension_;
    }
    bool StartObject() { return mode_ == 0 && ++depth_ <= 100; }
    bool EndObject(rapidjson::SizeType) { --depth_; return true; }
    bool StartArray()
    {
      if (mode_ == 1) { root_array_ = depth_ == 0; return ++depth_ == 1; }
      return mode_ == 0 && ++depth_ <= 100;
    }
    bool EndArray(rapidjson::SizeType) { --depth_; return mode_ != 1 || count_ == dimension_; }
    int mode_;
    uint64_t dimension_;
    int depth_;
    uint64_t count_;
    bool root_array_;
  };
  static bool parse(const std::string &text, int mode, uint64_t dimension)
  {
    rapidjson::MemoryStream stream(text.data(), text.size());
    rapidjson::Reader reader;
    Handler handler(mode, dimension);
    // Iterative parsing plus a depth cap avoids a model-controlled C++ stack.
    const bool ok = reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(stream, handler);
    return ok && stream.Tell() == text.size() &&
        (mode == 0 || (mode == 1 ? handler.root_array_ && handler.count_ == dimension : handler.count_ == 1));
  }
};

}}} // namespace oceanbase::observer::ai_column
