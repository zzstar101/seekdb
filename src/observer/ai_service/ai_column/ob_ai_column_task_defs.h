/**
 * Dependency free primitives of the AI column runtime: the task state machine,
 * the call type of a task, the task state names reported by the STATUS pseudo
 * column, and the bounded retry policy of a failed model call.
 *
 * This header is deliberately free of OceanBase headers so that these rules can
 * be covered by a standalone, executable unit test: see
 * unittest/observer/ai_column.
 */
#pragma once

#include <cstdint>

namespace oceanbase
{
namespace observer
{
namespace ai_column
{

// Task state machine.  The values are stored in __all_ai_column_task.state, and
// the same values are stored in the per row hidden "state" column of the user
// table.  That hidden column is what the STATUS pseudo column of an AI column
// reports, and what the strict read check looks at.
enum AiColumnTaskState : int64_t
{
  AI_COLUMN_TASK_PENDING = 1,
  AI_COLUMN_TASK_RUNNING = 2,
  AI_COLUMN_TASK_RETRY_WAIT = 3,
  AI_COLUMN_TASK_READY = 4,
  AI_COLUMN_TASK_FAILED = 5,
  AI_COLUMN_TASK_STATE_MAX
};

enum AiColumnCallType : int64_t
{
  AI_COLUMN_CALL_COMPLETE = 1,
  AI_COLUMN_CALL_EMBED = 2,
  AI_COLUMN_CALL_TYPE_MAX
};

constexpr int64_t AI_COLUMN_RETRY_BASE_DELAY_US = 100 * 1000L;      // 100ms
constexpr int64_t AI_COLUMN_RETRY_MAX_DELAY_US = 2 * 1000 * 1000L;  // 2s

// The AI COLUMN STATUS pseudo column values.  The public retry/strict read test
// matches on PENDING, READY and FAILED, so the mapping is part of the contract.
inline const char *ai_column_task_state_str(const int64_t state)
{
  const char *str = "UNKNOWN";
  switch (state) {
    case AI_COLUMN_TASK_PENDING: str = "PENDING"; break;
    case AI_COLUMN_TASK_RUNNING: str = "RUNNING"; break;
    case AI_COLUMN_TASK_RETRY_WAIT: str = "RETRY_WAIT"; break;
    case AI_COLUMN_TASK_READY: str = "READY"; break;
    case AI_COLUMN_TASK_FAILED: str = "FAILED"; break;
    default: break;
  }
  return str;
}

// A task is retried at most max_retries times, with a bounded exponential
// backoff.  The retry counter of a task is the number of failures already
// observed, so it becomes 1 while the task waits for its second attempt: that is
// the value the public worker test observes together with state = 3.
class AiColumnRetryPolicy
{
public:
  // max_retries = 0 means the first failure is final.  The counter is
  // normalized first, so a corrupt negative counter can not buy an extra attempt
  // on a column that allows no retry at all.
  static bool can_retry(const int64_t retry_count, const int64_t max_retries)
  {
    return max_retries > 0 && normalize_retry_count(retry_count) < max_retries;
  }
  // Bounded exponential backoff: 100ms, 200ms, 400ms, ... capped at 2s.
  static int64_t next_delay_us(const int64_t retry_count)
  {
    int64_t delay = AI_COLUMN_RETRY_BASE_DELAY_US;
    for (int64_t i = 0; i < retry_count && delay < AI_COLUMN_RETRY_MAX_DELAY_US; ++i) {
      delay *= 2;
    }
    return delay < AI_COLUMN_RETRY_MAX_DELAY_US ? delay : AI_COLUMN_RETRY_MAX_DELAY_US;
  }
  static int64_t next_eligible_ts(const int64_t now_us, const int64_t retry_count)
  {
    return now_us + next_delay_us(normalize_retry_count(retry_count));
  }
  // A negative retry counter (a corrupt task row) is treated as the first try,
  // so that the task still backs off instead of spinning on the model endpoint.
  static int64_t normalize_retry_count(const int64_t retry_count)
  {
    return retry_count > 0 ? retry_count : 0;
  }
};

} // namespace ai_column
} // namespace observer
} // namespace oceanbase
