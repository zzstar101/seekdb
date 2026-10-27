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

#include "share/schema/ob_ai_column_definition.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{

// The value of an AI column is the result of a model call, so business SQL must
// never assign the column itself: the assignment would be replaced by the
// publication of the computed result, or - worse - it would survive as a value
// that no model ever produced. Omitting the column is the supported way to let
// the AI column machinery fill it in.
//
// The machinery itself keeps writing those columns:
//
//  * the publication of a computed result goes through ObCommonSqlProxy, i.e.
//    through an ObInnerSQLConnection built with
//    create_connection_with_owned_session(), which marks its own session with
//    set_real_inner_session(true);
//  * the DML hook of ObTableModifyOp (it resets the result of a row whose
//    dependency changed) and the retry statement issue their statements on an
//    inner connection that shares the *user* session, so that session is inner
//    but not real inner; both hold an AiColumnHookGuard around those statements
//    and are recognised by it.
//
// Neither state is reachable from SQL: no grammar rule or session variable sets
// the inner flag, and the hidden state/request columns of an AI column can only
// be named by an inner session (ObStmtResolver::get_column_schema() rejects
// them otherwise), so an internal write cannot be forged either. A business
// statement - including a stored procedure, whose inner connection reuses the
// user session - is therefore rejected here.
class AiColumnWriteGuard
{
public:
  // The clause whose AI column target is being resolved, used for the error
  // message only.
  enum WriteClause
  {
    INSERT_CLAUSE = 0,
    ON_DUPLICATE_KEY_UPDATE_CLAUSE = 1,
    UPDATE_CLAUSE = 2
  };

  AiColumnWriteGuard() = delete;

  // `ai_column_hook_active` is AiColumnHookGuard::is_active(). It is passed in
  // by the caller so that this header keeps no dependency on the engine.
  static bool is_internal_write(const ObSQLSessionInfo *session, const bool ai_column_hook_active);
  static bool is_internal_write_flags(const bool is_inner_session,
                                      const bool is_real_inner_session,
                                      const bool ai_column_hook_active);
  // An AI column is recognised by its persisted schema flag, never by the text
  // of its comment: a user COMMENT can fake the AI column syntax.
  static bool is_protected_column(const share::schema::ObColumnSchemaV2 &column)
  {
    return share::schema::is_ai_column_column(column);
  }
  // Looks up `column_id` of `table_id` and reports whether it is an AI result
  // column.
  static int is_protected_column(ObSchemaChecker &schema_checker,
                                 const uint64_t table_id,
                                 const uint64_t column_id,
                                 bool &is_protected);
  // LOG_USER_ERROR(OB_NOT_SUPPORTED, ...) renders "<subject> not supported".
  static const char *write_subject(const WriteClause clause);
};

inline bool AiColumnWriteGuard::is_internal_write_flags(const bool is_inner_session,
                                                        const bool is_real_inner_session,
                                                        const bool ai_column_hook_active)
{
  return ai_column_hook_active || (is_inner_session && is_real_inner_session);
}

inline bool AiColumnWriteGuard::is_internal_write(const ObSQLSessionInfo *session,
                                                  const bool ai_column_hook_active)
{
  // the guard of the DML hook is a thread local that only the AI column runtime
  // and the retry statement hold, it decides on its own
  return is_internal_write_flags(NULL != session && session->is_inner(),
                                 NULL != session && session->is_real_inner_session(),
                                 ai_column_hook_active);
}

inline int AiColumnWriteGuard::is_protected_column(ObSchemaChecker &schema_checker,
                                                   const uint64_t table_id,
                                                   const uint64_t column_id,
                                                   bool &is_protected)
{
  int ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *column_schema = NULL;
  is_protected = false;
  if (OB_FAIL(schema_checker.get_column_schema(table_id, column_id, column_schema,
                                               true /*get_hidden*/))) {
  } else if (NULL != column_schema && is_protected_column(*column_schema)) {
    is_protected = true;
  }
  return ret;
}

inline const char *AiColumnWriteGuard::write_subject(const WriteClause clause)
{
  const char *subject = "assigning an AI column in INSERT is";
  if (ON_DUPLICATE_KEY_UPDATE_CLAUSE == clause) {
    subject = "assigning an AI column in ON DUPLICATE KEY UPDATE is";
  } else if (UPDATE_CLAUSE == clause) {
    subject = "assigning an AI column in UPDATE is";
  }
  return subject;
}

}  // namespace sql
}  // namespace oceanbase
