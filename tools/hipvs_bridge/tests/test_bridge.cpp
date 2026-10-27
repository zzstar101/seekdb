/* CPU-only contract tests for the real bridge translation unit. These mocks
 * deliberately defer device work until cuvsStreamSync; they are not a GPU or
 * CAGRA accuracy/performance benchmark. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/neighbors/cagra.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

struct MockIndexParams {};
struct MockSearchParams {};
struct MockIndex {
  long n = 0, dim = 0;
  cuvsResources_t owner = 0;
  std::vector<float> base;
};

namespace {
void check(bool ok, const char *expr, int line)
{
  if (!ok) {
    std::fprintf(stderr, "bridge test failed at line %d: %s\n", line, expr);
    std::abort();
  }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

struct Resource { std::deque<std::function<void()> > pending; };
struct Allocation { size_t bytes; cuvsResources_t owner; };
struct Counters {
  size_t calls = 0, allocs = 0, frees = 0, searches = 0, copies = 0, syncs = 0;
  size_t free_attempts = 0;
  size_t search_param_creates = 0;
  int live_index_params = 0, live_search_params = 0, live_indexes = 0;
} counters;
std::set<Resource *> resources;
std::map<void *, Allocation> allocations;
std::set<void *> quarantined;
enum class Point { None, ResourceCreate, StreamGet, IndexParams, IndexCreate,
                   Build, SearchParams, Alloc, FreeBefore, FreeAfter, Copy, Search, Sync };
Point fault = Point::None;
int fault_remaining = 0;

bool fail(Point point)
{
  if (fault != point || --fault_remaining != 0) return false;
  fault = Point::None;
  return true;
}

void inject(Point point, int occurrence = 1)
{
  CHECK(fault == Point::None);
  fault = point;
  fault_remaining = occurrence;
}

Resource *resource(cuvsResources_t handle)
{
  Resource *r = reinterpret_cast<Resource *>(handle);
  CHECK(resources.count(r) == 1);
  return r;
}

void tensor(const DLManagedTensor *t, DLDeviceType device, uint8_t dtype)
{
  CHECK(t != NULL && t->dl_tensor.data != NULL);
  CHECK(t->dl_tensor.device.device_type == device);
  CHECK(t->dl_tensor.device.device_id == 0);
  CHECK(t->dl_tensor.ndim == 2 && t->dl_tensor.strides == NULL);
  CHECK(t->dl_tensor.dtype.code == dtype && t->dl_tensor.dtype.bits == 32);
  CHECK(t->dl_tensor.dtype.lanes == 1 && t->dl_tensor.byte_offset == 0);
  CHECK(t->manager_ctx == NULL && t->deleter == NULL);
}

void device_buffer(void *p, size_t bytes, cuvsResources_t owner)
{
  const auto it = allocations.find(p);
  CHECK(it != allocations.end());
  CHECK(it->second.bytes >= bytes && it->second.owner == owner);
}

void clean()
{
  CHECK(resources.empty() && allocations.empty() && quarantined.empty());
  CHECK(counters.live_index_params == 0 && counters.live_search_params == 0);
  CHECK(counters.live_indexes == 0 && fault == Point::None);
}

const long N = 8, DIM = 2;
const float BASE[] = {0,0, 1,0, 2,0, 3,0, 0,2, 1,2, 2,2, 3,2};

std::vector<std::pair<float, unsigned> > nearest(const float *base, long n,
                                               long dim, const float *query)
{
  std::vector<std::pair<float, unsigned> > result;
  for (long i = 0; i < n; ++i) {
    float distance = 0;
    for (long d = 0; d < dim; ++d) {
      const float delta = base[i * dim + d] - query[d];
      distance += delta * delta;
    }
    result.emplace_back(distance, static_cast<unsigned>(i));
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<float> queries(long nq)
{
  std::vector<float> result(static_cast<size_t>(nq) * DIM);
  for (long q = 0; q < nq; ++q) {
    result[q * DIM] = static_cast<float>(q % 4) + 0.2f;
    result[q * DIM + 1] = static_cast<float>(q % 3) + 0.1f;
  }
  return result;
}

void search_ok(void *h, long nq, long k, bool with_distances = true)
{
  const auto query = queries(nq);
  std::vector<unsigned> ids(static_cast<size_t>(nq) * k, UINT32_MAX);
  std::vector<float> distances(ids.size(), -1.0f);
  CHECK(seekdb_cuvs_search(h, query.data(), nq, k, ids.data(),
                          with_distances ? distances.data() : NULL) == 0);
  for (long q = 0; q < nq; ++q) {
    const auto expected = nearest(BASE, N, DIM, query.data() + q * DIM);
    for (long kidx = 0; kidx < k; ++kidx) {
      const size_t offset = static_cast<size_t>(q) * k + kidx;
      CHECK(ids[offset] == expected[kidx].second);
      if (with_distances) CHECK(std::fabs(distances[offset] - expected[kidx].first) < 1e-6f);
    }
  }
}

void test_reuse_and_growth()
{
  /* Build must complete its queued host read before returning. */
  std::vector<float> base(BASE, BASE + N * DIM);
  void *h = seekdb_cuvs_build(base.data(), N, DIM);
  CHECK(h != NULL);
  std::fill(base.begin(), base.end(), -1000.0f);
  const size_t initial_allocs = counters.allocs;
  const size_t initial_params = counters.search_param_creates;
  search_ok(h, 2, 2);
  CHECK(counters.allocs == initial_allocs + 3);
  for (int i = 0; i < 100; ++i) search_ok(h, 2, 2);
  search_ok(h, 1, 1, false);
  CHECK(counters.allocs == initial_allocs + 3);
  CHECK(counters.search_param_creates == initial_params + 1);
  search_ok(h, 4, 2);
  CHECK(counters.allocs == initial_allocs + 6);
  search_ok(h, 4, 4);
  CHECK(counters.allocs == initial_allocs + 8);
  search_ok(h, 2, 8);
  CHECK(counters.allocs == initial_allocs + 8 && allocations.size() == 3);
  seekdb_cuvs_free(h);
  clean();
}

void test_invalid_inputs()
{
  void *h = seekdb_cuvs_build(BASE, N, DIM);
  CHECK(h != NULL);
  unsigned ids[8];
  float dist[8];
  const size_t calls = counters.calls;
  CHECK(seekdb_cuvs_search(NULL, BASE, 1, 1, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, NULL, 1, 1, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, 1, 1, NULL, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, 0, 1, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, -1, 1, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, 1, 0, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, 1, -1, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, 1, N + 1, ids, dist) != 0);
  CHECK(seekdb_cuvs_search(h, BASE, std::numeric_limits<long>::max(), N, ids, dist) != 0);
  CHECK(seekdb_cuvs_build(NULL, N, DIM) == NULL);
  CHECK(seekdb_cuvs_build(BASE, 0, DIM) == NULL);
  CHECK(seekdb_cuvs_build(BASE, N, -1) == NULL);
  CHECK(seekdb_cuvs_build(BASE, static_cast<long>(UINT32_MAX),
                        static_cast<long>(UINT32_MAX)) == NULL);
  CHECK(counters.calls == calls);
  search_ok(h, 1, 1);
  seekdb_cuvs_free(h);
  seekdb_cuvs_free(NULL);
  clean();
}

void test_build_failure_cleanup()
{
  const Point stages[] = {Point::ResourceCreate, Point::StreamGet, Point::IndexParams,
                          Point::IndexCreate, Point::Build, Point::Sync};
  for (Point point : stages) {
    inject(point);
    CHECK(seekdb_cuvs_build(BASE, N, DIM) == NULL);
    clean();
  }
}

void test_allocation_failures()
{
  const auto query = queries(4);
  unsigned ids[16];
  float distances[16];
  for (int fail_at = 1; fail_at <= 3; ++fail_at) {
    void *h = seekdb_cuvs_build(BASE, N, DIM);
    CHECK(h != NULL);
    inject(Point::Alloc, fail_at);
    CHECK(seekdb_cuvs_search(h, query.data(), 4, 4, ids, distances) != 0);
    search_ok(h, 4, 4);
    CHECK(allocations.size() == 3);
    seekdb_cuvs_free(h);
    clean();

    h = seekdb_cuvs_build(BASE, N, DIM);
    CHECK(h != NULL);
    search_ok(h, 1, 1);
    inject(Point::Alloc, fail_at);
    CHECK(seekdb_cuvs_search(h, query.data(), 4, 4, ids, distances) != 0);
    CHECK(allocations.size() == 3);
    search_ok(h, 1, 1);
    search_ok(h, 4, 4);
    CHECK(allocations.size() == 3);
    seekdb_cuvs_free(h);
    clean();
  }
}

void simulate_process_exit_cleanup()
{
  /* Production cannot safely retry these addresses. Their ownership is known
   * only to the mock; reclaim them as a GPU process exit would, not as a claim
   * of successful bridge cleanup after a fatal deallocation error. */
  CHECK(resources.empty() && allocations.size() == 1 && quarantined.size() == 1);
  void *data = *quarantined.begin();
  CHECK(allocations.count(data) == 1);
  allocations.erase(data);
  quarantined.erase(data);
  std::free(data);
}

void test_release_failures()
{
  const auto query = queries(4);
  unsigned ids[16];
  float distances[16];
  const Point stages[] = {Point::FreeBefore, Point::FreeAfter};
  for (Point stage : stages) {
    void *h = seekdb_cuvs_build(BASE, N, DIM);
    search_ok(h, 1, 1);
    const size_t attempts = counters.free_attempts;
    inject(stage);
    CHECK(seekdb_cuvs_search(h, query.data(), 4, 4, ids, distances) != 0);
    CHECK(counters.free_attempts == attempts + 2); // Old and replacement: once each.
    const size_t calls = counters.calls;
    CHECK(seekdb_cuvs_search(h, query.data(), 1, 1, ids, distances) != 0);
    CHECK(seekdb_cuvs_search(h, query.data(), 4, 4, ids, distances) != 0);
    CHECK(counters.calls == calls);
    seekdb_cuvs_free(h);
    CHECK(counters.free_attempts == attempts + 4); // Only the other two buffers.
    if (stage == Point::FreeBefore) simulate_process_exit_cleanup();
    clean();

    h = seekdb_cuvs_build(BASE, N, DIM);
    search_ok(h, 1, 1);
    const size_t destroy_attempts = counters.free_attempts;
    inject(stage);
    seekdb_cuvs_free(h);
    CHECK(counters.free_attempts == destroy_attempts + 3); // Continue remaining cleanup.
    if (stage == Point::FreeBefore) simulate_process_exit_cleanup();
    clean();
  }
}

void test_submission_failures()
{
  const auto query = queries(2);
  unsigned ids[4];
  float distances[4];
  for (int copy = 1; copy <= 3; ++copy) {
    void *h = seekdb_cuvs_build(BASE, N, DIM);
    search_ok(h, 2, 2);
    inject(Point::Copy, copy);
    CHECK(seekdb_cuvs_search(h, query.data(), 2, 2, ids, distances) != 0);
    search_ok(h, 2, 2);
    seekdb_cuvs_free(h);
    clean();
  }
  const Point stages[] = {Point::SearchParams, Point::Search};
  for (Point point : stages) {
    void *h = seekdb_cuvs_build(BASE, N, DIM);
    inject(point);
    CHECK(seekdb_cuvs_search(h, query.data(), 2, 2, ids, distances) != 0);
    search_ok(h, 2, 2);
    seekdb_cuvs_free(h);
    clean();
  }
  void *h = seekdb_cuvs_build(BASE, N, DIM);
  inject(Point::Sync);
  CHECK(seekdb_cuvs_search(h, query.data(), 2, 2, ids, distances) != 0);
  const size_t calls = counters.calls;
  CHECK(seekdb_cuvs_search(h, query.data(), 2, 2, ids, distances) != 0);
  CHECK(counters.calls == calls); // Never reuse workspace after failed sync.
  seekdb_cuvs_free(h);
  clean();
}

void test_handle_isolation_and_one_shot()
{
  void *first = seekdb_cuvs_build(BASE, N, DIM);
  void *second = seekdb_cuvs_build(BASE, N, DIM);
  CHECK(first && second);
  search_ok(first, 2, 2);
  search_ok(second, 4, 4);
  CHECK(allocations.size() == 6);
  seekdb_cuvs_free(first);
  CHECK(allocations.size() == 3);
  search_ok(second, 2, 2);
  seekdb_cuvs_free(second);
  clean();
  const auto query = queries(2);
  unsigned ids[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  CHECK(seekdb_cuvs_cagra_knn(BASE, N, DIM, query.data(), 2, 2, ids) == 0);
  for (long q = 0; q < 2; ++q) {
    const auto expected = nearest(BASE, N, DIM, query.data() + q * DIM);
    CHECK(ids[q * 2] == expected[0].second && ids[q * 2 + 1] == expected[1].second);
  }
  clean();
}
} // namespace

extern "C" {
const char *cuvsGetLastErrorText(void) { return NULL; }
cuvsError_t cuvsResourcesCreate(cuvsResources_t *handle)
{
  ++counters.calls;
  if (fail(Point::ResourceCreate)) return CUVS_ERROR;
  Resource *r = new Resource;
  resources.insert(r);
  *handle = reinterpret_cast<cuvsResources_t>(r);
  return CUVS_SUCCESS;
}
cuvsError_t cuvsResourcesDestroy(cuvsResources_t handle)
{
  ++counters.calls;
  Resource *r = resource(handle);
  CHECK(r->pending.empty());
  for (const auto &allocation : allocations) {
    CHECK(allocation.second.owner != handle || quarantined.count(allocation.first) == 1);
  }
  resources.erase(r);
  delete r;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsStreamGet(cuvsResources_t handle, cudaStream_t *stream)
{
  ++counters.calls;
  if (fail(Point::StreamGet)) return CUVS_ERROR;
  *stream = static_cast<cudaStream_t>(resource(handle));
  return CUVS_SUCCESS;
}
cuvsError_t cuvsStreamSync(cuvsResources_t handle)
{
  ++counters.calls;
  ++counters.syncs;
  Resource *r = resource(handle);
  while (!r->pending.empty()) {
    auto work = std::move(r->pending.front());
    r->pending.pop_front();
    work();
  }
  return fail(Point::Sync) ? CUVS_ERROR : CUVS_SUCCESS;
}
cuvsError_t cuvsRMMAlloc(cuvsResources_t handle, void **data, size_t bytes)
{
  ++counters.calls;
  resource(handle);
  if (fail(Point::Alloc)) return CUVS_ERROR;
  *data = std::malloc(bytes);
  CHECK(*data != NULL && bytes > 0);
  allocations.emplace(*data, Allocation{bytes, handle});
  ++counters.allocs;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsRMMFree(cuvsResources_t handle, void *data, size_t bytes)
{
  ++counters.calls;
  ++counters.free_attempts;
  CHECK(resource(handle)->pending.empty());
  CHECK(quarantined.count(data) == 0); // Never retry ownership-uncertain addresses.
  device_buffer(data, bytes, handle);
  CHECK(allocations[data].bytes == bytes); // Free using allocated capacity, not current shape.
  if (fail(Point::FreeBefore)) {
    quarantined.insert(data);
    return CUVS_ERROR;
  }
  allocations.erase(data);
  std::free(data);
  ++counters.frees;
  return fail(Point::FreeAfter) ? CUVS_ERROR : CUVS_SUCCESS;
}
cudaError_t cudaMemcpyAsync(void *dst, const void *src, size_t bytes,
                            cudaMemcpyKind kind, cudaStream_t stream)
{
  ++counters.calls;
  ++counters.copies;
  CHECK(kind == cudaMemcpyDefault);
  const cuvsResources_t handle = reinterpret_cast<cuvsResources_t>(stream);
  Resource *r = resource(handle); // Reject accidental use of a different/default stream.
  const bool dst_device = allocations.count(dst) != 0;
  const bool src_device = allocations.count(const_cast<void *>(src)) != 0;
  CHECK(dst_device || src_device);
  if (dst_device) device_buffer(dst, bytes, handle);
  if (src_device) device_buffer(const_cast<void *>(src), bytes, handle);
  if (fail(Point::Copy)) return cudaErrorUnknown;
  r->pending.emplace_back([=] { std::memcpy(dst, src, bytes); });
  return cudaSuccess;
}
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *params)
{
  ++counters.calls;
  if (fail(Point::IndexParams)) return CUVS_ERROR;
  *params = new MockIndexParams;
  ++counters.live_index_params;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t params)
{
  ++counters.calls;
  CHECK(params != NULL);
  delete params;
  --counters.live_index_params;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *index)
{
  ++counters.calls;
  if (fail(Point::IndexCreate)) return CUVS_ERROR;
  *index = new MockIndex;
  ++counters.live_indexes;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t index)
{
  ++counters.calls;
  CHECK(index != NULL);
  if (index->owner) CHECK(resource(index->owner)->pending.empty());
  delete index;
  --counters.live_indexes;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraBuild(cuvsResources_t handle, cuvsCagraIndexParams_t params,
                         DLManagedTensor *data, cuvsCagraIndex_t index)
{
  ++counters.calls;
  CHECK(params != NULL && index != NULL);
  tensor(data, kDLCPU, kDLFloat);
  index->n = static_cast<long>(data->dl_tensor.shape[0]);
  index->dim = static_cast<long>(data->dl_tensor.shape[1]);
  index->owner = handle;
  const float *source = static_cast<float *>(data->dl_tensor.data);
  resource(handle)->pending.emplace_back([=] {
    index->base.assign(source, source + index->n * index->dim);
  });
  return fail(Point::Build) ? CUVS_ERROR : CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *params)
{
  ++counters.calls;
  if (fail(Point::SearchParams)) return CUVS_ERROR;
  *params = new MockSearchParams;
  ++counters.live_search_params;
  ++counters.search_param_creates;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t params)
{
  ++counters.calls;
  CHECK(params != NULL);
  delete params;
  --counters.live_search_params;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearch(cuvsResources_t handle, cuvsCagraSearchParams_t params,
                          cuvsCagraIndex_t index, DLManagedTensor *query,
                          DLManagedTensor *neighbors, DLManagedTensor *distances,
                          cuvsFilter filter)
{
  ++counters.calls;
  ++counters.searches;
  CHECK(params != NULL && index->owner == handle);
  CHECK(filter.type == NO_FILTER && filter.addr == 0);
  tensor(query, kDLCUDA, kDLFloat);
  tensor(neighbors, kDLCUDA, kDLUInt);
  tensor(distances, kDLCUDA, kDLFloat);
  const long nq = static_cast<long>(query->dl_tensor.shape[0]);
  const long k = static_cast<long>(neighbors->dl_tensor.shape[1]);
  CHECK(query->dl_tensor.shape[1] == index->dim);
  CHECK(neighbors->dl_tensor.shape[0] == nq);
  CHECK(distances->dl_tensor.shape[0] == nq && distances->dl_tensor.shape[1] == k);
  float *query_ptr = static_cast<float *>(query->dl_tensor.data);
  unsigned *ids = static_cast<unsigned *>(neighbors->dl_tensor.data);
  float *dst = static_cast<float *>(distances->dl_tensor.data);
  device_buffer(query_ptr, nq * index->dim * sizeof(float), handle);
  device_buffer(ids, nq * k * sizeof(unsigned), handle);
  device_buffer(dst, nq * k * sizeof(float), handle);
  resource(handle)->pending.emplace_back([=] {
    CHECK(index->base.size() == static_cast<size_t>(index->n * index->dim));
    for (long q = 0; q < nq; ++q) {
      const auto result = nearest(index->base.data(), index->n, index->dim,
                                  query_ptr + q * index->dim);
      for (long i = 0; i < k; ++i) {
        ids[q * k + i] = result[i].second;
        dst[q * k + i] = result[i].first;
      }
    }
  });
  return fail(Point::Search) ? CUVS_ERROR : CUVS_SUCCESS;
}
} // extern "C"

int main()
{
  test_reuse_and_growth();
  test_invalid_inputs();
  test_build_failure_cleanup();
  test_allocation_failures();
  test_release_failures();
  test_submission_failures();
  test_handle_isolation_and_one_shot();
  clean();
  std::puts("PASS: bridge workspace reuse, deferred-stream ordering, growth, failures, cleanup, ABI");
  return 0;
}
