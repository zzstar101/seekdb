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
#include "lib/ob_name_def.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"
#include "observer/ai_service/ai_column/ob_ai_column_task_defs.h"

namespace oceanbase
{
namespace observer
{
namespace ai_column
{

// System table that holds the asynchronous AI column task queue.  See
// all_ai_column_task_def in src/share/inner_table/ob_inner_table_schema_def.py.
constexpr const char *AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";

// Catalog table shared with the DDL layer: one row per AI column.
constexpr const char *AI_COLUMN_CATALOG_TABLE = "__all_ai_column";

// The task state machine, the state names, the task call type and the retry
// policy live in ob_ai_column_task_defs.h, which has no OceanBase dependency so
// that they can be unit tested outside the server build.
//
// A task row is claimable when it is queued and its eligible time has come, or
// when it is RUNNING but the owner stopped heart beating (crash / restart).
constexpr int64_t AI_COLUMN_LEASE_TIMEOUT_US = 30 * 1000 * 1000L;      // 30s
constexpr int64_t AI_COLUMN_WORKER_BATCH_SIZE = 16;
constexpr int64_t AI_COLUMN_WORKER_PERIOD_US = 50 * 1000L;             // 50ms
// Upper bound of the in memory request id, "req$<usec>$<seq>" is ~40 bytes.
constexpr int64_t AI_COLUMN_REQUEST_ID_LEN = 96;
constexpr int64_t AI_COLUMN_ROWKEY_PREDICATE_LEN = 4096;

// Version tag of the JSON input envelope a task carries in
// __all_ai_column_task.prompt.  The enqueue statement evaluates both argument
// expressions of the AI function inside the user transaction with the native
// JSON_OBJECT constructor, which is what keeps their SQL types apart:
//   {"v": 1, "prompt": <prompt expr> [, "config": <config expr>]}
// A JSON prompt object (AI_PROMPT(...)) therefore reaches the worker as a JSON
// object, a plain VARCHAR reaches it as a JSON string -- and is never inspected
// for a prompt-like shape -- and a SQL NULL reaches it as JSON null.  Nothing
// here guesses the prompt type from the text: JSON_OBJECT produces exactly the
// type the expression had, and the worker only expands a genuine J_OBJECT that
// validates as a prompt object.
constexpr int64_t AI_COLUMN_INPUT_ENVELOPE_VERSION = 1;
constexpr const char *AI_COLUMN_INPUT_KEY_VERSION = "v";
constexpr const char *AI_COLUMN_INPUT_KEY_PROMPT = "prompt";
constexpr const char *AI_COLUMN_INPUT_KEY_CONFIG = "config";

// One AI column of one user table, as described by the __all_ai_column catalog
// row plus the schema information the runtime needs to build SQL.  All the
// strings are shallow copies owned by the caller supplied allocator.
struct AiColumnDef
{
  AiColumnDef()
    : table_id_(OB_INVALID_ID),
      column_id_(OB_INVALID_ID),
      state_column_id_(OB_INVALID_ID),
      request_column_id_(OB_INVALID_ID),
      result_spec_column_id_(OB_INVALID_ID),
      max_retries_(0),
      call_type_(AI_COLUMN_CALL_COMPLETE),
      dimension_(-1),
      definition_version_(1)
  {}
  bool is_valid() const
  {
    return OB_INVALID_ID != table_id_ && OB_INVALID_ID != column_id_ &&
           OB_INVALID_ID != state_column_id_ && OB_INVALID_ID != request_column_id_ &&
           !column_name_.empty() && !model_name_.empty() && !prompt_expr_.empty() &&
           !table_name_.empty() && !state_column_name_.empty() &&
           !request_column_name_.empty();
  }
  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(model_name),
               K_(max_retries), K_(call_type), K_(dimension), K_(definition_version),
               K_(state_column_id), K_(request_column_id), K_(result_spec_column_id),
               K_(state_column_name), K_(request_column_name),
               K_(table_name), K_(prompt_expr), K_(config_expr), K_(result_spec),
               K_(deps));
  uint64_t table_id_;
  int64_t column_id_;            // the AI result column itself
  int64_t state_column_id_;      // hidden column: PENDING/RUNNING/.../READY/FAILED
  int64_t request_column_id_;    // hidden column: request id watermark
  int64_t result_spec_column_id_;// hidden column: type spec of the result
  int64_t max_retries_;          // WITH (max_retries = N)
  int64_t call_type_;            // AiColumnCallType
  int64_t dimension_;            // vector dimension for AI_EMBED, -1 for AI_COMPLETE
  // __all_ai_column.definition_version: written when the AI column is created
  // (CREATE TABLE / ALTER TABLE ... ADD COLUMN) and never updated afterwards --
  // MODIFY / CHANGE COLUMN on an AI column is rejected instead of redefining it --
  // so today it always keeps its initial value.  A task carries the version it was
  // queued with and the worker drops a result whose version no longer matches the
  // catalog row; that check only starts to filter anything once redefinition
  // exists.
  int64_t definition_version_;
  common::ObString column_name_; // result column name, used to build SQL
  // Names of the two hidden columns the runtime owns, resolved from the catalog
  // column ids through the table schema (the SQL is text, so ids are not enough).
  common::ObString state_column_name_;
  common::ObString request_column_name_;
  common::ObString model_name_;  // model name passed to AI_COMPLETE / AI_EMBED
  // The prompt expression of the AI column, as *evaluable SQL text* over the
  // user table, e.g. "concat('Classify: ', content)".  Materializing the prompt
  // inside the enqueuing transaction is what makes the task independent of any
  // later change of the row.
  common::ObString prompt_expr_;
  // The config expression of the AI column (the third argument of AI_COMPLETE),
  // as evaluable SQL text over the user table, e.g.
  // "json_object('temperature', 0.2)".  Empty when the statement spelled none
  // out; it is snapshotted by the enqueue statement, exactly like the prompt.
  common::ObString config_expr_;
  // The whole normalized expression of the column, as printed by the DDL layer,
  // e.g. "AI_COMPLETE('model', concat('Classify: ', `content`))".  The runtime
  // reads it to know which columns the result depends on: an update that does not
  // touch any of them keeps the result of the column.
  common::ObString normalized_expr_;
  // comma separated column names of __all_ai_column.deps: the columns this ai
  // column is computed from.  A DML that changes none of them keeps the result.
  common::ObString deps_;
  common::ObString table_name_;  // "db.table", already quoted by the caller
  common::ObString result_spec_; // 'json' / 'integer' / 'vector:4' / empty
};

typedef common::ObSEArray<AiColumnDef, 4> AiColumnDefArray;

// A row of __all_ai_column_task, plus the AI column definition it refers to.
struct AiColumnTaskRow
{
  AiColumnTaskRow()
    : table_id_(OB_INVALID_ID),
      column_id_(OB_INVALID_ID),
      state_(AI_COLUMN_TASK_PENDING),
      retry_count_(0),
      max_retries_(0),
      eligible_ts_(0),
      call_type_(AI_COLUMN_CALL_COMPLETE),
      definition_version_(1),
      result_is_null_(false)
  {}
  // Print identifiers, state and sizes only.  The materialized prompt (it holds
  // the row data the AI column is computed from) and the model output are row
  // content: they must never reach observer.log, so only their lengths are
  // printed here.
  TO_STRING_KV(K_(table_id), K_(column_id), K_(rowkey_predicate), K_(request_id),
               K_(state), K_(retry_count), K_(max_retries), K_(eligible_ts),
               K_(call_type), K_(definition_version), K_(lease_owner),
               K_(model_name), K_(result_spec),
               "prompt_len", prompt_.length(),
               "result_data_len", result_data_.length(),
               K_(result_is_null));
  uint64_t table_id_;
  int64_t column_id_;
  common::ObString rowkey_predicate_;  // "`id` = 2", exactly as stored
  common::ObString request_id_;
  int64_t state_;
  int64_t retry_count_;
  int64_t max_retries_;
  int64_t eligible_ts_;                // usec, compared with now(6)
  int64_t call_type_;
  int64_t definition_version_;         // definition the task was queued with
  // fencing token: the worker that claimed the task.  Every terminal update of
  // the queue row is conditional on it, so a worker whose lease expired can not
  // overwrite the progress of its replacement.
  common::ObString lease_owner_;
  common::ObString model_name_;
  common::ObString prompt_;
  common::ObString result_spec_;
  common::ObString result_data_;
  // True when the staged model output for this task is SQL NULL rather than a
  // string: AI_EMBED on a NULL input propagates SQL NULL, which must stay
  // distinguishable from an empty string / empty vector.
  bool result_is_null_;
  common::ObString error_msg_;
};

typedef common::ObSEArray<AiColumnTaskRow, AI_COLUMN_WORKER_BATCH_SIZE> AiColumnTaskRowArray;

// SQL text builder for every statement the AI column runtime issues.  This is a
// pure function class: no state, no I/O, so the exact SQL (and therefore the
// transaction / CAS semantics) is checkable in isolation.
class AiColumnTaskSql
{
public:
  // ---- statements issued inside the business transaction (DML hook) --------

  // Invalidate every AI column of one row and stamp the new request id:
  //   UPDATE db.tbl SET `tags` = NULL, `__ai_state_0` = 1, `__ai_req_0` = 'req..'
  //   WHERE `id` = 2
  // One statement for all columns of the row so the transaction can never
  // observe a half invalidated row.
  static int invalidate_row(const AiColumnDefArray &defs,
                            const common::ObString &rowkey_predicate,
                            const common::ObString &request_id,
                            common::ObSqlString &sql);

  // Enqueue (or replace) the task of one AI column of one row.  The prompt is
  // evaluated by the same transaction, against the row the DML just wrote:
  //   REPLACE INTO __all_ai_column_task (...)
  //   SELECT 500001, 16, '`id` = 2', 'req..', 1, 0, 2, now(6), 1, 'model',
  //          concat('Classify: ', content), ''
  //   FROM db.tbl WHERE `id` = 2
  static int enqueue_task(const AiColumnDef &def,
                          const common::ObString &rowkey_predicate,
                          const common::ObString &request_id,
                          common::ObSqlString &sql);

  // Drop the tasks of one row, used when the row is deleted or when its primary
  // key changes.
  static int delete_tasks_of_row(const uint64_t table_id,
                                 const common::ObString &rowkey_predicate,
                                 common::ObSqlString &sql);

  // ---- statements issued by the background worker -------------------------

  // Pick claimable tasks.  RUNNING tasks whose lease expired are picked too, so
  // a restarted server resumes the work of a crashed one.
  static int pick_eligible_tasks(const int64_t batch_size, common::ObSqlString &sql);

  // Claim one task.  Affected rows == 1 means this worker won the CAS; anything
  // else means another worker owns it or the request id changed (stale task).
  static int claim_task(const AiColumnTaskRow &task,
                        const common::ObString &lease_owner,
                        const int64_t lease_timeout_us,
                        common::ObSqlString &sql);

  // Stage the model output, still uncommitted w.r.t. the user table.
  // `result_is_null` stages SQL NULL (AI_EMBED of a NULL input) instead of a
  // string; result_data is then ignored.
  static int store_success(const AiColumnTaskRow &task,
                           const common::ObString &result_data,
                           common::ObSqlString &sql,
                           const bool result_is_null = false);

  // Reschedule with a bounded exponential backoff.
  static int store_retry(const AiColumnTaskRow &task,
                         const int64_t next_eligible_ts,
                         const common::ObString &error_msg,
                         common::ObSqlString &sql);

  // Stage terminal failure. Keep the row until all siblings are terminal and
  // the group's READY/FAILED states have been atomically published.
  static int store_fail(const AiColumnTaskRow &task,
                        const common::ObString &error_msg,
                        common::ObSqlString &sql);

  // Publish gate: how many sibling tasks of the same row change are neither
  // READY nor FAILED. Publishing is allowed only at zero, which makes
  // "one row changes two AI columns" atomically visible.
  static int count_unfinished_siblings(const uint64_t table_id,
                                       const common::ObString &rowkey_predicate,
                                       const common::ObString &request_id,
                                       common::ObSqlString &sql);

  // Read the staged output of all siblings, so that the publishing statement
  // can write every AI column of the row at once.
  static int select_publish_payload(const uint64_t table_id,
                                    const common::ObString &rowkey_predicate,
                                    const common::ObString &request_id,
                                    common::ObSqlString &sql);

  // Atomically publish every AI column of one row:
  //   UPDATE db.tbl SET `tags` = '..', `score` = '..',
  //          `__ai_state_0` = 4, `__ai_req_0` = NULL, ...
  //   WHERE `id` = 2 AND `__ai_req_0` = 'req..'
  // The trailing request id predicates are the anti-late-overwrite CAS: a stale
  // request never matches, because a newer DML already stamped a new request id.
  static int publish_row(const AiColumnDefArray &defs,
                         const common::ObString &rowkey_predicate,
                         const common::ObString &request_id,
                         const common::ObIArray<common::ObString> &results,
                         common::ObSqlString &sql,
                         const common::ObIArray<int64_t> *states = nullptr,
                         const common::ObIArray<bool> *result_nulls = nullptr);

  // Remove this captured task only; do not delete newer requests or siblings.
  static int delete_task(const AiColumnTaskRow &task, common::ObSqlString &sql);

  // Flush the terminal failure of one AI column into the user row, so that the
  // STATUS pseudo column reports FAILED and the result detail is kept.
  static int publish_failure(const AiColumnDef &def,
                             const common::ObString &rowkey_predicate,
                             const common::ObString &request_id,
                             common::ObSqlString &sql);

  static int delete_tasks_of_request(const uint64_t table_id,
                                     const common::ObString &rowkey_predicate,
                                     const common::ObString &request_id,
                                     common::ObSqlString &sql);

  // ---- strict read support -----------------------------------------------

  // "Does this AI column still have any row that is not READY?"  The strict
  // check is column scope, not row scope: any pending / failed row rejects every
  // statement that references the column.
  static int count_not_ready_rows(const AiColumnDef &def, common::ObSqlString &sql);

  // The AI column catalog row of one table.
  static int select_catalog(const uint64_t table_id, common::ObSqlString &sql);

  // ---- helpers -----------------------------------------------------------

  // Append 'value' as a quoted SQL literal, escaping ' and \.
  static int append_literal(common::ObSqlString &sql, const common::ObString &value);
  static int append_literal(common::ObSqlString &sql, const char *value);
  static int append_backquoted(common::ObSqlString &sql, const common::ObString &name);
  static int append_rowkey_predicate(common::ObSqlString &sql,
                                     const common::ObString &predicate);
  // "`id` = 2", for a single key column.  Multiple key columns are joined with
  // " and ", and string keys are quoted, e.g. "`k1` = 1 and `k2` = 'x'".
  static int build_rowkey_predicate(common::ObIAllocator &allocator,
                                    const common::ObIArray<common::ObString> &column_names,
                                    const common::ObIArray<common::ObString> &literal_values,
                                    common::ObString &predicate);
};

// The retry policy (attempts left, and when the next attempt is due) is defined
// in ob_ai_column_task_defs.h.

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
