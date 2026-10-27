/*
 * Copyright (c) 2026 OceanBase.
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

#ifndef OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_
#define OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <new>
#include <pthread.h>

namespace oceanbase {
namespace common {
namespace obvsag {
namespace detail {

// cuVS can exceed the small seekdb worker stack. Keep its calls on a dedicated
// pthread, but reuse that pthread instead of creating one for every search.
// run() is synchronous: the callback and its argument are no longer referenced
// when it returns. Callbacks must return normally and must not re-enter run().
class CuvsWorker
{
public:
  typedef void *(*Job)(void *);
  static const size_t STACK_SIZE = 32UL * 1024 * 1024;

  CuvsWorker()
      : started_(false), closed_(false), job_(nullptr), arg_(nullptr), status_(0)
  {}

  ~CuvsWorker() { close(); }

  int run(Job job, void *arg)
  {
    if (job == nullptr) { return EINVAL; }
    // Serialize submissions separately from completion notification. A second
    // submitter must not overwrite status_ before the first has consumed it.
    std::lock_guard<std::mutex> submit_guard(submit_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) { return ECANCELED; }
    if (!started_) {
      pthread_attr_t attr;
      int rc = pthread_attr_init(&attr);
      if (rc != 0) { return rc; }
      rc = pthread_attr_setstacksize(&attr, STACK_SIZE);
      if (rc == 0) { rc = pthread_create(&thread_, &attr, thread_main, this); }
      pthread_attr_destroy(&attr);
      if (rc != 0) { return rc; }
      started_ = true;
    }
    arg_ = arg;
    status_ = 0;
    job_ = job;
    work_ready_.notify_one();
    job_done_.wait(lock, [this] { return job_ == nullptr; });
    return status_;
  }

  // Owners stop new submissions before destruction. close() also waits for the
  // current synchronous submission, so the worker never outlives its state.
  void close()
  {
    std::lock_guard<std::mutex> submit_guard(submit_mutex_);
    {
      std::lock_guard<std::mutex> guard(mutex_);
      closed_ = true;
      work_ready_.notify_one();
    }
    if (started_) {
      pthread_join(thread_, nullptr);
      started_ = false;
    }
  }

private:
  static void *thread_main(void *arg)
  {
    static_cast<CuvsWorker *>(arg)->work();
    return nullptr;
  }

  void work()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      work_ready_.wait(lock, [this] { return closed_ || job_ != nullptr; });
      if (closed_) { return; }
      Job job = job_;
      void *arg = arg_;
      lock.unlock();
      int status = 0;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
      try {
        job(arg);
      } catch (...) {
        // A failing callback must still wake its submitter. The caller keeps
        // its existing CPU fallback/error handling, and the cache retires us.
        status = EFAULT;
      }
#else
      job(arg);
#endif
      lock.lock();
      status_ = status;
      arg_ = nullptr;
      job_ = nullptr;
      job_done_.notify_one();
    }
  }

  CuvsWorker(const CuvsWorker &) = delete;
  CuvsWorker &operator=(const CuvsWorker &) = delete;

  std::mutex submit_mutex_;
  std::mutex mutex_;
  std::condition_variable work_ready_;
  std::condition_variable job_done_;
  pthread_t thread_;
  bool started_;
  bool closed_;
  Job job_;
  void *arg_;
  int status_;
};

// Cache only two idle workers (64 MiB of reserved stack address space, not
// eagerly committed RSS). Active requests may create additional workers, as
// they did before, so concurrent raw batch calls are not globally serialized.
// This cache owns threads only; it never caches vectors, graphs, or results.
class CuvsWorkerCache
{
public:
  CuvsWorkerCache() : idle_(nullptr), idle_count_(0), active_(0), closed_(false) {}
  ~CuvsWorkerCache() { close(); }

  int run(CuvsWorker::Job job, void *arg)
  {
    if (job == nullptr) { return EINVAL; }
    Slot *slot = nullptr;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (closed_) { return ECANCELED; }
      ++active_;
      if (idle_ != nullptr) {
        slot = idle_;
        idle_ = slot->next_;
        --idle_count_;
      }
    }
    int rc = 0;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try {
#endif
      if (slot == nullptr) { slot = new (std::nothrow) Slot; }
      rc = slot != nullptr ? slot->worker_.run(job, arg) : ENOMEM;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    } catch (...) {
      // This includes allocation/condition-variable setup failures. No job
      // has escaped run(); retire the worker and release the active lease.
      rc = EFAULT;
    }
#endif
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (slot != nullptr && rc == 0 && !closed_ && idle_count_ < MAX_IDLE) {
        slot->next_ = idle_;
        idle_ = slot;
        ++idle_count_;
        slot = nullptr;
      }
    }
    // Joining a retired worker must not hold the cache mutex. Keep our active
    // lease until cleanup ends so close() also waits for these threads.
    delete slot;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      --active_;
      if (active_ == 0) { drained_.notify_all(); }
    }
    return rc;
  }

  // No new callers may enter during destruction. Existing leases drain before
  // idle workers are joined, and neither callbacks nor joins hold mutex_.
  void close()
  {
    Slot *idle = nullptr;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      closed_ = true;
      drained_.wait(lock, [this] { return active_ == 0; });
      idle = idle_;
      idle_ = nullptr;
      idle_count_ = 0;
    }
    while (idle != nullptr) {
      Slot *next = idle->next_;
      delete idle;
      idle = next;
    }
  }

private:
  struct Slot {
    CuvsWorker worker_;
    Slot *next_ = nullptr;
  };
  static const size_t MAX_IDLE = 2;

  CuvsWorkerCache(const CuvsWorkerCache &) = delete;
  CuvsWorkerCache &operator=(const CuvsWorkerCache &) = delete;

  std::mutex mutex_;
  std::condition_variable drained_;
  Slot *idle_;
  size_t idle_count_;
  size_t active_;
  bool closed_;
};

} // namespace detail
} // namespace obvsag
} // namespace common
} // namespace oceanbase

#endif // OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_
