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
#include "lib/string/ob_sql_string.h"
#include "observer/ai_service/ai_column/ob_ai_column_task.h"
#include "sql/engine/cmd/ob_ai_column_retry_executor.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
namespace sqlclient
{
class ObISQLConnection;
}
}
namespace observer
{
namespace ai_column
{

// The retry half of an AI column: it turns
//
//   ALTER TABLE t RETRY AI COLUMN c [WHERE <predicate>]
//
// into one invalidation and one task per selected row, executed on the inner
// connection of the ALTER, i.e. inside the transaction of the statement.  The
// rows that the WHERE clause selects are the rows whose result is dropped and
// whose task is re-queued, so a retry of zero rows is a successful no-op that
// leaves the task queue untouched.
//
// The engine can not do this itself: it does not know the state machine of an AI
// column, it can not resolve the STATUS pseudo column to the hidden state column
// and it can not build the task SQL.  So the engine resolves the ALTER, opens the
// inner connection and calls this runner through sql::IAiColumnRetryHook; the
// statements themselves come from the very same builders the DML hook uses
// (AiColumnTaskSql::invalidate_row / enqueue_task), which keeps the retry task
// indistinguishable from the task the DML hook queues for a changed row.
//
// No model is called here and nothing is written after the commit: the task is
// queued in the user transaction and only the background worker, after the
// commit, talks to the model.
//
// Privilege boundary: the WHERE clause is the predicate of the caller, so the
// enumeration SELECT runs with the privilege check of the caller enabled -- a
// subquery over a table the caller may not read fails with
// OB_ERR_NO_TABLE_PRIVILEGE, exactly like the same subquery in a SELECT.  The
// catalog lookups and the maintenance SQL that reset the result and queue the
// task are internal SQL of the ALTER and keep running with the check disabled.
class AiColumnRetryRunner : public sql::IAiColumnRetryHook
{
public:
  static AiColumnRetryRunner &get_instance();

  virtual ~AiColumnRetryRunner() {}

  // Retry the column of every row selected by where_clause on `conn`, inside the
  // transaction of the caller.  An empty where_clause selects every row.
  int retry_ai_column(common::ObIAllocator &allocator,
                      const uint64_t table_id,
                      const uint64_t column_id,
                      const common::ObString &where_clause,
                      common::sqlclient::ObISQLConnection &conn,
                      int64_t &affected_rows) override;

private:
  // The hidden state column the STATUS pseudo column reports is an integer, while
  // the user writes names.  The rewrite replaces an unquoted `status` with a CASE
  // expression that maps the integer back to its name, so the predicate keeps its
  // own semantics (SQL NULL, LIKE, arithmetic, ...) and the runtime never has to
  // parse the user predicate itself.
  static int rewrite_status_predicate(const AiColumnDef &def,
                                      const common::ObString &where_clause,
                                      common::ObSqlString &rewritten);
  static int build_state_name_expr(const AiColumnDef &def, common::ObSqlString &expr);

  // The definition of the named column, or NULL when the catalog says the column
  // is not an AI column.
  static const AiColumnDef *find_def(const AiColumnDefArray &defs, const uint64_t column_id);

  int new_request_id(common::ObIAllocator &allocator, common::ObString &request_id);

  AiColumnRetryRunner() : request_seq_(0) {}

  int64_t request_seq_;

  DISALLOW_COPY_AND_ASSIGN(AiColumnRetryRunner);
};

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
