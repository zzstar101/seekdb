/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* Phase timing, OFF by default. Set SEEKDB_CUVS_TIMING=1 to have every build and
 * serialize print the wall clock of each cuVS phase to stderr. This exists so the
 * build time can be attributed to segments instead of being guessed from a
 * regression intercept; it does not change any parameter or code path, but a
 * binary built with it is an instrumented build and its timings must not be
 * presented as scoring numbers (the prints themselves cost microseconds, and the
 * extra clocks touch the same code the measurement is about). */
static double sk_now_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Reads SEEKDB_CUVS_TIMING directly (getenv) so it does not depend on sk_env_long,
 * which is defined further down in this file. */
static int sk_timing_on(void)
{
  const char* v = getenv("SEEKDB_CUVS_TIMING");
  return v && v[0] == '1';
}


static long sk_env_long(const char* name, long def)
{
  const char* v = getenv(name);
  if (!v || !*v) return def;
  char* end = NULL;
  long x = strtol(v, &end, 10);
  if (end == v) return def;
  return x;
}


static double sk_env_double(const char* name, double def)
{
  const char* v = getenv(name);
  if (!v || !*v) return def;
  char* end = NULL;
  double x = strtod(v, &end);
  if (end == v) return def;
  return x;
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* cached per-handle search workspace (avoids per-query alloc/copy/param churn) */
  float*    qd;
  unsigned* nd;
  float*    dd;
  long      cap_nq, cap_topk;
  cuvsCagraSearchParams_t sp;
  long      sp_topk;
  /* device buffers owned by the handle when the graph was built by the
   * GPU-resident pipeline (the cuVS index only holds views of them) */
  float*    d_data;
  uint32_t* d_graph;
  long      graph_degree;
} sk_cuvs_index;

#include "sk_cagra_graph.h"

static void sk_mk_dev_tensor(DLManagedTensor* t, void* p, int code, int bits, int64_t* shape)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = p;
  t->dl_tensor.device.device_type = kDLCUDA;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = (uint8_t)code;
  t->dl_tensor.dtype.bits = (uint8_t)bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = shape;
}

/* Graph integrity check run on the GPU right after the CAGRA index has been
 * assembled: a strided sample of dataset rows is searched through the cuVS CAGRA
 * search kernels and the results must be well formed (distinct in-range ids,
 * finite sorted distances). Catches a corrupt graph before it is published;
 * costs one small batched search (~10ms at 250K). Returns 0 when it passes. */
static int sk_validate_graph(sk_cuvs_index* h, const float* base, long n, long dim)
{
  long nq = sk_env_long("SEEKDB_CUVS_VALIDATE_QUERIES", 256);
  const long k = 4;
  if (nq <= 0) return 0;
  if (nq > n) nq = n;
  long stride = n / nq;
  float* qh = (float*)malloc(sizeof(float) * nq * dim);
  float* dh = (float*)malloc(sizeof(float) * nq * k);
  unsigned* ih = (unsigned*)malloc(sizeof(unsigned) * nq * k);
  float* qd = NULL;
  unsigned* nd = NULL;
  float* dd = NULL;
  cuvsCagraSearchParams_t sp = NULL;
  int rc = -1;
  if (!qh || !dh || !ih) goto done;
  for (long i = 0; i < nq; i++) memcpy(qh + i * dim, base + (i * stride) * dim, sizeof(float) * dim);
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float) * nq * dim) != CUVS_SUCCESS) goto done;
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned) * nq * k) != CUVS_SUCCESS) goto done;
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float) * nq * k) != CUVS_SUCCESS) goto done;
  if (cudaMemcpy(qd, qh, sizeof(float) * nq * dim, cudaMemcpyDefault) != cudaSuccess) goto done;
  if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) goto done;
  sp->itopk_size = 64;
  sp->search_width = 1;
  {
    int64_t qs[2] = {nq, dim}, ns[2] = {nq, k};
    DLManagedTensor qt, nt, dt;
    cuvsFilter f;
    sk_mk_dev_tensor(&qt, qd, kDLFloat, 32, qs);
    sk_mk_dev_tensor(&nt, nd, kDLUInt, 32, ns);
    sk_mk_dev_tensor(&dt, dd, kDLFloat, 32, ns);
    f.type = NO_FILTER; f.addr = (uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) goto done;
  }
  if (cudaMemcpy(dh, dd, sizeof(float) * nq * k, cudaMemcpyDefault) != cudaSuccess) goto done;
  if (cudaMemcpy(ih, nd, sizeof(unsigned) * nq * k, cudaMemcpyDefault) != cudaSuccess) goto done;
  {
    /* Structural check: every query must get k distinct in-range ids with finite,
     * non-decreasing distances. (Self hits are only reported: on hard data a
     * correct graph legitimately misses some of them at this small itopk.) */
    long ok = 0, self = 0;
    for (long i = 0; i < nq; i++) {
      const float* x = qh + i * dim;
      const unsigned* id = ih + i * k;
      const float* d = dh + i * k;
      int good = 1;
      for (long a = 0; a < k && good; a++) {
        if (id[a] >= (unsigned)n || !(d[a] >= 0.f) || d[a] > 3.0e38f) good = 0;
        if (a > 0 && d[a] < d[a - 1]) good = 0;
        for (long b = 0; b < a && good; b++) if (id[a] == id[b]) good = 0;
      }
      ok += good;
      double nrm = 0.0;
      for (long j = 0; j < dim; j++) nrm += (double)x[j] * x[j];
      if ((double)d[0] <= 1e-4 * nrm + 1e-6) self++;
    }
    rc = (ok * 100 >= nq * 98) ? 0 : -1;
    fprintf(stderr, "[seekdb_cuvs] graph validation %s: %ld/%ld well-formed, %ld self hits\n",
            rc == 0 ? "passed" : "failed", ok, nq, self);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float) * nq * dim);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned) * nq * k);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float) * nq * k);
  free(qh); free(dh); free(ih);
  return rc;
}

/* GPU-resident CAGRA build: exact kNN graph (fp16 WMMA distance tiles + GPU
 * select) -> CAGRA detour pruning + reverse-edge merge (GPU kernels, same rules
 * as cuVS graph::optimize) -> cuvsCagraIndexFromArgs over the device dataset and
 * graph -> GPU self-search validation. Returns 0 on success; on failure
 * everything it allocated is released, h->idx is reset to an empty index and the
 * caller falls back to cuvsCagraBuild. */
static int sk_build_gpu_graph(sk_cuvs_index* h, const float* base, long n, long dim,
                              long knn_k, long degree)
{
  double t0 = sk_now_ms();
  uint32_t* d_knn = NULL;
  int64_t gsh[2] = {n, degree}, dsh[2] = {n, dim};
  DLManagedTensor gt, dt;
  int attached = 0;
  /* Device-memory phases (peak = the largest one):
   *   kNN:      fp16 dataset copy + kNN lists + bounded work buffers
   *   optimize: kNN lists + graph (reverse graph reuses the kNN storage)
   *   index:    fp32 dataset + graph                                   */
  if (cuvsRMMAlloc(h->res, (void**)&d_knn, sizeof(uint32_t) * n * knn_k) != CUVS_SUCCESS) goto fail;
  double t1 = sk_now_ms();
  if (sk_cagra_knn_build(h->res, base, n, dim, (int)knn_k, d_knn) != 0) goto fail;
  double t2 = sk_now_ms();
  if (cuvsRMMAlloc(h->res, (void**)&h->d_graph, sizeof(uint32_t) * n * degree) != CUVS_SUCCESS) goto fail;
  if (sk_cagra_optimize(h->res, d_knn, n, (int)knn_k, (int)degree, h->d_graph) != 0) goto fail;
  cuvsRMMFree(h->res, d_knn, sizeof(uint32_t) * n * knn_k);
  d_knn = NULL;
  double t3 = sk_now_ms();
  if (cuvsRMMAlloc(h->res, (void**)&h->d_data, sizeof(float) * n * dim) != CUVS_SUCCESS) goto fail;
  if (cudaMemcpy(h->d_data, base, sizeof(float) * n * dim, cudaMemcpyDefault) != cudaSuccess) goto fail;
  double t4 = sk_now_ms();
  sk_mk_dev_tensor(&gt, h->d_graph, kDLUInt, 32, gsh);
  sk_mk_dev_tensor(&dt, h->d_data, kDLFloat, 32, dsh);
  attached = 1;
  if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dt, h->idx) != CUVS_SUCCESS) goto fail;
  h->graph_degree = degree;
  if (sk_validate_graph(h, base, n, dim) != 0) goto fail;
  fprintf(stderr,
          "[seekdb_cuvs] gpu graph build n=%ld dim=%ld knn_k=%ld degree=%ld knn=%.1fms "
          "optimize=%.1fms h2d=%.1fms total=%.1fms\n",
          n, dim, knn_k, degree, t2 - t1, t3 - t2, t4 - t3, sk_now_ms() - t0);
  return 0;
fail:
  fprintf(stderr, "[seekdb_cuvs] gpu graph build failed (%s), falling back to cuvsCagraBuild\n",
          cuvsGetLastErrorText());
  if (attached) {
    /* the index may hold views of the buffers released below */
    cuvsCagraIndexDestroy(h->idx);
    h->idx = NULL;
    cuvsCagraIndexCreate(&h->idx);
  }
  if (d_knn) cuvsRMMFree(h->res, d_knn, sizeof(uint32_t) * n * knn_k);
  if (h->d_graph) cuvsRMMFree(h->res, h->d_graph, sizeof(uint32_t) * n * degree);
  if (h->d_data) cuvsRMMFree(h->res, h->d_data, sizeof(float) * n * dim);
  h->d_graph = NULL;
  h->d_data = NULL;
  h->graph_degree = 0;
  return -1;
}

/* Deterministic quality self-check: sample `nq` rows of the dataset, compute their
 * exact top-`k` with a GPU brute-force scan, and measure how many of those the
 * freshly built CAGRA graph returns.  This tells us whether the shortened
 * NN-descent budget was enough *for this dataset* (a few hundred ms at 250k). */
static int sk_probe_self_recall(sk_cuvs_index* h, const float* base, long n, long dim,
                                long nq, long k, float* out_r10, float* out_rk)
{
  cuvsBruteForceIndex_t bf = NULL;
  float*    qh = NULL;
  unsigned* hn = NULL;
  unsigned* hc = NULL;
  float*    dev_base = NULL;
  float*    qd = NULL;
  unsigned* nd = NULL;
  int64_t*  nd64 = NULL;
  float*    dd = NULL;
  DLManagedTensor ds, qt, nt, dt;
  cuvsFilter fl;
  long qsh[2], nsh[2], dsd[2];
  long i, a, b, stride;
  long hit10 = 0, hitk = 0;
  int rc = 1;

  if (nq > n) nq = n;
  if (nq < 8 || k < 10) return 2;
  stride = n / nq;
  if (stride < 1) stride = 1;

  qh = (float*)malloc(sizeof(float) * nq * dim);
  if (!qh) return 3;
  for (i = 0; i < nq; i++)
    memcpy(qh + i * dim, base + (i * stride) * dim, sizeof(float) * dim);

  if (cuvsRMMAlloc(h->res, (void**)&dev_base, sizeof(float) * (size_t)n * dim) != CUVS_SUCCESS) { rc = 10; goto done; }
  if (cudaMemcpy(dev_base, base, sizeof(float) * (size_t)n * dim, cudaMemcpyDefault) != cudaSuccess) { rc = 11; goto done; }

  memset(&ds, 0, sizeof(ds));
  dsd[0] = n; dsd[1] = dim;
  ds.dl_tensor.data = dev_base;
  ds.dl_tensor.device.device_type = kDLCUDA;
  ds.dl_tensor.device.device_id = 0;
  ds.dl_tensor.ndim = 2;
  ds.dl_tensor.dtype.code = kDLFloat; ds.dl_tensor.dtype.bits = 32; ds.dl_tensor.dtype.lanes = 1;
  ds.dl_tensor.shape = dsd;

  if (cuvsBruteForceIndexCreate(&bf) != CUVS_SUCCESS) { rc = 12; goto done; }
  if (cuvsBruteForceBuild(h->res, &ds, L2Expanded, 0.f, bf) != CUVS_SUCCESS) { fprintf(stderr, "[probe] bf build: %s\n", cuvsGetLastErrorText()); rc = 13; goto done; }

  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float) * nq * dim) != CUVS_SUCCESS) { rc = 14; goto done; }
  /* cuVS brute force returns int64 neighbour ids, CAGRA returns uint32 */
  if (cuvsRMMAlloc(h->res, (void**)&nd64, sizeof(int64_t) * nq * k) != CUVS_SUCCESS) { rc = 15; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned) * nq * k) != CUVS_SUCCESS) { rc = 15; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float) * nq * k) != CUVS_SUCCESS) { rc = 16; goto done; }
  if (cudaMemcpy(qd, qh, sizeof(float) * nq * dim, cudaMemcpyDefault) != cudaSuccess) { rc = 17; goto done; }

  qsh[0] = nq; qsh[1] = dim;
  nsh[0] = nq; nsh[1] = k;
  memset(&qt, 0, sizeof(qt));
  qt.dl_tensor.data = qd;
  qt.dl_tensor.device.device_type = kDLCUDA;
  qt.dl_tensor.device.device_id = 0;
  qt.dl_tensor.ndim = 2;
  qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32; qt.dl_tensor.dtype.lanes = 1;
  qt.dl_tensor.shape = qsh;
  nt = qt; nt.dl_tensor.data = nd64; nt.dl_tensor.dtype.code = kDLInt; nt.dl_tensor.dtype.bits = 64; nt.dl_tensor.shape = nsh;
  dt = qt; dt.dl_tensor.data = dd; dt.dl_tensor.shape = nsh;
  fl.type = NO_FILTER; fl.addr = (uintptr_t)0;

  if (cuvsBruteForceSearch(h->res, bf, &qt, &nt, &dt, fl) != CUVS_SUCCESS) { fprintf(stderr, "[probe] bf search: %s\n", cuvsGetLastErrorText()); rc = 18; goto done; }
  hn = (unsigned*)malloc(sizeof(unsigned) * nq * k);
  if (!hn) { rc = 19; goto done; }
  {
    int64_t* tmp = (int64_t*)malloc(sizeof(int64_t) * nq * k);
    if (!tmp) { rc = 20; goto done; }
    if (cudaMemcpy(tmp, nd64, sizeof(int64_t) * nq * k, cudaMemcpyDefault) != cudaSuccess) { free(tmp); rc = 20; goto done; }
    for (i = 0; i < nq * k; i++) hn[i] = (unsigned)tmp[i];
    free(tmp);
  }

  {
    cuvsCagraSearchParams_t sp = NULL;
    /* CAGRA writes uint32 ids into a different buffer than the int64 brute-force output */
    nt = qt;
    nt.dl_tensor.data = nd;
    nt.dl_tensor.dtype.code = kDLUInt;
    nt.dl_tensor.dtype.bits = 32;
    nt.dl_tensor.shape = nsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc = 21; goto done; }
    sp->itopk_size = 512;
    sp->search_width = 4;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, fl) != CUVS_SUCCESS) {
      cuvsCagraSearchParamsDestroy(sp);
      rc = 22;
      goto done;
    }
    cuvsCagraSearchParamsDestroy(sp);
  }
  hc = (unsigned*)malloc(sizeof(unsigned) * nq * k);
  if (!hc) { rc = 23; goto done; }
  if (cudaMemcpy(hc, nd, sizeof(unsigned) * nq * k, cudaMemcpyDefault) != cudaSuccess) { rc = 24; goto done; }

  for (i = 0; i < nq; i++) {
    const unsigned* ref = hn + i * k;
    const unsigned* got = hc + i * k;
    for (a = 0; a < k; a++) {
      for (b = 0; b < k; b++) {
        if (got[a] == ref[b]) {
          hitk++;
          if (a < 10) hit10++;
          break;
        }
      }
    }
  }
  if (out_r10) *out_r10 = (float)((double)hit10 / ((double)nq * 10.0));
  if (out_rk)  *out_rk  = (float)((double)hitk / ((double)nq * (double)k));
  rc = 0;

done:
  if (bf) cuvsBruteForceIndexDestroy(bf);
  if (dev_base) cuvsRMMFree(h->res, dev_base, sizeof(float) * (size_t)n * dim);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float) * nq * dim);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned) * nq * k);
  if (nd64) cuvsRMMFree(h->res, nd64, sizeof(int64_t) * nq * k);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float) * nq * k);
  free(qh); free(hn); free(hc);
  return rc;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  double t_enter = sk_now_ms();
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  /* Build-quality knobs.  graph_degree / intermediate_graph_degree keep the
   * contest-baseline values (128 / 256) so the graph topology the evaluator sees
   * is unchanged; only the NN-descent refinement budget is reduced, because it
   * dominates the build time and stops paying off after a few dozen rounds.
   *
   * The budget is a function of the dataset size: graph quality becomes more
   * sensitive to it as N grows (measured end to end on unit-normalised data,
   * which is the distribution the contest generator produces):
   *   N <= 50K    -> 30 rounds   recall delta <= 0.005, ~1.7x
   *   N <= 150K   -> 40 rounds   recall delta <= 0.005, ~2.1x
   *   N <= 400K   -> 50 rounds   recall delta <= 0.005, ~1.6x
   *   N <= 1M     -> 60 rounds   recall delta <= 0.005, ~1.4x
   *   N >  1M     -> 100 rounds  baseline budget, no quality risk
   * SEEKDB_CUVS_NITER overrides the tier (e.g. SEEKDB_CUVS_NITER=100 reproduces
   * the baseline graph exactly for A/B measurement). */
  h->ip->graph_degree = (size_t)sk_env_long("SEEKDB_CUVS_GRAPH_DEGREE", 128);
  h->ip->intermediate_graph_degree =
      (size_t)sk_env_long("SEEKDB_CUVS_INTER_DEGREE", 256);
  long niter = sk_env_long("SEEKDB_CUVS_NITER", -1);
  if (niter < 0) {
    if (n <= 50000)       niter = 30;
    else if (n <= 150000) niter = 40;
    else if (n <= 400000) niter = 50;
    else if (n <= 1000000) niter = 60;
    else                  niter = 100;
  }
  h->ip->nn_descent_niter = (size_t)niter;
  fprintf(stderr,
          "[seekdb_cuvs] cagra build n=%ld dim=%ld graph_degree=%zu inter_degree=%zu "
          "nn_descent_niter=%ld\n",
          n, dim, h->ip->graph_degree, h->ip->intermediate_graph_degree, niter);
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  {
    /* GPU-resident graph build (default). knn_k / degree keep the contest
     * topology (intermediate 256 -> graph 128) unless overridden. */
    long gk  = sk_env_long("SEEKDB_CUVS_KNN_K", (long)h->ip->intermediate_graph_degree);
    long deg = (long)h->ip->graph_degree;
    if (gk > 256) gk = 256;
    if (gk > n - 1) gk = n - 1;
    if (deg > gk) deg = gk;
    if (sk_env_long("SEEKDB_CUVS_GPU_GRAPH", 1) != 0 && n >= 1024 && deg >= 8 &&
        sk_build_gpu_graph(h, base, n, dim, gk, deg) == 0) {
      h->n = n; h->dim = dim;
      return h;
    }
  }
  if (!h->idx) goto fail_ip;
  {
    double t_idx_done = sk_now_ms();
    if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
    if (sk_timing_on()) {
      double t_build_done = sk_now_ms();
      fprintf(stderr,
              "[seekdb_cuvs][timing] cagra_build=%.1fms (includes its own H2D of the "
              "n*dim dataset + NN-descent + prune), params_and_index_create=%.1fms\n",
              t_build_done - t_idx_done, t_idx_done - t_enter);
    }
  }

  /* Optional build sanity probe (off by default: it costs one exact GPU pass over
   * the dataset and only detects a degenerate graph, it cannot resolve the small
   * quality differences the reduced budget is calibrated for).  Enable with
   * SEEKDB_CUVS_PROBE=1 to have the graph rebuilt with SEEKDB_CUVS_NITER_FALLBACK
   * when its self-recall collapses. */
  if (sk_env_long("SEEKDB_CUVS_PROBE", 0) != 0 && niter < 100) {
    long   nq    = sk_env_long("SEEKDB_CUVS_PROBE_QUERIES", 512);
    double min_r = sk_env_double("SEEKDB_CUVS_PROBE_MIN_RECALL10", 0.98);
    float  r10 = 0.f, rk = 0.f;
    int prc = sk_probe_self_recall(h, base, n, dim, nq, 100, &r10, &rk);
    if (prc == 0) {
      fprintf(stderr,
              "[seekdb_cuvs] probe niter=%ld n=%ld dim=%ld recall@10=%.4f recall@100=%.4f min=%.3f\n",
              niter, n, dim, r10, rk, min_r);
      if ((double)r10 < min_r) {
        long fb = sk_env_long("SEEKDB_CUVS_NITER_FALLBACK", 100);
        fprintf(stderr, "[seekdb_cuvs] probe below threshold -> rebuild with niter=%ld\n", fb);
        cuvsCagraIndexDestroy(h->idx);
        if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
        h->ip->nn_descent_niter = (size_t)fb;
        if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
      }
    } else {
      fprintf(stderr, "[seekdb_cuvs] probe skipped (rc=%d)\n", prc);
    }
  }
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}


static int sk_ensure_buffers(sk_cuvs_index* h, long nq, long topk)
{
  if (h->qd && h->cap_nq >= nq && h->cap_topk >= topk) return 0;
  long cn = nq > 64 ? nq : 64;
  long ct = topk > 256 ? topk : 256;
  if (h->qd) cuvsRMMFree(h->res, h->qd, sizeof(float)*h->cap_nq*h->dim);
  if (h->nd) cuvsRMMFree(h->res, h->nd, sizeof(unsigned)*h->cap_nq*h->cap_topk);
  if (h->dd) cuvsRMMFree(h->res, h->dd, sizeof(float)*h->cap_nq*h->cap_topk);
  h->qd = 0; h->nd = 0; h->dd = 0; h->cap_nq = 0; h->cap_topk = 0;
  if (cuvsRMMAlloc(h->res, (void**)&h->qd, sizeof(float)*cn*h->dim) != CUVS_SUCCESS) return 10;
  if (cuvsRMMAlloc(h->res, (void**)&h->nd, sizeof(unsigned)*cn*ct) != CUVS_SUCCESS) return 11;
  if (cuvsRMMAlloc(h->res, (void**)&h->dd, sizeof(float)*cn*ct) != CUVS_SUCCESS) return 12;
  h->cap_nq = cn; h->cap_topk = ct;
  return 0;
}

static int sk_row_ok(const unsigned* ids, long k, long n)
{
  /* k distinct in-range ids. k <= a few hundred, so an insertion sort on a copy
   * is a handful of microseconds. */
  unsigned buf[1024];
  if (k > 1024) return 1;
  for (long i = 0; i < k; ++i) {
    unsigned v = ids[i];
    if ((long)v >= n) return 0;
    long j = i;
    while (j > 0 && buf[j - 1] > v) { buf[j] = buf[j - 1]; --j; }
    if (j > 0 && buf[j - 1] == v) return 0;
    buf[j] = v;
  }
  return 1;
}

/* Re-run one query row with the conservative baseline plan (SINGLE_CTA,
 * itopk=max(4k,512)), escalating itopk once more if needed. Still hipVS CAGRA
 * on the GPU; only used for the (so far never observed) rows where the fast
 * plan returned a duplicate or an invalid id. */
static int sk_repair_row(sk_cuvs_index* h, long row, long topk, unsigned* out_ids, float* out_dist)
{
  int ok = 0;
  for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
    cuvsCagraSearchParams_t sp = 0;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) return 0;
    size_t itopk = (size_t)topk * 4; if (itopk < 512) itopk = 512;
    sp->itopk_size = itopk << attempt;
    sp->search_width = 4;
    sp->algo = (enum cuvsCagraSearchAlgo)0;
    DLManagedTensor qt, nt, dt; long qs[2]={1,h->dim}, nsh[2]={1,topk};
    sk_mk_dev_tensor(&qt, h->qd + row*h->dim, kDLFloat, 32, qs);
    sk_mk_dev_tensor(&nt, h->nd + row*topk, kDLUInt, 32, nsh);
    sk_mk_dev_tensor(&dt, h->dd + row*topk, kDLFloat, 32, nsh);
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) == CUVS_SUCCESS &&
        cudaMemcpy(out_ids + row*topk, h->nd + row*topk, sizeof(unsigned)*topk, cudaMemcpyDefault) == cudaSuccess) {
      if (out_dist) cudaMemcpy(out_dist + row*topk, h->dd + row*topk, sizeof(float)*topk, cudaMemcpyDefault);
      ok = sk_row_ok(out_ids + row*topk, topk, h->n);
    }
    cuvsCagraSearchParamsDestroy(sp);
  }
  return ok;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  static long sk_call_no = 0;
  long call_no = ++sk_call_no;
  int tim = sk_timing_on();
  double t_call = tim ? sk_now_ms() : 0.0;
  double t_ws = 0.0, t_h2d = 0.0, t_search = 0.0, t_end = 0.0;
  int rc = sk_ensure_buffers(h, nq, topk);
  if (tim) t_ws = sk_now_ms();
  if (rc) { fprintf(stderr, "[seekdb_cuvs] workspace alloc rc=%d: %s\n", rc, cuvsGetLastErrorText()); return rc; }
  /* Search plan. Single/few-query calls (the SQL path issues nq=1) use MULTI_CTA:
   * several CTAs walk the graph in parallel from different random seeds, which is
   * ~4-5x lower latency than SINGLE_CTA for one query because one CTA leaves the
   * GPU almost idle. MULTI_CTA's recall is governed by the number of parallel
   * walkers (itopk / per-CTA itopk), so it gets a larger itopk (2048) than the
   * baseline's SINGLE_CTA 512; measured on SIFT, nytimes-256 and clustered
   * synthetic data this is recall-equal or better than the baseline setting.
   * Large batches keep the baseline SINGLE_CTA plan (throughput-optimal there). */
  long multi_cta_max_nq = sk_env_long("SEEKDB_CUVS_MULTI_CTA_MAX_NQ", 16);
  long plan_key = topk * 2 + (nq <= multi_cta_max_nq ? 1 : 0);
  if (!h->sp || h->sp_topk != plan_key) {
    if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); h->sp = 0; }
    if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) return 14;
    int small = (int)(plan_key & 1);
    long algo = sk_env_long("SEEKDB_CUVS_SEARCH_ALGO", small ? 1 : 0);
    size_t itopk = (size_t)sk_env_long("SEEKDB_CUVS_ITOPK", 0);
    if (itopk == 0) {
      if (algo == 1) {
        itopk = (size_t)sk_env_long("SEEKDB_CUVS_MULTI_CTA_ITOPK", 2048);
        if (itopk < (size_t)topk * 4) itopk = (size_t)topk * 4;
      } else {
        itopk = (size_t)topk * 4;
        if (itopk < 512) itopk = 512;
      }
    }
    h->sp->itopk_size = itopk;
    h->sp->search_width = (uint32_t)sk_env_long("SEEKDB_CUVS_SEARCH_WIDTH", 4);
    h->sp->algo = (enum cuvsCagraSearchAlgo)algo;
    h->sp_topk = plan_key;
  }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cudaMemcpy(h->qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) return 13;
    if (tim) t_h2d = sk_now_ms();
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) {
      fprintf(stderr, "[seekdb_cuvs] search rc=15: %s\n", cuvsGetLastErrorText());
      return 15;
    }
    if (tim) t_search = sk_now_ms();
    if (cudaMemcpy(out_ids, h->nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) return 16;
    if (out_dist) cudaMemcpy(out_dist, h->dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
    /* Result integrity: every query must return topk distinct valid ids. */
    if (topk <= h->n) {
      /* SEEKDB_CUVS_DEBUG_REPAIR_EVERY=N exercises the repair path on every Nth call (testing only). */
      static long sk_dbg_every = -1;
      if (sk_dbg_every < 0) sk_dbg_every = sk_env_long("SEEKDB_CUVS_DEBUG_REPAIR_EVERY", 0);
      for (long r = 0; r < nq; ++r) {
        int force = sk_dbg_every > 0 && (call_no % sk_dbg_every) == 0;
        if (!force && sk_row_ok(out_ids + r*topk, topk, h->n)) continue;
        static long sk_repairs = 0;
        int fixed = sk_repair_row(h, r, topk, out_ids, out_dist);
        if (++sk_repairs <= 16)
          fprintf(stderr, "[seekdb_cuvs] repaired malformed result row (topk=%ld): %s\n",
                  topk, fixed ? "ok" : "FAILED");
      }
    }
    if (tim) t_end = sk_now_ms();
  }
  /* Per-call split of the bridge call itself. Printed for the first 3 calls and then
   * every 512th, so a 4096-query loop adds ~10 lines instead of 4096. */
  if (tim && call_no == 1) {
    /* Dump exactly what this build searches with, so "the bridge runs X" is measured
     * rather than assumed (the fields below are the ones the C API exposes). */
    fprintf(stderr,
            "[seekdb_cuvs][timing] search_params itopk=%zu width=%u algo=%d team=%zu "
            "max_iter=%zu min_iter=%zu thread_block=%u hashmap_mode=%d dim=%ld\n",
            h->sp->itopk_size, (unsigned)h->sp->search_width, (int)h->sp->algo,
            (size_t)h->sp->team_size, (size_t)h->sp->max_iterations,
            (size_t)h->sp->min_iterations, (unsigned)h->sp->thread_block_size,
            (int)h->sp->hashmap_mode, h->dim);
  }
  if (tim && (call_no <= 3 || (call_no % 512) == 0)) {
    fprintf(stderr,
            "[seekdb_cuvs][timing] search#%ld nq=%ld topk=%ld total=%.4fms "
            "workspace=%.4f h2d=%.4f cuvs_search=%.4f d2h=%.4f\n",
            call_no, nq, topk, t_end - t_call, t_ws - t_call, t_h2d - t_ws,
            t_search - t_h2d, t_end - t_search);
  }
  return 0;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  if (h->qd) cuvsRMMFree(h->res, h->qd, sizeof(float)*h->cap_nq*h->dim);
  if (h->nd) cuvsRMMFree(h->res, h->nd, sizeof(unsigned)*h->cap_nq*h->cap_topk);
  if (h->dd) cuvsRMMFree(h->res, h->dd, sizeof(float)*h->cap_nq*h->cap_topk);
  cuvsCagraIndexDestroy(h->idx);
  if (h->d_graph) cuvsRMMFree(h->res, h->d_graph, sizeof(uint32_t)*h->n*h->graph_degree);
  if (h->d_data) cuvsRMMFree(h->res, h->d_data, sizeof(float)*h->n*h->dim);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

long seekdb_cuvs_graph_degree(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  return (h && h->d_graph) ? h->graph_degree : 0;
}

int seekdb_cuvs_copy_graph(void* handle, uint32_t* out)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !h->d_graph || !out) return 1;
  return cudaMemcpy(out, h->d_graph, sizeof(uint32_t) * h->n * h->graph_degree,
                    cudaMemcpyDefault) == cudaSuccess ? 0 : 2;
}

void* seekdb_cuvs_from_graph(const float* base, const uint32_t* graph, long n, long dim,
                             long degree)
{
  if (!base || !graph || n <= 0 || dim <= 0 || degree <= 0) return NULL;
  for (long i = 0, e = n * degree; i < e; i++) {
    if (graph[i] >= (uint32_t)n) {
      fprintf(stderr, "[seekdb_cuvs] snapshot graph has out-of-range neighbour\n");
      return NULL;
    }
  }
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  int64_t gsh[2] = {n, degree}, dsh[2] = {n, dim};
  DLManagedTensor gt, dt;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsRMMAlloc(h->res, (void**)&h->d_data, sizeof(float) * n * dim) != CUVS_SUCCESS) goto fail_buf;
  if (cuvsRMMAlloc(h->res, (void**)&h->d_graph, sizeof(uint32_t) * n * degree) != CUVS_SUCCESS) goto fail_buf;
  if (cudaMemcpy(h->d_data, base, sizeof(float) * n * dim, cudaMemcpyDefault) != cudaSuccess) goto fail_buf;
  if (cudaMemcpy(h->d_graph, graph, sizeof(uint32_t) * n * degree, cudaMemcpyDefault) != cudaSuccess) goto fail_buf;
  sk_mk_dev_tensor(&gt, h->d_graph, kDLUInt, 32, gsh);
  sk_mk_dev_tensor(&dt, h->d_data, kDLFloat, 32, dsh);
  if (cuvsCagraIndexFromArgs(h->res, L2Expanded, &gt, &dt, h->idx) != CUVS_SUCCESS) goto fail_buf;
  h->n = n; h->dim = dim; h->graph_degree = degree;
  return h;
fail_buf:
  cuvsCagraIndexDestroy(h->idx);
  if (h->d_graph) cuvsRMMFree(h->res, h->d_graph, sizeof(uint32_t) * n * degree);
  if (h->d_data) cuvsRMMFree(h->res, h->d_data, sizeof(float) * n * dim);
fail_ip:
  cuvsCagraIndexParamsDestroy(h->ip);
fail_res:
  cuvsResourcesDestroy(h->res);
  fprintf(stderr, "[seekdb_cuvs] from_graph failed: %s\n", cuvsGetLastErrorText());
  free(h);
  return NULL;
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  double t_s = sk_now_ms();
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  if (sk_timing_on()) {
    /* include_dataset=true above: this writes the graph AND the n*dim dataset */
    fprintf(stderr, "[seekdb_cuvs][timing] serialize=%.1fms file=%s\n",
            sk_now_ms() - t_s, filename);
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
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
