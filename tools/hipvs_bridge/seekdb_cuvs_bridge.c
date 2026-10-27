/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  cudaStream_t stream;
  int stream_ready;
  int unusable;
  void *queries, *neighbors, *distances;
  size_t query_capacity, neighbor_capacity, distance_capacity;
  long n, dim;
} sk_cuvs_index;

static int matrix_bytes(long rows, long cols, size_t element_size, size_t *bytes)
{
  if (rows <= 0 || cols <= 0 || (uintmax_t)rows > SIZE_MAX ||
      (uintmax_t)cols > SIZE_MAX || (size_t)rows > SIZE_MAX / (size_t)cols) {
    return 1;
  }
  const size_t count = (size_t)rows * (size_t)cols;
  if (count > SIZE_MAX / element_size) return 1;
  *bytes = count * element_size;
  return 0;
}

static void init_tensor(DLManagedTensor *tensor, void *data,
                        int64_t *shape, DLDeviceType device, uint8_t dtype)
{
  memset(tensor, 0, sizeof(*tensor));
  tensor->dl_tensor.data = data;
  tensor->dl_tensor.device.device_type = device;
  tensor->dl_tensor.ndim = 2;
  tensor->dl_tensor.dtype.code = dtype;
  tensor->dl_tensor.dtype.bits = 32;
  tensor->dl_tensor.dtype.lanes = 1;
  tensor->dl_tensor.shape = shape;
}

static void log_error(const char *operation, int rc)
{
  const char *error = cuvsGetLastErrorText();
  fprintf(stderr, "[seekdb_cuvs] %s rc=%d: %s\n", operation, rc,
          error ? error : "GPU operation failed");
}

static int release_buffer(sk_cuvs_index *h, void **data, size_t *capacity)
{
  void *owned = *data;
  const size_t bytes = *capacity;
  /* Forget the address before calling the backend: a release may take effect
   * and still report an error, so retrying that address can double-free it. */
  *data = NULL;
  *capacity = 0;
  if (owned != NULL && cuvsRMMFree(h->res, owned, bytes) != CUVS_SUCCESS) {
    h->unusable = 1;
    fprintf(stderr, "[seekdb_cuvs] workspace release failed; ownership unknown, "
                    "pointer quarantined without retry (%zu bytes). "
                    "Unreleased device memory may remain until process exit.\n", bytes);
    return 1;
  }
  return 0;
}

/* Keep the previous allocation if allocating its replacement fails. Capacities
 * are exact high-water marks to limit retained device memory. */
static int reserve_buffer(sk_cuvs_index *h, void **data, size_t *capacity, size_t required)
{
  void *next = NULL;
  if (required <= *capacity) return 0;
  if (cuvsRMMAlloc(h->res, &next, required) != CUVS_SUCCESS) return 1;
  if (release_buffer(h, data, capacity)) {
    size_t next_capacity = required;
    release_buffer(h, &next, &next_capacity);
    return 1;
  }
  *data = next;
  *capacity = required;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  size_t base_bytes = 0;
  DLManagedTensor dset;
  int64_t ds[2] = {n, dim};
  /* Neighbor offsets and CAGRA dimensions are represented by uint32_t. */
  if (base == NULL || n <= 0 || (uintmax_t)n > UINT32_MAX ||
      dim <= 0 || (uintmax_t)dim > UINT32_MAX ||
      matrix_bytes(n, dim, sizeof(float), &base_bytes) != 0) {
    return NULL;
  }
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (cuvsStreamGet(h->res, &h->stream) != CUVS_SUCCESS) goto fail;
  h->stream_ready = 1;
  init_tensor(&dset, (void*)base, ds, kDLCPU, kDLFloat);

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail;
  /* The caller may release base immediately after a successful build. */
  if (cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail;
  h->n = n; h->dim = dim;
  return h;

fail:
  log_error("build", 1);
  seekdb_cuvs_free(h);
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  size_t query_bytes = 0, neighbor_bytes = 0, distance_bytes = 0;
  int rc = 0;
  int submitted = 0;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n ||
      matrix_bytes(nq, h->dim, sizeof(float), &query_bytes) != 0 ||
      matrix_bytes(nq, topk, sizeof(unsigned), &neighbor_bytes) != 0 ||
      matrix_bytes(nq, topk, sizeof(float), &distance_bytes) != 0) {
    return 1;
  }
  if (h->unusable) return 18;
  if (reserve_buffer(h, &h->queries, &h->query_capacity, query_bytes)) { rc=10; goto done; }
  if (reserve_buffer(h, &h->neighbors, &h->neighbor_capacity, neighbor_bytes)) { rc=11; goto done; }
  if (reserve_buffer(h, &h->distances, &h->distance_capacity, distance_bytes)) { rc=12; goto done; }
  if (!h->sp && cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc=14; goto done; }
  /* Allocation/free must not be relied on for implicit synchronization: all
   * transfers and CAGRA work use the resource stream and finish before return. */
  submitted = 1;
  if (cudaMemcpyAsync(h->queries, query, query_bytes, cudaMemcpyDefault, h->stream)
      != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt;
    int64_t qs[2]={nq,h->dim}, ns[2]={nq,topk};
    init_tensor(&qt, h->queries, qs, kDLCUDA, kDLFloat);
    init_tensor(&nt, h->neighbors, ns, kDLCUDA, kDLUInt);
    init_tensor(&dt, h->distances, ns, kDLCUDA, kDLFloat);
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpyAsync(out_ids, h->neighbors, neighbor_bytes, cudaMemcpyDefault, h->stream)
        != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpyAsync(out_dist, h->distances, distance_bytes,
                                    cudaMemcpyDefault, h->stream) != cudaSuccess) { rc=17; goto done; }
  }
done:
  /* Drain even a failed submission before reusing workspace or returning host
   * buffers to the caller. A synchronization failure makes this handle unusable. */
  if (submitted && cuvsStreamSync(h->res) != CUVS_SUCCESS) {
    h->unusable = 1;
    rc = 18;
  }
  if (rc) log_error("search", rc);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->res && h->stream_ready) cuvsStreamSync(h->res);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  release_buffer(h, &h->distances, &h->distance_capacity);
  release_buffer(h, &h->neighbors, &h->neighbor_capacity);
  release_buffer(h, &h->queries, &h->query_capacity);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  if (h->res) cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
