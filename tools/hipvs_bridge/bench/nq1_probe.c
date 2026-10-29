/* nq1_probe: single-query (nq=1) CAGRA search latency probe.
 *
 * Purpose: separate the three quantities that the first-round report conflated
 *   (a) host wall clock of a nq=1 call, sequential, one call at a time
 *   (b) host wall clock of a nq=BATCH call divided by BATCH  (amortised)
 *   (c) GPU event elapsed for a nq=1 call
 * and sweep the *execution mechanism* knobs (search algo / team size) at a
 * FIXED itopk, so recall is not traded away.
 *
 * Build once, then run every config on the same index object.
 *
 * cfg syntax:  itopk:width:algo:team:maxit:minit
 *   algo  0=SINGLE_CTA 1=MULTI_CTA 2=MULTI_KERNEL 3=AUTO
 *   team  0=auto
 * A cfg prefixed with 'B' (e.g. B256:...) runs one batch call of that many
 * queries and reports the amortised value, i.e. quantity (b).
 *
 * usage: nq1_probe base.f32 n dim query.f32 nq topk deg inter niter cfg[,cfg...] outprefix
 * writes <outprefix>_<cfgindex>.bin  (uint32 nq*topk) for recall checking,
 * and one JSON-ish line per cfg on stdout.
 */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double pct(double *v, long n, double p)
{
  if (n <= 0) return -1;
  double *c = (double *)malloc(sizeof(double) * n);
  memcpy(c, v, sizeof(double) * n);
  qsort(c, n, sizeof(double), cmp_double);
  long i = (long)(p * (double)(n - 1) + 0.5);
  if (i < 0) i = 0;
  if (i >= n) i = n - 1;
  double r = c[i];
  free(c);
  return r;
}


/* Mirror of what the server does per query: src/oblib/lib/vector/ob_vsag_adaptor.cpp:286-289
 * creates a 32MB-stack pthread for EVERY query, runs the whole bridge search on it and
 * joins. Setting PROBE_FRESH_THREAD=1 reproduces that so the cost can be attributed to
 * the thread rather than guessed. */
typedef struct {
  cuvsResources_t res; cuvsCagraSearchParams_t sp; cuvsCagraIndex_t idx;
  const float *qsrc; float *qd; unsigned *nd; float *dd; unsigned *out;
  DLManagedTensor *qt, *nt, *dt; cuvsFilter fl; long batch, dim, topk; int rc;
} sk_pjob;

static void *sk_pjob_run(void *arg)
{
  sk_pjob *j = (sk_pjob *)arg;
  hipMemcpy(j->qd, j->qsrc, sizeof(float) * j->batch * j->dim, hipMemcpyDefault);
  j->rc = (int)cuvsCagraSearch(j->res, j->sp, j->idx, j->qt, j->nt, j->dt, j->fl);
  hipMemcpy(j->out, j->nd, sizeof(unsigned) * j->batch * j->topk, hipMemcpyDefault);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 12) {
    fprintf(stderr, "usage: %s base n dim query nq topk deg inter niter cfg,cfg.. outprefix\n", argv[0]);
    return 2;
  }
  const char *bf = argv[1];
  long n = atol(argv[2]), dim = atol(argv[3]);
  const char *qf = argv[4];
  long nq = atol(argv[5]), topk = atol(argv[6]);
  long deg = atol(argv[7]), inter = atol(argv[8]), niter = atol(argv[9]);
  const char *cfgs = argv[10];
  const char *op = argv[11];

  float *base = (float *)malloc(sizeof(float) * n * dim);
  FILE *f = fopen(bf, "rb");
  if (!f || fread(base, sizeof(float), (size_t)n * dim, f) != (size_t)n * dim) {
    printf("ERR read base\n");
    return 1;
  }
  fclose(f);
  float *qs = (float *)malloc(sizeof(float) * nq * dim);
  f = fopen(qf, "rb");
  if (!f || fread(qs, sizeof(float), (size_t)nq * dim, f) != (size_t)nq * dim) {
    printf("ERR read query\n");
    return 1;
  }
  fclose(f);

  cuvsResources_t res;
  if (cuvsResourcesCreate(&res) != CUVS_SUCCESS) { printf("ERR resources\n"); return 1; }
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndexParamsCreate(&ip);
  ip->build_algo = NN_DESCENT;
  ip->graph_degree = (size_t)deg;
  ip->intermediate_graph_degree = (size_t)inter;
  ip->nn_descent_niter = (size_t)niter;
  cuvsCagraIndex_t idx;
  cuvsCagraIndexCreate(&idx);
  DLManagedTensor ds;
  long dsh[2] = {n, dim};
  ds.dl_tensor.data = base;
  ds.dl_tensor.device.device_type = kDLCPU;
  ds.dl_tensor.device.device_id = 0;
  ds.dl_tensor.ndim = 2;
  ds.dl_tensor.dtype.code = kDLFloat;
  ds.dl_tensor.dtype.bits = 32;
  ds.dl_tensor.dtype.lanes = 1;
  ds.dl_tensor.shape = dsh;
  ds.dl_tensor.strides = 0;
  ds.dl_tensor.byte_offset = 0;

  double t0 = now_ms();
  if (cuvsCagraBuild(res, ip, &ds, idx) != CUVS_SUCCESS) {
    printf("ERR build %s\n", cuvsGetLastErrorText());
    return 1;
  }
  double build_ms = now_ms() - t0;
  printf("{\"event\":\"build\",\"n\":%ld,\"dim\":%ld,\"deg\":%ld,\"inter\":%ld,\"niter\":%ld,"
         "\"build_ms\":%.1f}\n", n, dim, deg, inter, niter, build_ms);
  fflush(stdout);

  /* Optional: reproduce what the server does between build and search, i.e. write the
   * index to disk and search the DESERIALIZED object (the bridge always does this:
   * CREATE VECTOR INDEX serializes, and every later search uses the reloaded index).
   * If per-search cost jumps only in this mode, the cost lives in the deserialize path's
   * dataset handling rather than in the graph or in the search parameters. */
  if (getenv("PROBE_SERIALIZE_ROUNDTRIP")) {
    char path[1024];
    snprintf(path, sizeof(path), "%s_idx.bin", op);
    double s0 = now_ms();
    if (cuvsCagraSerialize(res, path, idx, true) != CUVS_SUCCESS) {
      printf("ERR serialize %s\n", cuvsGetLastErrorText());
      return 1;
    }
    double ser_ms = now_ms() - s0;
    cuvsCagraIndexDestroy(idx);
    cuvsCagraIndexCreate(&idx);
    double d0 = now_ms();
    if (cuvsCagraDeserialize(res, path, idx) != CUVS_SUCCESS) {
      printf("ERR deserialize %s\n", cuvsGetLastErrorText());
      return 1;
    }
    double des_ms = now_ms() - d0;
    printf("{\"event\":\"roundtrip\",\"serialize_ms\":%.1f,\"deserialize_ms\":%.1f}\n",
           ser_ms, des_ms);
    fflush(stdout);
  }

  /* device buffers sized for the largest batch we will use */
  long maxnq = nq > 256 ? nq : 256;
  float *qd = 0;
  unsigned *nd = 0;
  float *dd = 0;
  cuvsRMMAlloc(res, (void **)&qd, sizeof(float) * maxnq * dim);
  cuvsRMMAlloc(res, (void **)&nd, sizeof(unsigned) * maxnq * topk);
  cuvsRMMAlloc(res, (void **)&dd, sizeof(float) * maxnq * topk);

  DLManagedTensor qt, nt, dt;
  long qsh[2] = {1, dim}, nsh[2] = {1, topk}, ddh[2] = {1, topk};
  qt.dl_tensor.data = qd;
  qt.dl_tensor.device.device_type = kDLCUDA;
  qt.dl_tensor.device.device_id = 0;
  qt.dl_tensor.ndim = 2;
  qt.dl_tensor.dtype.code = kDLFloat;
  qt.dl_tensor.dtype.bits = 32;
  qt.dl_tensor.dtype.lanes = 1;
  qt.dl_tensor.shape = qsh;
  qt.dl_tensor.strides = 0;
  qt.dl_tensor.byte_offset = 0;
  nt = qt; nt.dl_tensor.data = nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
  dt = qt; dt.dl_tensor.data = dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = ddh;

  hipEvent_t ev0, ev1;
  hipEventCreate(&ev0);
  hipEventCreate(&ev1);

  const int fresh_thread = getenv("PROBE_FRESH_THREAD") ? 1 : 0;
  /* PROBE_GAP_MS inserts host idle between queries. In the server one query is issued
   * every ~7ms, so the GPU sits idle in between; a tight benchmark loop keeps it at
   * boost clocks and can therefore report a per-query cost the server never sees. */
  long gap_ms = getenv("PROBE_GAP_MS") ? atol(getenv("PROBE_GAP_MS")) : 0;
  char *list = strdup(cfgs), *save = NULL;
  int ci = 0;
  for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    long batch = 1, itopk = 512, width = 4, algo = 0, team = 0, maxit = 0, minit = 0;
    char *p = tok;
    if (*p == 'B') {
      char *colon = strchr(p, ':');
      if (!colon) { printf("ERR cfg %s\n", tok); return 1; }
      *colon = 0;
      batch = atol(p + 1);
      p = colon + 1;
    }
    sscanf(p, "%ld:%ld:%ld:%ld:%ld:%ld", &itopk, &width, &algo, &team, &maxit, &minit);

    cuvsCagraSearchParams_t sp;
    cuvsCagraSearchParamsCreate(&sp);
    sp->itopk_size = (size_t)itopk;
    sp->search_width = (uint32_t)width;
    sp->algo = (enum cuvsCagraSearchAlgo)algo;
    if (team) sp->team_size = (size_t)team;
    sp->max_iterations = (size_t)maxit;
    sp->min_iterations = (size_t)minit;
    cuvsFilter fl;
    fl.type = NO_FILTER;
    fl.addr = (uintptr_t)0;

    /* shape tensors for this batch size */
    long qs2[2] = {batch, dim}, ns2[2] = {batch, topk}, ds2[2] = {batch, topk};
    qt.dl_tensor.shape = qs2;
    nt.dl_tensor.shape = ns2;
    dt.dl_tensor.shape = ds2;

    /* one correctness/alloc warmup call so workspace setup is not in the sample */
    hipMemcpy(qd, qs, sizeof(float) * batch * dim, hipMemcpyDefault);
    cuvsCagraSearch(res, sp, idx, &qt, &nt, &dt, fl);

    long rounds = batch == 1 ? nq : (nq / batch > 0 ? nq / batch : 1);
    double *wall = (double *)malloc(sizeof(double) * rounds);
    double *gpu = (double *)malloc(sizeof(double) * rounds);
    unsigned *out = (unsigned *)malloc(sizeof(unsigned) * batch * topk);
    /* for batch==1 runs we accumulate every query's top-k here, so recall can be
     * checked without a second pass over the data */
    unsigned *outall = (unsigned *)malloc(sizeof(unsigned) * nq * topk);
    long off = 0;
    for (long r = 0; r < rounds; r++) {
      const float *qsrc = qs + (off * dim);
      hipEventRecord(ev0, 0);
      double s0 = now_ms();
      sk_pjob job;
      job.res = res; job.sp = sp; job.idx = idx; job.qsrc = qsrc;
      job.qd = qd; job.nd = nd; job.dd = dd; job.out = out;
      job.qt = &qt; job.nt = &nt; job.dt = &dt; job.fl = fl;
      job.batch = batch; job.dim = dim; job.topk = topk; job.rc = 0;
      if (fresh_thread) {
        pthread_t tid; pthread_attr_t attr; pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
        if (pthread_create(&tid, &attr, sk_pjob_run, &job) == 0) { pthread_join(tid, 0); }
        pthread_attr_destroy(&attr);
      } else {
        sk_pjob_run(&job);
      }
      if (job.rc != CUVS_SUCCESS) {
        printf("ERR search %s\n", cuvsGetLastErrorText());
        return 1;
      }
      double s1 = now_ms();
      hipEventRecord(ev1, 0);
      hipEventSynchronize(ev1);
      float evms = 0;
      hipEventElapsedTime(&evms, ev0, ev1);
      wall[r] = s1 - s0;
      gpu[r] = (double)evms;
      if (batch == 1) memcpy(outall + r * topk, out, sizeof(unsigned) * topk);
      if (gap_ms > 0) { struct timespec ts; ts.tv_sec = gap_ms / 1000; ts.tv_nsec = (gap_ms % 1000) * 1000000L; nanosleep(&ts, 0); }
      off = (off + batch) % nq;
    }
    /* dump ids so recall can be checked: full nq*topk for batch==1 runs */
    char path[512];
    snprintf(path, sizeof(path), "%s_%d.bin", op, ci);
    FILE *o = fopen(path, "wb");
    if (o) {
      if (batch == 1)
        fwrite(outall, sizeof(unsigned), (size_t)nq * topk, o);
      else
        fwrite(out, sizeof(unsigned), (size_t)batch * topk, o);
      fclose(o);
    }

    double total = 0;
    for (long r = 0; r < rounds; r++) total += wall[r];
    printf("{\"event\":\"cfg\",\"ci\":%d,\"cfg\":\"%s\",\"batch\":%ld,\"itopk\":%ld,\"width\":%ld,"
           "\"algo\":%ld,\"team\":%ld,\"maxit\":%ld,\"minit\":%ld,\"rounds\":%ld,"
           "\"wall_p50_ms\":%.4f,\"wall_p95_ms\":%.4f,\"wall_mean_ms\":%.4f,"
           "\"gpu_p50_ms\":%.4f,\"gpu_p95_ms\":%.4f,\"amortised_per_query_ms\":%.4f}\n",
           ci, tok, batch, itopk, width, algo, team, maxit, minit, rounds,
           pct(wall, rounds, 0.50), pct(wall, rounds, 0.95), total / rounds,
           pct(gpu, rounds, 0.50), pct(gpu, rounds, 0.95), (total / rounds) / (double)batch);
    fflush(stdout);
    free(wall); free(gpu); free(out); free(outall);
    cuvsCagraSearchParamsDestroy(sp);
    ci++;
  }
  return 0;
}
