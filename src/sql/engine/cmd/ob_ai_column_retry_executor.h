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
#include "lib/string/ob_string.h"
#include "sql/engine/ob_exec_context.h"

namespace oceanbase
{
namespace common
{
namespace sqlclient
{
class ObISQLConnection;
}
} // namespace common
namespace sql
{

class ObAIColumnRetryStmt;

// The runtime side of `ALTER TABLE ... RETRY AI COLUMN ...`.
//
// The SQL layer owns the syntax, the "the named column really is an AI column"
// check and the transaction, but it cannot build the statements that perform the
// retry: the `status` state pseudo column, the hidden state column, the request id
// and the task table are AI column runtime concepts, and the SQL layer does not
// depend on observer code. The runtime therefore installs an implementation of this
// interface in a server service slot at startup, next to the AI column DML hook:
//   share::bind_server_service<sql::IAiColumnRetryHook>(&runner);
class IAiColumnRetryHook
{
public:
  virtual ~IAiColumnRetryHook() {}
  // Re-queue the tasks of one AI column for the rows `where_clause` selects (an
  // empty clause selects every row of the table). For every selected row the AI
  // column result is reset, the hidden state goes back to pending, a fresh request
  // id is stamped on the request column and one task row is queued, all of it on
  // `conn`, which carries the transaction of the statement. Selecting zero rows is
  // a success and queues nothing. `affected_rows` reports the number of rows whose
  // tasks were re-queued.
  virtual int retry_ai_column(common::ObIAllocator &allocator,
                              const uint64_t table_id,
                              const uint64_t column_id,
                              const common::ObString &where_clause,
                              common::sqlclient::ObISQLConnection &conn,
                              int64_t &affected_rows) = 0;
};

// Executes the retry action of an alter table statement: it opens the inner
// connection of the current session, so that the result resets and the task rows are
// written in the transaction of the statement itself, and hands the work over to the
// AI column runtime. No schema is changed, nothing is written after the commit and
// no model call is made here.
class ObAIColumnRetryExecutor
{
public:
  ObAIColumnRetryExecutor() {}
  virtual ~ObAIColumnRetryExecutor() {}
  int execute(ObExecContext &ctx, ObAIColumnRetryStmt &stmt);

private:
  DISALLOW_COPY_AND_ASSIGN(ObAIColumnRetryExecutor);
};

} // namespace sql
} // namespace oceanbase
