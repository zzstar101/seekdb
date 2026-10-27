/* Standalone tests of the production large-stack worker; no GPU is mocked. */
#include "src/oblib/lib/vector/ob_cuvs_worker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using oceanbase::common::obvsag::detail::CuvsWorker;
using oceanbase::common::obvsag::detail::CuvsWorkerCache;

namespace {
std::mutex tracked_mutex;
std::set<pthread_t> tracked_threads;
std::atomic<int> worker_creates(0);
std::atomic<int> worker_joins(0);
std::atomic<bool> fail_next_create(false);

size_t live_workers()
{
  std::lock_guard<std::mutex> guard(tracked_mutex);
  return tracked_threads.size();
}

struct Probe {
  pthread_t thread;
  size_t stack_size = 0;
  int value = 0;
};

void *probe_job(void *arg)
{
  Probe *probe = static_cast<Probe *>(arg);
  probe->thread = pthread_self();
  pthread_attr_t attr;
  assert(pthread_getattr_np(pthread_self(), &attr) == 0);
  assert(pthread_attr_getstacksize(&attr, &probe->stack_size) == 0);
  assert(pthread_attr_destroy(&attr) == 0);
  probe->value = 42;
  return nullptr;
}

void *increment_job(void *arg)
{
  ++*static_cast<int *>(arg);
  return nullptr;
}

struct Barrier {
  std::mutex mutex;
  std::condition_variable cv;
  int entered = 0;
  bool release = false;
};

void *blocked_job(void *arg)
{
  Barrier *barrier = static_cast<Barrier *>(arg);
  std::unique_lock<std::mutex> lock(barrier->mutex);
  ++barrier->entered;
  barrier->cv.notify_all();
  barrier->cv.wait(lock, [barrier] { return barrier->release; });
  return nullptr;
}

void wait_entered(Barrier &barrier, int count)
{
  std::unique_lock<std::mutex> lock(barrier.mutex);
  assert(barrier.cv.wait_for(lock, std::chrono::seconds(10),
                             [&barrier, count] { return barrier.entered == count; }));
}

void release_jobs(Barrier &barrier)
{
  std::lock_guard<std::mutex> guard(barrier.mutex);
  barrier.release = true;
  barrier.cv.notify_all();
}

void test_reuse_and_stack()
{
  const int before = worker_creates.load();
  {
    CuvsWorkerCache cache;
    Probe first;
    assert(cache.run(probe_job, &first) == 0);
    assert(first.value == 42);
    assert(first.stack_size >= CuvsWorker::STACK_SIZE);
    for (int i = 0; i < 100; ++i) {
      Probe next;
      assert(cache.run(probe_job, &next) == 0);
      assert(next.value == 42);
      assert(pthread_equal(first.thread, next.thread));
    }
    assert(worker_creates.load() == before + 1);
    assert(live_workers() == 1);
    assert(cache.run(nullptr, nullptr) == EINVAL);
  }
  assert(live_workers() == 0);
}

void test_parallel_calls_and_idle_limit()
{
  CuvsWorkerCache cache;
  Barrier barrier;
  std::vector<std::thread> callers;
  for (int i = 0; i < 6; ++i) {
    callers.emplace_back([&cache, &barrier] {
      assert(cache.run(blocked_job, &barrier) == 0);
    });
  }
  // All callbacks must enter before any is released: the idle limit must not
  // become a concurrency limit or globally serialize independent raw batches.
  wait_entered(barrier, 6);
  assert(live_workers() == 6);
  release_jobs(barrier);
  for (std::thread &caller : callers) { caller.join(); }
  assert(live_workers() == 2);
  cache.close();
  assert(live_workers() == 0);
  int count = 0;
  assert(cache.run(increment_job, &count) == ECANCELED);
  assert(count == 0);
}

void test_create_failure_and_retry()
{
  CuvsWorkerCache cache;
  int count = 0;
  fail_next_create = true;
  assert(cache.run(increment_job, &count) == EAGAIN);
  assert(count == 0);
  assert(live_workers() == 0);
  assert(cache.run(increment_job, &count) == 0);
  assert(count == 1);
}

void test_exception_and_retry()
{
  CuvsWorkerCache cache;
  assert(cache.run([](void *) -> void * {
    throw std::runtime_error("injected callback failure");
  }, nullptr) == EFAULT);
  assert(live_workers() == 0);
  int count = 0;
  assert(cache.run(increment_job, &count) == 0);
  assert(count == 1);
}

void test_close_waits_for_inflight_job()
{
  CuvsWorkerCache cache;
  Barrier barrier;
  std::thread caller([&cache, &barrier] {
    assert(cache.run(blocked_job, &barrier) == 0);
  });
  wait_entered(barrier, 1);
  std::future<void> closed = std::async(std::launch::async, [&cache] { cache.close(); });
  assert(closed.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
  release_jobs(barrier);
  caller.join();
  closed.get();
  assert(live_workers() == 0);
  int count = 0;
  assert(cache.run(increment_job, &count) == ECANCELED);
}

void test_worker_submission_serialization()
{
  CuvsWorker worker;
  int count = 0;
  std::vector<std::thread> callers;
  for (int i = 0; i < 8; ++i) {
    callers.emplace_back([&worker, &count] {
      for (int j = 0; j < 100; ++j) {
        assert(worker.run(increment_job, &count) == 0);
      }
    });
  }
  for (std::thread &caller : callers) { caller.join(); }
  assert(count == 800);
  worker.close();
  assert(worker.run(increment_job, &count) == ECANCELED);
  assert(live_workers() == 0);
}
} // namespace

extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);
extern "C" int __real_pthread_join(pthread_t, void **);

extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                     void *(*start)(void *), void *arg)
{
  size_t stack_size = 0;
  const bool worker = attr != nullptr && pthread_attr_getstacksize(attr, &stack_size) == 0
      && stack_size == CuvsWorker::STACK_SIZE;
  if (worker && fail_next_create.exchange(false)) { return EAGAIN; }
  const int rc = __real_pthread_create(thread, attr, start, arg);
  if (rc == 0 && worker) {
    std::lock_guard<std::mutex> guard(tracked_mutex);
    tracked_threads.insert(*thread);
    ++worker_creates;
  }
  return rc;
}

extern "C" int __wrap_pthread_join(pthread_t thread, void **result)
{
  const int rc = __real_pthread_join(thread, result);
  if (rc == 0) {
    std::lock_guard<std::mutex> guard(tracked_mutex);
    if (tracked_threads.erase(thread) != 0) { ++worker_joins; }
  }
  return rc;
}

int main()
{
  test_reuse_and_stack();
  test_parallel_calls_and_idle_limit();
  test_create_failure_and_retry();
  test_exception_and_retry();
  test_close_waits_for_inflight_job();
  test_worker_submission_serialization();
  assert(worker_creates.load() == worker_joins.load());
  std::cout << "worker tests passed: reuse, stack, concurrent batches, idle cap, "
               "create failure, callback failure, shutdown, submission serialization\n";
  return 0;
}
