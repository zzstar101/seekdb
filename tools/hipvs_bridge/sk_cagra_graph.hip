// GPU-resident CAGRA graph construction for the seekdb hipVS bridge.
//
// Pipeline (all on the GPU, on the cuVS resource stream):
//   1. exact kNN graph: tiled fp16-WMMA distance blocks + threshold top-k select
//   2. CAGRA rank-based 2-hop detour pruning   (same rule as cuVS graph::optimize)
//   3. reverse-edge graph + CAGRA edge merge   (same rule as cuVS graph::optimize)
// The result is a device uint32 [n x out_degree] CAGRA graph that is handed to
// cuvsCagraIndexFromArgs together with the device dataset.
//
// cuVS' optimize() runs the 2-hop count on the GPU but does the edge selection,
// the reverse-graph gather, the merge and two O(n*deg^2) validation passes on the
// host; at 250K x 256 that host work (plus NN-descent's host-side list updates)
// dominates the build. Here every step is a GPU kernel.
#include <hip/hip_runtime.h>
#include <cuvs/core/c_api.h>
#include <dlpack/dlpack.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sk_cagra_graph.h"

namespace {

#define SK_HIP_OK(x)                                                     \
  do {                                                                   \
    hipError_t e__ = (x);                                                \
    if (e__ != hipSuccess) {                                             \
      fprintf(stderr, "[sk_cagra_graph] %s: %s\n", #x, hipGetErrorString(e__)); \
      return -1;                                                         \
    }                                                                    \
  } while (0)
#define SK_CUVS_OK(x)                                                    \
  do {                                                                   \
    if ((x) != CUVS_SUCCESS) {                                           \
      fprintf(stderr, "[sk_cagra_graph] %s: %s\n", #x, cuvsGetLastErrorText()); \
      rc = -1;                                                           \
      goto done;                                                         \
    }                                                                    \
  } while (0)

void mk_tensor(DLManagedTensor* t, void* p, int code, int bits, int64_t* shape)
{
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data               = p;
  t->dl_tensor.device.device_type = kDLCUDA;
  t->dl_tensor.device.device_id   = 0;
  t->dl_tensor.ndim               = 2;
  t->dl_tensor.dtype.code         = code;
  t->dl_tensor.dtype.bits         = bits;
  t->dl_tensor.dtype.lanes        = 1;
  t->dl_tensor.shape              = shape;
}


// 2-hop detour counting with the cuVS rule: for edge A->B at rank kAB, count the
// neighbors D of A with rank kAD < kAB whose kNN list contains B. A's list is held
// in an LDS open-addressing table (node id -> rank) so every probe is O(1)
// instead of cuVS' linear scan of A's list.
template <int MAX_K, int HASH>
__global__ void __launch_bounds__(256) cagra_prune_kernel(const uint32_t* __restrict__ knn,
                                                          uint32_t n,
                                                          int k,
                                                          int out_degree,
                                                          uint32_t* __restrict__ pruned)
{
  __shared__ uint32_t s_list[MAX_K];
  __shared__ uint32_t s_cnt[MAX_K];
  __shared__ uint32_t s_key[HASH];
  __shared__ uint16_t s_val[HASH];

  const uint32_t a = blockIdx.x;
  if (a >= n) return;
  const uint32_t* la = knn + (uint64_t)a * k;
  for (int i = threadIdx.x; i < HASH; i += blockDim.x) s_key[i] = 0xffffffffu;
  for (int i = threadIdx.x; i < k; i += blockDim.x) {
    s_list[i] = la[i];
    s_cnt[i]  = 0;
  }
  __syncthreads();
  for (int i = threadIdx.x; i < k; i += blockDim.x) {
    const uint32_t v = s_list[i];
    if (v >= n) continue;
    uint32_t h = (v * 2654435761u) & (HASH - 1);
    while (true) {
      const uint32_t prev = atomicCAS(&s_key[h], 0xffffffffu, v);
      if (prev == 0xffffffffu) {
        s_val[h] = (uint16_t)i;
        break;
      }
      if (prev == v) break;  // duplicated id (kNN lists are duplicate-free in practice)
      h = (h + 1) & (HASH - 1);
    }
  }
  __syncthreads();

  // (kAD, kDB) pairs flattened over the block
  const int total = (k - 1) * k;
  for (int t = threadIdx.x; t < total; t += blockDim.x) {
    const int kad    = t / k;
    const int kdb    = t - kad * k;
    const uint32_t d = s_list[kad];
    if (d >= n) continue;
    const uint32_t b = knn[(uint64_t)d * k + kdb];
    if (b >= n) continue;
    uint32_t h = (b * 2654435761u) & (HASH - 1);
    while (true) {
      const uint32_t key = s_key[h];
      if (key == b) {
        const int kab = s_val[h];
        if (kab > kad) atomicAdd(&s_cnt[kab], 1u);
        break;
      }
      if (key == 0xffffffffu) break;
      h = (h + 1) & (HASH - 1);
    }
  }
  __syncthreads();

  // stable selection of the out_degree edges with the smallest (detour, rank)
  for (int i = threadIdx.x; i < k; i += blockDim.x) {
    const uint32_t v = s_list[i];
    if (v >= n || v == a) continue;
    const uint32_t ci = min(s_cnt[i], 255u);
    int pos           = 0;
    for (int j = 0; j < k; j++) {
      const uint32_t vj = s_list[j];
      if (vj >= n || vj == a) continue;
      const uint32_t cj = min(s_cnt[j], 255u);
      pos += (cj < ci || (cj == ci && j < i)) ? 1 : 0;
    }
    if (pos < out_degree) pruned[(uint64_t)a * out_degree + pos] = v;
  }
}

__global__ void cagra_rev_column_kernel(const uint32_t* __restrict__ pruned,
                                        uint32_t* __restrict__ rev,
                                        uint32_t* __restrict__ rev_count,
                                        uint32_t n,
                                        int degree,
                                        int col)
{
  const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const uint32_t d = pruned[(uint64_t)i * degree + col];
  if (d >= n) return;
  const uint32_t pos = atomicAdd(rev_count + d, 1u);
  if (pos < (uint32_t)degree) rev[(uint64_t)d * degree + pos] = i;
}

// cuVS merge rule (no MST): keep the first degree/2 pruned edges, then insert the
// reverse edges (last first) right after the protected prefix, shifting the rest.
template <int MAX_DEG>
__global__ void __launch_bounds__(MAX_DEG) cagra_merge_kernel(uint32_t* __restrict__ graph,
                                                              const uint32_t* __restrict__ rev,
                                                              const uint32_t* __restrict__ rev_count,
                                                              uint32_t n,
                                                              int degree)
{
  __shared__ uint32_t s_out[MAX_DEG];
  __shared__ int s_pos;
  const uint32_t i = blockIdx.x;
  if (i >= n) return;
  const int t = threadIdx.x;
  if (t < degree) s_out[t] = graph[(uint64_t)i * degree + t];
  const int prot = degree / 2;
  int kr         = min((int)rev_count[i], degree);
  __syncthreads();
  while (kr > 0) {
    kr -= 1;
    const uint32_t v = rev[(uint64_t)i * degree + kr];
    if (v >= n) continue;
    if (t == 0) s_pos = degree;
    __syncthreads();
    if (t < degree && s_out[t] == v) atomicMin(&s_pos, t);
    __syncthreads();
    const int pos = s_pos;
    if (pos < prot) {
      __syncthreads();
      continue;
    }
    int num_shift = pos - prot;
    if (pos >= degree) num_shift = degree - prot - 1;
    // shift s_out[prot .. prot+num_shift-1] right by one
    uint32_t moved = 0;
    const bool mover = (t >= prot && t < prot + num_shift);
    if (mover) moved = s_out[t];
    __syncthreads();
    if (mover) s_out[t + 1] = moved;
    if (t == prot) s_out[prot] = v;
    __syncthreads();
  }
  if (t < degree) graph[(uint64_t)i * degree + t] = s_out[t];
}

}  // namespace

// ---------------------------------------------------------------------------
// Exact kNN: rocBLAS GEMM tile (-2 q.b) + per-row radix select of the k
// smallest |b|^2 - 2 q.b (|q|^2 is constant per row and does not change ranks).
// ---------------------------------------------------------------------------
namespace {

__global__ void cagra_row_norm_kernel(const float* __restrict__ x, int64_t n, int dim,
                                      float* __restrict__ out)
{
  const int64_t r = (int64_t)blockIdx.x * (blockDim.x / 32) + threadIdx.x / 32;
  const int lane  = threadIdx.x & 31;
  if (r >= n) return;
  const float* p = x + r * dim;
  float s        = 0.f;
  for (int j = lane; j < dim; j += 32) s += p[j] * p[j];
  for (int o = 16; o > 0; o >>= 1) s += __shfl_xor(s, o, 32);
  if (lane == 0) out[r] = s;
}

__device__ __forceinline__ uint32_t f2key(float f)
{
  uint32_t b = __float_as_uint(f);
  return (b & 0x80000000u) ? ~b : (b | 0x80000000u);
}

}  // namespace

// ---------------------------------------------------------------------------
// Exact kNN graph (all on the GPU, fp16 WMMA dot products with fp32 accumulate).
//
// The dataset is staged to the device once as a padded fp16 copy with a
// per-row power-of-two scale (exactly undone in the epilogue) plus fp32 row norms.
// For every block of query rows:
//   1. distances to a fixed stratified sample of S dataset rows give, per query,
//      the (k+1)-th smallest sample distance T. The sample is a subset of the
//      dataset, so at least k non-self points lie within T.
//   2. one pass of the distance kernel over the whole dataset appends only the
//      candidates with distance <= T (expected ~k*n/S per row) to a per-row list.
//   3. the exact top-k of each candidate list is selected and sorted.
// Rows whose candidate list overflows (or, defensively, holds fewer than k
// entries) are recomputed with the full distance row and an exact radix select,
// so the result is always the exact kNN under the fp16-product metric.
// The n x n distance matrix is never materialised.
namespace {

typedef _Float16 sk_half8 __attribute__((ext_vector_type(8)));
typedef _Float16 sk_half16 __attribute__((ext_vector_type(16)));
typedef float sk_float8 __attribute__((ext_vector_type(8)));

constexpr int kBQ      = 128;       // query rows per block
constexpr int kBD      = 256;       // dataset rows per block
constexpr int kBK      = 32;        // K step staged in LDS
constexpr int kLP      = kBK + 8;   // padded LDS row (halves)
constexpr int kChunk   = 64;        // columns per chunk minimum (dense path)
constexpr int kSelCap  = 2048;      // shared candidate buffer of the dense select
constexpr int kLocCap  = 32;        // per-row staging slots per block (sparse path)

__device__ __forceinline__ uint32_t key_of(float f) { return f2key(f); }

// fp32 rows -> padded fp16 rows scaled by 2^e_i (max |x| ~ 2^14), fp32 norm, 2^-e_i
__global__ void cagra_to_half_kernel(const float* __restrict__ x, int64_t rows, int dim, int dp,
                                     _Float16* __restrict__ xh, float* __restrict__ norm,
                                     float* __restrict__ inv)
{
  const int64_t r = (int64_t)blockIdx.x * (blockDim.x / 32) + threadIdx.x / 32;
  const int lane  = threadIdx.x & 31;
  if (r >= rows) return;
  const float* p = x + r * dim;
  float s = 0.f;
  uint32_t mb = 0;
  for (int j = lane; j < dim; j += 32) {
    const float v = p[j];
    s += v * v;
    mb = max(mb, __float_as_uint(v) & 0x7fffffffu);
  }
  for (int o = 16; o > 0; o >>= 1) {
    s += __shfl_xor(s, o, 32);
    mb = max(mb, (uint32_t)__shfl_xor((int)mb, o, 32));
  }
  int sh = 0;
  if (mb != 0 && mb < 0x7f800000u) {
    int e = 0;
    (void)frexpf(__uint_as_float(mb), &e);
    sh = 14 - e;
    sh = sh > 60 ? 60 : (sh < -60 ? -60 : sh);
  }
  const float sc = ldexpf(1.f, sh);
  _Float16* q = xh + r * dp;
  for (int j = lane; j < dp; j += 32) q[j] = (j < dim) ? (_Float16)(p[j] * sc) : (_Float16)0.f;
  if (lane == 0) {
    norm[r] = s;
    inv[r]  = ldexpf(1.f, -sh);
  }
}

__global__ void cagra_gather_rows_kernel(const _Float16* __restrict__ xh, const float* __restrict__ norm,
                                         const float* __restrict__ inv, int dp, int64_t n, int64_t S,
                                         _Float16* __restrict__ sh, float* __restrict__ snorm,
                                         float* __restrict__ sinv)
{
  const int64_t s = (int64_t)blockIdx.x * (blockDim.x / 32) + threadIdx.x / 32;
  const int lane  = threadIdx.x & 31;
  if (s >= S) return;
  // stratified sample: one pseudo-random row out of every n/S consecutive rows
  const int64_t lo = s * n / S, hi = (s + 1) * n / S;
  const uint64_t h = (uint64_t)(s + 1) * 0x9E3779B97F4A7C15ull;
  const int64_t src = lo + (int64_t)((h >> 33) % (uint64_t)(hi - lo));
  const uint4* a = (const uint4*)(xh + src * dp);
  uint4* b       = (uint4*)(sh + s * dp);
  for (int j = lane; j < dp / 8; j += 32) b[j] = a[j];
  if (lane == 0) {
    snorm[s] = norm[src];
    sinv[s]  = inv[src];
  }
}

// MODE 0: write D[r*ldd + c] (self = +inf) and optional 64-column chunk minima.
// MODE 1: append (key, column) of every distance with key <= thr[r] to cand[r*cap ..].
// Query row r of the block maps to dataset row qidx ? qidx[q0 + r] : q0 + r.
template <int MODE>
__global__ void __launch_bounds__(512) cagra_dist_kernel(
  const _Float16* __restrict__ xq, const float* __restrict__ qinv, const uint32_t* __restrict__ qidx,
  int64_t q0, int m, const _Float16* __restrict__ xd, const float* __restrict__ dnorm,
  const float* __restrict__ dinv, int64_t ncol, int dp, int self_excl,
  float* __restrict__ D, int64_t ldd, float* __restrict__ cmin, int64_t nchunk,
  const uint32_t* __restrict__ thr, uint32_t* __restrict__ cnt, uint2* __restrict__ cand, int cap)
{
  __shared__ __attribute__((aligned(16))) _Float16 sA[2][kBQ * kLP];
  __shared__ __attribute__((aligned(16))) _Float16 sB[2][kBD * kLP];
  __shared__ uint32_t s_cnt[kBQ];
  __shared__ uint32_t s_base[kBQ];
  const int tid  = threadIdx.x;
  const int wave = tid >> 5;
  const int lane = tid & 31;
  const int lr   = lane & 15;
  const int hi   = lane >> 4;
  const int64_t col0 = (int64_t)blockIdx.x * kBD;
  const int row0     = blockIdx.y * kBQ;
  const int wq = wave >> 2;  // 0..3 : 32 query rows each
  const int wd = wave & 3;   // 0..3 : 64 dataset rows each
  if (MODE == 1 && tid < kBQ) s_cnt[tid] = 0;

  // global -> register staging: one 16B piece of A, two of B per thread
  const int ar = tid >> 2, ap = (tid & 3) * 8;
  const bool aok = row0 + ar < m;
  const int64_t ag = aok ? (qidx ? (int64_t)qidx[q0 + row0 + ar] : q0 + row0 + ar) : 0;
  const _Float16* asrc = xq + ag * dp + ap;
  const int br0 = tid >> 2, br1 = (tid >> 2) + 128, bp = (tid & 3) * 8;
  const bool b0ok = col0 + br0 < ncol, b1ok = col0 + br1 < ncol;
  const _Float16* b0src = xd + (b0ok ? col0 + br0 : 0) * dp + bp;
  const _Float16* b1src = xd + (b1ok ? col0 + br1 : 0) * dp + bp;
  const uint4 z = make_uint4(0, 0, 0, 0);
  uint4 ra = aok ? *(const uint4*)asrc : z;
  uint4 rb0 = b0ok ? *(const uint4*)b0src : z;
  uint4 rb1 = b1ok ? *(const uint4*)b1src : z;
  *(uint4*)(&sA[0][ar * kLP + ap])  = ra;
  *(uint4*)(&sB[0][br0 * kLP + bp]) = rb0;
  *(uint4*)(&sB[0][br1 * kLP + bp]) = rb1;
  __syncthreads();

  sk_float8 acc[2][4];
#pragma unroll
  for (int i = 0; i < 2; i++)
#pragma unroll
    for (int j = 0; j < 4; j++)
#pragma unroll
      for (int v = 0; v < 8; v++) acc[i][j][v] = 0.f;

  const int nk = dp / kBK;
  for (int kt = 0; kt < nk; kt++) {
    const int cur = kt & 1;
    const bool more = kt + 1 < nk;
    if (more) {
      const int ko = (kt + 1) * kBK;
      ra  = aok ? *(const uint4*)(asrc + ko) : z;
      rb0 = b0ok ? *(const uint4*)(b0src + ko) : z;
      rb1 = b1ok ? *(const uint4*)(b1src + ko) : z;
    }
#pragma unroll
    for (int ks = 0; ks < kBK; ks += 16) {
      sk_half16 a[2], b[4];
#pragma unroll
      for (int i = 0; i < 2; i++) {
        const _Float16* p = &sA[cur][(wq * 32 + i * 16 + lr) * kLP + ks];
        const sk_half8 lo = *(const sk_half8*)p;
        const sk_half8 h8 = *(const sk_half8*)(p + 8);
        a[i] = __builtin_shufflevector(lo, h8, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      }
#pragma unroll
      for (int j = 0; j < 4; j++) {
        const _Float16* p = &sB[cur][(wd * 64 + j * 16 + lr) * kLP + ks];
        const sk_half8 lo = *(const sk_half8*)p;
        const sk_half8 h8 = *(const sk_half8*)(p + 8);
        b[j] = __builtin_shufflevector(lo, h8, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
      }
#pragma unroll
      for (int i = 0; i < 2; i++)
#pragma unroll
        for (int j = 0; j < 4; j++)
          acc[i][j] = __builtin_amdgcn_wmma_f32_16x16x16_f16_w32(a[i], b[j], acc[i][j]);
    }
    if (more) {
      *(uint4*)(&sA[cur ^ 1][ar * kLP + ap])  = ra;
      *(uint4*)(&sB[cur ^ 1][br0 * kLP + bp]) = rb0;
      *(uint4*)(&sB[cur ^ 1][br1 * kLP + bp]) = rb1;
    }
    __syncthreads();
  }

  // epilogue: acc[i][j][v] = <q(row), x(col)> * 2^(eq+ex) with
  //   row = row0 + wq*32 + i*16 + 2v + hi,  col = col0 + wd*64 + j*16 + lr
  const float INF = __int_as_float(0x7f800000);
  const int64_t cbase = col0 + wd * 64;
  float cn[4], ci[4];
  bool cok[4];
#pragma unroll
  for (int j = 0; j < 4; j++) {
    const int64_t col = cbase + j * 16 + lr;
    cok[j] = col < ncol;
    cn[j]  = cok[j] ? dnorm[col] : 0.f;
    ci[j]  = cok[j] ? dinv[col] : 0.f;
  }
  uint2* stage = (uint2*)&sB[0][0];  // reused after the main loop (last iteration synced)
#pragma unroll
  for (int i = 0; i < 2; i++) {
    float mn[8];
#pragma unroll
    for (int v = 0; v < 8; v++) mn[v] = INF;
#pragma unroll
    for (int v = 0; v < 8; v++) {
      const int rl  = wq * 32 + i * 16 + 2 * v + hi;  // row inside the block
      const int r   = row0 + rl;
      const bool rok = r < m;
      int64_t self = -1;
      float qi = 0.f;
      uint32_t t = 0;
      if (rok) {
        if (self_excl) self = qidx ? (int64_t)qidx[q0 + r] : q0 + r;
        qi = qinv[qidx ? (int64_t)qidx[q0 + r] : q0 + r];
        if (MODE == 1) t = thr[r];
      }
#pragma unroll
      for (int j = 0; j < 4; j++) {
        const int64_t col = cbase + j * 16 + lr;
        float val = __fmaf_rn(acc[i][j][v], -2.f * (qi * ci[j]), cn[j]);
        if (!cok[j] || col == self || !rok) val = INF;
        if (MODE == 0) {
          if (cok[j] && rok) D[(int64_t)r * ldd + col] = val;
          mn[v] = fminf(mn[v], val);
        } else {
          const uint32_t key = key_of(val);
          if (rok && cok[j] && key <= t) {
            const uint32_t p = atomicAdd(&s_cnt[rl], 1u);
            if (p < (uint32_t)kLocCap) {
              stage[rl * kLocCap + p] = make_uint2(key, (uint32_t)col);
            } else {
              const uint32_t g = atomicAdd(&cnt[r], 1u);
              if (g < (uint32_t)cap) cand[(int64_t)r * cap + g] = make_uint2(key, (uint32_t)col);
            }
          }
        }
      }
    }
    if (MODE == 0 && cmin != nullptr) {
#pragma unroll
      for (int v = 0; v < 8; v++)
        for (int o = 8; o > 0; o >>= 1) mn[v] = fminf(mn[v], __shfl_xor(mn[v], o, 32));
      if (lr == 0 && cbase < ncol) {
        const int64_t c = cbase / kChunk;
#pragma unroll
        for (int v = 0; v < 8; v++) {
          const int r = row0 + wq * 32 + i * 16 + 2 * v + hi;
          if (r < m) cmin[(int64_t)r * nchunk + c] = mn[v];
        }
      }
    }
  }
  if (MODE == 1) {
    __syncthreads();
    if (tid < kBQ) {
      const uint32_t c = min(s_cnt[tid], (uint32_t)kLocCap);
      s_base[tid] = (c > 0 && row0 + tid < m) ? atomicAdd(&cnt[row0 + tid], c) : 0u;
    }
    __syncthreads();
    for (int e = tid; e < kBQ * kLocCap; e += 512) {
      const int rl = e / kLocCap, ii = e % kLocCap;
      if (row0 + rl < m && ii < (int)min(s_cnt[rl], (uint32_t)kLocCap)) {
        const uint32_t g = s_base[rl] + ii;
        if (g < (uint32_t)cap) cand[(int64_t)(row0 + rl) * cap + g] = stage[e];
      }
    }
  }
}

// k-th smallest (1-based `need`) key among get(0..len) by a 3-level radix select;
// returns that key in *prefix_out and in *rem_out how many keys equal to it to take.
template <typename F>
__device__ void block_kth_key(F get, int64_t len, uint32_t need, uint32_t* s_hist,
                              uint32_t* s_prefix, uint32_t* s_need, uint32_t* prefix_out,
                              uint32_t* rem_out)
{
  const int tid   = threadIdx.x;
  uint32_t prefix = 0;
  const int shifts[3] = {20, 8, 0};
  const int widths[3] = {12, 12, 8};
  for (int lvl = 0; lvl < 3; lvl++) {
    const int sh = shifts[lvl];
    const int nb = 1 << widths[lvl];
    const uint32_t hi_mask = (lvl == 0) ? 0u : (0xffffffffu << (sh + widths[lvl]));
    for (int i = tid; i < nb; i += blockDim.x) s_hist[i] = 0;
    __syncthreads();
    for (int64_t j = tid; j < len; j += blockDim.x) {
      const uint32_t key = get(j);
      if ((key & hi_mask) != prefix) continue;
      atomicAdd(&s_hist[(key >> sh) & (nb - 1)], 1u);
    }
    __syncthreads();
    if (tid == 0) {
      uint32_t acc = 0;
      int b        = 0;
      for (; b < nb; b++) {
        if (acc + s_hist[b] >= need) break;
        acc += s_hist[b];
      }
      if (b >= nb) b = nb - 1;
      *s_prefix = prefix | ((uint32_t)b << sh);
      *s_need   = need - acc;
    }
    __syncthreads();
    prefix = *s_prefix;
    need   = *s_need;
    __syncthreads();
  }
  *prefix_out = prefix;
  *rem_out    = need;
}

__device__ void block_sort_pairs(uint32_t* s_key, uint32_t* s_id, int size)
{
  for (int sz = 2; sz <= size; sz <<= 1) {
    for (int stride = sz >> 1; stride > 0; stride >>= 1) {
      for (int i = threadIdx.x; i < size; i += blockDim.x) {
        const int partner = i ^ stride;
        if (partner > i) {
          const bool up     = ((i & sz) == 0);
          const uint32_t ka = s_key[i], kb = s_key[partner];
          const uint32_t ia = s_id[i], ib = s_id[partner];
          const bool gt     = (ka > kb) || (ka == kb && ia > ib);
          if (gt == up) {
            s_key[i] = kb; s_key[partner] = ka;
            s_id[i]  = ib; s_id[partner]  = ia;
          }
        }
      }
      __syncthreads();
    }
  }
}

// collect exactly k entries (keys < P, then `need` keys == P), sort, write ids
template <typename F>
__device__ void block_collect_sort_write(F get_key, F get_id, int64_t len, int k, uint32_t P,
                                         uint32_t need, uint32_t* s_key, uint32_t* s_id,
                                         uint32_t* s_c1, uint32_t* s_c2, uint32_t* o)
{
  const int tid = threadIdx.x;
  if (tid == 0) { *s_c1 = 0; *s_c2 = 0; }
  __syncthreads();
  const uint32_t n_lt = (uint32_t)k - need;
  for (int64_t j = tid; j < len; j += blockDim.x) {
    const uint32_t key = get_key(j);
    if (key < P) {
      const uint32_t p = atomicAdd(s_c1, 1u);
      if (p < n_lt) { s_key[p] = key; s_id[p] = get_id(j); }
    } else if (key == P) {
      const uint32_t p = atomicAdd(s_c2, 1u);
      if (p < need) { s_key[n_lt + p] = key; s_id[n_lt + p] = get_id(j); }
    }
  }
  __syncthreads();
  int size = 1;
  while (size < k) size <<= 1;
  const uint32_t filled = min((uint32_t)k, min(*s_c1, n_lt) + min(*s_c2, need));
  for (int i = tid; i < size; i += blockDim.x)
    if (i >= (int)filled) { s_key[i] = 0xffffffffu; s_id[i] = 0xffffffffu; }
  __syncthreads();
  block_sort_pairs(s_key, s_id, size);
  for (int i = tid; i < k; i += blockDim.x) o[i] = s_id[i];
}

// per row: thr = (k+1)-th smallest distance key to the sample
__global__ void __launch_bounds__(256) cagra_sample_thr_kernel(const float* __restrict__ Ds, int64_t S,
                                                               int k, uint32_t* __restrict__ thr)
{
  __shared__ uint32_t s_hist[4096];
  __shared__ uint32_t s_prefix, s_need;
  const float* d = Ds + (int64_t)blockIdx.x * S;
  uint32_t P, rem;
  block_kth_key([d](int64_t j) { return key_of(d[j]); }, S, (uint32_t)(k + 1), s_hist, &s_prefix,
                &s_need, &P, &rem);
  if (threadIdx.x == 0) thr[blockIdx.x] = P;
}

// per row of a sparse tile: exact top-k of the candidate list, or defer the row
__global__ void __launch_bounds__(256) cagra_cand_select_kernel(const uint2* __restrict__ cand,
                                                                const uint32_t* __restrict__ cnt,
                                                                int cap, int64_t q0, int k,
                                                                uint32_t* __restrict__ out,
                                                                uint32_t* __restrict__ fb,
                                                                uint32_t* __restrict__ fb_cnt)
{
  __shared__ uint32_t s_hist[4096];
  __shared__ uint32_t s_key[256];
  __shared__ uint32_t s_id[256];
  __shared__ uint32_t s_prefix, s_need, s_c1, s_c2;
  const int64_t r = blockIdx.x;
  const uint32_t c = cnt[r];
  if (c < (uint32_t)k || c > (uint32_t)cap) {
    if (threadIdx.x == 0) fb[atomicAdd(fb_cnt, 1u)] = (uint32_t)(q0 + r);
    return;
  }
  const uint2* cr = cand + r * (int64_t)cap;
  uint32_t P, need;
  struct Sel { const uint2* p; int w; __device__ uint32_t operator()(int64_t j) const { return w ? p[j].y : p[j].x; } };
  block_kth_key(Sel{cr, 0}, (int64_t)c, (uint32_t)k, s_hist, &s_prefix, &s_need, &P, &need);
  block_collect_sort_write(Sel{cr, 0}, Sel{cr, 1}, (int64_t)c, k, P, need, s_key, s_id, &s_c1, &s_c2,
                           out + (q0 + r) * (int64_t)k);
}

// dense path: full distance rows (+chunk minima) for the rows listed in qidx[q0..q0+m)
__global__ void __launch_bounds__(256) cagra_row_select_dense_kernel(const float* __restrict__ D,
                                                                     const float* __restrict__ cmin,
                                                                     int64_t n, int64_t nchunk,
                                                                     const uint32_t* __restrict__ qidx,
                                                                     int64_t q0, int k,
                                                                     uint32_t* __restrict__ out)
{
  __shared__ uint32_t s_hist[4096];
  __shared__ uint32_t s_key[kSelCap];
  __shared__ uint32_t s_id[kSelCap];
  __shared__ uint32_t s_prefix, s_need, s_cnt, s_c2;
  const int tid     = threadIdx.x;
  const int64_t row = blockIdx.x;
  const int64_t orow = qidx ? (int64_t)qidx[q0 + row] : q0 + row;
  const float* __restrict__ d = D + row * n;
  uint32_t* o = out + orow * (int64_t)k;
  uint32_t T = 0xffffffffu, rem = 0;
  if (nchunk >= k) {
    const float* c = cmin + row * nchunk;
    block_kth_key([c](int64_t j) { return key_of(c[j]); }, nchunk, (uint32_t)k, s_hist, &s_prefix,
                  &s_need, &T, &rem);
  }
  if (tid == 0) s_cnt = 0;
  __syncthreads();
  for (int64_t j = tid; j < n; j += 256) {
    const uint32_t key = key_of(d[j]);
    if (key <= T) {
      const uint32_t p = atomicAdd(&s_cnt, 1u);
      if (p < (uint32_t)kSelCap) { s_key[p] = key; s_id[p] = (uint32_t)j; }
    }
  }
  __syncthreads();
  const uint32_t cnt = s_cnt;
  if (cnt <= (uint32_t)kSelCap && cnt >= (uint32_t)k) {
    int size = 1;
    while (size < (int)cnt) size <<= 1;
    for (int i = (int)cnt + tid; i < size; i += 256) { s_key[i] = 0xffffffffu; s_id[i] = 0xffffffffu; }
    __syncthreads();
    block_sort_pairs(s_key, s_id, size);
    for (int i = tid; i < k; i += 256) o[i] = s_id[i];
    return;
  }
  struct DSel { const float* p; int w; __device__ uint32_t operator()(int64_t j) const { return w ? (uint32_t)j : key_of(p[j]); } };
  uint32_t P, need;
  block_kth_key(DSel{d, 0}, n, (uint32_t)k, s_hist, &s_prefix, &s_need, &P, &need);
  block_collect_sort_write(DSel{d, 0}, DSel{d, 1}, n, k, P, need, s_key, s_id, &s_cnt, &s_c2, o);
}

}  // namespace

extern "C" int sk_cagra_knn_build(cuvsResources_t res, const float* h_base, int64_t n, int64_t dim,
                                  int k, uint32_t* d_knn)
{
  hipStream_t stream;
  if (cuvsStreamGet(res, &stream) != CUVS_SUCCESS) return -1;
  if (k <= 0 || k > 256 || n <= k || dim <= 0 || n >= 0x7fffffffLL) return -1;
  const int dp = (int)(((dim + kBK - 1) / kBK) * kBK);
  const int64_t S_cfg = 16384;
  const bool sparse = n >= 4 * S_cfg;     // small sets use dense rows for everything
  const int64_t S   = sparse ? S_cfg : 0;
  const int64_t R   = 512;                // query rows per sparse tile
  const int cap     = 8192;               // candidate list capacity per row
  const int64_t FB  = 64;                 // rows per dense batch
  const int64_t nchunk = (n + kChunk - 1) / kChunk;
  const int64_t stage_rows = ((int64_t)(32 << 20) / ((int64_t)sizeof(float) * dim)) > 0
                               ? ((int64_t)(32 << 20) / ((int64_t)sizeof(float) * dim)) : 1;

  struct Buf { void* p; size_t b; };
  Buf bufs[16];
  int nb = 0;
  int rc = 0;
  auto alloc = [&](void** p, size_t b) -> bool {
    if (cuvsRMMAlloc(res, p, b) != CUVS_SUCCESS) { *p = nullptr; return false; }
    bufs[nb++] = Buf{*p, b};
    return true;
  };
  _Float16 *xh = nullptr, *sh = nullptr;
  float *norm = nullptr, *inv = nullptr, *snorm = nullptr, *sinv = nullptr, *stage = nullptr;
  float *Ds = nullptr, *Dd = nullptr, *cm = nullptr;
  uint32_t *thr = nullptr, *cnt = nullptr, *fb = nullptr, *fb_cnt = nullptr, *all_idx = nullptr;
  uint2* cand = nullptr;
  uint32_t h_fb = 0;
  const bool noselect = getenv("SK_KNN_NOSELECT") != nullptr;

  if (!alloc((void**)&xh, sizeof(_Float16) * n * dp) || !alloc((void**)&norm, sizeof(float) * n) ||
      !alloc((void**)&inv, sizeof(float) * n) || !alloc((void**)&stage, sizeof(float) * stage_rows * dim) ||
      !alloc((void**)&Dd, sizeof(float) * FB * n) || !alloc((void**)&cm, sizeof(float) * FB * nchunk) ||
      !alloc((void**)&fb, sizeof(uint32_t) * n) || !alloc((void**)&fb_cnt, sizeof(uint32_t))) {
    rc = -1;
    goto out;
  }
  // stage the dataset: fp32 host -> device chunk -> fp16 rows + norms + scales
  for (int64_t r0 = 0; r0 < n; r0 += stage_rows) {
    const int64_t rows = (n - r0 < stage_rows) ? (n - r0) : stage_rows;
    if (hipMemcpyAsync(stage, h_base + r0 * dim, sizeof(float) * rows * dim, hipMemcpyHostToDevice, stream) !=
        hipSuccess) { rc = -1; goto out; }
    hipLaunchKernelGGL(cagra_to_half_kernel, dim3((unsigned)((rows + 7) / 8)), dim3(256), 0, stream, stage,
                       rows, (int)dim, dp, xh + r0 * dp, norm + r0, inv + r0);
  }
  if (hipMemsetAsync(fb_cnt, 0, sizeof(uint32_t), stream) != hipSuccess) { rc = -1; goto out; }
  if (sparse) {
    if (!alloc((void**)&sh, sizeof(_Float16) * S * dp) || !alloc((void**)&snorm, sizeof(float) * S) ||
        !alloc((void**)&sinv, sizeof(float) * S) || !alloc((void**)&Ds, sizeof(float) * R * S) ||
        !alloc((void**)&thr, sizeof(uint32_t) * R) || !alloc((void**)&cnt, sizeof(uint32_t) * R) ||
        !alloc((void**)&cand, sizeof(uint2) * R * cap)) {
      rc = -1;
      goto out;
    }
    hipLaunchKernelGGL(cagra_gather_rows_kernel, dim3((unsigned)((S + 7) / 8)), dim3(256), 0, stream, xh, norm,
                       inv, dp, n, S, sh, snorm, sinv);
    for (int64_t q0 = 0; q0 < n; q0 += R) {
      const int m = (int)((n - q0 < R) ? (n - q0) : R);
      dim3 gs((unsigned)((S + kBD - 1) / kBD), (unsigned)((m + kBQ - 1) / kBQ));
      hipLaunchKernelGGL((cagra_dist_kernel<0>), gs, dim3(512), 0, stream, xh, inv, (const uint32_t*)nullptr, q0, m,
                         sh, snorm, sinv, S, dp, 0, Ds, S, (float*)nullptr, (int64_t)0, (const uint32_t*)nullptr,
                         (uint32_t*)nullptr, (uint2*)nullptr, 0);
      hipLaunchKernelGGL(cagra_sample_thr_kernel, dim3((unsigned)m), dim3(256), 0, stream, (const float*)Ds, S, k,
                         thr);
      (void)hipMemsetAsync(cnt, 0, sizeof(uint32_t) * m, stream);
      dim3 g((unsigned)((n + kBD - 1) / kBD), (unsigned)((m + kBQ - 1) / kBQ));
      hipLaunchKernelGGL((cagra_dist_kernel<1>), g, dim3(512), 0, stream, xh, inv, (const uint32_t*)nullptr, q0, m,
                         xh, norm, inv, n, dp, 1, (float*)nullptr, (int64_t)0, (float*)nullptr, (int64_t)0,
                         (const uint32_t*)thr, cnt, cand, cap);
      if (noselect) continue;
      hipLaunchKernelGGL(cagra_cand_select_kernel, dim3((unsigned)m), dim3(256), 0, stream, (const uint2*)cand,
                         (const uint32_t*)cnt, cap, q0, k, d_knn, fb, fb_cnt);
    }
    if (hipMemcpyAsync(&h_fb, fb_cnt, sizeof(uint32_t), hipMemcpyDeviceToHost, stream) != hipSuccess ||
        hipStreamSynchronize(stream) != hipSuccess) { rc = -1; goto out; }
  } else {
    // every row goes through the dense path
    if (!alloc((void**)&all_idx, sizeof(uint32_t) * n)) { rc = -1; goto out; }
    {
      uint32_t* hi = (uint32_t*)malloc(sizeof(uint32_t) * n);
      if (!hi) { rc = -1; goto out; }
      for (int64_t i = 0; i < n; i++) hi[i] = (uint32_t)i;
      hipError_t e = hipMemcpy(all_idx, hi, sizeof(uint32_t) * n, hipMemcpyHostToDevice);
      free(hi);
      if (e != hipSuccess) { rc = -1; goto out; }
    }
    if (hipMemcpyAsync(fb, all_idx, sizeof(uint32_t) * n, hipMemcpyDeviceToDevice, stream) != hipSuccess) {
      rc = -1;
      goto out;
    }
    h_fb = (uint32_t)n;
  }
  for (int64_t f0 = 0; f0 < (int64_t)h_fb; f0 += FB) {
    const int m = (int)(((int64_t)h_fb - f0 < FB) ? ((int64_t)h_fb - f0) : FB);
    dim3 g((unsigned)((n + kBD - 1) / kBD), (unsigned)((m + kBQ - 1) / kBQ));
    hipLaunchKernelGGL((cagra_dist_kernel<0>), g, dim3(512), 0, stream, xh, inv, (const uint32_t*)fb, f0, m, xh,
                       norm, inv, n, dp, 1, Dd, n, cm, nchunk, (const uint32_t*)nullptr, (uint32_t*)nullptr,
                       (uint2*)nullptr, 0);
    hipLaunchKernelGGL(cagra_row_select_dense_kernel, dim3((unsigned)m), dim3(256), 0, stream, (const float*)Dd,
                       (const float*)cm, n, nchunk, (const uint32_t*)fb, f0, k, d_knn);
  }
  if (hipGetLastError() != hipSuccess || hipStreamSynchronize(stream) != hipSuccess) rc = -1;
  if (getenv("SK_KNN_VERBOSE")) fprintf(stderr, "[sk_cagra_graph] knn n=%ld dense_rows=%u\n", (long)n, h_fb);
out:
  for (int i = nb - 1; i >= 0; i--) cuvsRMMFree(res, bufs[i].p, bufs[i].b);
  return rc;
}



namespace {
// Replace the last `nrand` slots of every row with pseudo-random long-range
// edges (distinct, not self, not already in the row). An exact kNN graph of
// clustered data has (almost) no inter-cluster edges, which leaves CAGRA's
// randomly seeded greedy search unable to reach other clusters; NN-descent
// graphs keep such edges from their random initialisation. A few random links
// per node restore global navigability at negligible local cost.
__global__ void cagra_random_edges_kernel(uint32_t* __restrict__ graph, uint32_t n, int deg, int nrand)
{
  const uint32_t v = blockIdx.x * blockDim.x + threadIdx.x;
  if (v >= n) return;
  uint32_t* row = graph + (int64_t)v * deg;
  uint64_t h = ((uint64_t)v + 1) * 0x9E3779B97F4A7C15ull;
  for (int s = deg - nrand; s < deg; s++) {
    for (int tries = 0; tries < 16; tries++) {
      h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 29;
      const uint32_t c = (uint32_t)((h >> 32) % n);
      if (c == v) continue;
      bool dup = false;
      for (int j = 0; j < deg && !dup; j++) dup = (j != s && row[j] == c);
      if (dup) continue;
      row[s] = c;
      break;
    }
  }
}
}  // namespace

extern "C" int sk_cagra_optimize(cuvsResources_t res,
                                 uint32_t* d_knn,
                                 int64_t n,
                                 int k,
                                 int out_degree,
                                 uint32_t* d_graph)
{
  hipStream_t stream;
  if (cuvsStreamGet(res, &stream) != CUVS_SUCCESS) return -1;
  if (k > 256 || out_degree > 256 || out_degree > k || n <= 0 || n >= 0xffffffffLL) return -1;
  SK_HIP_OK(hipMemsetAsync(d_graph, 0xff, sizeof(uint32_t) * n * out_degree, stream));
  hipLaunchKernelGGL((cagra_prune_kernel<256, 512>), dim3((unsigned)n), dim3(256), 0, stream,
                     d_knn, (uint32_t)n, k, out_degree, d_graph);
  SK_HIP_OK(hipGetLastError());
  // The kNN lists are dead once pruned: reuse their storage for the reverse graph
  // (n x out_degree) and, when it fits, the per-node reverse counters, so the
  // optimize phase needs no device memory beyond the kNN and output graphs.
  uint32_t* d_rev   = d_knn;
  uint32_t* d_count = nullptr;
  bool own_count = false;
  if (k > out_degree) {
    d_count = d_knn + n * (int64_t)out_degree;
  } else {
    if (cuvsRMMAlloc(res, (void**)&d_count, sizeof(uint32_t) * n) != CUVS_SUCCESS) return -1;
    own_count = true;
  }
  (void)hipMemsetAsync(d_rev, 0xff, sizeof(uint32_t) * n * out_degree, stream);
  (void)hipMemsetAsync(d_count, 0, sizeof(uint32_t) * n, stream);
  const unsigned blocks = (unsigned)((n + 255) / 256);
  for (int c = 0; c < out_degree; c++) {
    hipLaunchKernelGGL(cagra_rev_column_kernel, dim3(blocks), dim3(256), 0, stream,
                       (const uint32_t*)d_graph, d_rev, d_count, (uint32_t)n, out_degree, c);
  }
  hipLaunchKernelGGL((cagra_merge_kernel<256>), dim3((unsigned)n), dim3(out_degree <= 64 ? 64 : (out_degree <= 128 ? 128 : 256)),
                     0, stream, d_graph, (const uint32_t*)d_rev, (const uint32_t*)d_count, (uint32_t)n,
                     out_degree);
  {
    const char* ev = getenv("SK_CAGRA_RANDOM_EDGES");
    int nrand = ev ? atoi(ev) : out_degree / 16;  // 8 of 128
    if (nrand > out_degree / 4) nrand = out_degree / 4;
    if (nrand > 0 && n > out_degree + 1)
      hipLaunchKernelGGL(cagra_random_edges_kernel, dim3(blocks), dim3(256), 0, stream, d_graph, (uint32_t)n,
                         out_degree, nrand);
  }
  hipError_t e = hipStreamSynchronize(stream);
  if (own_count) cuvsRMMFree(res, d_count, sizeof(uint32_t) * n);
  if (e != hipSuccess) {
    fprintf(stderr, "[sk_cagra_graph] optimize: %s\n", hipGetErrorString(e));
    return -1;
  }
  return 0;
}
