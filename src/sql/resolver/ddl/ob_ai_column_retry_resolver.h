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

#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/resolver/ddl/ob_ai_column_retry_stmt.h"

namespace oceanbase
{
namespace sql
{

// Resolves `ALTER TABLE <table> RETRY AI COLUMN <column> [WHERE <predicate>]`.
//
// The retry is an *alter table action* in the grammar, not a statement of its own,
// so that `RETRY AI COLUMN c, ADD COLUMN x INT` is refused as an unsupported
// combination instead of being a syntax error. ObAlterTableResolver therefore
// inspects the action list first (find_retry_action) and hands a statement that
// consists of the retry action alone to this resolver; a statement that mixes the
// retry with any other action is refused with OB_NOT_SUPPORTED before anything is
// resolved.
//
// What this resolver checks is the part the schema alone can answer: the table
// exists, the named column exists, and it really is an AI column. Everything about
// the rows to retry (the status pseudo column, the hidden state column, the task
// table) belongs to the AI column runtime and is left to the executor.
class ObAIColumnRetryResolver : public ObDDLResolver
{
public:
  // Layout of the `ALTER TABLE` node (T_ALTER_TABLE, see alter_table_stmt) and of
  // the retry action node (T_AI_COLUMN_RETRY).
  static const int64_t ALTER_TABLE_NODE_COUNT = 4;
  static const int64_t TABLE_NODE = 0;
  static const int64_t ACTION_LIST_NODE = 1;
  static const int64_t ACTION_CHILD_COUNT = 2;
  static const int64_t ACTION_COLUMN_NODE = 0;
  static const int64_t ACTION_WHERE_NODE = 1;

  // Locate the retry action in the (flattened) alter table action list.
  // `retry_action_index` is -1 when the statement has no retry action; `action_count`
  // is the number of actions of the statement either way.
  static int find_retry_action(const ParseNode *action_list,
                               int64_t &retry_action_index,
                               int64_t &action_count);

  explicit ObAIColumnRetryResolver(ObResolverParams &params);
  virtual ~ObAIColumnRetryResolver();
  virtual int resolve(const ParseNode &parse_tree) override;

private:
  int resolve_table(const ParseNode &table_node, ObAIColumnRetryStmt &stmt);
  int resolve_retry_action(const ParseNode &action_node, ObAIColumnRetryStmt &stmt);

  // The table the retry statement targets, resolved from the schema.
  const share::schema::ObTableSchema *table_schema_;

  DISALLOW_COPY_AND_ASSIGN(ObAIColumnRetryResolver);
};

} // namespace sql
} // namespace oceanbase
