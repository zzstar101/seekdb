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

#define USING_LOG_PREFIX SQL_EXE

#include "sql/engine/cmd/ob_ai_column_retry_executor.h"
#include "sql/resolver/ddl/ob_ai_column_retry_stmt.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan_ctx.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/rc/ob_server_runtime.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace
{
// RETRY is a command, not an active DML statement. Grant its internal SQL
// hidden-column visibility without inventing a nested statement/transaction.
class AiRetrySessionGuard
{
public:
  explicit AiRetrySessionGuard(ObSQLSessionInfo &session)
      : session_(session), was_inner_(session.is_inner()), type_(session.get_session_type())
  {
    session_.set_inner_session();
  }
  ~AiRetrySessionGuard()
  {
    if (was_inner_) {
      session_.set_inner_session();
    } else {
      session_.set_user_session();
    }
    session_.set_session_type(type_);
  }
private:
  ObSQLSessionInfo &session_;
  bool was_inner_;
  ObSQLSessionInfo::SessionType type_;
};
}

int ObAIColumnRetryExecutor::execute(ObExecContext &ctx, ObAIColumnRetryStmt &stmt)
{
  int ret = OB_SUCCESS;
  const ObAIColumnRetryArg &arg = stmt.get_ai_column_retry_arg();
  ObSQLSessionInfo *session = ctx.get_my_session();
  IAiColumnRetryHook *hook = share::server_service<IAiColumnRetryHook>();
  sqlclient::ObISQLConnection *conn = NULL;
  sqlclient::ObISQLConnectionGuard conn_guard;
  ObArenaAllocator allocator("AIColumnRetry");
  bool need_tx = false;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the retry statement carries no ai column", K(ret), K(arg));
  } else if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null", K(ret));
  } else if (OB_ISNULL(ctx.get_sql_proxy())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", K(ret));
  } else if (OB_ISNULL(hook)) {
    // Without the runtime there is nothing that could reset the result or queue the
    // tasks, so the statement is refused instead of silently doing nothing.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying an AI column without the AI column runtime");
    LOG_WARN("the ai column runtime is not installed", K(ret), K(arg));
  } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(
                       session, conn_guard))) {
    LOG_WARN("failed to create the inner sql connection of the retry statement", K(ret));
  } else if (OB_ISNULL(conn = conn_guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the inner sql connection is null", K(ret));
  } else if (!session->is_in_transaction()) {
    // Reuse the transaction of the session when the statement runs inside one, so the
    // resets and the queued tasks commit or roll back together with the caller's work.
    if (OB_FAIL(conn->start_transaction())) {
      LOG_WARN("failed to start the transaction of the retry statement", K(ret));
    } else {
      need_tx = true;
    }
  }
  if (OB_SUCC(ret)) {
    AiRetrySessionGuard session_guard(*session);
    AiColumnHookGuard hook_guard;
    if (OB_FAIL(hook->retry_ai_column(allocator, arg.table_id_, arg.column_id_,
                                     arg.where_clause_, *conn, affected_rows))) {
      LOG_WARN("failed to retry the ai column", K(ret), K(arg));
    }
  }
  if (need_tx) {
    if (OB_SUCC(ret)) {
      int tmp_ret = conn->commit();
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to commit the retry statement", K(tmp_ret), K(arg));
        ret = tmp_ret;
      }
    } else {
      int tmp_ret = conn->rollback();
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to rollback the retry statement", K(tmp_ret), K(arg));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObPhysicalPlanCtx *plan_ctx = ctx.get_physical_plan_ctx();
    if (OB_NOT_NULL(plan_ctx)) {
      plan_ctx->set_affected_rows(affected_rows);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
