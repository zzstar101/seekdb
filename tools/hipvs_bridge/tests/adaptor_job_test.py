#!/usr/bin/env python3
"""Fault-inject the production adaptor callbacks without seekdb/GPU dependencies.

Extracts the real entry/job definitions and compiles them with the real worker
header. A bad_alloc during the ID-map copy must preserve the old graph, mapping
and generation. This is a host-side regression test, not a GPU/Recall test.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]
ADAPTOR = REPO / "src/oblib/lib/vector/ob_vsag_adaptor.cpp"
source = ADAPTOR.read_text()
entry = source[source.index("struct ObCuvsEntry {"):source.index("static std::mutex g_ob_cuvs_mu;")]
jobs = source[source.index("struct ObCuvsJob {"):source.index("// [BATCH one-shot]")]

prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>
#include "src/oblib/lib/vector/ob_cuvs_worker.h"

static thread_local bool fail_next_new = false;
extern "C" void *__real__Znwm(std::size_t bytes);
extern "C" void *__wrap__Znwm(std::size_t bytes)
{
  if (fail_next_new) { fail_next_new = false; throw std::bad_alloc(); }
  return __real__Znwm(bytes);
}

static int old_graph, new_graph;
static int build_calls, free_calls, search_calls;
static bool fail_build;
static void *seekdb_cuvs_build(const float *, long n, long dim)
{
  assert(n == 4 && dim == 2);
  ++build_calls;
  return fail_build ? nullptr : &new_graph;
}
static void seekdb_cuvs_free(void *handle)
{
  assert(handle == &old_graph);
  ++free_calls;
}
static int seekdb_cuvs_search(void *handle, const float *, long nq, long topk,
                             unsigned *ids, float *distances)
{
  assert(handle == &old_graph || handle == &new_graph);
  ++search_calls;
  for (long i = 0; i < nq * topk; ++i) {
    ids[i] = static_cast<unsigned>(i % topk);
    distances[i] = static_cast<float>(i);
  }
  return 0;
}
'''

suffix = r'''
using oceanbase::common::obvsag::detail::CuvsWorkerCache;
struct FaultJob { void *(*fn)(void *); void *arg; };
static void *allocation_failure(void *arg)
{
  FaultJob *job = static_cast<FaultJob *>(arg);
  fail_next_new = true;
  return job->fn(job->arg);
}
static ObCuvsEntry entry_fixture()
{
  ObCuvsEntry entry;
  entry.bridge_ = &old_graph;
  entry.dim_ = 2;
  entry.built_n_ = 2;
  entry.ids_ = {100, 200};
  entry.buf_ids_ = {100, 200, 300, 400};
  entry.buf_vecs_.resize(8, 1.0f);
  build_calls = free_calls = search_calls = 0;
  fail_build = false;
  return entry;
}
static void assert_preserved(const ObCuvsEntry &entry)
{
  assert(entry.bridge_ == &old_graph);
  assert(entry.built_n_ == 2);
  assert((entry.ids_ == std::vector<int64_t>{100, 200}));
  assert(free_calls == 0 && search_calls == 0);
}
static void assert_published(const ObCuvsEntry &entry)
{
  assert(entry.bridge_ == &new_graph);
  assert(entry.built_n_ == 4);
  assert(entry.ids_ == entry.buf_ids_);
  assert(build_calls == 1 && free_calls == 1 && search_calls == 1);
}
int main()
{
  CuvsWorkerCache workers;
  const float query[] = {1, 2, 3, 4};
  std::vector<unsigned> offsets(4);
  std::vector<float> distances(4);
  {
    auto entry = entry_fixture();
    ObCuvsJob job{&entry, query, 2, 2, 4, true,
                  &offsets, &distances, -1, false};
    FaultJob fault{ob_cuvs_job, &job};
    assert(workers.run(allocation_failure, &fault) == EFAULT);
    assert_preserved(entry);
    assert(build_calls == 0 && !job.built_ && job.rc_ == -1);
    assert(workers.run(ob_cuvs_job, &job) == 0);
    assert_published(entry);
    assert(job.built_ && job.rc_ == 0);
  }
  {
    auto entry = entry_fixture();
    ObCuvsBatchJob job{&entry, query, 2, 2, 4, true,
                       offsets.data(), distances.data(), 0, false};
    FaultJob fault{ob_cuvs_batch_job, &job};
    assert(workers.run(allocation_failure, &fault) == EFAULT);
    assert_preserved(entry);
    assert(build_calls == 0 && !job.built_ && job.served_ == 0);
    assert(workers.run(ob_cuvs_batch_job, &job) == 0);
    assert_published(entry);
    assert(job.built_ && job.served_ == 2);
  }
  // A failed CAGRA build also preserves the old generation and never searches
  // it using the new generation's row count.
  {
    auto entry = entry_fixture();
    fail_build = true;
    ObCuvsJob job{&entry, query, 2, 2, 4, true,
                  &offsets, &distances, -1, false};
    assert(workers.run(ob_cuvs_job, &job) == 0);
    assert_preserved(entry);
    assert(build_calls == 1 && !job.built_ && job.rc_ == -1);
  }
  {
    auto entry = entry_fixture();
    fail_build = true;
    ObCuvsBatchJob job{&entry, query, 2, 2, 4, true,
                       offsets.data(), distances.data(), 0, false};
    assert(workers.run(ob_cuvs_batch_job, &job) == 0);
    assert_preserved(entry);
    assert(build_calls == 1 && !job.built_ && job.served_ == 0);
  }
  workers.close();
}
'''

with tempfile.TemporaryDirectory(prefix="seekdb-adaptor-job-") as directory:
    test_file = Path(directory) / "adaptor_job_test.cpp"
    binary = Path(directory) / "adaptor_job_test"
    test_file.write_text(prefix + entry + jobs + suffix)
    command = shlex.split(os.environ.get("CXX", "c++"))
    command += ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-pthread", "-O1", "-g"]
    if os.environ.get("SANITIZE") == "1":
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I", str(REPO), str(test_file), "-Wl,--wrap=_Znwm", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: production single/batch callbacks preserve ID-map state on allocation/build failure")
