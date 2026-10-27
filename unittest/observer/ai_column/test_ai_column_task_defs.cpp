/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
// Standalone, dependency free tests for the AI column task state machine, the
// STATUS pseudo column names and the bounded retry policy.  The rules asserted
// here are the ones the public suite observes:
//   ai_column_worker.test          task.state = 3 with retry_count = 1 while the
//                                  task waits for its second attempt, then a
//                                  success on that attempt, and a task that is
//                                  gone (not queued, not FAILED-in-place) after
//                                  the last allowed attempt failed.
//   ai_column_retry_strict.test    STATUS reports PENDING / READY / FAILED.
#include "observer/ai_service/ai_column/ob_ai_column_task_defs.h"

#include <cstring>
#include <iostream>

using namespace oceanbase::observer::ai_column;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; ++failures; } } while (0)

static void test_state_values()
{
  // These integers are persisted in __all_ai_column_task.state and in the hidden
  // per row state column, so they must never be renumbered.
  CHECK(1 == AI_COLUMN_TASK_PENDING);
  CHECK(2 == AI_COLUMN_TASK_RUNNING);
  CHECK(3 == AI_COLUMN_TASK_RETRY_WAIT);
  CHECK(4 == AI_COLUMN_TASK_READY);
  CHECK(5 == AI_COLUMN_TASK_FAILED);
  CHECK(AI_COLUMN_TASK_STATE_MAX > AI_COLUMN_TASK_FAILED);
  // A row is READY only in exactly one state, which is what makes "count the
  // rows whose state is not READY" a valid completeness check for strict read.
  CHECK(AI_COLUMN_TASK_READY != AI_COLUMN_TASK_PENDING);
  CHECK(AI_COLUMN_TASK_READY != AI_COLUMN_TASK_FAILED);
}

static void test_call_types()
{
  CHECK(1 == AI_COLUMN_CALL_COMPLETE);
  CHECK(2 == AI_COLUMN_CALL_EMBED);
  CHECK(AI_COLUMN_CALL_TYPE_MAX > AI_COLUMN_CALL_EMBED);
}

static void test_state_strings()
{
  CHECK(0 == std::strcmp("PENDING", ai_column_task_state_str(AI_COLUMN_TASK_PENDING)));
  CHECK(0 == std::strcmp("RUNNING", ai_column_task_state_str(AI_COLUMN_TASK_RUNNING)));
  CHECK(0 == std::strcmp("RETRY_WAIT", ai_column_task_state_str(AI_COLUMN_TASK_RETRY_WAIT)));
  CHECK(0 == std::strcmp("READY", ai_column_task_state_str(AI_COLUMN_TASK_READY)));
  CHECK(0 == std::strcmp("FAILED", ai_column_task_state_str(AI_COLUMN_TASK_FAILED)));
  // An unknown state must not be reported as one of the contract values.
  CHECK(0 == std::strcmp("UNKNOWN", ai_column_task_state_str(0)));
  CHECK(0 == std::strcmp("UNKNOWN", ai_column_task_state_str(42)));
}

static void test_can_retry()
{
  // max_retries = 0: the first failure is final, which is the terminal failure
  // case of the public worker test.
  CHECK(!AiColumnRetryPolicy::can_retry(0, 0));
  // max_retries = 2: two further attempts after the first failure.
  CHECK(AiColumnRetryPolicy::can_retry(0, 2));
  CHECK(AiColumnRetryPolicy::can_retry(1, 2));
  CHECK(!AiColumnRetryPolicy::can_retry(2, 2));
  CHECK(!AiColumnRetryPolicy::can_retry(3, 2));
  // The retry counter is compared as a signed value: a corrupt negative counter
  // must not disable the bound and loop forever.
  CHECK(!AiColumnRetryPolicy::can_retry(-1, 0));
  CHECK(AiColumnRetryPolicy::can_retry(-1, 1));
}

static void test_backoff()
{
  CHECK(AI_COLUMN_RETRY_BASE_DELAY_US == AiColumnRetryPolicy::next_delay_us(0));
  CHECK(200 * 1000L == AiColumnRetryPolicy::next_delay_us(1));
  CHECK(400 * 1000L == AiColumnRetryPolicy::next_delay_us(2));
  CHECK(800 * 1000L == AiColumnRetryPolicy::next_delay_us(3));
  CHECK(1600 * 1000L == AiColumnRetryPolicy::next_delay_us(4));
  // Capped, never unbounded: the worker must not stop polling a task forever.
  CHECK(AI_COLUMN_RETRY_MAX_DELAY_US == AiColumnRetryPolicy::next_delay_us(5));
  CHECK(AI_COLUMN_RETRY_MAX_DELAY_US == AiColumnRetryPolicy::next_delay_us(64));
  // The delay never decreases.
  for (int64_t i = 1; i < 70; ++i) {
    CHECK(AiColumnRetryPolicy::next_delay_us(i) >= AiColumnRetryPolicy::next_delay_us(i - 1));
  }
}

static void test_eligible_ts()
{
  const int64_t now = 1000 * 1000L;
  // The eligible timestamp is what __all_ai_column_task.eligible_ts stores; the
  // public DML test requires a freshly queued task to be eligible immediately.
  CHECK(now == AiColumnRetryPolicy::next_eligible_ts(now, 0) - AI_COLUMN_RETRY_BASE_DELAY_US);
  CHECK(now + AI_COLUMN_RETRY_BASE_DELAY_US == AiColumnRetryPolicy::next_eligible_ts(now, 0));
  CHECK(now + 200 * 1000L == AiColumnRetryPolicy::next_eligible_ts(now, 1));
  // A negative counter is normalized, so a corrupt row waits like the first try
  // instead of becoming eligible in the past (or in the far future).
  CHECK(now + AI_COLUMN_RETRY_BASE_DELAY_US == AiColumnRetryPolicy::next_eligible_ts(now, -5));
  CHECK(0 == AiColumnRetryPolicy::normalize_retry_count(-7));
  CHECK(3 == AiColumnRetryPolicy::normalize_retry_count(3));
}

int main()
{
  test_state_values();
  test_call_types();
  test_state_strings();
  test_can_retry();
  test_backoff();
  test_eligible_ts();
  if (0 != failures) {
    std::cerr << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "ai column task state machine and retry policy: all checks passed\n";
  return 0;
}
