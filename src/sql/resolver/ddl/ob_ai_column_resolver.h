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

#include "lib/ob_define.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "share/schema/ob_ai_column_definition.h"
#include "share/schema/ob_column_schema.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{

// One AI column definition. It lives in share/schema so that the schema service
// (which persists the __all_ai_column catalog row) uses the very same struct and
// codec as the resolver; the resolver only adds the syntax layer on top of it.
using ObAIColumnDefinition = share::schema::AIColumnDefinition;

// DDL support for AI columns: syntax validation, metadata encoding and the
// hidden columns the asynchronous runtime owns.
class ObAIColumnResolver
{
public:
  enum AIColumnCallType
  {
    AI_COLUMN_CALL_COMPLETE = 1,
    AI_COLUMN_CALL_EMBED = 2
  };

  // Prefix of the encoded metadata that an AI result column carries in its
  // comment. It is *not* how an AI column is recognised (see is_ai_column).
  static const char *AI_COLUMN_META_PREFIX;
  // Prefixes of the three hidden columns owned by the runtime.
  static const char *AI_STATE_COLUMN_PREFIX;       // __ai_state_<col_id>
  static const char *AI_REQUEST_COLUMN_PREFIX;     // __ai_req_<col_id>
  static const char *AI_RESULT_SPEC_COLUMN_PREFIX; // __ai_result_spec_<col_id>
  // Result type instruction appended to the normalized expression.
  static const char *AI_JSON_RESULT_INSTRUCTION;   // " Return exactly one valid JSON value"
  static const char *AI_VECTOR_RESULT_INSTRUCTION; // " Return a vector of dimension N"

  // ---------------------------------------------------------------------
  // column <-> metadata
  // ---------------------------------------------------------------------

  // Is this column an AI result column? Answered by the persisted column flag
  // (share::schema::AI_COLUMN_FLAG, stored in __all_column.column_flags), never by
  // the text of the comment, so an ordinary COMMENT cannot forge or clear one.
  static bool is_ai_column(const share::schema::ObColumnSchemaV2 &column)
  { return share::schema::is_ai_column_column(column); }

  // Is this column an AI column whose comment still holds its definition?
  static bool has_definition(const share::schema::ObColumnSchemaV2 &column)
  { return share::schema::has_ai_column_definition(column); }

  // Is `comment` shaped like encoded metadata? A syntactic check only: writing such
  // a comment on an ordinary column does NOT make it an AI column.
  static bool is_ai_column_meta(const common::ObString &comment)
  { return share::schema::comment_has_ai_column_meta(comment); }

  // Encode / decode the metadata carried by an AI result column comment. Format:
  // "__ai_column__;" followed by "<key>=<len>:<raw>;" fields, where <len> is the
  // byte length of <raw>. Length prefixing keeps arbitrary prompt text (quotes,
  // ';', '=') round-trippable without escaping, and the same holds for the user
  // comment, which is stored as the "uc" field of the payload.
  static int encode_definition(common::ObIAllocator &alloc,
                               const ObAIColumnDefinition &def,
                               common::ObString &comment)
  { return share::schema::encode_ai_column_definition(alloc, def, comment); }
  static int encode_definition(common::ObIAllocator &alloc,
                               const ObAIColumnDefinition &def,
                               const common::ObString &user_comment,
                               common::ObString &comment)
  { return share::schema::encode_ai_column_definition(alloc, def, user_comment, comment); }
  static int decode_definition(common::ObIAllocator &alloc,
                               const common::ObString &comment,
                               ObAIColumnDefinition &def)
  { return share::schema::decode_ai_column_definition(alloc, comment, def); }
  static int decode_definition(common::ObIAllocator &alloc,
                               const common::ObString &comment,
                               ObAIColumnDefinition &def,
                               common::ObString &user_comment)
  { return share::schema::decode_ai_column_definition(alloc, comment, def, user_comment); }

  // Install `def` on `column`, which also marks it as an AI column. The only way
  // an AI column is created.
  static int set_definition(common::ObIAllocator &alloc,
                            const ObAIColumnDefinition &def,
                            const common::ObString &user_comment,
                            share::schema::ObColumnSchemaV2 &column)
  { return share::schema::set_ai_column_definition(alloc, def, user_comment, column); }

  // The comment the user wrote on the column (payload "uc" for an AI column, the
  // plain comment otherwise), so callers never show the internal payload.
  static int get_user_comment(common::ObIAllocator &alloc,
                              const share::schema::ObColumnSchemaV2 &column,
                              common::ObString &user_comment)
  { return share::schema::get_ai_column_user_comment(alloc, column, user_comment); }

  // Handle a COMMENT clause: it never touches the AI definition, it only rewrites
  // the user comment (inside the payload for an AI column).
  static int set_user_comment(common::ObIAllocator &alloc,
                              const common::ObString &user_comment,
                              share::schema::ObColumnSchemaV2 &column)
  { return share::schema::set_ai_column_user_comment(alloc, NULL, user_comment, column); }
  static int set_user_comment(common::ObIAllocator &alloc,
                              const share::schema::ObColumnSchemaV2 &base_column,
                              const common::ObString &user_comment,
                              share::schema::ObColumnSchemaV2 &column)
  { return share::schema::set_ai_column_user_comment(alloc, &base_column, user_comment, column); }

  // Carry the AI definition of the stored schema onto the column ALTER is building.
  static int inherit_definition(common::ObIAllocator &alloc,
                                const share::schema::ObColumnSchemaV2 &base_column,
                                share::schema::ObColumnSchemaV2 &column)
  { return share::schema::inherit_ai_column_definition(alloc, base_column, column); }

  // ---------------------------------------------------------------------
  // CREATE TABLE / ALTER TABLE ... ADD|MODIFY COLUMN
  // ---------------------------------------------------------------------

  // Resolve the `AI COLUMN ( <ai_func> ) [WITH (...)]` attribute of one column:
  // validates the AI function, its arguments, the result column type (json /
  // varchar / vector(N)), the declared dimension and the WITH options, fills
  // `def`, and installs it on the column together with `user_comment` (the comment
  // already attached to the column, if the statement wrote one).
  static int resolve_ai_column_attribute(const ParseNode *ai_column_node,
                                         share::schema::ObColumnSchemaV2 &column,
                                         common::ObIAllocator &alloc,
                                         const common::ObString &user_comment,
                                         ObAIColumnDefinition &def);

  // Reject chains of AI columns: every column referenced by the prompt must be
  // an ordinary column of the table, never another AI column (oracle: 1235).
  static int check_ai_column_dependencies(const share::schema::ObTableSchema &table_schema,
                                          const ObAIColumnDefinition &def);

  // Append the three hidden columns (state / request / result_spec) of every AI
  // column of the table. Idempotent: columns that already exist are skipped.
  static int append_hidden_columns(share::schema::ObTableSchema &table_schema,
                                   common::ObIAllocator &alloc);

  // ---------------------------------------------------------------------
  // helpers (unit tested in unittest/sql/resolver/ddl/test_ob_ai_column.cpp)
  // ---------------------------------------------------------------------

  // Name of the hidden column of one AI result column: prefix + result column
  // name, e.g. "__ai_state_tags". The name (not the column id) is used because
  // the id of the result column is only known after the schema service assigns
  // it, while the hidden columns have to be created in the same resolver pass.
  static int build_hidden_column_name(common::ObIAllocator &alloc,
                                      const char *prefix,
                                      const common::ObString &result_column_name,
                                      common::ObString &name)
  { return share::schema::build_ai_hidden_column_name(alloc, prefix, result_column_name, name); }
  // "json" / "varchar" / "vector:4" for a result column of the given type.
  static int build_result_spec(common::ObIAllocator &alloc,
                               const share::schema::ObColumnSchemaV2 &column,
                               int64_t &dimension,
                               common::ObString &result_spec);
  // Parse one option list value into sync_mode / max_retries.
  static int parse_option_value(const common::ObString &name,
                                const int64_t int_value,
                                const common::ObString &str_value,
                                ObAIColumnDefinition &def);
};

} // namespace sql
} // namespace oceanbase
