/* A fake cuVS C API backend, used only by the offline bridge tests.
 *
 * It replaces the GPU with a deterministic "search": the top-1 id is derived
 * from the first component of the query vector, and the returned distance is
 * that component.  A short sleep stands in for the device work, which is what
 * makes an unsynchronised share of the per-handle query buffer observable: the
 * caller copies its query into a shared buffer before calling in, so a second
 * thread that overwrites that buffer produces a wrong id here.
 *
 * Never linked into the real bridge; see run_fake_test.sh.
 */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#include <cuda_runtime.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fake_index {
  long n;
  long dim;
  size_t niter;
  size_t graph_degree;
  size_t inter_degree;
};

const char* cuvsGetLastErrorText(void) { return "fake cuvs"; }

cuvsError_t cuvsResourcesCreate(cuvsResources_t* r) { *r = (void*)(uintptr_t)1; return CUVS_SUCCESS; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t r) { (void)r; return CUVS_SUCCESS; }
cuvsError_t cuvsRMMAlloc(cuvsResources_t r, void** p, size_t n) {
  (void)r; *p = malloc(n); return *p ? CUVS_SUCCESS : 1;
}
cuvsError_t cuvsRMMFree(cuvsResources_t r, void* p, size_t n) {
  (void)r; (void)n; free(p); return CUVS_SUCCESS;
}

cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t* p) {
  *p = (cuvsCagraIndexParams_t)calloc(1, sizeof(**p)); return *p ? CUVS_SUCCESS : 1;
}
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { free(p); return CUVS_SUCCESS; }

cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t* i) {
  *i = calloc(1, sizeof(struct fake_index)); return *i ? CUVS_SUCCESS : 1;
}
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t i) { free(i); return CUVS_SUCCESS; }

cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t* p) {
  *p = (cuvsCagraSearchParams_t)calloc(1, sizeof(**p)); return *p ? CUVS_SUCCESS : 1;
}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { free(p); return CUVS_SUCCESS; }

static struct fake_index g_last;
static long g_sp_itopk;
static long g_sp_width;

cuvsError_t cuvsCagraBuild(cuvsResources_t r, cuvsCagraIndexParams_t p,
                           DLManagedTensor* ds, cuvsCagraIndex_t idx) {
  struct fake_index* f = (struct fake_index*)idx;
  (void)r;
  f->n = (long)ds->dl_tensor.shape[0];
  f->dim = (long)ds->dl_tensor.shape[1];
  f->niter = p->nn_descent_niter;
  f->graph_degree = p->graph_degree;
  f->inter_degree = p->intermediate_graph_degree;
  g_last = *f;
  return CUVS_SUCCESS;
}

long fake_last_niter(void) { return (long)g_last.niter; }
long fake_last_itopk(void) { return g_sp_itopk; }
long fake_last_width(void) { return g_sp_width; }
long fake_last_degree(void) { return (long)g_last.graph_degree; }
long fake_last_inter_degree(void) { return (long)g_last.inter_degree; }

cuvsError_t cuvsCagraSearch(cuvsResources_t r, cuvsCagraSearchParams_t sp,
                            cuvsCagraIndex_t idx, DLManagedTensor* qt,
                            DLManagedTensor* nt, DLManagedTensor* dt, cuvsFilter f) {
  const struct fake_index* fi = (const struct fake_index*)idx;
  const float* q = (const float*)qt->dl_tensor.data;   /* the caller's shared buffer */
  unsigned* ids = (unsigned*)nt->dl_tensor.data;
  float* dist = (float*)dt->dl_tensor.data;
  long nq = (long)qt->dl_tensor.shape[0];
  long topk = (long)nt->dl_tensor.shape[1];
  long i, j;
  (void)r; (void)f;
  g_sp_itopk = (long)sp->itopk_size;
  g_sp_width = (long)sp->search_width;

  usleep(200); /* stand-in for device work: widens the overwrite window */

  for (i = 0; i < nq; i++) {
    for (j = 0; j < topk; j++) {
      ids[i * topk + j] = (unsigned)((long)(fabsf(q[i * fi->dim]) * 1e6f) + j) % (unsigned)fi->n;
      dist[i * topk + j] = q[i * fi->dim];
    }
  }
  return CUVS_SUCCESS;
}

cuvsError_t cuvsCagraSerialize(cuvsResources_t r, const char* path, cuvsCagraIndex_t idx, bool b) {
  FILE* fp; const struct fake_index* f = (const struct fake_index*)idx;
  (void)r; (void)b;
  fp = fopen(path, "wb");
  if (!fp) return 1;
  fwrite(f, sizeof(*f), 1, fp);
  fclose(fp);
  return CUVS_SUCCESS;
}

cuvsError_t cuvsCagraDeserialize(cuvsResources_t r, const char* path, cuvsCagraIndex_t idx) {
  FILE* fp; struct fake_index* f = (struct fake_index*)idx;
  (void)r;
  fp = fopen(path, "rb");
  if (!fp) return 1;
  if (fread(f, sizeof(*f), 1, fp) != 1) { fclose(fp); return 1; }
  fclose(fp);
  return CUVS_SUCCESS;
}

cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t idx, int64_t* out) {
  *out = (int64_t)((const struct fake_index*)idx)->n; return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t idx, int64_t* out) {
  *out = (int64_t)((const struct fake_index*)idx)->dim; return CUVS_SUCCESS;
}



/* the fake runtime's memcpy: the "device" and the host share memory here */
int cudaMemcpy(void* dst, const void* src, size_t n, int kind) {
  (void)kind;
  memcpy(dst, src, n);
  return cudaSuccess;
}
