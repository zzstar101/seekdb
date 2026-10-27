/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0.
 */

// Compile the production task and its production header with deterministic
// substitutes for its SQL, timer and clock dependencies. No task implementation
// is copied here; the runner supplies only empty dependency include shims.
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace oceanbase
{
namespace common
{
constexpr int OB_SUCCESS = 0;
constexpr int OB_NOT_INIT = -4006;
constexpr int OB_INIT_TWICE = -4005;
constexpr int OB_ERR_UNEXPECTED = -4016;
constexpr int OB_ENTRY_NOT_EXIST = -4018;
constexpr int OB_TIMEOUT = -4012;
constexpr int64_t OB_INVALID_ID = -1;
class ObMySQLProxy {};
}

namespace test
{
struct Dependencies
{
  bool writable = true;
  bool job_exists = false;
  bool job_complete = false;
  int count_error = 0;
  int create_error = 0;
  int find_error = 0;
  int load_error = 0;
  int complete_error = 0;
  int count_calls = 0;
  int create_calls = 0;
  int find_calls = 0;
  int load_calls = 0;
  int complete_calls = 0;
  int sleep_calls = 0;
  int64_t now_us = 0;
  std::function<void()> after_sleep;
} deps;
common::ObMySQLProxy proxy;

int count_jobs(int64_t &count)
{
  ++deps.count_calls;
  count = deps.job_exists ? 1 : 0;
  return deps.count_error;
}
int create_job(int64_t &id)
{
  ++deps.create_calls;
  if (0 == deps.create_error) {
    deps.job_exists = true;
    id = 7;
  }
  return deps.create_error;
}
int find_job(int64_t &id)
{
  ++deps.find_calls;
  id = 7;
  return deps.find_error ? deps.find_error
      : (!deps.job_exists || deps.job_complete ? common::OB_ENTRY_NOT_EXIST : 0);
}
int complete_job(int64_t id, int result)
{
  ++deps.complete_calls;
  if (7 != id || 0 != result) {
    return common::OB_ERR_UNEXPECTED;
  }
  if (0 == deps.complete_error) {
    deps.job_complete = true;
  }
  return deps.complete_error;
}
}

struct TestGlobalContext
{
  common::ObMySQLProxy *sql_proxy_ = &test::proxy;
  bool sys_package_ready_ = false;
} GCTX;

namespace common
{
class ObTimeoutCtx
{
public:
  explicit ObTimeoutCtx(int64_t deadline_us) : deadline_us_(deadline_us) {}
  bool is_timeouted() const { return test::deps.now_us >= deadline_us_; }
private:
  int64_t deadline_us_;
};
inline void ob_usleep(int64_t us)
{
  test::deps.now_us += us;
  ++test::deps.sleep_calls;
  if (test::deps.after_sleep) {
    test::deps.after_sleep();
  }
}
class ObTimerTask
{
public:
  virtual ~ObTimerTask() = default;
  virtual void runTimerTask() = 0;
};
class ObTimer
{
public:
  bool inited() const { return true; }
  bool task_exist(ObTimerTask &) const { return scheduled; }
  int schedule(ObTimerTask &, int64_t delay, bool repeat, bool immediate = false)
  {
    ++schedule_calls;
    scheduled = true;
    repeated = repeat;
    first_run_immediate = immediate;
    delay_us = delay;
    return OB_SUCCESS;
  }
  int cancel_task(ObTimerTask &) { scheduled = false; return OB_SUCCESS; }
  int wait_task(ObTimerTask &) { return OB_SUCCESS; }
  bool scheduled = false;
  bool repeated = false;
  bool first_run_immediate = false;
  int schedule_calls = 0;
  int64_t delay_us = 0;
};
}
namespace share
{
inline bool server_is_write_enabled() { return test::deps.writable; }
}
namespace pl
{
class ObPLPackageManager
{
public:
  static int load_all_common_sys_package(common::ObMySQLProxy &proxy, bool from_file)
  {
    ++test::deps.load_calls;
    if (&proxy != GCTX.sql_proxy_ || from_file) {
      return common::OB_ERR_UNEXPECTED;
    }
    return test::deps.load_error;
  }
};
}
namespace rootserver
{
using namespace common;
}
}

#define OB_UNLIKELY(expr) (expr)
#define OB_ISNULL(ptr) (nullptr == (ptr))
#define OB_FAIL(expr) (oceanbase::common::OB_SUCCESS != (ret = (expr)))
#define OB_SUCC(ret) (oceanbase::common::OB_SUCCESS == (ret))
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define DISALLOW_COPY_AND_ASSIGN(T) T(const T &) = delete; T &operator=(const T &) = delete
#define GET_ADMIN_JOB_COUNT(type, count) oceanbase::test::count_jobs(count)
#define ADMIN_JOB_CREATE_WITH_RET(id, type) oceanbase::test::create_job(id)
#define ADMIN_JOB_FIND(type, id) oceanbase::test::find_job(id)
#define ADMIN_JOB_COMPLETE(id, result) oceanbase::test::complete_job(id, result)

#ifndef SYSTEM_PACKAGE_LOAD_TASK_SOURCE
#define SYSTEM_PACKAGE_LOAD_TASK_SOURCE "observer/ob_system_package_load_task.cpp"
#endif
#include SYSTEM_PACKAGE_LOAD_TASK_SOURCE

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::rootserver;
using oceanbase::test::deps;

#define CHECK(expr) do { if (!(expr)) { throw std::runtime_error(#expr); } } while (0)

static ObSystemPackageLoadTask &init_task(ObSystemPackageLoadTask &task)
{
  CHECK(OB_SUCCESS == task.init());
  return task;
}

static void fresh_catalog_finishes_on_first_tick()
{
  ObSystemPackageLoadTask task;
  init_task(task).runTimerTask();
  CHECK(GCTX.sys_package_ready_);
  CHECK(deps.job_complete);
  CHECK(1 == deps.create_calls);
  CHECK(1 == deps.load_calls);
  CHECK(1 == deps.complete_calls);
  task.runTimerTask();
  CHECK(1 == deps.load_calls);
}

static void starts_immediately_and_keeps_retry_interval()
{
  ObSystemPackageLoadTask task;
  ObTimer timer;
  CHECK(OB_SUCCESS == init_task(task).start(timer));
  CHECK(timer.first_run_immediate);
  CHECK(timer.repeated);
  CHECK(3000000 == timer.delay_us);
  CHECK(OB_SUCCESS == task.start(timer));
  CHECK(1 == timer.schedule_calls);
}

static void failed_load_is_not_ready_and_can_retry()
{
  ObSystemPackageLoadTask task;
  deps.load_error = OB_TIMEOUT;
  init_task(task).runTimerTask();
  CHECK(!GCTX.sys_package_ready_);
  CHECK(1 == deps.load_calls);
  CHECK(0 == deps.complete_calls);
  deps.load_error = OB_SUCCESS;
  task.runTimerTask();
  CHECK(GCTX.sys_package_ready_);
  CHECK(1 == deps.create_calls);
  CHECK(2 == deps.load_calls);
  CHECK(1 == deps.complete_calls);
}

static void failed_completion_is_not_ready_and_can_retry()
{
  ObSystemPackageLoadTask task;
  deps.complete_error = OB_TIMEOUT;
  init_task(task).runTimerTask();
  CHECK(!GCTX.sys_package_ready_);
  CHECK(!deps.job_complete);
  CHECK(1 == deps.load_calls);
  CHECK(1 == deps.complete_calls);
  deps.complete_error = OB_SUCCESS;
  task.runTimerTask();
  CHECK(GCTX.sys_package_ready_);
  CHECK(1 == deps.create_calls);
  CHECK(2 == deps.complete_calls);
}

static void restarted_completed_job_does_not_reload()
{
  deps.job_exists = true;
  deps.job_complete = true;
  ObSystemPackageLoadTask task;
  init_task(task).runTimerTask();
  CHECK(GCTX.sys_package_ready_);
  CHECK(1 == deps.find_calls);
  CHECK(0 == deps.load_calls);
  CHECK(0 == deps.complete_calls);
}

static void absent_job_is_not_successful_readiness()
{
  ObSystemPackageLoadTask task;
  init_task(task);
  deps.after_sleep = [&task] { task.runTimerTask(); };
  CHECK(OB_SUCCESS == ObSystemPackageLoadTask::wait_system_package_ready(ObTimeoutCtx(1000000)));
  CHECK(deps.sleep_calls > 0);
  CHECK(GCTX.sys_package_ready_);
  CHECK(1 == deps.load_calls);
}

static void absent_job_wait_respects_deadline()
{
  CHECK(OB_TIMEOUT == ObSystemPackageLoadTask::wait_system_package_ready(ObTimeoutCtx(1000000)));
  CHECK(!GCTX.sys_package_ready_);
  CHECK(deps.now_us >= 1000000);
  CHECK(deps.now_us <= 1500000);
}

static void catalog_errors_do_not_publish_readiness()
{
  for (int stage = 0; stage < 3; ++stage) {
    deps = test::Dependencies();
    GCTX.sys_package_ready_ = false;
    ObSystemPackageLoadTask task;
    if (0 == stage) {
      deps.count_error = OB_TIMEOUT;
    } else if (1 == stage) {
      deps.create_error = OB_TIMEOUT;
    } else {
      deps.job_exists = true;
      deps.find_error = OB_TIMEOUT;
    }
    init_task(task).runTimerTask();
    CHECK(!GCTX.sys_package_ready_);
    CHECK(0 == deps.load_calls);
    CHECK(0 == deps.complete_calls);
  }
}

static void read_only_does_not_load_or_wait_for_writes()
{
  deps.writable = false;
  ObSystemPackageLoadTask task;
  init_task(task).runTimerTask();
  CHECK(!GCTX.sys_package_ready_);
  CHECK(0 == deps.count_calls);
  CHECK(OB_SUCCESS == ObSystemPackageLoadTask::wait_system_package_ready(ObTimeoutCtx(1000000)));
  CHECK(0 == deps.sleep_calls);
}

int main()
{
  struct Case { const char *name; void (*run)(); };
  const Case cases[] = {
    {"fresh catalog finishes on first tick", fresh_catalog_finishes_on_first_tick},
    {"immediate first tick and unchanged retry interval", starts_immediately_and_keeps_retry_interval},
    {"failed package load remains pending and retries", failed_load_is_not_ready_and_can_retry},
    {"failed job completion remains pending and retries", failed_completion_is_not_ready_and_can_retry},
    {"completed job after restart does not reload", restarted_completed_job_does_not_reload},
    {"absent job waits for real readiness", absent_job_is_not_successful_readiness},
    {"absent job wait respects deadline", absent_job_wait_respects_deadline},
    {"catalog errors do not publish readiness", catalog_errors_do_not_publish_readiness},
    {"read-only startup does not load or wait", read_only_does_not_load_or_wait_for_writes},
  };
  int failures = 0;
  for (const Case &test_case : cases) {
    deps = test::Dependencies();
    GCTX.sql_proxy_ = &test::proxy;
    GCTX.sys_package_ready_ = false;
    try {
      test_case.run();
      std::cout << "PASS: " << test_case.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL: " << test_case.name << ": " << error.what() << '\n';
    }
  }
  return failures ? 1 : 0;
}
