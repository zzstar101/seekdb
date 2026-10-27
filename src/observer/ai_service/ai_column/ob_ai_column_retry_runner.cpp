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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_service/ai_column/ob_ai_column_retry_runner.h"

#include "lib/allocator/page_arena.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/container/ob_se_array.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/object/ob_object.h"
#include "observer/ai_service/ai_column/ob_ai_column_service.h"

namespace oceanbase
{
using namespace common;
namespace observer
{
namespace ai_column
{

namespace
{

constexpr int64_t STATUS_KEYWORD_LEN = 6;

inline bool is_space_char(const char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c;
}

inline bool is_identifier_start(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || '_' == c;
}

inline bool is_identifier_char(const char c)
{
  return is_identifier_start(c) || (c >= '0' && c <= '9') || '$' == c;
}

inline bool is_digit_char(const char c) { return c >= '0' && c <= '9'; }

// The bare spelling `status` addresses the STATUS pseudo column.  The business
// column of the user is only reachable through the quoted spelling `status`, so
// the case insensitive keyword is the whole difference between the two.
inline bool is_status_keyword(const char *str, const int64_t len)
{
  bool is_status = (STATUS_KEYWORD_LEN == len);
  const char *keyword = "status";
  for (int64_t i = 0; is_status && i < len; ++i) {
    char c = str[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    is_status = (keyword[i] == c);
  }
  return is_status;
}

// The inner connection of the ALTER is created with privilege checking disabled,
// because its normal statements are the internal catalog lookups and the
// maintenance SQL over the task queue and the AI column catalog.  The enumeration
// SELECT is not one of them: it carries the WHERE clause of the caller, so it has
// to be resolved and executed under the privileges of the caller.  Otherwise a
// caller that may only ALTER and SELECT the target table could read any other
// table through a subquery of its predicate, e.g.
//
//   ALTER TABLE t RETRY AI COLUMN c WHERE EXISTS (SELECT 1 FROM secret_table)
//
// This guard turns the check on for exactly that statement and restores the
// previous value on the way out (also on the error path, so the maintenance SQL
// that follows a failed enumeration still runs as internal SQL of the ALTER).
class AiColumnCallerPrivGuard
{
public:
  explicit AiColumnCallerPrivGuard(common::sqlclient::ObISQLConnection &conn)
      : conn_(conn), saved_check_priv_(conn.is_check_priv())
  {
    conn_.set_check_priv(true);
  }
  ~AiColumnCallerPrivGuard() { conn_.set_check_priv(saved_check_priv_); }
private:
  common::sqlclient::ObISQLConnection &conn_;
  bool saved_check_priv_;
  DISALLOW_COPY_AND_ASSIGN(AiColumnCallerPrivGuard);
};

} // namespace

AiColumnRetryRunner &AiColumnRetryRunner::get_instance()
{
  static AiColumnRetryRunner instance;
  return instance;
}

const AiColumnDef *AiColumnRetryRunner::find_def(const AiColumnDefArray &defs,
                                                 const uint64_t column_id)
{
  const AiColumnDef *def = NULL;
  for (int64_t i = 0; NULL == def && i < defs.count(); ++i) {
    if (defs.at(i).column_id_ == column_id) {
      def = &defs.at(i);
    }
  }
  return def;
}

int AiColumnRetryRunner::build_state_name_expr(const AiColumnDef &def, ObSqlString &expr)
{
  int ret = OB_SUCCESS;
  expr.reset();
  if (OB_FAIL(expr.append_fmt("(CASE `%.*s`",
                              static_cast<int32_t>(def.state_column_name_.length()),
                              def.state_column_name_.ptr()))) {
    LOG_WARN("append the status pseudo column head failed", K(ret));
  }
  for (int64_t state = AI_COLUMN_TASK_PENDING;
       OB_SUCC(ret) && state < AI_COLUMN_TASK_STATE_MAX; ++state) {
    if (OB_FAIL(expr.append_fmt(" WHEN %ld THEN '%s'", state,
                                ai_column_task_state_str(state)))) {
      LOG_WARN("append a status pseudo column value failed", K(ret), K(state));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(expr.append(" END)"))) {
    LOG_WARN("append the status pseudo column tail failed", K(ret));
  }
  return ret;
}

int AiColumnRetryRunner::rewrite_status_predicate(const AiColumnDef &def,
                                                  const ObString &where_clause,
                                                  ObSqlString &rewritten)
{
  int ret = OB_SUCCESS;
  ObSqlString state_expr;
  rewritten.reset();
  if (where_clause.empty()) {
    // A retry without a predicate selects every row of the table.
    if (OB_FAIL(rewritten.append("1 = 1"))) {
      LOG_WARN("append the all rows predicate failed", K(ret));
    }
  } else if (OB_FAIL(build_state_name_expr(def, state_expr))) {
    LOG_WARN("failed to build the status pseudo column expression", K(ret), K(def));
  } else {
    const char *text = where_clause.ptr();
    const int64_t len = where_clause.length();
    int64_t i = 0;
    while (OB_SUCC(ret) && i < len) {
      const char c = text[i];
      if ('\'' == c || '"' == c) {
        // A literal is copied unchanged: `status` inside a string is data of the
        // user, not the pseudo column.
        const int64_t start = i;
        const char quote = c;
        bool closed = false;
        ++i;
        while (i < len && !closed) {
          if ('\\' == text[i]) {
            i += (i + 1 < len) ? 2 : 1;
          } else if (quote == text[i]) {
            if (i + 1 < len && quote == text[i + 1]) {
              i += 2;
            } else {
              ++i;
              closed = true;
            }
          } else {
            ++i;
          }
        }
        if (OB_FAIL(rewritten.append(text + start, i - start))) {
          LOG_WARN("append a predicate literal failed", K(ret));
        }
      } else if ('`' == c) {
        // A quoted identifier is a column of the table, including `status`.
        const int64_t start = i;
        ++i;
        while (i < len && '`' != text[i]) {
          ++i;
        }
        if (i < len) {
          ++i;
        }
        if (OB_FAIL(rewritten.append(text + start, i - start))) {
          LOG_WARN("append a predicate identifier failed", K(ret));
        }
      } else if (is_identifier_start(c)) {
        int64_t prev = i - 1;
        while (prev >= 0 && is_space_char(text[prev])) {
          --prev;
        }
        const int64_t start = i;
        while (i < len && is_identifier_char(text[i])) {
          ++i;
        }
        if (is_status_keyword(text + start, i - start)) {
          if (prev >= 0 && '.' == text[prev]) {
            // Rewriting a qualified reference would leave the qualifier dangling in
            // front of the expression, so the qualified spelling is rejected instead
            // of producing a confusing syntax error.
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "qualifying the AI column status pseudo column");
          } else if (OB_FAIL(rewritten.append(state_expr.string()))) {
            LOG_WARN("append the status pseudo column expression failed", K(ret));
          }
        } else if (OB_FAIL(rewritten.append(text + start, i - start))) {
          LOG_WARN("append a predicate identifier failed", K(ret));
        }
      } else if (is_digit_char(c)) {
        const int64_t start = i;
        while (i < len && (is_digit_char(text[i]) || '.' == text[i])) {
          ++i;
        }
        if (OB_FAIL(rewritten.append(text + start, i - start))) {
          LOG_WARN("append a predicate number failed", K(ret));
        }
      } else {
        if (OB_FAIL(rewritten.append(text + i, 1))) {
          LOG_WARN("append a predicate character failed", K(ret));
        }
        ++i;
      }
    }
  }
  return ret;
}

int AiColumnRetryRunner::new_request_id(ObIAllocator &allocator, ObString &request_id)
{
  int ret = OB_SUCCESS;
  char buf[AI_COLUMN_REQUEST_ID_LEN];
  int64_t pos = 0;
  if (OB_FAIL(databuff_printf(buf, sizeof(buf), pos, "req$%ld$%ld",
                              ObTimeUtility::current_time(),
                              ATOMIC_FAA(&request_seq_, 1)))) {
    LOG_WARN("failed to format an ai column request id", K(ret));
  } else if (pos <= 0 || pos >= static_cast<int64_t>(sizeof(buf))) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("failed to build an ai column request id", K(ret), K(pos));
  } else if (OB_FAIL(ob_write_string(allocator, ObString(static_cast<int32_t>(pos), buf),
                                    request_id))) {
    LOG_WARN("failed to copy the ai column request id", K(ret));
  }
  return ret;
}

int AiColumnRetryRunner::retry_ai_column(ObIAllocator &allocator,
                                         const uint64_t table_id,
                                         const uint64_t column_id,
                                         const ObString &where_clause,
                                         sqlclient::ObISQLConnection &conn,
                                         int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  AiColumnDefArray defs;
  AiColumnDefArray one_def;
  ObSEArray<sql::AiColumnKeyColumn, 4> pk_columns;
  const AiColumnDef *def = NULL;
  ObString request_id;
  ObSqlString predicate;
  ObSqlString select_sql;
  ObSqlString invalidate_sql;
  ObSqlString enqueue_sql;
  ObSEArray<ObString, 16> selected_rowkeys;
  ObISQLClient::ReadResult read_result;
  sqlclient::ObMySQLResult *result = NULL;
  affected_rows = 0;

  if (OB_FAIL(AiColumnService::get_instance().get_ai_column_defs(allocator, table_id, defs))) {
    LOG_WARN("failed to load the ai column definitions", K(ret), K(table_id));
  } else if (OB_ISNULL(def = find_def(defs, column_id))) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  } else if (!def->is_valid()) {
    // The runtime can only queue a column whose definition carries the hidden
    // columns, the model and a materialized prompt expression.
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying an AI column that the runtime does not maintain");
  } else {
    bool has_ai_column = false;
    if (OB_FAIL(AiColumnService::get_instance().get_table_ai_columns(allocator, table_id,
                                                                    has_ai_column, pk_columns))) {
      LOG_WARN("failed to load the primary key of the table", K(ret), K(table_id));
    } else if (pk_columns.empty()) {
      // A task row is addressed by the printed primary key of its row, so a table
      // without one can not carry a row scoped retry.
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying an AI column of a table without a primary key");
    }
  }

  if (OB_SUCC(ret) && OB_FAIL(new_request_id(allocator, request_id))) {
    LOG_WARN("failed to build an ai column request id", K(ret));
  }

  if (OB_SUCC(ret) && OB_FAIL(rewrite_status_predicate(*def, where_clause, predicate))) {
    LOG_WARN("failed to rewrite the retry predicate", K(ret), K(table_id), K(column_id));
  }
  // The enumeration is a plain, unlocked SELECT.  A locking read (FOR UPDATE) is
  // checked by check_single_table_priv_for_update_, which requires UPDATE or
  // DELETE on top of SELECT, so a caller that may only ALTER and SELECT its own
  // table could not retry it at all.  The rows are kept stable across the
  // enumeration / enqueue boundary by the request id watermark instead: the
  // invalidation below stamps the request id of this retry on the row, and a
  // concurrent DML of the same row stamps a newer one, so a row that raced with
  // this enumeration is retried at most one extra time and never publishes a
  // stale result.
  if (OB_SUCC(ret)) {
    if (OB_FAIL(select_sql.assign("SELECT "))) {
      LOG_WARN("failed to build the enumeration statement", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < pk_columns.count(); ++i) {
      if (i > 0 && OB_FAIL(select_sql.append(", "))) {
        LOG_WARN("failed to build the enumeration statement", K(ret));
      } else if (OB_FAIL(select_sql.append("`"))
                 || OB_FAIL(select_sql.append(pk_columns.at(i).column_name_))
                 || OB_FAIL(select_sql.append("`"))) {
        LOG_WARN("failed to append a primary key column", K(ret), K(i));
      }
    }
    if (OB_SUCC(ret)
        && (OB_FAIL(select_sql.append(" FROM "))
            || OB_FAIL(select_sql.append(def->table_name_))
            || OB_FAIL(select_sql.append(" WHERE "))
            || OB_FAIL(select_sql.append(predicate.string())))) {
      LOG_WARN("failed to build the enumeration statement", K(ret), K(*def));
    }
  }

  // The enumeration SELECT carries the WHERE clause of the caller, so it is the
  // one statement of the retry that must be privilege checked: the guard below
  // enables the check of the inner connection for exactly this statement (and
  // reading its rows) and restores it before the maintenance SQL below.
  if (OB_SUCC(ret)) {
    AiColumnCallerPrivGuard caller_priv_guard(conn);
    if (OB_SUCC(ret) && OB_FAIL(conn.execute_read(select_sql.string(), read_result))) {
      LOG_WARN("failed to select the rows of the retry", K(ret), K(select_sql));
    } else if (OB_SUCC(ret) && OB_ISNULL(result = read_result.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the result of the retry enumeration is null", K(ret));
    }

    while (OB_SUCC(ret) && OB_SUCC(ret = result->next())) {
      // The task row is addressed by the printed primary key of the row, exactly like
      // the DML hook prints it, so both halves of the runtime agree on a row.
      ObArenaAllocator row_allocator("AIColRetryRow");
      ObSEArray<ObString, 4> key_names;
      ObSEArray<ObString, 4> key_literals;
      ObString rowkey;
      ObString saved_rowkey;
      for (int64_t i = 0; OB_SUCC(ret) && i < pk_columns.count(); ++i) {
        ObObj value;
        char *literal_buf = NULL;
        int64_t literal_len = 0;
        int64_t literal_pos = 0;
        ObObjPrintParams key_print_params;
        // Keep the same byte-preserving character identity as the DML enqueue path.
        key_print_params.character_hex_safe_represent_ = true;
        if (OB_FAIL(result->get_obj(i, value))) {
          LOG_WARN("failed to read a primary key value", K(ret), K(i));
        } else if (OB_FAIL(value.print_sql_literal(literal_buf, literal_len, literal_pos,
                                                   row_allocator, key_print_params))) {
          LOG_WARN("failed to print a primary key literal", K(ret), K(i));
        } else if (OB_FAIL(key_names.push_back(pk_columns.at(i).column_name_))) {
          LOG_WARN("failed to collect a primary key column", K(ret), K(i));
        } else if (OB_FAIL(key_literals.push_back(
                     ObString(static_cast<int32_t>(literal_pos), literal_buf)))) {
          LOG_WARN("failed to collect a primary key literal", K(ret), K(i));
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(AiColumnTaskSql::build_rowkey_predicate(row_allocator, key_names,
                                                             key_literals, rowkey))) {
        LOG_WARN("failed to build the rowkey predicate of the retry", K(ret));
      }
      if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, rowkey, saved_rowkey))) {
        LOG_WARN("failed to retain the selected row identity", K(ret));
      } else if (OB_SUCC(ret) && OB_FAIL(selected_rowkeys.push_back(saved_rowkey))) {
        LOG_WARN("failed to collect the selected row identity", K(ret));
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  }
  // An inner connection cannot execute another statement while a ReadResult
  // refers to it.  Retain only keys, release the cursor, then write in the same
  // transaction: the invalidation of the user row and the task queue row is
  // internal SQL of the ALTER, so it runs with privilege checking disabled again
  // and the guard above covers only the caller predicate and nothing else.
  read_result.reset();
  result = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < selected_rowkeys.count(); ++i) {
    const ObString &rowkey = selected_rowkeys.at(i);
    int64_t written = 0;
    one_def.reuse();
    if (OB_FAIL(one_def.push_back(*def))) {
      LOG_WARN("failed to collect the retried column", K(ret));
    } else if (OB_SUCC(ret)
               && OB_FAIL(AiColumnTaskSql::invalidate_row(one_def, rowkey, request_id,
                                                          invalidate_sql))) {
      LOG_WARN("failed to build the invalidation of the retried row", K(ret), K(rowkey));
    } else if (OB_SUCC(ret) && OB_FAIL(conn.execute_write(invalidate_sql.string(), written))) {
      LOG_WARN("failed to invalidate the retried row", K(ret), K(invalidate_sql));
    } else if (OB_SUCC(ret)
               && OB_FAIL(AiColumnTaskSql::enqueue_task(*def, rowkey, request_id,
                                                        enqueue_sql))) {
      LOG_WARN("failed to build the task of the retried row", K(ret), K(rowkey));
    } else if (OB_SUCC(ret) && OB_FAIL(conn.execute_write(enqueue_sql.string(), written))) {
      LOG_WARN("failed to queue the task of the retried row", K(ret), K(enqueue_sql));
    } else if (OB_SUCC(ret)) {
      ++affected_rows;
    }
  }
  LOG_INFO("retried an ai column", K(ret), K(table_id), K(column_id), K(where_clause),
           K(affected_rows), K(select_sql));
  return ret;
}

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
