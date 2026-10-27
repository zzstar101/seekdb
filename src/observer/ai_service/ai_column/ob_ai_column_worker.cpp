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

#include "config_bridge.h"
#include "observer/ai_service/ai_column/ob_ai_column_worker.h"
#include "observer/ai_service/ai_column/ob_ai_column_typed_result.h"
#include "observer/ai_service/ai_column/ob_ai_column_service.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/time/ob_time_utility.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "query/ai/ob_ai_endpoint_resolver.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace observer
{
namespace ai_column
{

namespace
{

// Every statement of the worker goes through one of these two helpers.
//
// The sql proxy can not hand out a connection (ObCommonSqlProxy::get_connection()
// returns NULL, and even when it does not, a connection is not ours to keep), so
// the statements are executed by the client itself: ObCommonSqlProxy::read/write
// for a statement of its own, ObSingleConnectionProxy::read/write when the client
// is an ObMySQLTransaction, i.e. when a whole row change has to be published
// atomically on one connection.
int execute_write(ObISQLClient &client, const ObSqlString &sql, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  if (OB_UNLIKELY(sql.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column sql", K(ret));
  } else if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
    // ObSqlString::ptr() is NUL terminated, so it is safe to pass it as the
    // C string form of the statement.
    //
    // The statement text itself is deliberately never printed: the publish
    // statements of AiColumnTaskSql carry the model output as a literal, i.e.
    // row content, which must not reach observer.log.  Only the length of the
    // statement is logged, which is still enough to tell an empty builder apart
    // from a real statement in a support case.
    LOG_WARN("execute ai column write failed", K(ret), "sql_len", sql.length());
  } else {
    LOG_DEBUG("ai column sql done", "sql_len", sql.length(), K(affected_rows));
  }
  return ret;
}

int execute_read(ObISQLClient &client, const ObSqlString &sql, ObISQLClient::ReadResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(sql.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column sql", K(ret));
  } else if (OB_FAIL(client.read(res, sql.ptr()))) {
    LOG_WARN("execute ai column read failed", K(ret), K(sql));
  }
  return ret;
}

int worker_copy_varchar(ObIAllocator &allocator, sqlclient::ObMySQLResult &result,
                 const char *name, ObString &value)
{
  int ret = OB_SUCCESS;
  ObString tmp;
  value.reset();
  if (OB_SUCCESS != (ret = result.get_varchar(name, tmp))) {
    if (OB_ERR_NULL_VALUE == ret || OB_ERR_COLUMN_NOT_FOUND == ret) {
      // a NULL optional column is an empty string for the runtime
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("get varchar failed", K(ret), K(name));
    }
  } else if (OB_FAIL(ob_write_string(allocator, tmp, value))) {
    LOG_WARN("copy string failed", K(ret), K(name));
  }
  return ret;
}

int worker_get_int_or_default(sqlclient::ObMySQLResult &result,
                       const char *name,
                       int64_t default_value,
                       int64_t &value)
{
  int ret = OB_SUCCESS;
  value = default_value;
  if (OB_SUCCESS != (ret = result.get_int(name, value))) {
    if (OB_ERR_NULL_VALUE == ret || OB_ERR_COLUMN_NOT_FOUND == ret) {
      ret = OB_SUCCESS;
      value = default_value;
    } else {
      LOG_WARN("get int failed", K(ret), K(name));
    }
  }
  return ret;
}

// read the single scalar of a count(*) statement
int read_scalar_count(ObISQLClient &client, const ObSqlString &sql, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  SMART_VAR(ObISQLClient::ReadResult, res) {
    sqlclient::ObMySQLResult *result = nullptr;
    if (OB_FAIL(execute_read(client, sql, res))) {
      LOG_WARN("read ai column count failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null ai column count result", K(ret));
    } else if (OB_FAIL(result->next())) {
      LOG_WARN("read the ai column count row failed", K(ret), K(sql));
    } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), count))) {
      LOG_WARN("read the ai column count value failed", K(ret), K(sql));
    }
  }
  return ret;
}

// Which definition version does the catalog hold for this column *now*?
// A column that is no longer in the catalog reports no version at all, and its
// queued tasks can never be published.
bool find_catalog_version(const ObIArray<int64_t> &column_ids,
                          const ObIArray<int64_t> &versions,
                          const int64_t column_id,
                          int64_t &version)
{
  bool found = false;
  version = -1;
  for (int64_t i = 0; !found && i < column_ids.count(); ++i) {
    if (column_ids.at(i) == column_id) {
      version = versions.at(i);
      found = true;
    }
  }
  return found;
}

// Marks an input error with the message the task failure will carry.
int set_input_error(ObIAllocator &allocator, const char *message, ObString &error_msg)
{
  int ret = OB_SUCCESS;
  error_msg.reset();
  if (OB_FAIL(ob_write_string(allocator, ObString::make_string(message), error_msg))) {
    LOG_WARN("copy the ai column input error message failed", K(ret));
  }
  return ret;
}

// Reads the members of the versioned input envelope.
//
// The enqueue statement evaluated both argument expressions with the native
// JSON_OBJECT constructor, which is what preserved their SQL types here: a plain
// VARCHAR became a JSON string, an AI_PROMPT(...) object became a JSON object and
// a SQL NULL became JSON null.  So nothing in this function guesses what the
// prompt was: a JSON string is sent to the model verbatim (even when its text
// looks like a prompt object), only a genuine JSON object is validated as one and
// expanded, and only JSON null means SQL NULL.
int decode_input_envelope(ObIAllocator &allocator,
                          ObJsonObject &envelope,
                          AiColumnTaskInput &input,
                          ObString &error_msg)
{
  int ret = OB_SUCCESS;
  ObJsonNode *prompt_node = envelope.get_value(ObString::make_string(AI_COLUMN_INPUT_KEY_PROMPT));
  if (OB_ISNULL(prompt_node)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai column task input has no prompt member", K(ret));
    int tmp_ret = set_input_error(allocator, "ai column task input has no prompt member", error_msg);
    if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; }
  } else if (ObJsonNodeType::J_NULL == prompt_node->json_type()) {
    input.prompt_is_null_ = true;
  } else if (ObJsonNodeType::J_STRING == prompt_node->json_type()) {
    // a plain string prompt is never parsed for a prompt-like shape
    input.prompt_ = static_cast<const ObJsonString *>(prompt_node)->get_str();
  } else if (ObJsonNodeType::J_OBJECT == prompt_node->json_type()) {
    ObJsonObject *prompt_object = static_cast<ObJsonObject *>(prompt_node);
    if (!ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("ai_complete, prompt object is invalid", K(ret));
      int tmp_ret = set_input_error(allocator,
          "ai_complete, prompt object must have a string template and an args array", error_msg);
      if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; }
    } else if (OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                 allocator, prompt_object, input.prompt_))) {
      LOG_WARN("render the ai_complete prompt object failed", K(ret));
      int tmp_ret = set_input_error(allocator, "ai_complete, prompt object can not be rendered", error_msg);
      if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai_complete, prompt must be a string or a prompt object", K(ret),
             "json_type", prompt_node->json_type());
    int tmp_ret = set_input_error(allocator,
        "ai_complete, prompt must be a string or a prompt object", error_msg);
    if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; }
  }

  if (OB_SUCC(ret)) {
    ObJsonNode *config_node = envelope.get_value(ObString::make_string(AI_COLUMN_INPUT_KEY_CONFIG));
    if (NULL == config_node || ObJsonNodeType::J_NULL == config_node->json_type()) {
      // an omitted config and a NULL config mean the same: no config
      input.config_ = NULL;
    } else if (ObJsonNodeType::J_OBJECT == config_node->json_type()) {
      input.config_ = static_cast<ObJsonObject *>(config_node);
    } else if (ObJsonNodeType::J_STRING == config_node->json_type()) {
      // parity with the expression path, which converts a string config to JSON
      const ObString config_text = static_cast<const ObJsonString *>(config_node)->get_str();
      ObIJsonBase *config_base = NULL;
      if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, config_text,
                                                   ObJsonInType::JSON_TREE,
                                                   ObJsonInType::JSON_TREE,
                                                   config_base))) {
        LOG_WARN("ai_complete, config must be a JSON object", K(ret));
      } else if (NULL == config_base
                 || ObJsonNodeType::J_OBJECT != config_base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        input.config_ = static_cast<ObJsonObject *>(config_base);
      }
      if (OB_FAIL(ret)) {
        ret = OB_INVALID_ARGUMENT;
        int tmp_ret = set_input_error(allocator, "ai_complete, config must be a JSON object",
                                      error_msg);
        if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; }
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("ai_complete, config must be a JSON object", K(ret),
               "json_type", config_node->json_type());
      int tmp_ret = set_input_error(allocator, "ai_complete, config must be a JSON object",
                                    error_msg);
      if (OB_SUCCESS != tmp_ret) { ret = tmp_ret; }
    }
  }
  return ret;
}

} // anonymous namespace

AiColumnWorker::AiColumnWorker()
  : is_inited_(false),
    sql_proxy_(nullptr),
    timer_(),
    allocator_(ObModIds::OB_DUTY_TASK),
    lease_seq_(0)
{
}

AiColumnWorker::~AiColumnWorker()
{
  destroy();
}

int AiColumnWorker::init(ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_FAIL(timer_.init("AiColumnWorker", ObMemAttr("AiColumnWorker")))) {
    LOG_WARN("init timer failed", K(ret));
  } else {
    sql_proxy_ = &sql_proxy;
    is_inited_ = true;
  }
  return ret;
}

int AiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(timer_.schedule(*this, SCHEDULE_PERIOD, true))) {
    LOG_WARN("schedule ai column worker failed", K(ret));
  }
  return ret;
}

void AiColumnWorker::stop()
{
  timer_.stop();
}

void AiColumnWorker::wait()
{
  timer_.wait();
}

void AiColumnWorker::destroy()
{
  timer_.destroy();
  is_inited_ = false;
  sql_proxy_ = nullptr;
}

void AiColumnWorker::runTimerTask()
{
  int ret = OB_SUCCESS;
  allocator_.reset_remain_one_page();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(do_work())) {
    LOG_WARN("ai column worker round failed", K(ret));
  }
}

int AiColumnWorker::do_work()
{
  int ret = OB_SUCCESS;
  AiColumnTaskRowArray tasks;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(pick_tasks(allocator_, tasks))) {
    LOG_WARN("pick ai column tasks failed", K(ret));
  } else if (tasks.empty()) {
    // nothing to do, the common case
  } else {
    LOG_DEBUG("picked ai column tasks", "count", tasks.count());
    for (int64_t i = 0; i < tasks.count(); ++i) {
      AiColumnDefArray defs;
      AiColumnTaskRow &task = tasks.at(i);
      int tmp_ret = OB_SUCCESS;
      // one broken task must not stop the whole round
      if (OB_SUCCESS != (tmp_ret = load_defs(allocator_, task.table_id_, defs))) {
        LOG_WARN("load ai column defs failed", K(tmp_ret), K(task));
      } else if (OB_SUCCESS != (tmp_ret = handle_task(allocator_, defs, task))) {
        LOG_WARN("handle ai column task failed", K(tmp_ret), K(task));
      }
    }
  }
  return ret;
}

int AiColumnWorker::pick_tasks(ObIAllocator &allocator, AiColumnTaskRowArray &tasks)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  tasks.reuse();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(AiColumnTaskSql::pick_eligible_tasks(AI_COLUMN_WORKER_BATCH_SIZE, sql))) {
    LOG_WARN("build pick sql failed", K(ret));
  } else {
    // the rows are materialized into the allocator inside this scope, so the
    // result is closed before the worker issues any other statement
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(execute_read(*sql_proxy_, sql, res))) {
        LOG_WARN("read ai column tasks failed", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null ai column task result", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          AiColumnTaskRow task;
          int64_t table_id = 0;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("iterate ai column tasks failed", K(ret));
            }
          } else if (OB_FAIL(result->get_int("table_id", table_id))) {
            LOG_WARN("get table id failed", K(ret));
          } else if (table_id <= 0) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid queued table id", K(ret), K(table_id));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "column_id",
                                                       static_cast<int64_t>(OB_INVALID_ID),
                                                       task.column_id_))) {
            LOG_WARN("get column id failed", K(ret));
          } else if (OB_FAIL(worker_copy_varchar(allocator, *result, "rowkey_predicate",
                                                 task.rowkey_predicate_))) {
            LOG_WARN("get predicate failed", K(ret));
          } else if (OB_FAIL(worker_copy_varchar(allocator, *result, "request_id",
                                                 task.request_id_))) {
            LOG_WARN("get request id failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "state",
                                                       AI_COLUMN_TASK_PENDING, task.state_))) {
            LOG_WARN("get state failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "retry_count", 0,
                                                       task.retry_count_))) {
            LOG_WARN("get retry count failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "max_retries", 0,
                                                       task.max_retries_))) {
            LOG_WARN("get max retries failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "call_type",
                                                       AI_COLUMN_CALL_COMPLETE, task.call_type_))) {
            LOG_WARN("get call type failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "definition_version", 0,
                                                       task.definition_version_))) {
            LOG_WARN("get definition version failed", K(ret));
          } else if (OB_FAIL(worker_copy_varchar(allocator, *result, "model_name",
                                                 task.model_name_))) {
            LOG_WARN("get model name failed", K(ret));
          } else if (OB_FAIL(worker_copy_varchar(allocator, *result, "prompt", task.prompt_))) {
            LOG_WARN("get prompt failed", K(ret));
          } else if (OB_FAIL(worker_copy_varchar(allocator, *result, "result_spec",
                                                 task.result_spec_))) {
            LOG_WARN("get result spec failed", K(ret));
          } else {
            task.table_id_ = static_cast<uint64_t>(table_id);
            if (OB_FAIL(tasks.push_back(task))) {
              LOG_WARN("push task failed", K(ret));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int AiColumnWorker::load_defs(ObIAllocator &allocator,
                              const uint64_t table_id,
                              AiColumnDefArray &defs)
{
  int ret = OB_SUCCESS;
  defs.reuse();
  if (OB_INVALID_ID == table_id) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table id", K(ret));
  } else if (OB_FAIL(AiColumnService::get_instance().get_ai_column_defs(allocator,
                                                                       table_id,
                                                                       defs))) {
    // The catalog, the hidden column names and the prompt expression are all read
    // by the DML hook's loader, so the worker can never work with a definition the
    // enqueuing transaction did not see.
    LOG_WARN("load ai column definitions failed", K(ret), K(table_id));
  }
  return ret;
}

int AiColumnWorker::find_def(const AiColumnDefArray &defs,
                             const int64_t column_id,
                             const AiColumnDef *&def)
{
  int ret = OB_SUCCESS;
  def = nullptr;
  for (int64_t i = 0; NULL == def && i < defs.count(); ++i) {
    if (defs.at(i).column_id_ == column_id) {
      def = &defs.at(i);
    }
  }
  if (NULL == def) {
    ret = OB_ENTRY_NOT_EXIST;
    LOG_WARN("ai column is not in the catalog", K(ret), K(column_id), "def_count", defs.count());
  }
  return ret;
}

bool AiColumnWorker::is_terminal_state(const int64_t state)
{
  return AI_COLUMN_TASK_READY == state || AI_COLUMN_TASK_FAILED == state;
}

int AiColumnWorker::handle_task(ObIAllocator &allocator,
                                AiColumnDefArray &defs,
                                AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  if (is_terminal_state(task.state_)) {
    // The recovery scan picked a column that is already done: the previous worker
    // staged it but did not publish its row change.  Running the model again would
    // overwrite a decided result with a new one, so only the group publish (which
    // is idempotent) is attempted.
    ret = handle_terminal_task(allocator, defs, task);
  } else {
    ret = handle_pending_task(allocator, defs, task);
  }
  return ret;
}

int AiColumnWorker::handle_terminal_task(ObIAllocator &allocator,
                                         const AiColumnDefArray &defs,
                                         const AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  const AiColumnDef *def = nullptr;
  if (OB_FAIL(find_def(defs, task.column_id_, def))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      // The column is no longer in the catalog, so its result can never be
      // published.  Only this one task is dropped: the other columns of the row
      // change may still be publishable.
      ret = OB_SUCCESS;
      LOG_INFO("drop a terminal task of a dropped ai column", K(task));
      if (OB_FAIL(drop_task(*sql_proxy_, task, affected_rows))) {
        LOG_WARN("drop orphan terminal task failed", K(ret), K(task));
      }
    }
  } else if (def->definition_version_ != task.definition_version_) {
    LOG_INFO("drop a terminal task queued by an older ai column definition", K(task),
             "catalog_version", def->definition_version_);
    if (OB_FAIL(drop_task(*sql_proxy_, task, affected_rows))) {
      LOG_WARN("drop stale definition terminal task failed", K(ret), K(task));
    }
  } else if (OB_FAIL(publish_group(allocator, defs, task))) {
    LOG_WARN("publish the ai column group of a terminal task failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnWorker::handle_pending_task(ObIAllocator &allocator,
                                        const AiColumnDefArray &defs,
                                        AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  const AiColumnDef *def = nullptr;
  int64_t affected_rows = 0;
  ObString result;
  if (OB_FAIL(find_def(defs, task.column_id_, def))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      // the catalog row is gone (column dropped): the task cannot be executed and
      // must not stay in the queue forever.  Only this task is dropped, never the
      // tasks of the other columns of the same row.
      ret = OB_SUCCESS;
      LOG_INFO("drop an orphan ai column task", K(task));
      if (OB_FAIL(drop_task(*sql_proxy_, task, affected_rows))) {
        LOG_WARN("drop orphan task failed", K(ret), K(task));
      }
    }
  } else if (def->definition_version_ != task.definition_version_) {
    // The AI column was redefined after this task was queued.  Its prompt and its
    // result spec belong to the older definition, so its result must never be
    // published; the task is discarded instead.
    LOG_INFO("drop a task queued by an older ai column definition", K(task),
             "catalog_version", def->definition_version_);
    if (OB_FAIL(drop_task(*sql_proxy_, task, affected_rows))) {
      LOG_WARN("drop stale definition task failed", K(ret), K(task));
    }
  } else if (OB_FAIL(claim_task(task, affected_rows))) {
    LOG_WARN("claim ai column task failed", K(ret), K(task));
  } else if (1 != affected_rows) {
    // another worker owns it, or a newer request replaced the task: skip silently
    LOG_DEBUG("task is not claimable any more", K(task), K(affected_rows));
  } else if (OB_FAIL(handle_claimed_task(allocator, defs, task, *def))) {
    LOG_WARN("execute an ai column task failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnWorker::handle_claimed_task(ObIAllocator &allocator,
                                        const AiColumnDefArray &defs,
                                        const AiColumnTaskRow &task,
                                        const AiColumnDef &def)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObString result;
  ObString input_error;
  AiColumnTaskInput input;
  const int decode_ret = decode_task_input(allocator, task, input, input_error);
  if (OB_SUCCESS != decode_ret) {
    // A task input that can not be decoded (malformed envelope, invalid prompt
    // object, invalid config) is a permanent input error: it is decided FAILED
    // without ever calling the model and without retrying, and the already
    // committed business DML is left untouched.  It must never crash the worker
    // and must never fall back to sending the raw text.
    ret = OB_SUCCESS;
    ObString error_msg = input_error;
    if (error_msg.empty()
        && OB_FAIL(ob_write_string(allocator,
                                   ObString::make_string(ob_strerror(decode_ret)),
                                   error_msg))) {
      LOG_WARN("copy the ai column input error message failed", K(ret));
    } else if (OB_FAIL(abandon_task(allocator, defs, task, error_msg, false))) {
      LOG_WARN("fail the ai column task with an invalid input failed", K(ret), K(task));
    }
  } else if (input.prompt_is_null_ || input.prompt_.empty()) {
    if (input.prompt_is_null_ && AI_COLUMN_CALL_EMBED == def.call_type_) {
      // AI_EMBED is a single row function: a NULL input propagates SQL NULL into
      // its result.  The row becomes READY with a NULL result -- never an empty
      // vector, never a model call.
      if (OB_FAIL(stage_success(task, ObString(), affected_rows, true))) {
        LOG_WARN("stage the NULL ai column result failed", K(ret), K(task));
      } else if (1 != affected_rows) {
        LOG_DEBUG("the staged ai column task was replaced while running", K(task),
                  K(affected_rows));
      } else if (OB_FAIL(publish_group(allocator, defs, task))) {
        LOG_WARN("publish the ai column group failed", K(ret), K(task));
      }
    } else {
      // AI_COMPLETE with a NULL or empty prompt is a terminal failure of the
      // column: the column stays NULL, no request is sent.
      ret = OB_SUCCESS;
      const char *reason = input.prompt_is_null_
          ? "ai_complete, prompt expression evaluated to NULL"
          : "ai_complete, prompt expression evaluated to an empty string";
      ObString error_msg;
      if (OB_FAIL(ob_write_string(allocator, ObString::make_string(reason), error_msg))) {
        LOG_WARN("copy the ai column prompt error message failed", K(ret));
      } else if (OB_FAIL(abandon_task(allocator, defs, task, error_msg, false))) {
        LOG_WARN("fail the ai column task with an unusable prompt failed", K(ret), K(task));
      }
    }
  } else if (OB_FAIL(call_model(allocator, def, input.prompt_, input.config_, result))) {
    // the model call is never inside a transaction, so it is retried from the
    // queue row instead of being rolled back
    const int call_error = ret;
    ObString error_msg;
    ret = OB_SUCCESS;
    if (OB_FAIL(ob_write_string(allocator, ObString::make_string(ob_strerror(call_error)),
                                error_msg))) {
      LOG_WARN("copy the model error message failed", K(ret));
    } else if (OB_FAIL(abandon_task(allocator, defs, task, error_msg,
                                    AiColumnRetryPolicy::can_retry(task.retry_count_,
                                                                   task.max_retries_)))) {
      LOG_WARN("abandon ai column task failed", K(ret), K(task));
    }
  } else if (OB_FAIL(stage_success(task, result, affected_rows))) {
    LOG_WARN("stage the ai column result failed", K(ret), K(task));
  } else if (1 != affected_rows) {
    // the task was replaced by a newer request while the model was running: the
    // late result is dropped here, the row of the new request publishes itself
    LOG_DEBUG("the staged ai column task was replaced while running", K(task),
              K(affected_rows));
  } else if (OB_FAIL(publish_group(allocator, defs, task))) {
    LOG_WARN("publish the ai column group failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnWorker::decode_task_input(ObIAllocator &allocator,
                                      const AiColumnTaskRow &task,
                                      AiColumnTaskInput &input,
                                      ObString &error_msg)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = nullptr;
  input.prompt_.reset();
  input.prompt_is_null_ = false;
  input.config_ = nullptr;
  error_msg.reset();
  if (task.prompt_.empty()) {
    // Nothing was snapshotted: the prompt argument is empty.
    input.prompt_ = task.prompt_;
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, task.prompt_,
                                                      ObJsonInType::JSON_TREE,
                                                      ObJsonInType::JSON_TREE,
                                                      j_base))) {
    LOG_INFO("ai column task carries no input envelope, read it as a plain prompt",
             K(task), K(ret));
    ret = OB_SUCCESS;
    // The row predates the input envelope: its stored text *is* the prompt.  Its
    // content is never inspected for a prompt-like shape.
    input.prompt_ = task.prompt_;
  } else if (OB_ISNULL(j_base)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null ai column task input", K(ret));
  } else {
    ObJsonNode *version_node = nullptr;
    bool is_envelope = false;
    if (ObJsonNodeType::J_OBJECT == j_base->json_type()) {
      ObJsonObject *envelope = static_cast<ObJsonObject *>(j_base);
      version_node = envelope->get_value(ObString::make_string(AI_COLUMN_INPUT_KEY_VERSION));
      is_envelope = NULL != version_node
          && (ObJsonNodeType::J_INT == version_node->json_type()
              || ObJsonNodeType::J_UINT == version_node->json_type())
          && AI_COLUMN_INPUT_ENVELOPE_VERSION == version_node->get_int();
    }
    if (!is_envelope) {
      // JSON shaped, but not our envelope: it is a prompt value, verbatim.
      input.prompt_ = task.prompt_;
    } else {
      ret = decode_input_envelope(allocator, *static_cast<ObJsonObject *>(j_base), input, error_msg);
    }
  }
  return ret;
}

int AiColumnWorker::claim_task(AiColumnTaskRow &task, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString lease_owner;
  affected_rows = 0;
  if (OB_FAIL(to_worker_request_id(allocator_, lease_owner))) {
    LOG_WARN("build lease owner failed", K(ret));
  } else if (OB_FAIL(AiColumnTaskSql::claim_task(task, lease_owner,
                                                 AI_COLUMN_LEASE_TIMEOUT_US, sql))) {
    LOG_WARN("build claim sql failed", K(ret));
  } else if (OB_FAIL(execute_write(*sql_proxy_, sql, affected_rows))) {
    LOG_WARN("claim task failed", K(ret));
  } else if (1 == affected_rows) {
    // remember the fencing token: every later write of this task carries it, so a
    // worker whose lease expired can not overwrite the state of its successor
    task.lease_owner_ = lease_owner;
  }
  return ret;
}

int AiColumnWorker::call_model(ObIAllocator &allocator,
                               const AiColumnDef &def,
                               const ObString &prompt,
                               ObJsonObject *config,
                               ObString &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = nullptr;
  ObString prompt_copy = prompt;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver = nullptr;
  result.reset();
  if (prompt.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty ai column prompt", K(ret), K(def));
  } else if (OB_ISNULL(resolver = share::server_service<query::ObIAiEndpointResolver>())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai endpoint resolver is not ready", K(ret));
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, def.model_name_, info))) {
    LOG_WARN("resolve ai model failed", K(ret), K(def));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null ai func info", K(ret));
  } else if (OB_FAIL(resolver->resolve_by_model_name(def.model_name_, allocator, endpoint))) {
    LOG_WARN("resolve ai endpoint failed", K(ret), K(def));
  } else {
    ObAIFuncModel model(allocator, *info, endpoint);
    if (AI_COLUMN_CALL_EMBED == def.call_type_) {
      ObJsonObject *config = nullptr;
      ObJsonInt *dimension = nullptr;
      if (def.dimension_ <= 0) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, def.dimension_, dimension))) {
      } else if (OB_FAIL(config->add("dimensions", dimension))) {
      } else if (OB_FAIL(model.call_dense_embedding(prompt_copy, config, result))) {
        LOG_WARN("ai embed call failed", K(ret), K(def));
      }
    } else {
      ObSqlString instruction;
      const std::string spec(def.result_spec_.ptr(), def.result_spec_.length());
      if (OB_FAIL(instruction.append(AiColumnTypedResult::instruction(spec)))) {
      } else if (OB_FAIL(instruction.append_fmt(" Declared SQL result: %.*s.",
                                                def.result_spec_.length(), def.result_spec_.ptr()))) {
      } else {
        const ObString system_prompt = instruction.string();
        if (OB_FAIL(model.call_completion(prompt_copy, config, result, system_prompt))) {
          LOG_WARN("ai complete call failed", K(ret), K(def));
        }
      }
    }
    // Type failures follow the same bounded retry/terminal FAILED path as model
    // errors. Never stage them READY and poison the sibling publication UPDATE.
    if (OB_SUCC(ret) && !AiColumnTypedResult::validate(
          std::string(def.result_spec_.ptr(), def.result_spec_.length()),
          std::string(result.ptr() == nullptr ? "" : result.ptr(), result.length()))) {
      ret = OB_INVALID_DATA;
      LOG_WARN("ai model response does not match declared result type", K(ret), K(def.result_spec_));
    }
  }
  return ret;
}

int AiColumnWorker::stage_success(const AiColumnTaskRow &task,
                                  const ObString &result,
                                  int64_t &affected_rows,
                                  const bool result_is_null)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  affected_rows = 0;
  if (OB_FAIL(AiColumnTaskSql::store_success(task, result, sql, result_is_null))) {
    LOG_WARN("build the ai column stage statement failed", K(ret), K(task));
  } else if (OB_FAIL(execute_write(*sql_proxy_, sql, affected_rows))) {
    // a transient failure keeps the task row: the recovery scan or the retry of
    // this worker picks it up again
    LOG_WARN("stage the ai column result failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnWorker::stage_failure(const AiColumnTaskRow &task,
                                  const ObString &error_msg,
                                  int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  affected_rows = 0;
  if (OB_FAIL(AiColumnTaskSql::store_fail(task, error_msg, sql))) {
    LOG_WARN("build the ai column fail statement failed", K(ret), K(task));
  } else if (OB_FAIL(execute_write(*sql_proxy_, sql, affected_rows))) {
    LOG_WARN("stage the ai column failure failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnWorker::abandon_task(ObIAllocator &allocator,
                                 const AiColumnDefArray &defs,
                                 const AiColumnTaskRow &task,
                                 const ObString &error_msg,
                                 const bool can_retry)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (can_retry) {
    const int64_t next_ts = AiColumnRetryPolicy::next_eligible_ts(
        ObTimeUtility::fast_current_time(), task.retry_count_);
    if (OB_FAIL(AiColumnTaskSql::store_retry(task, next_ts, error_msg, sql))) {
      LOG_WARN("build retry sql failed", K(ret));
    } else if (OB_FAIL(execute_write(*sql_proxy_, sql, affected_rows))) {
      LOG_WARN("store retry failed", K(ret), K(task));
    } else {
      // test contract: while waiting for the retry the durable state is
      // state=3 with an incremented retry_count; nothing is published here
      LOG_DEBUG("ai column task rescheduled", K(task), K(next_ts), K(affected_rows));
    }
  } else {
    // The last allowed attempt failed: the column is decided (state 5), it stays
    // NULL, and the row change publishes as soon as its siblings are decided too.
    if (OB_FAIL(stage_failure(task, error_msg, affected_rows))) {
      LOG_WARN("stage the terminal ai column failure failed", K(ret), K(task));
    } else if (1 != affected_rows) {
      LOG_DEBUG("the failed ai column task was replaced while running", K(task),
                K(affected_rows));
    } else if (OB_FAIL(publish_group(allocator, defs, task))) {
      LOG_WARN("publish the ai column group of a failed column failed", K(ret), K(task));
    } else {
      LOG_INFO("ai column task failed terminally", K(task), K(error_msg));
    }
  }
  return ret;
}

int AiColumnWorker::read_sibling_count(ObISQLClient &client,
                                       const AiColumnTaskRow &task,
                                       int64_t &unfinished)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  unfinished = 0;
  if (OB_FAIL(AiColumnTaskSql::count_unfinished_siblings(task.table_id_,
                                                         task.rowkey_predicate_,
                                                         task.request_id_, sql))) {
    LOG_WARN("build sibling count sql failed", K(ret), K(task));
  } else if (OB_FAIL(read_scalar_count(client, sql, unfinished))) {
    LOG_WARN("count unfinished siblings failed", K(ret), K(task));
  }
  return ret;
}

int AiColumnWorker::read_publish_payload(ObISQLClient &client,
                                         ObIAllocator &allocator,
                                         const AiColumnTaskRow &task,
                                         AiColumnTaskRowArray &payload)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  payload.reuse();
  if (OB_FAIL(AiColumnTaskSql::select_publish_payload(task.table_id_,
                                                      task.rowkey_predicate_,
                                                      task.request_id_, sql))) {
    LOG_WARN("build the publish payload sql failed", K(ret), K(task));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(execute_read(client, sql, res))) {
        LOG_WARN("read the publish payload failed", K(ret), K(task));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null publish payload result", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          AiColumnTaskRow staged;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("iterate the publish payload failed", K(ret));
            }
          } else if (OB_FAIL(worker_get_int_or_default(*result, "column_id",
                                                       static_cast<int64_t>(OB_INVALID_ID),
                                                       staged.column_id_))) {
            LOG_WARN("get payload column id failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "state",
                                                       AI_COLUMN_TASK_PENDING, staged.state_))) {
            LOG_WARN("get payload state failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "definition_version", 0,
                                                       staged.definition_version_))) {
            LOG_WARN("get payload definition version failed", K(ret));
          } else if (OB_FAIL(worker_copy_varchar(allocator, *result, "result_data",
                                                 staged.result_data_))) {
            LOG_WARN("get payload result failed", K(ret));
          } else {
            int64_t result_is_null = 0;
            if (OB_FAIL(worker_get_int_or_default(*result, "result_is_null", 0,
                                                  result_is_null))) {
              LOG_WARN("get payload result null flag failed", K(ret));
            } else {
              staged.result_is_null_ = 0 != result_is_null;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (!is_terminal_state(staged.state_)) {
            // the sibling count was read before this row: a row that is still
            // running must never be published, and must never be deleted either
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("publish payload contains a non terminal ai column task", K(ret),
                     K(staged), K(task));
          } else {
            staged.table_id_ = task.table_id_;
            staged.rowkey_predicate_ = task.rowkey_predicate_;
            staged.request_id_ = task.request_id_;
            if (OB_FAIL(payload.push_back(staged))) {
              LOG_WARN("push the staged payload row failed", K(ret));
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int AiColumnWorker::read_catalog_versions(ObISQLClient &client,
                                          const uint64_t table_id,
                                          ObIArray<int64_t> &column_ids,
                                          ObIArray<int64_t> &versions)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  column_ids.reuse();
  versions.reuse();
  if (OB_FAIL(AiColumnTaskSql::select_catalog(table_id, sql))) {
    LOG_WARN("build the catalog sql failed", K(ret), K(table_id));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(execute_read(client, sql, res))) {
        LOG_WARN("read the ai column catalog failed", K(ret), K(table_id));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null ai column catalog result", K(ret));
      } else {
        while (OB_SUCC(ret)) {
          int64_t column_id = static_cast<int64_t>(OB_INVALID_ID);
          int64_t version = 0;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("iterate the ai column catalog failed", K(ret));
            }
          } else if (OB_FAIL(worker_get_int_or_default(*result, "column_id",
                                                       static_cast<int64_t>(OB_INVALID_ID),
                                                       column_id))) {
            LOG_WARN("get catalog column id failed", K(ret));
          } else if (OB_FAIL(worker_get_int_or_default(*result, "definition_version", 0,
                                                       version))) {
            LOG_WARN("get catalog definition version failed", K(ret));
          } else if (OB_FAIL(column_ids.push_back(column_id))) {
            LOG_WARN("push the catalog column id failed", K(ret));
          } else if (OB_FAIL(versions.push_back(version))) {
            LOG_WARN("push the catalog definition version failed", K(ret));
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int AiColumnWorker::drop_task(ObISQLClient &client,
                              const AiColumnTaskRow &task,
                              int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  affected_rows = 0;
  if (OB_FAIL(AiColumnTaskSql::delete_task(task, sql))) {
    LOG_WARN("build the precise task delete sql failed", K(ret), K(task));
  } else if (OB_FAIL(execute_write(client, sql, affected_rows))) {
    LOG_WARN("drop the ai column task failed", K(ret), K(task));
  } else {
    LOG_INFO("dropped an ai column task", K(task), K(affected_rows));
  }
  return ret;
}

int AiColumnWorker::publish_group(ObIAllocator &allocator,
                                  const AiColumnDefArray &defs,
                                  const AiColumnTaskRow &task)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  AiColumnTaskRowArray payload;
  AiColumnDefArray publish_defs;
  AiColumnTaskRowArray publish_tasks;
  ObSEArray<ObString, 4> publish_results;
  ObSEArray<int64_t, 4> publish_states;
  // The staged results that are SQL NULL (AI_EMBED of a NULL input): publishing
  // them must write NULL, not an empty string.
  ObSEArray<bool, 4> publish_nulls;
  ObSEArray<int64_t, 4> catalog_column_ids;
  ObSEArray<int64_t, 4> catalog_versions;
  int64_t unfinished = 0;
  int64_t affected_rows = 0;
  int64_t dropped = 0;
  bool published = false;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(trans.start(sql_proxy_))) {
    LOG_WARN("start the ai column publish transaction failed", K(ret), K(task));
  } else if (OB_FAIL(read_sibling_count(trans, task, unfinished))) {
    LOG_WARN("count the unfinished siblings failed", K(ret), K(task));
  } else if (0 != unfinished) {
    // another AI column of the same row change is still running; it publishes the
    // whole row when it is the last one, so this worker leaves the row alone
    LOG_DEBUG("the ai column row change is not complete yet", K(task), K(unfinished));
  } else if (OB_FAIL(read_publish_payload(trans, allocator, task, payload))) {
    LOG_WARN("read the publish payload failed", K(ret), K(task));
  } else if (payload.empty()) {
    // another worker already published and cleaned this row change, or the tasks
    // of the request are gone: nothing to do, and nothing to do twice
    LOG_DEBUG("the ai column row change has no staged column any more", K(task));
  } else if (OB_FAIL(read_catalog_versions(trans, task.table_id_,
                                           catalog_column_ids, catalog_versions))) {
    LOG_WARN("re-read the ai column catalog failed", K(ret), K(task));
  } else {
    // 1. decide, per staged column, whether its result still belongs to the
    //    definition the catalog holds now, and drop the tasks that do not
    for (int64_t i = 0; OB_SUCC(ret) && i < payload.count(); ++i) {
      const AiColumnTaskRow &staged = payload.at(i);
      const AiColumnDef *def = nullptr;
      int64_t catalog_version = -1;
      const bool has_catalog_row = find_catalog_version(catalog_column_ids, catalog_versions,
                                                        staged.column_id_, catalog_version);
      if (OB_FAIL(find_def(defs, staged.column_id_, def))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          // the column is not in the catalog any more: its staged result can never
          // be published, so its task is dropped precisely and the other columns
          // of the row change still publish
          ret = OB_SUCCESS;
          LOG_INFO("drop a staged task of a dropped ai column", K(staged));
          if (OB_FAIL(drop_task(trans, staged, affected_rows))) {
            LOG_WARN("drop the orphan staged task failed", K(ret), K(staged));
          } else {
            ++dropped;
          }
        }
      } else if (!has_catalog_row ||
                 catalog_version != staged.definition_version_ ||
                 def->definition_version_ != staged.definition_version_) {
        // The definition changed while the result was computed: publishing it would
        // expose a result of the older definition, so this task is dropped and the
        // newer definition queues its own task.  Only this column is dropped.
        LOG_INFO("drop a staged task queued by an older ai column definition", K(staged),
                 K(catalog_version), "catalog_def_version", def->definition_version_);
        if (OB_FAIL(drop_task(trans, staged, affected_rows))) {
          LOG_WARN("drop the stale staged task failed", K(ret), K(staged));
        } else {
          ++dropped;
        }
      } else if (OB_FAIL(publish_defs.push_back(*def))) {
        LOG_WARN("push the publish def failed", K(ret), K(staged));
      } else if (OB_FAIL(publish_results.push_back(staged.result_data_))) {
        LOG_WARN("push the publish result failed", K(ret), K(staged));
      } else if (OB_FAIL(publish_nulls.push_back(staged.result_is_null_))) {
        LOG_WARN("push the publish null flag failed", K(ret), K(staged));
      } else if (OB_FAIL(publish_states.push_back(staged.state_))) {
        LOG_WARN("push the publish state failed", K(ret), K(staged));
      } else if (OB_FAIL(publish_tasks.push_back(staged))) {
        LOG_WARN("push the publish task failed", K(ret), K(staged));
      }
    }
    // 2. publish the whole row change with one statement: the hidden request
    //    column of every published column is the CAS, so a newer request of the
    //    row can never be overwritten by this one
    if (OB_SUCC(ret) && !publish_defs.empty()) {
      ObSqlString sql;
      if (OB_FAIL(AiColumnTaskSql::publish_row(publish_defs, task.rowkey_predicate_,
                                               task.request_id_, publish_results, sql,
                                               &publish_states, &publish_nulls))) {
        LOG_WARN("build the publish sql failed", K(ret), K(task));
      } else if (OB_FAIL(execute_write(trans, sql, affected_rows))) {
        // a transient failure keeps every task row: the whole transaction is
        // rolled back below and the row change is published later
        LOG_WARN("publish the ai column row failed", K(ret), K(task));
      } else if (0 == affected_rows) {
        // A dependency update may replace just one sibling after we read the
        // payload. The combined CAS then misses even though other columns still
        // need this request. Roll back and re-read the current queue on the next
        // pass; transactional DML already replaces/deletes obsolete task rows.
        ret = OB_EAGAIN;
        LOG_DEBUG("ai column publication needs a fresh payload", K(task));
      } else if (1 != affected_rows) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column publication affected multiple rows", K(ret), K(affected_rows));
      } else {
        published = true;
        LOG_DEBUG("ai column row change published", K(task), "columns", publish_defs.count());
      }
    }
    // 3. Retire this payload only after its guarded publication succeeded.
    if (OB_SUCC(ret) && published) {
      for (int64_t i = 0; OB_SUCC(ret) && i < publish_tasks.count(); ++i) {
        if (OB_FAIL(drop_task(trans, publish_tasks.at(i), affected_rows))) {
          LOG_WARN("drop the published task failed", K(ret), K(publish_tasks.at(i)));
        }
      }
    }
  }
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    // the commit flag is materialized first: a LOG macro would expand OB_SUCC(ret)
    // more than once and assign to ret unsequenced
    const bool commit = OB_SUCC(ret);
    if (OB_SUCCESS != (tmp_ret = trans.end(commit))) {
      LOG_WARN("end the ai column publish transaction failed", K(tmp_ret), K(commit));
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }
  if (OB_SUCC(ret) && (published || dropped > 0)) {
    // one line per handled row change: what the publish transaction did
    LOG_DEBUG("ai column row change handled", K(task), "published", published,
              "dropped_columns", dropped);
  }
  return ret;
}

int AiColumnWorker::to_worker_request_id(ObIAllocator &allocator, ObString &lease_owner)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  lease_owner.reset();
  if (OB_FAIL(sql.append_fmt("ai_column_worker$%ld$%ld",
                             ObTimeUtility::fast_current_time(),
                             ATOMIC_AAF(&lease_seq_, 1)))) {
    LOG_WARN("build lease owner failed", K(ret));
  } else if (OB_FAIL(ob_write_string(allocator, ObString(sql.length(), sql.ptr()),
                                     lease_owner))) {
    LOG_WARN("copy lease owner failed", K(ret));
  }
  return ret;
}

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
