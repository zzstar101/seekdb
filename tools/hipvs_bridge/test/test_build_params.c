/* Contract tests for the build-parameter policy and the serialise round trip.
 *
 * Both matter for scoring rather than for speed:
 *   - the niter tier decides the recall delta against the frozen baseline, and
 *     a silent shift in the table would move a scored dataset over the 0.005
 *     limit without any visible failure;
 *   - the serialise/deserialise round trip is what makes the index queryable
 *     after the build, and its n/dim guard is what stops a mismatched file from
 *     being loaded into a handle built for another dataset.
 *
 * Runs offline against test/fake_cuvs.c, see run_fake_test.sh.
 */
#include "seekdb_cuvs_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

long fake_last_niter(void);
long fake_last_itopk(void);
long fake_last_width(void);
long fake_last_degree(void);
long fake_last_inter_degree(void);

struct tier_case {
  long n;
  long dim;
  long want_niter;
  const char* why;
};

static const struct tier_case TIERS[] = {
    {100, 128, 30, "small dataset"},
    {24999, 128, 30, "just under 25K"},
    {25000, 128, 30, "25K boundary"},
    {50000, 128, 30, "50K boundary"},
    {50001, 128, 40, "just over 50K"},
    {100000, 128, 40, "100K"},
    {150000, 128, 40, "150K boundary"},
    {150001, 128, 50, "just over 150K"},
    {250000, 128, 50, "SIFT250K, the scored 128-dim dataset"},
    {400000, 128, 50, "400K boundary"},
    {400001, 128, 60, "just over 400K"},
    {1000000, 128, 60, "1M boundary"},
    {1000001, 128, 100, "above 1M: baseline budget"},
    {250000, 256, 60, "Hidden250K, the scored 256-dim dataset"},
    {25000, 256, 60, "small but 256-dim: the high-dim floor applies"},
    {250000, 192, 60, "192 dim is the floor boundary"},
    {250000, 191, 50, "191 dim: no floor"},
};

static int check_tiers(void) {
  float* base;
  long i;
  int bad = 0;
  for (i = 0; i < (long)(sizeof(TIERS) / sizeof(TIERS[0])); i++) {
    void* h;
    long got;
    base = (float*)malloc(sizeof(float) * TIERS[i].n * TIERS[i].dim);
    if (!base) return 1;
    memset(base, 0, sizeof(float) * TIERS[i].n * TIERS[i].dim);
    h = seekdb_cuvs_build(base, TIERS[i].n, TIERS[i].dim);
    if (h == NULL) {
      fprintf(stderr, "  build failed for n=%ld dim=%ld\n", TIERS[i].n, TIERS[i].dim);
      free(base);
      return 1;
    }
    got = fake_last_niter();
    if (got != TIERS[i].want_niter || fake_last_degree() != 128 || fake_last_inter_degree() != 256) {
      fprintf(stderr, "  FAIL n=%ld dim=%ld want niter=%ld got niter=%ld degree=%ld/%ld (%s)\n",
              TIERS[i].n, TIERS[i].dim, TIERS[i].want_niter, got, fake_last_degree(),
              fake_last_inter_degree(), TIERS[i].why);
      bad++;
    }
    seekdb_cuvs_free(h);
    free(base);
  }
  printf("  tiers: %ld cases, %d wrong\n", (long)(sizeof(TIERS) / sizeof(TIERS[0])), bad);
  return bad == 0 ? 0 : 1;
}

static int check_serialize(void) {
  const long n = 5000, dim = 16;
  char path[256];
  float* base = (float*)malloc(sizeof(float) * n * dim);
  void* h;
  void* back;
  int bad = 0;
  long i;

  if (!base) return 1;
  for (i = 0; i < n * dim; i++) base[i] = (float)(i % 97) * 0.01f;
  snprintf(path, sizeof(path), "/tmp/seekdb_bridge_roundtrip_%ld.bin", (long)getpid());

  h = seekdb_cuvs_build(base, n, dim);
  if (!h) { free(base); return 1; }
  if (seekdb_cuvs_serialize(h, path) != 0) {
    fprintf(stderr, "  FAIL serialize\n");
    bad++;
  } else {
    back = seekdb_cuvs_deserialize(path, n, dim);
    if (!back) {
      fprintf(stderr, "  FAIL deserialize with the matching shape returned NULL\n");
      bad++;
    } else {
      seekdb_cuvs_free(back);
    }
    /* wrong shape must be refused, not silently loaded */
    back = seekdb_cuvs_deserialize(path, n, dim + 1);
    if (back != NULL) {
      fprintf(stderr, "  FAIL deserialize accepted a mismatched dim\n");
      bad++;
      seekdb_cuvs_free(back);
    }
    back = seekdb_cuvs_deserialize(path, n + 1, dim);
    if (back != NULL) {
      fprintf(stderr, "  FAIL deserialize accepted a mismatched n\n");
      bad++;
      seekdb_cuvs_free(back);
    }
    unlink(path);
  }
  seekdb_cuvs_free(h);
  free(base);
  printf("  round trip: %s\n", bad == 0 ? "ok" : "broken");
  return bad == 0 ? 0 : 1;
}

/* the query effort must stay a function of topk, not a constant: the pool has
 * to be deep enough for the requested k with margin, and no deeper */
/* The pool size is a recall decision, not a speed knob: itopk=64 was measured to
 * take SQL-side Recall@10 from 0.995 to 0.54 at 250K, so the baseline's
 * max(4*topk, 512) / width 4 is pinned here.  Changing it needs a recall run. */
static int check_query_effort(long topk, long want_itopk, long want_width) {
  float base[64 * 4];
  float q[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  unsigned ids[256];
  float dist[256];
  void* h;
  long i;
  int bad = 0;
  for (i = 0; i < 64 * 4; i++) base[i] = (float)(i % 31) * 0.01f;
  h = seekdb_cuvs_build(base, 64, 4);
  if (h == NULL) return 1;
  if (seekdb_cuvs_search(h, q, 1, topk, ids, dist) != 0) {
    fprintf(stderr, "  FAIL search topk=%ld\n", topk);
    bad++;
  } else if (fake_last_itopk() != want_itopk || fake_last_width() != want_width) {
    fprintf(stderr, "  FAIL topk=%ld want itopk=%ld/width=%ld got itopk=%ld/width=%ld\n",
            topk, want_itopk, want_width, fake_last_itopk(), fake_last_width());
    bad++;
  }
  seekdb_cuvs_free(h);
  return bad;
}

int main(void) {
  int rc = 0;
  printf("== build parameter policy\n");
  rc |= check_tiers();
  printf("== query effort policy\n");
  rc |= check_query_effort(10, 512, 4);   /* the reported-QPS case */
  rc |= check_query_effort(100, 512, 4);  /* recall@100 case */
  rc |= check_query_effort(200, 800, 4);  /* 4x topk once topk is large */
  printf("== serialise round trip\n");
  rc |= check_serialize();
  return rc;
}
