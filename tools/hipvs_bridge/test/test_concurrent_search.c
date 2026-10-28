/* Concurrency regression test for seekdb_cuvs_search().
 *
 * The bridge keeps one search workspace per index handle (query buffer, id
 * buffer, distance buffer) so a query does not allocate on every call.  The
 * server searches the same handle from many SQL worker threads while holding
 * only a read lock (TCRLockGuard -> rdlock), so those buffers are shared: unless
 * the bridge serialises the call, one thread's query overwrites another's
 * before the backend reads it, and the answer belongs to the wrong query.
 *
 * This test drives the real bridge code against fake_cuvs.c, whose search
 * derives the answer from the first query component, so a cross-talk shows up
 * as an id / distance mismatch.  run_fake_test.sh also builds a copy with the
 * lock removed to prove the test actually catches the defect.
 */
#include "seekdb_cuvs_bridge.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREAD 8
#define NQ_PER_THREAD 200
#define TOPK 10
#define N_BASE 100000
#define DIM 8

static void* g_handle;
static const float* g_base;
static long g_mismatch;
static pthread_mutex_t g_mm = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* arg) {
  const long tid = (long)arg;
  float qv[DIM];
  unsigned ids[TOPK];
  float dist[TOPK];
  long j, k;
  long local_bad = 0;

  memset(qv, 0, sizeof(qv));
  for (j = 0; j < NQ_PER_THREAD; j++) {
    /* unique, exactly representable first component per (thread, query) */
    qv[0] = (float)(tid * NQ_PER_THREAD + j + 1);
    if (seekdb_cuvs_search(g_handle, qv, 1, TOPK, ids, dist) != 0) {
      local_bad++;
      continue;
    }
    for (k = 0; k < TOPK; k++) {
      const unsigned want = (unsigned)((long)(fabsf(qv[0]) * 1e6f) + k) % (unsigned)N_BASE;
      if (ids[k] != want || dist[k] != qv[0]) {
        local_bad++;
        break;
      }
    }
  }
  pthread_mutex_lock(&g_mm);
  g_mismatch += local_bad;
  pthread_mutex_unlock(&g_mm);
  return NULL;
}

static int run(void) {
  pthread_t th[NTHREAD];
  long t;
  g_mismatch = 0;
  for (t = 0; t < NTHREAD; t++) {
    if (pthread_create(&th[t], NULL, worker, (void*)t) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      return 2;
    }
  }
  for (t = 0; t < NTHREAD; t++) pthread_join(th[t], NULL);
  printf("  queries=%d threads=%d mismatches=%ld\n",
         NTHREAD * NQ_PER_THREAD, NTHREAD, g_mismatch);
  return g_mismatch == 0 ? 0 : 1;
}

int main(void) {
  float* base = (float*)malloc(sizeof(float) * N_BASE * DIM);
  long i;
  int rc;
  if (!base) return 2;
  for (i = 0; i < N_BASE * DIM; i++) base[i] = (float)((i * 37) % 199) * 0.01f;
  g_base = base;
  (void)g_base;

  g_handle = seekdb_cuvs_build(base, N_BASE, DIM);
  if (g_handle == NULL) {
    fprintf(stderr, "seekdb_cuvs_build failed\n");
    free(base);
    return 2;
  }
  rc = run();
  seekdb_cuvs_free(g_handle);
  free(base);
  return rc;
}
