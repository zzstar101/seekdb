#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success, after all transfers
 *    finish; nq > 0 and 0 < topk <= n are required. Output is unspecified on
 *    failure. Query/result workspace and default search params are reused by
 *    the handle. After a stream synchronization or workspace release failure
 *    the handle must be freed. A failed workspace release has unknown ownership:
 *    that address is never reused or released again; unreleased device memory
 *    can remain until process exit. Other known resources are still cleaned up.
 *  - seekdb_cuvs_free: releases the handle and its retained query workspace.
 * Calls using the same handle, including free, must be serialized by the caller.
 * Separate handles own separate workspaces; no previous index or query result
 * is cached across handle lifetimes.
 * All ROCm/cuVS state lives inside the bridge; seekdb only sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
#ifdef __cplusplus
}
#endif
#endif
