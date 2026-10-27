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

#include "lib/task/ob_timer.h"
#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "observer/ai_service/ai_column/ob_ai_column_task.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
class ObIAllocator;
class ObJsonObject;
namespace sqlclient
{
class ObMySQLResult;
}
}
namespace observer
{
namespace ai_column
{

// Background half of the AI column runtime.
//
// It never runs inside a user transaction: it claims a queued task row, calls the
// model outside of any transaction, and only then touches the durable state.  A
// finished column is *staged* in its own queue row (state READY or FAILED, both
// terminal); the row change is published only when no sibling of the same
// (table_id, rowkey_predicate, request_id) group is still unfinished.  So a row
// becomes visible with all of its AI columns at once, never one by one.
//
// The decoded input of one task: the versioned JSON envelope the business DML
// snapshotted into the queue row, so the model is called with exactly the
// arguments the statement evaluated, never with a re-read of the table.
struct AiColumnTaskInput
{
  AiColumnTaskInput() : prompt_(), prompt_is_null_(false), config_(NULL) {}
  // The prompt text sent to the model: verbatim for a plain string argument,
  // fully expanded for a prompt object.
  common::ObString prompt_;
  // The prompt argument evaluated to SQL NULL (AI_COMPLETE fails terminally,
  // AI_EMBED propagates SQL NULL into its result).
  bool prompt_is_null_;
  // The config object (third argument of AI_COMPLETE), NULL when omitted.
  common::ObJsonObject *config_;
};

// All statements go through the sql proxy (`read` / `write`) or through an
// ObMySQLTransaction that spans the whole publish of one row change.  The proxy's
// get_connection() returns NULL by design, so it must never be used here.
class AiColumnWorker : private common::ObTimerTask
{
  static constexpr int64_t SCHEDULE_PERIOD = AI_COLUMN_WORKER_PERIOD_US;

public:
  AiColumnWorker();
  virtual ~AiColumnWorker();

  int init(common::ObMySQLProxy &sql_proxy);
  int start();
  void stop();
  void wait();
  void destroy();

private:
  void runTimerTask() override;

  // ---- one round of the worker ------------------------------------------
  int do_work();
  // The queue rows to work on: pending / retry-wait / expired-running rows, plus
  // terminal rows whose row change the previous worker did not manage to publish.
  int pick_tasks(common::ObIAllocator &allocator, AiColumnTaskRowArray &tasks);
  int load_defs(common::ObIAllocator &allocator,
                const uint64_t table_id,
                AiColumnDefArray &defs);
  int handle_task(common::ObIAllocator &allocator,
                  AiColumnDefArray &defs,
                  AiColumnTaskRow &task);
  // READY and FAILED are terminal: such a task must never call the model again,
  // only the group publish the siblings are waiting for.
  static bool is_terminal_state(const int64_t state);
  int handle_terminal_task(common::ObIAllocator &allocator,
                           const AiColumnDefArray &defs,
                           const AiColumnTaskRow &task);
  int handle_pending_task(common::ObIAllocator &allocator,
                          const AiColumnDefArray &defs,
                          AiColumnTaskRow &task);
  int find_def(const AiColumnDefArray &defs, const int64_t column_id, const AiColumnDef *&def);
  int claim_task(AiColumnTaskRow &task, int64_t &affected_rows);
  // The task was claimed successfully: decode its input envelope, decide the
  // terminal outcomes that never reach the model (NULL / empty prompt, invalid
  // prompt object or config), and otherwise call the model and stage the result.
  int handle_claimed_task(common::ObIAllocator &allocator,
                          const AiColumnDefArray &defs,
                          const AiColumnTaskRow &task,
                          const AiColumnDef &def);
  // Decodes the versioned JSON input envelope of a task row.  A row that does
  // not carry an envelope (queued by a version that stored the raw prompt) is
  // read as a plain string prompt.  On failure error_msg holds a
  // self-explanatory message for the terminal task failure.
  int decode_task_input(common::ObIAllocator &allocator,
                        const AiColumnTaskRow &task,
                        AiColumnTaskInput &input,
                        common::ObString &error_msg);
  int call_model(common::ObIAllocator &allocator,
                 const AiColumnDef &def,
                 const common::ObString &prompt,
                 common::ObJsonObject *config,
                 common::ObString &result);

  // ---- phase one: stage this column's own outcome in its queue row -------
  // `result_is_null` stages SQL NULL: AI_EMBED of a NULL input is a READY row
  // whose *result* is NULL, which must not be confused with an empty vector.
  int stage_success(const AiColumnTaskRow &task,
                    const common::ObString &result,
                    int64_t &affected_rows,
                    const bool result_is_null = false);
  int stage_failure(const AiColumnTaskRow &task,
                    const common::ObString &error_msg,
                    int64_t &affected_rows);
  // Bounded retry (state 3, retry_count + 1, committed so the test observes it) or
  // the terminal failure of a column; the terminal path stages state 5 first and
  // publishes after that, the retry path publishes nothing.
  int abandon_task(common::ObIAllocator &allocator,
                   const AiColumnDefArray &defs,
                   const AiColumnTaskRow &task,
                   const common::ObString &error_msg,
                   const bool can_retry);

  // ---- phase two: publish the whole row change in one transaction --------
  // No model call may ever happen here.
  int publish_group(common::ObIAllocator &allocator,
                    const AiColumnDefArray &defs,
                    const AiColumnTaskRow &task);
  int read_sibling_count(common::ObISQLClient &client,
                         const AiColumnTaskRow &task,
                         int64_t &unfinished);
  // The staged rows of one request, materialized: those rows *are* the publish
  // payload, the local model results are never used for it.
  int read_publish_payload(common::ObISQLClient &client,
                           common::ObIAllocator &allocator,
                           const AiColumnTaskRow &task,
                           AiColumnTaskRowArray &payload);
  // A fresh read of the catalog, bypassing the definition cache: the publish has
  // to compare against what the catalog says *now*.
  int read_catalog_versions(common::ObISQLClient &client,
                            const uint64_t table_id,
                            common::ObIArray<int64_t> &column_ids,
                            common::ObIArray<int64_t> &versions);
  // Precise single task cleanup: table_id + column_id + rowkey_predicate +
  // request_id + definition_version.  Never touches a sibling column and never a
  // newer request of the same row.
  int drop_task(common::ObISQLClient &client,
                const AiColumnTaskRow &task,
                int64_t &affected_rows);

  int to_worker_request_id(common::ObIAllocator &allocator, common::ObString &lease_owner);

private:
  bool is_inited_;
  common::ObMySQLProxy *sql_proxy_;
  common::ObTimer timer_;
  common::ObArenaAllocator allocator_;
  int64_t lease_seq_;
};

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
