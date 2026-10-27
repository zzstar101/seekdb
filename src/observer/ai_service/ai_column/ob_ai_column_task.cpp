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

#include "observer/ai_service/ai_column/ob_ai_column_task.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace observer
{
namespace ai_column
{

using namespace common;

////////////////////////////////////////////////////////////////////////////////
// helpers
////////////////////////////////////////////////////////////////////////////////

int AiColumnTaskSql::append_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  // Hex encoding preserves embedded NUL and is independent of the session's
  // NO_BACKSLASH_ESCAPES mode. Explicit conversion keeps JSON/text operations
  // in the character domain rather than inheriting a binary collation.
  static const char hex[] = "0123456789ABCDEF";
  if (value.length() > 0 && OB_ISNULL(value.ptr())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append("CONVERT(X'"))) {
    LOG_WARN("append literal prefix failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value.ptr()[i]);
    const char pair[] = {hex[c >> 4], hex[c & 15]};
    if (OB_FAIL(sql.append(pair, 2))) {
      LOG_WARN("append literal bytes failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("' USING utf8mb4)"))) {
    LOG_WARN("append literal suffix failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::append_literal(ObSqlString &sql, const char *value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(value)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null literal", K(ret));
  } else {
    const ObString str(static_cast<int32_t>(strlen(value)), value);
    ret = append_literal(sql, str);
  }
  return ret;
}

int AiColumnTaskSql::append_backquoted(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (name.empty() || OB_ISNULL(name.ptr()) || nullptr != memchr(name.ptr(), '\0', name.length())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid identifier", K(ret));
  } else if (OB_FAIL(sql.append("`"))) {
    LOG_WARN("append identifier prefix failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (OB_FAIL(sql.append(name.ptr() + i, 1))) {
      LOG_WARN("append identifier byte failed", K(ret));
    } else if ('`' == name.ptr()[i] && OB_FAIL(sql.append("`"))) {
      LOG_WARN("escape identifier delimiter failed", K(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("`"))) {
    LOG_WARN("append identifier suffix failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::append_rowkey_predicate(ObSqlString &sql, const ObString &predicate)
{
  int ret = OB_SUCCESS;
  if (predicate.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty rowkey predicate", K(ret));
  } else if (OB_FAIL(sql.append(predicate))) {
    LOG_WARN("append predicate failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::build_rowkey_predicate(ObIAllocator &allocator,
                                            const ObIArray<ObString> &column_names,
                                            const ObIArray<ObString> &literal_values,
                                            ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  predicate.reset();
  if (column_names.count() != literal_values.count() || 0 == column_names.count()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("rowkey column / value count mismatch", K(ret),
             "column_count", column_names.count(), "value_count", literal_values.count());
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_names.count(); ++i) {
    if (0 != i && OB_FAIL(sql.append(" and "))) {
      LOG_WARN("append separator failed", K(ret));
    } else if (OB_FAIL(append_backquoted(sql, column_names.at(i)))) {
      LOG_WARN("append column name failed", K(ret));
    } else if (OB_FAIL(sql.append(" = "))) {
      LOG_WARN("append equal failed", K(ret));
    } else if (OB_FAIL(sql.append(literal_values.at(i)))) {
      // literal_values must already be a valid SQL literal: "2" or "'abc'"
      LOG_WARN("append literal failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (sql.length() > AI_COLUMN_ROWKEY_PREDICATE_LEN) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("rowkey predicate too long", K(ret), "len", sql.length());
    } else if (OB_FAIL(ob_write_string(allocator, ObString(sql.length(), sql.ptr()), predicate, true))) {
      LOG_WARN("copy predicate failed", K(ret));
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// DML side (same transaction as the business DML)
////////////////////////////////////////////////////////////////////////////////

// UPDATE db.tbl
//   SET `tags` = NULL, `__ai_req_tags` = 'req-1', `__ai_state_tags` = 1,
//       `emb` = NULL, `__ai_req_emb` = 'req-1', `__ai_state_emb` = 1
//   WHERE `id` = 2
int AiColumnTaskSql::invalidate_row(const AiColumnDefArray &defs,
                                    const ObString &rowkey_predicate,
                                    const ObString &request_id,
                                    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (0 == defs.count() || rowkey_predicate.empty() || request_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), "def_count", defs.count(),
             K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET ",
                                    static_cast<int32_t>(defs.at(0).table_name_.length()),
                                    defs.at(0).table_name_.ptr()))) {
    LOG_WARN("append update head failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
    const AiColumnDef &def = defs.at(i);
    if (OB_FAIL(sql.append(0 == i ? "" : ", "))
        || OB_FAIL(append_backquoted(sql, def.column_name_))
        || OB_FAIL(sql.append(" = NULL, "))
        || OB_FAIL(append_backquoted(sql, def.state_column_name_))
        || OB_FAIL(sql.append_fmt(" = %ld, ", AI_COLUMN_TASK_PENDING))
        || OB_FAIL(append_backquoted(sql, def.request_column_name_))
        || OB_FAIL(sql.append(" = "))) {
      LOG_WARN("append invalidation failed", K(ret), K(def));
    } else if (OB_FAIL(append_literal(sql, request_id))) {
      LOG_WARN("append request id failed", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(sql.append(" WHERE "))) {
      LOG_WARN("append where failed", K(ret));
    } else if (OB_FAIL(append_rowkey_predicate(sql, rowkey_predicate))) {
      LOG_WARN("append predicate failed", K(ret));
    }
  }
  return ret;
}

// REPLACE INTO __all_ai_column_task
//   (table_id, column_id, rowkey_predicate, request_id, state, retry_count,
//    max_retries, definition_version, eligible_ts, call_type, model_name,
//    result_spec, prompt, gmt_create, gmt_modified)
// SELECT 500001, 16, '`id` = 2', 'req-1', 1, 0, 2, 1, now(6), 1, 'mock',
//        '', JSON_OBJECT('v', 1, 'prompt', (concat('Classify: ', content)),
//                        'config', (json_object('temperature', 0.2))),
//        now(6), now(6)
// FROM db.tbl WHERE `id` = 2
//
// The prompt column carries the versioned JSON input envelope, built by the same
// statement (and therefore by the same user transaction) that evaluated the DML:
// the native JSON_OBJECT constructor preserves the SQL type of both argument
// expressions, so a JSON prompt object stays an object, a plain VARCHAR stays a
// string and a NULL stays JSON null -- no text sniffing anywhere.  A prompt that
// evaluates to NULL must not abort the business DML either, which is the second
// reason the expression is wrapped instead of pasted into the NOT NULL `prompt`
// column directly.
int AiColumnTaskSql::enqueue_task(const AiColumnDef &def,
                                  const ObString &rowkey_predicate,
                                  const ObString &request_id,
                                  ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (!def.is_valid() || rowkey_predicate.empty() || request_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(def), K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt(
                 "REPLACE INTO %s (table_id, column_id, rowkey_predicate, request_id, "
                 "state, retry_count, max_retries, definition_version, eligible_ts, call_type, "
                 "model_name, "
                 "result_spec, prompt, gmt_create, gmt_modified) SELECT %lu, %ld, ",
                 AI_COLUMN_TASK_TABLE, def.table_id_, def.column_id_))) {
    LOG_WARN("append enqueue head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, rowkey_predicate))) {
    LOG_WARN("append predicate literal failed", K(ret));
  } else if (OB_FAIL(sql.append(", "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, request_id))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(", %ld, 0, %ld, %ld, now(6), %ld, ",
                                    AI_COLUMN_TASK_PENDING, def.max_retries_,
                                    def.definition_version_, def.call_type_))) {
    LOG_WARN("append state values failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, def.model_name_))) {
    LOG_WARN("append model name failed", K(ret));
  } else if (OB_FAIL(sql.append(", "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, def.result_spec_))) {
    LOG_WARN("append result spec failed", K(ret));
  } else if (OB_FAIL(sql.append(", JSON_OBJECT("))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt("'%s', %ld, '%s', (",
                                    AI_COLUMN_INPUT_KEY_VERSION,
                                    AI_COLUMN_INPUT_ENVELOPE_VERSION,
                                    AI_COLUMN_INPUT_KEY_PROMPT))) {
    LOG_WARN("append envelope head failed", K(ret));
  } else if (OB_FAIL(sql.append(def.prompt_expr_))) {
    // the prompt is evaluated by this very transaction, against the row the DML
    // just wrote, so the task never depends on later row changes
    LOG_WARN("append prompt expression failed", K(ret), K(def));
  } else if (OB_FAIL(sql.append(")"))) {
    LOG_WARN("append prompt expression failed", K(ret));
  } else if (!def.config_expr_.empty() &&
             (OB_FAIL(sql.append_fmt(", '%s', (", AI_COLUMN_INPUT_KEY_CONFIG)) ||
              OB_FAIL(sql.append(def.config_expr_)) ||
              OB_FAIL(sql.append(")")))) {
    // the config is snapshotted here as well; a config that evaluates to NULL
    // becomes JSON null and behaves exactly like an omitted one
    LOG_WARN("append config expression failed", K(ret), K(def));
  } else if (OB_FAIL(sql.append("), now(6), now(6) FROM "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(sql.append(def.table_name_))) {
    LOG_WARN("append table name failed", K(ret));
  } else if (OB_FAIL(sql.append(" WHERE "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_rowkey_predicate(sql, rowkey_predicate))) {
    LOG_WARN("append predicate failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::delete_tasks_of_row(const uint64_t table_id,
                                         const ObString &rowkey_predicate,
                                         ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == table_id || rowkey_predicate.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_id), K(rowkey_predicate));
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu AND "
                                    "rowkey_predicate = ",
                                    AI_COLUMN_TASK_TABLE, table_id))) {
    LOG_WARN("append delete head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, rowkey_predicate))) {
    LOG_WARN("append predicate literal failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::delete_tasks_of_request(const uint64_t table_id,
                                             const ObString &rowkey_predicate,
                                             const ObString &request_id,
                                             ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == table_id || rowkey_predicate.empty() || request_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_id), K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu AND "
                                    "rowkey_predicate = ",
                                    AI_COLUMN_TASK_TABLE, table_id))) {
    LOG_WARN("append delete head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, rowkey_predicate))) {
    LOG_WARN("append predicate literal failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, request_id))) {
    LOG_WARN("append request id failed", K(ret));
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// worker side
////////////////////////////////////////////////////////////////////////////////

// Fencing predicates of every terminal update of a queue row: the row must still
// be the one this worker claimed (same request id, same definition version) and
// this worker must still be the lease owner.  A worker whose lease expired and
// was taken over by another one therefore can not write anything.
static int append_task_fence(ObSqlString &sql, const AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(" AND definition_version = %ld", task.definition_version_))) {
    LOG_WARN("append definition version guard failed", K(ret), K(task));
  } else if (OB_FAIL(sql.append(" AND lease_owner = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(AiColumnTaskSql::append_literal(sql, task.lease_owner_))) {
    LOG_WARN("append lease owner failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::delete_task(const AiColumnTaskRow &task, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (task.table_id_ == OB_INVALID_ID || task.column_id_ <= 0 ||
      task.definition_version_ <= 0 || task.rowkey_predicate_.empty() || task.request_id_.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append_fmt("DELETE FROM %s WHERE table_id = %lu AND column_id = %ld "
                                   "AND definition_version = %ld AND rowkey_predicate = ",
                                   AI_COLUMN_TASK_TABLE, task.table_id_, task.column_id_,
                                   task.definition_version_))
             || OB_FAIL(append_literal(sql, task.rowkey_predicate_))
             || OB_FAIL(sql.append(" AND request_id = "))
             || OB_FAIL(append_literal(sql, task.request_id_))) {
    LOG_WARN("build exact task cleanup failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::pick_eligible_tasks(const int64_t batch_size, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (batch_size <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid batch size", K(ret), K(batch_size));
  } else if (OB_FAIL(sql.append_fmt(
                 "SELECT table_id, column_id, rowkey_predicate, request_id, state, "
                 "retry_count, max_retries, call_type, definition_version, model_name, "
                 "prompt, result_spec "
                 "FROM %s t WHERE ((state IN (%ld, %ld) AND "
                 "(eligible_ts IS NULL OR eligible_ts <= now(6))) "
                 "OR (state = %ld AND (lease_expire_ts IS NULL OR lease_expire_ts < now(6))) "
                 "OR (state IN (%ld, %ld) AND NOT EXISTS (SELECT 1 FROM %s sibling "
                 "WHERE sibling.table_id = t.table_id AND sibling.rowkey_predicate = t.rowkey_predicate "
                 "AND sibling.request_id = t.request_id AND sibling.state NOT IN (%ld, %ld)))) "
                 "ORDER BY eligible_ts, table_id, column_id LIMIT %ld",
                 AI_COLUMN_TASK_TABLE,
                 AI_COLUMN_TASK_PENDING, AI_COLUMN_TASK_RETRY_WAIT,
                 AI_COLUMN_TASK_RUNNING, AI_COLUMN_TASK_READY, AI_COLUMN_TASK_FAILED,
                 AI_COLUMN_TASK_TABLE, AI_COLUMN_TASK_READY, AI_COLUMN_TASK_FAILED, batch_size))) {
    LOG_WARN("append pick sql failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::claim_task(const AiColumnTaskRow &task,
                                const ObString &lease_owner,
                                const int64_t lease_timeout_us,
                                ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == task.table_id_ || OB_INVALID_ID == static_cast<uint64_t>(task.column_id_) ||
      task.rowkey_predicate_.empty() || task.request_id_.empty() || lease_owner.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(task), K(lease_owner));
  } else if (OB_FAIL(sql.append_fmt("UPDATE %s SET state = %ld, lease_owner = ",
                                    AI_COLUMN_TASK_TABLE, AI_COLUMN_TASK_RUNNING))) {
    LOG_WARN("append claim head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, lease_owner))) {
    LOG_WARN("append owner failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(", lease_expire_ts = date_add(now(6), interval %ld microsecond), "
                                    "gmt_modified = now(6) WHERE table_id = %lu AND column_id = %ld "
                                    "AND rowkey_predicate = ",
                                    lease_timeout_us,
                                    task.table_id_, task.column_id_))) {
    LOG_WARN("append claim body failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.rowkey_predicate_))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.request_id_))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(
                 " AND definition_version = %ld AND state NOT IN (%ld, %ld) AND "
                 "((state IN (%ld, %ld) AND (eligible_ts IS NULL OR eligible_ts <= now(6))) "
                 "OR (state = %ld AND (lease_expire_ts IS NULL OR lease_expire_ts < now(6))))",
                 task.definition_version_,
                 AI_COLUMN_TASK_READY, AI_COLUMN_TASK_FAILED,
                 AI_COLUMN_TASK_PENDING, AI_COLUMN_TASK_RETRY_WAIT,
                 AI_COLUMN_TASK_RUNNING))) {
    LOG_WARN("append claim condition failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::store_success(const AiColumnTaskRow &task,
                                   const ObString &result_data,
                                   ObSqlString &sql,
                                   const bool result_is_null)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_FAIL(sql.append_fmt("UPDATE %s SET state = %ld, result_data = ",
                             AI_COLUMN_TASK_TABLE, AI_COLUMN_TASK_READY))) {
    LOG_WARN("append head failed", K(ret));
  } else if (OB_FAIL(result_is_null ? sql.append("NULL")
                                    : append_literal(sql, result_data))) {
    LOG_WARN("append result failed", K(ret));
  } else if (OB_FAIL(sql.append(", error_msg = NULL, gmt_modified = now(6) WHERE table_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt("%lu AND column_id = %ld AND rowkey_predicate = ",
                                    task.table_id_, task.column_id_))) {
    LOG_WARN("append key failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.rowkey_predicate_))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.request_id_))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND state = %ld", AI_COLUMN_TASK_RUNNING))) {
    LOG_WARN("append state guard failed", K(ret));
  } else if (OB_FAIL(append_task_fence(sql, task))) {
    LOG_WARN("append task fence failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnTaskSql::store_retry(const AiColumnTaskRow &task,
                                 const int64_t next_eligible_ts,
                                 const ObString &error_msg,
                                 ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (next_eligible_ts <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid next eligible ts", K(ret), K(next_eligible_ts));
  } else if (OB_FAIL(sql.append_fmt("UPDATE %s SET state = %ld, retry_count = retry_count + 1, "
                                    "eligible_ts = from_unixtime(%ld.%06ld), "
                                    "lease_owner = NULL, lease_expire_ts = NULL, error_msg = ",
                                    AI_COLUMN_TASK_TABLE, AI_COLUMN_TASK_RETRY_WAIT,
                                    next_eligible_ts / 1000000L, next_eligible_ts % 1000000L))) {
    LOG_WARN("append retry head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, error_msg))) {
    LOG_WARN("append error msg failed", K(ret));
  } else if (OB_FAIL(sql.append(", gmt_modified = now(6) WHERE table_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt("%lu AND column_id = %ld AND rowkey_predicate = ",
                                    task.table_id_, task.column_id_))) {
    LOG_WARN("append key failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.rowkey_predicate_))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.request_id_))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND state = %ld", AI_COLUMN_TASK_RUNNING))) {
    LOG_WARN("append state guard failed", K(ret));
  } else if (OB_FAIL(append_task_fence(sql, task))) {
    LOG_WARN("append task fence failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnTaskSql::store_fail(const AiColumnTaskRow &task,
                                const ObString &error_msg,
                                ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_FAIL(sql.append_fmt("UPDATE %s SET state = %ld, error_msg = ",
                             AI_COLUMN_TASK_TABLE, AI_COLUMN_TASK_FAILED))) {
    LOG_WARN("append head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, error_msg))) {
    LOG_WARN("append error msg failed", K(ret));
  } else if (OB_FAIL(sql.append(", lease_owner = NULL, lease_expire_ts = NULL, "
                                "gmt_modified = now(6) WHERE table_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt("%lu AND column_id = %ld AND rowkey_predicate = ",
                                    task.table_id_, task.column_id_))) {
    LOG_WARN("append key failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.rowkey_predicate_))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, task.request_id_))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND state = %ld", AI_COLUMN_TASK_RUNNING))) {
    LOG_WARN("append state guard failed", K(ret));
  } else if (OB_FAIL(append_task_fence(sql, task))) {
    LOG_WARN("append task fence failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnTaskSql::count_unfinished_siblings(const uint64_t table_id,
                                               const ObString &rowkey_predicate,
                                               const ObString &request_id,
                                               ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == table_id || rowkey_predicate.empty() || request_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_id), K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt("SELECT count(*) FROM %s WHERE table_id = %lu AND "
                                    "rowkey_predicate = ",
                                    AI_COLUMN_TASK_TABLE, table_id))) {
    LOG_WARN("append head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, rowkey_predicate))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, request_id))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND state NOT IN (%ld, %ld)",
                                    AI_COLUMN_TASK_READY, AI_COLUMN_TASK_FAILED))) {
    LOG_WARN("append state condition failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::select_publish_payload(const uint64_t table_id,
                                            const ObString &rowkey_predicate,
                                            const ObString &request_id,
                                            ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == table_id || rowkey_predicate.empty() || request_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_id), K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt(
                 "SELECT column_id, state, definition_version, result_data, "
                 "result_data IS NULL AS result_is_null, result_spec, call_type FROM %s "
                 "WHERE table_id = %lu AND rowkey_predicate = ",
                 AI_COLUMN_TASK_TABLE, table_id))) {
    LOG_WARN("append head failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, rowkey_predicate))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append(" AND request_id = "))) {
    LOG_WARN("append failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, request_id))) {
    LOG_WARN("append request id failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND state IN (%ld, %ld) ORDER BY column_id",
                                    AI_COLUMN_TASK_READY, AI_COLUMN_TASK_FAILED))) {
    LOG_WARN("append failed", K(ret));
  }
  return ret;
}

// UPDATE db.tbl SET
//   `tags` = '{"category":"mock"}', `__ai_state_tags` = 4, `__ai_req_tags` = NULL,
//   `score` = '7', `__ai_state_score` = 4, `__ai_req_score` = NULL
// WHERE `id` = 2 AND `__ai_req_tags` = 'req-1' AND `__ai_req_score` = 'req-1'
int AiColumnTaskSql::publish_row(const AiColumnDefArray &defs,
                                 const ObString &rowkey_predicate,
                                 const ObString &request_id,
                                 const ObIArray<ObString> &results,
                                 ObSqlString &sql,
                                 const ObIArray<int64_t> *states,
                                 const ObIArray<bool> *result_nulls)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (0 == defs.count() || defs.count() != results.count() || rowkey_predicate.empty() ||
      request_id.empty() || (nullptr != states && states->count() != defs.count()) ||
      (nullptr != result_nulls && result_nulls->count() != defs.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), "def_count", defs.count(),
             "result_count", results.count(), K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET ",
                                    static_cast<int32_t>(defs.at(0).table_name_.length()),
                                    defs.at(0).table_name_.ptr()))) {
    LOG_WARN("append update head failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
    const AiColumnDef &def = defs.at(i);
    const ObString &result = results.at(i);
    const int64_t state = nullptr == states ? AI_COLUMN_TASK_READY : states->at(i);
    // a failed sibling, and a successful EMBED whose input was SQL NULL, both
    // publish SQL NULL; an empty string stays an empty string
    const bool publish_null = state == AI_COLUMN_TASK_FAILED ||
                              (nullptr != result_nulls && result_nulls->at(i));
    if (state != AI_COLUMN_TASK_READY && state != AI_COLUMN_TASK_FAILED) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("cannot publish a nonterminal AI column", K(ret), K(state));
    } else if (OB_FAIL(sql.append(0 == i ? "" : ", "))
        || OB_FAIL(append_backquoted(sql, def.column_name_))
        || OB_FAIL(sql.append(" = "))) {
      LOG_WARN("append column failed", K(ret), K(def));
    } else if (OB_FAIL(publish_null ? sql.append("NULL") : append_literal(sql, result))) {
      LOG_WARN("append result failed", K(ret));
    } else if (OB_FAIL(sql.append(", "))
        || OB_FAIL(append_backquoted(sql, def.state_column_name_))
        || OB_FAIL(sql.append_fmt(" = %ld, ", state))
        || OB_FAIL(append_backquoted(sql, def.request_column_name_))
        || OB_FAIL(sql.append(" = NULL"))) {
      LOG_WARN("append hidden columns failed", K(ret), K(def));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(sql.append(" WHERE "))) {
      LOG_WARN("append failed", K(ret));
    } else if (OB_FAIL(append_rowkey_predicate(sql, rowkey_predicate))) {
      LOG_WARN("append predicate failed", K(ret));
    }
    // anti late overwrite CAS: only the request that is still recorded in the
    // hidden request columns may publish
    for (int64_t i = 0; OB_SUCC(ret) && i < defs.count(); ++i) {
      const AiColumnDef &def = defs.at(i);
      if (OB_FAIL(sql.append(" AND "))
          || OB_FAIL(append_backquoted(sql, def.request_column_name_))
          || OB_FAIL(sql.append(" = "))) {
        LOG_WARN("append cas column failed", K(ret));
      } else if (OB_FAIL(append_literal(sql, request_id))) {
        LOG_WARN("append cas value failed", K(ret));
      }
    }
  }
  return ret;
}

int AiColumnTaskSql::publish_failure(const AiColumnDef &def,
                                     const ObString &rowkey_predicate,
                                     const ObString &request_id,
                                     ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (!def.is_valid() || rowkey_predicate.empty() || request_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(def), K(rowkey_predicate), K(request_id));
  } else if (OB_FAIL(sql.append_fmt("UPDATE %.*s SET `%.*s` = %ld, `%.*s` = NULL WHERE ",
                                    static_cast<int32_t>(def.table_name_.length()),
                                    def.table_name_.ptr(),
                                    static_cast<int32_t>(def.state_column_name_.length()),
                                    def.state_column_name_.ptr(),
                                    AI_COLUMN_TASK_FAILED,
                                    static_cast<int32_t>(def.request_column_name_.length()),
                                    def.request_column_name_.ptr()))) {
    LOG_WARN("append failure head failed", K(ret));
  } else if (OB_FAIL(append_rowkey_predicate(sql, rowkey_predicate))) {
    LOG_WARN("append predicate failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt(" AND `%.*s` = ",
                                    static_cast<int32_t>(def.request_column_name_.length()),
                                    def.request_column_name_.ptr()))) {
    LOG_WARN("append cas column failed", K(ret));
  } else if (OB_FAIL(append_literal(sql, request_id))) {
    LOG_WARN("append cas value failed", K(ret));
  }
  return ret;
}

int AiColumnTaskSql::count_not_ready_rows(const AiColumnDef &def, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (!def.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column def", K(ret), K(def));
  } else if (OB_FAIL(sql.append_fmt("SELECT count(*) FROM %.*s WHERE `%.*s` IS NULL OR `%.*s` <> %ld",
                                    static_cast<int32_t>(def.table_name_.length()),
                                    def.table_name_.ptr(),
                                    static_cast<int32_t>(def.state_column_name_.length()),
                                    def.state_column_name_.ptr(),
                                    static_cast<int32_t>(def.state_column_name_.length()),
                                    def.state_column_name_.ptr(),
                                    AI_COLUMN_TASK_READY))) {
    LOG_WARN("append strict read sql failed", K(ret), K(def));
  }
  return ret;
}

int AiColumnTaskSql::select_catalog(const uint64_t table_id, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  sql.reset();
  if (OB_INVALID_ID == table_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table id", K(ret), K(table_id));
  } else if (OB_FAIL(sql.append_fmt(
                 "SELECT column_id, column_name, max_retries, definition_version, "
                 "normalized_expr, prompt_expr, "
                 "state_column_id, request_column_id, result_spec_column_id, "
                 "model_name, call_type, result_spec, dimension, deps FROM %s WHERE table_id = %lu",
                 AI_COLUMN_CATALOG_TABLE, table_id))) {
    LOG_WARN("append catalog sql failed", K(ret));
  }
  return ret;
}

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
