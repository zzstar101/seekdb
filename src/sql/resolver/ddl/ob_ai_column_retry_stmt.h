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
#include "lib/string/ob_string.h"
#include "sql/resolver/ddl/ob_alter_table_stmt.h"

namespace oceanbase
{
namespace sql
{

// What `ALTER TABLE <table> RETRY AI COLUMN <column> [WHERE <predicate>]` carries.
//
// The statement is an alter table statement as far as the privilege check
// (stmt::T_ALTER_TABLE) and the command dispatcher are concerned, but it changes
// no schema: it re-queues the AI column tasks of the rows the predicate selects,
// inside the transaction of the statement itself.
//
// The predicate is kept as the text the user wrote, never resolved against the
// schema. An unquoted `status` in it is the AI column state pseudo column
// (PENDING / READY / FAILED) while a backquoted `status` is the user column, and
// only the AI column runtime knows the hidden state column and the state values it
// stores, so the predicate is rewritten there. That is also why an ordinary column
// named `status` can never make the pseudo column ambiguous at resolve time.
struct ObAIColumnRetryArg
{
  ObAIColumnRetryArg()
    : table_id_(common::OB_INVALID_ID),
      column_id_(common::OB_INVALID_ID)
  {
  }
  ~ObAIColumnRetryArg() {}
  bool is_valid() const
  {
    return common::OB_INVALID_ID != table_id_ && common::OB_INVALID_ID != column_id_;
  }
  TO_STRING_KV(K_(table_id), K_(column_id), K_(database_name), K_(table_name),
               K_(column_name), K_(where_clause));

  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString database_name_;
  common::ObString table_name_;
  common::ObString column_name_;
  // Text of the user WHERE clause, empty when the statement selects every row.
  common::ObString where_clause_;
};

class ObAIColumnRetryStmt : public ObAlterTableStmt
{
public:
  ObAIColumnRetryStmt() : ObAlterTableStmt(), retry_arg_() {}
  explicit ObAIColumnRetryStmt(common::ObIAllocator *name_pool)
    : ObAlterTableStmt(name_pool), retry_arg_() {}
  virtual ~ObAIColumnRetryStmt() {}

  const ObAIColumnRetryArg &get_ai_column_retry_arg() const { return retry_arg_; }
  ObAIColumnRetryArg &get_ai_column_retry_arg() { return retry_arg_; }

private:
  ObAIColumnRetryArg retry_arg_;
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnRetryStmt);
};

} // namespace sql
} // namespace oceanbase
