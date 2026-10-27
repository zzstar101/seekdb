/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "share/schema/ob_column_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
using namespace common;

// 1 = AI_COMPLETE, 2 = AI_EMBED (mirrors observer::ai_column::AiColumnCallType)
static const int64_t AI_COLUMN_CALL_COMPLETE = 1;
static const int64_t AI_COLUMN_CALL_EMBED = 2;

// The persisted marker of an AI result column.
//
// An AI column is identified by a *column flag*, i.e. one bit of
// __all_column.column_flags (the low 32 bits are stored, see
// STORED_COLUMN_FLAGS_MASK in ob_schema_struct.h), and never by the text of the
// comment: the comment is user data and a user may legitimately write
// "__ai_column__ ..." in it. Only the AI COLUMN resolver sets this flag, and it
// always sets it together with the definition below, therefore
//   * an ordinary COMMENT can neither forge an AI column nor clear one,
//   * the identification is a schema property, not a string sniff, and
//   * the runtime can tell both apart without parsing anything.
// Bit 5 is claimed from RESERVED_COLUMN_FLAG_5: no other code reads or writes it.
static const int64_t AI_COLUMN_FLAG = RESERVED_COLUMN_FLAG_5;

// The definition of an AI column and the comment the user wrote live in the
// comment of its result column: a column schema has exactly one comment field and
// __all_column.comment already persists it, so no new schema object is needed and
// the AI runtime only needs the table schema to rebuild everything it uses.
//
// Encoding: the prefix below, followed by "<key>=<len>:<raw>;" fields where
// <len> is the byte length of <raw>. Length prefixing keeps arbitrary prompt
// text (quotes, ';', '=') and arbitrary user comment text round-trippable without
// escaping. The user comment is one of the fields (key "uc"), so writing a
// comment through this codec never drops the definition.
static const char *const kAIColumnMetaPrefix = "__ai_column__";
// Prefixes of the three hidden columns owned by the runtime.
static const char *const kAIStateColumnPrefix = "__ai_state_";
static const char *const kAIRequestColumnPrefix = "__ai_req_";
static const char *const kAIResultSpecColumnPrefix = "__ai_result_spec_";
// Result type instruction appended to the normalized expression.
static const char *const kAIJsonResultInstruction = " Return exactly one valid JSON value";
static const char *const kAIVectorResultInstruction = " Return a vector of dimension ";

// One AI column definition, as written by the user in
//   <col> <type> AI COLUMN (AI_COMPLETE('model', <prompt>) | AI_EMBED('model', <prompt>[, dim]))
//                WITH (sync_mode = 'async', max_retries = N)
//
// The struct is the single source of truth shared by the DDL layer (which fills
// it while resolving the column and persists it twice: encoded in the AI result
// column comment and as a __all_ai_column row) and by the AI column runtime
// (src/observer/ai_service/ai_column/ob_ai_column_task.h, AiColumnDef), which
// rebuilds the prompt SQL, the model name, the retry policy and the hidden
// column ids from it.
struct AIColumnDefinition
{
  AIColumnDefinition();
  ~AIColumnDefinition() {}
  void reset();
  bool is_valid() const;
  int assign(const AIColumnDefinition &other);
  TO_STRING_KV(K_(call_type), K_(ai_function_name), K_(model_name), K_(prompt_expr),
               K_(config_expr), K_(normalized_expr), K_(result_spec), K_(sync_mode),
               K_(max_retries),
               K_(dimension), K_(definition_version), K_(dep_column_names),
               K_(state_column_name), K_(request_column_name),
               K_(result_spec_column_name));

  // 1 = AI_COMPLETE, 2 = AI_EMBED
  int64_t call_type_;
  // "AI_COMPLETE" / "AI_EMBED", kept for the catalog and for error messages
  ObString ai_function_name_;
  ObString model_name_;
  // prompt as *evaluable SQL text* over the user table, e.g. "concat('Classify: ', content)"
  ObString prompt_expr_;
  // config as *evaluable SQL text* over the user table (the third argument of
  // AI_COMPLETE), e.g. "json_object('temperature', 0.2)". Empty when the
  // statement did not spell a config out; a config expression that evaluates to
  // SQL NULL behaves exactly like an omitted one.
  ObString config_expr_;
  // prompt_expr plus the result type instruction, e.g.
  // "... Return exactly one valid JSON value" for a json result column
  ObString normalized_expr_;
  // "json" / "varchar" / "vector:4"
  ObString result_spec_;
  ObString sync_mode_;
  int64_t max_retries_;
  // -1 for AI_COMPLETE, the declared vector dimension for AI_EMBED
  int64_t dimension_;
  int64_t definition_version_;
  // ordinary (non AI) columns referenced by the prompt
  ObSEArray<ObString, 4> dep_column_names_;
  // names of the three hidden columns of this AI column
  ObString state_column_name_;
  ObString request_column_name_;
  ObString result_spec_column_name_;
};

inline AIColumnDefinition::AIColumnDefinition()
  : call_type_(AI_COLUMN_CALL_COMPLETE),
    max_retries_(0),
    dimension_(-1),
    definition_version_(1)
{
}

inline void AIColumnDefinition::reset()
{
  call_type_ = AI_COLUMN_CALL_COMPLETE;
  ai_function_name_.reset();
  model_name_.reset();
  prompt_expr_.reset();
  config_expr_.reset();
  normalized_expr_.reset();
  result_spec_.reset();
  sync_mode_.reset();
  max_retries_ = 0;
  dimension_ = -1;
  definition_version_ = 1;
  dep_column_names_.reset();
  state_column_name_.reset();
  request_column_name_.reset();
  result_spec_column_name_.reset();
}

inline bool AIColumnDefinition::is_valid() const
{
  return !model_name_.empty() && !prompt_expr_.empty() && !result_spec_.empty() &&
         !sync_mode_.empty() && max_retries_ >= 0;
}

inline int AIColumnDefinition::assign(const AIColumnDefinition &other)
{
  int ret = OB_SUCCESS;
  call_type_ = other.call_type_;
  ai_function_name_ = other.ai_function_name_;
  model_name_ = other.model_name_;
  prompt_expr_ = other.prompt_expr_;
  config_expr_ = other.config_expr_;
  normalized_expr_ = other.normalized_expr_;
  result_spec_ = other.result_spec_;
  sync_mode_ = other.sync_mode_;
  max_retries_ = other.max_retries_;
  dimension_ = other.dimension_;
  definition_version_ = other.definition_version_;
  state_column_name_ = other.state_column_name_;
  request_column_name_ = other.request_column_name_;
  result_spec_column_name_ = other.result_spec_column_name_;
  return dep_column_names_.assign(other.dep_column_names_);
}

namespace ai_column_meta
{
// Metadata field keys of the encoded comment. The order is stable: it is what
// make/test compare.
const char *const META_KEY_VERSION = "v";
const char *const META_KEY_CALL = "call";
const char *const META_KEY_FUNCTION = "fn";
const char *const META_KEY_MODEL = "model";
const char *const META_KEY_PROMPT = "prompt";
const char *const META_KEY_CONFIG = "config";
const char *const META_KEY_NORMALIZED = "normalized";
const char *const META_KEY_RESULT_SPEC = "spec";
const char *const META_KEY_SYNC_MODE = "sync";
const char *const META_KEY_MAX_RETRIES = "retries";
const char *const META_KEY_DIMENSION = "dim";
const char *const META_KEY_DEFINITION_VERSION = "def_ver";
const char *const META_KEY_DEPS = "deps";
const char *const META_KEY_STATE_COLUMN = "state";
const char *const META_KEY_REQUEST_COLUMN = "req";
const char *const META_KEY_RESULT_SPEC_COLUMN = "spec_col";
// The comment the user wrote on the column (COMMENT 'x'). Stored inside the
// payload so that the definition and the user comment do not fight over the
// single comment field of the column schema.
const char *const META_KEY_USER_COMMENT = "uc";

inline int append_field(ObIAllocator &alloc,
                 ObSqlString &buf,
                 const char *key,
                 const char *value,
                 const int64_t value_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(key)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buf.append_fmt("%s=%ld:", key, value_len))) {
  } else if (value_len > 0 && OB_FAIL(buf.append(value, value_len))) {
  } else if (OB_FAIL(buf.append(";"))) {
  } else {
    UNUSED(alloc);
  }
  return ret;
}

inline int append_field(ObIAllocator &alloc, ObSqlString &buf, const char *key, const ObString &value)
{
  return append_field(alloc, buf, key,
                      NULL == value.ptr() ? "" : value.ptr(),
                      NULL == value.ptr() ? 0 : value.length());
}

inline int append_field(ObIAllocator &alloc, ObSqlString &buf, const char *key, const int64_t value)
{
  char num_buf[32];
  int64_t pos = 0;
  int ret = OB_SUCCESS;
  if (OB_FAIL(databuff_printf(num_buf, sizeof(num_buf), pos, "%ld", value))) {
  } else if (OB_FAIL(append_field(alloc, buf, key, num_buf, pos))) {
  }
  return ret;
}

// Parse the byte length of a field: the decimal digits sit between the '=' and the
// ':' that opens the value, i.e. the field reads "<key>=<len>:<value>;". The digits
// have to be parsed by hand because the buffer is not terminated there, which rules
// out ob_atoll() (it rejects any trailing byte).
inline int parse_field_length(const char *buf,
                              const int64_t eq,
                              const int64_t colon,
                              int64_t &len)
{
  int ret = OB_SUCCESS;
  len = 0;
  if (OB_ISNULL(buf) || eq + 1 >= colon) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = eq + 1; OB_SUCC(ret) && i < colon; ++i) {
      if (buf[i] < '0' || buf[i] > '9') {
        ret = OB_INVALID_ARGUMENT;
      } else if (len > (INT64_MAX - (buf[i] - '0')) / 10) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        len = len * 10 + (buf[i] - '0');
      }
    }
  }
  return ret;
}

// Read "<key>=<len>:" at `pos`, then the <len> raw bytes, then ';'. Advances pos.
inline int read_field(const char *buf,
               const int64_t buf_len,
               int64_t &pos,
               ObString &key,
               ObString &value)
{
  int ret = OB_SUCCESS;
  int64_t eq = -1;
  int64_t colon = -1;
  int64_t len = 0;
  if (OB_ISNULL(buf) || pos < 0 || pos >= buf_len) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = pos; OB_SUCC(ret) && i < buf_len; ++i) {
      if ('=' == buf[i] && -1 == eq) {
        eq = i;
      } else if (':' == buf[i] && -1 != eq) {
        colon = i;
        break;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (-1 == eq || -1 == colon) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(parse_field_length(buf, eq, colon, len))) {
    } else if (colon + 1 + len > buf_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      key.assign_ptr(buf + pos, static_cast<int32_t>(eq - pos));
      value.assign_ptr(buf + colon + 1, static_cast<int32_t>(len));
      pos = colon + 1 + len;
      if (pos < buf_len && ';' == buf[pos]) {
        ++pos;
      }
    }
  }
  return ret;
}

inline int copy_str(ObIAllocator &alloc, const ObString &src, ObString &dst)
{
  return ob_write_string(alloc, src, dst);
}

inline int copy_literal(ObIAllocator &alloc, const char *src, ObString &dst)
{
  ObString tmp;
  tmp.assign_ptr(src, static_cast<int32_t>(strlen(src)));
  return copy_str(alloc, tmp, dst);
}

} // namespace ai_column_meta

inline int build_ai_hidden_column_name(ObIAllocator &alloc,
                                                 const char *prefix,
                                                 const ObString &result_column_name,
                                                 ObString &name)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  if (OB_ISNULL(prefix) || result_column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(static_cast<int64_t>(strlen(prefix)) + result_column_name.length() >
                         OB_MAX_COLUMN_NAME_LENGTH)) {
    ret = OB_ERR_TOO_LONG_IDENT;
    LOG_USER_ERROR(OB_ERR_TOO_LONG_IDENT);
    LOG_WARN("ai column name is too long for its hidden columns", K(ret), K(prefix),
             K(result_column_name));
  } else if (OB_FAIL(buf.append(prefix)) ||
             OB_FAIL(buf.append(result_column_name))) {
  } else if (OB_FAIL(ai_column_meta::copy_str(alloc, buf.string(), name))) {
  }
  return ret;
}

// Cheap syntactic check: does `comment` start with the metadata marker? This is
// *not* how an AI column is identified (only AI_COLUMN_FLAG identifies one); it
// only recognises an existing payload, e.g. to keep it while a comment is
// rewritten.
inline bool comment_has_ai_column_meta(const ObString &comment)
{
  const int64_t prefix_len = static_cast<int64_t>(strlen(kAIColumnMetaPrefix));
  return NULL != comment.ptr() && comment.length() >= prefix_len &&
         0 == strncmp(comment.ptr(), kAIColumnMetaPrefix, prefix_len);
}

// Is this column an AI result column? Answered by the persisted column flag, so a
// user COMMENT can never forge (or clear) an AI column.
inline bool is_ai_column_column(const ObColumnSchemaV2 &column)
{
  return column.has_column_flag(AI_COLUMN_FLAG);
}

// Is this column an AI column *and* does its comment still hold a definition? Any
// consumer that reads the metadata must see true here; a false with a set flag is
// an inconsistent schema.
inline bool has_ai_column_definition(const ObColumnSchemaV2 &column)
{
  return is_ai_column_column(column) && comment_has_ai_column_meta(column.get_comment_str());
}

// A config expression is persisted only when the statement spelled one out, so
// the payload of a column without a config stays byte-identical to the ones
// written before the field existed (and older servers keep decoding it).
inline int append_config_field(ObIAllocator &alloc,
                              ObSqlString &buf,
                              const AIColumnDefinition &def)
{
  int ret = OB_SUCCESS;
  if (!def.config_expr_.empty()) {
    ret = ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_CONFIG, def.config_expr_);
  }
  return ret;
}

// Encode the definition together with the comment the user wrote on the column.
inline int encode_ai_column_definition(ObIAllocator &alloc,
                                          const AIColumnDefinition &def,
                                          const ObString &user_comment,
                                          ObString &comment)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  ObString deps;
  ObSqlString deps_buf;
  if (OB_FAIL(deps_buf.append(kAIColumnMetaPrefix)) ||
      OB_FAIL(deps_buf.append(";"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < def.dep_column_names_.count(); ++i) {
    if (i > 0 && OB_FAIL(deps_buf.append(","))) {
    } else if (OB_FAIL(deps_buf.append(def.dep_column_names_.at(i)))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(buf.append(kAIColumnMetaPrefix)) ||
             OB_FAIL(buf.append(";"))) {
  } else if (OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_VERSION, static_cast<int64_t>(1))) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_CALL, def.call_type_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_FUNCTION, def.ai_function_name_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_MODEL, def.model_name_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_PROMPT, def.prompt_expr_)) ||
             OB_FAIL(append_config_field(alloc, buf, def)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_NORMALIZED, def.normalized_expr_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_RESULT_SPEC, def.result_spec_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_SYNC_MODE, def.sync_mode_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_MAX_RETRIES, def.max_retries_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_DIMENSION, def.dimension_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_DEFINITION_VERSION, def.definition_version_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_STATE_COLUMN, def.state_column_name_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_REQUEST_COLUMN, def.request_column_name_)) ||
             OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_RESULT_SPEC_COLUMN, def.result_spec_column_name_))) {
  } else if (OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_DEPS, deps_buf.string()))) {
  } else if (OB_FAIL(ai_column_meta::append_field(alloc, buf, ai_column_meta::META_KEY_USER_COMMENT, user_comment))) {
  } else if (OB_FAIL(ai_column_meta::copy_str(alloc, buf.string(), comment))) {
  }
  UNUSED(deps);
  return ret;
}

// Encode the definition alone, for callers that only round-trip the definition
// (the catalog row does not need the user comment, __all_ai_column.comment holds it).
inline int encode_ai_column_definition(ObIAllocator &alloc,
                                          const AIColumnDefinition &def,
                                          ObString &comment)
{
  return encode_ai_column_definition(alloc, def, ObString(), comment);
}

// Decode the definition and the user comment it carries. `user_comment` is reset
// and stays empty when the payload predates the user comment field.
inline int decode_ai_column_definition(ObIAllocator &alloc,
                                          const ObString &comment,
                                          AIColumnDefinition &def,
                                          ObString &user_comment)
{
  int ret = OB_SUCCESS;
  const int64_t prefix_len = static_cast<int64_t>(strlen(kAIColumnMetaPrefix));
  def.reset();
  user_comment.reset();
  def.sync_mode_ = ObString(); // filled below
  if (!comment_has_ai_column_meta(comment)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not an ai column comment", K(ret), K(comment));
  } else {
    int64_t pos = prefix_len;
    if (pos < comment.length() && ';' == comment.ptr()[pos]) {
      ++pos;
    }
    while (OB_SUCC(ret) && pos < comment.length()) {
      ObString key;
      ObString value;
      if (OB_FAIL(ai_column_meta::read_field(comment.ptr(), comment.length(), pos, key, value))) {
      } else if (key == ai_column_meta::META_KEY_CALL) {
        def.call_type_ = atoll(value.ptr());
      } else if (key == ai_column_meta::META_KEY_FUNCTION) {
        ret = ai_column_meta::copy_str(alloc, value, def.ai_function_name_);
      } else if (key == ai_column_meta::META_KEY_MODEL) {
        ret = ai_column_meta::copy_str(alloc, value, def.model_name_);
      } else if (key == ai_column_meta::META_KEY_PROMPT) {
        ret = ai_column_meta::copy_str(alloc, value, def.prompt_expr_);
      } else if (key == ai_column_meta::META_KEY_CONFIG) {
        ret = ai_column_meta::copy_str(alloc, value, def.config_expr_);
      } else if (key == ai_column_meta::META_KEY_NORMALIZED) {
        ret = ai_column_meta::copy_str(alloc, value, def.normalized_expr_);
      } else if (key == ai_column_meta::META_KEY_RESULT_SPEC) {
        ret = ai_column_meta::copy_str(alloc, value, def.result_spec_);
      } else if (key == ai_column_meta::META_KEY_SYNC_MODE) {
        ret = ai_column_meta::copy_str(alloc, value, def.sync_mode_);
      } else if (key == ai_column_meta::META_KEY_MAX_RETRIES) {
        def.max_retries_ = atoll(value.ptr());
      } else if (key == ai_column_meta::META_KEY_DIMENSION) {
        def.dimension_ = atoll(value.ptr());
      } else if (key == ai_column_meta::META_KEY_DEFINITION_VERSION) {
        def.definition_version_ = atoll(value.ptr());
      } else if (key == ai_column_meta::META_KEY_STATE_COLUMN) {
        ret = ai_column_meta::copy_str(alloc, value, def.state_column_name_);
      } else if (key == ai_column_meta::META_KEY_REQUEST_COLUMN) {
        ret = ai_column_meta::copy_str(alloc, value, def.request_column_name_);
      } else if (key == ai_column_meta::META_KEY_RESULT_SPEC_COLUMN) {
        ret = ai_column_meta::copy_str(alloc, value, def.result_spec_column_name_);
      } else if (key == ai_column_meta::META_KEY_USER_COMMENT) {
        ret = ai_column_meta::copy_str(alloc, value, user_comment);
      } else if (key == ai_column_meta::META_KEY_DEPS) {
        // comma separated ordinary column names; the leading marker is dropped
        const int64_t marker_len = prefix_len + 1;
        if (value.length() > marker_len) {
          ObString list;
          list.assign_ptr(value.ptr() + marker_len, value.length() - marker_len);
          ObString remain = list;
          while (OB_SUCC(ret) && !remain.empty()) {
            const char *comma = static_cast<const char *>(memchr(remain.ptr(), ',', remain.length()));
            const int32_t len = NULL == comma ? remain.length()
                                              : static_cast<int32_t>(comma - remain.ptr());
            ObString one;
            ObString raw;
            raw.assign_ptr(remain.ptr(), len);
            if (OB_FAIL(ai_column_meta::copy_str(alloc, raw, one))) {
            } else if (OB_FAIL(def.dep_column_names_.push_back(one))) {
            } else if (NULL == comma) {
              remain.reset();
            } else {
              remain.assign_ptr(comma + 1, remain.length() - len - 1);
            }
          }
        }
      } else {
        LOG_DEBUG("skip unknown ai column meta field", K(key), K(value));
      }
    }
  }
  return ret;
}

// Decode the definition only, for callers that do not care about the user comment.
inline int decode_ai_column_definition(ObIAllocator &alloc,
                                          const ObString &comment,
                                          AIColumnDefinition &def)
{
  ObString ignored_user_comment;
  return decode_ai_column_definition(alloc, comment, def, ignored_user_comment);
}

// Install `def` and the comment the user wrote on `column`, marking `column` as an
// AI column. This is the only way an AI column comes into existence: the flag and
// the definition are written together and never separately, so no user COMMENT can
// create, forge or clear one.
inline int set_ai_column_definition(ObIAllocator &alloc,
                                    const AIColumnDefinition &def,
                                    const ObString &user_comment,
                                    ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  ObString comment;
  if (OB_FAIL(encode_ai_column_definition(alloc, def, user_comment, comment))) {
    LOG_WARN("fail to encode ai column definition", K(ret), K(def));
  } else if (OB_FAIL(column.set_comment(comment))) {
    LOG_WARN("fail to set ai column comment", K(ret), K(column.get_column_name_str()));
  } else {
    column.add_column_flag(AI_COLUMN_FLAG);
  }
  return ret;
}

// The comment the user wrote on `column`: the one the payload carries for an AI
// column, the plain comment otherwise. Reading a column comment for display should
// go through this so the internal payload never leaks out.
inline int get_ai_column_user_comment(ObIAllocator &alloc,
                                      const ObColumnSchemaV2 &column,
                                      ObString &user_comment)
{
  int ret = OB_SUCCESS;
  user_comment.reset();
  if (!is_ai_column_column(column)) {
    ret = ai_column_meta::copy_str(alloc, column.get_comment_str(), user_comment);
  } else {
    AIColumnDefinition def;
    if (OB_FAIL(decode_ai_column_definition(alloc, column.get_comment_str(), def, user_comment))) {
      LOG_WARN("fail to decode ai column definition", K(ret), K(column.get_column_name_str()));
    }
  }
  return ret;
}

// Write the user comment of `column` without touching any AI definition: an AI
// column keeps its definition (only the payload field changes), an ordinary column
// keeps the plain comment. `base_column` is the stored schema the new one derives
// from (ALTER TABLE MODIFY/CHANGE) and supplies the definition when `column` itself
// carries none; the ALTER path copies the resulting comment into the persisted
// column afterwards (ObDDLService::fill_new_column_attributes).
// An empty `user_comment` means "the statement did not spell a comment out": the
// existing one is kept, because no statement can distinguish the two cases here.
inline int set_ai_column_user_comment(ObIAllocator &alloc,
                                      const ObColumnSchemaV2 *base_column,
                                      const ObString &user_comment,
                                      ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  AIColumnDefinition def;
  ObString old_user_comment;
  // "Is this column an AI column?" is answered by the flag, never by the shape of
  // its comment: a user may write anything in a comment, including a string that
  // looks like encoded metadata, and that must stay an ordinary column.
  const bool has_payload = is_ai_column_column(column);
  const bool base_has_payload = NULL != base_column && is_ai_column_column(*base_column);
  if (has_payload) {
    if (OB_FAIL(decode_ai_column_definition(alloc, column.get_comment_str(), def, old_user_comment))) {
      LOG_WARN("fail to decode ai column definition", K(ret), K(column.get_column_name_str()));
    } else if (OB_FAIL(set_ai_column_definition(alloc,
                                                def,
                                                user_comment.empty() ? old_user_comment : user_comment,
                                                column))) {
      LOG_WARN("fail to set ai column definition", K(ret), K(column.get_column_name_str()));
    }
  } else if (base_has_payload) {
    if (OB_FAIL(decode_ai_column_definition(alloc, base_column->get_comment_str(), def, old_user_comment))) {
      LOG_WARN("fail to decode ai column definition", K(ret), K(base_column->get_column_name_str()));
    } else if (OB_FAIL(set_ai_column_definition(alloc,
                                                def,
                                                user_comment.empty() ? old_user_comment : user_comment,
                                                column))) {
      LOG_WARN("fail to set ai column definition", K(ret), K(column.get_column_name_str()));
    }
  } else if (OB_FAIL(column.set_comment(user_comment))) {
    LOG_WARN("fail to set column comment", K(ret), K(column.get_column_name_str()));
  }
  return ret;
}

// Carry the AI definition of `base_column` (the stored schema) onto `column`, the
// new schema an ALTER TABLE MODIFY/CHANGE statement built. A fresh AI COLUMN clause
// in the same statement wins; otherwise the stored definition is reused, and the
// stored user comment is kept unless the statement spelled one out. This is what
// makes "MODIFY c json" or "MODIFY c json COMMENT 'x'" preserve the AI
// configuration instead of silently dropping it while the flag survives.
inline int inherit_ai_column_definition(ObIAllocator &alloc,
                                        const ObColumnSchemaV2 &base_column,
                                        ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  AIColumnDefinition base_def;
  ObString base_user_comment;
  ObString own_user_comment;
  const bool own_has_definition = is_ai_column_column(column);
  if (!is_ai_column_column(base_column)) {
    // The stored column is not an AI column: nothing to carry over. The new column
    // may still be one (MODIFY turning an ordinary column into an AI column), in
    // which case the statement itself installed the definition.
  } else if (OB_FAIL(decode_ai_column_definition(alloc,
                                                 base_column.get_comment_str(),
                                                 base_def,
                                                 base_user_comment))) {
    LOG_WARN("fail to decode ai column definition", K(ret), K(base_column.get_column_name_str()));
  } else if (own_has_definition) {
    // the statement repeated AI COLUMN(...): keep that definition, keep the stored
    // user comment unless the statement spelled a new one out
    AIColumnDefinition own_def;
    if (OB_FAIL(decode_ai_column_definition(alloc,
                                            column.get_comment_str(),
                                            own_def,
                                            own_user_comment))) {
      LOG_WARN("fail to decode ai column definition", K(ret), K(column.get_column_name_str()));
    } else if (!own_user_comment.empty() || base_user_comment.empty()) {
      column.add_column_flag(AI_COLUMN_FLAG);
    } else if (OB_FAIL(set_ai_column_definition(alloc, own_def, base_user_comment, column))) {
      LOG_WARN("fail to set ai column definition", K(ret), K(column.get_column_name_str()));
    }
  } else {
    // no AI COLUMN clause: reuse the stored definition, refresh the user comment
    ObString user_comment;
    if (OB_FAIL(ai_column_meta::copy_str(alloc, column.get_comment_str(), user_comment))) {
      LOG_WARN("fail to copy column comment", K(ret), K(column.get_column_name_str()));
    } else if (OB_FAIL(set_ai_column_definition(alloc,
                                                base_def,
                                                user_comment.empty() ? base_user_comment : user_comment,
                                                column))) {
      LOG_WARN("fail to set ai column definition", K(ret), K(column.get_column_name_str()));
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
