#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#include <stdint.h>
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
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);
/* Graph export/import for indexes built by the GPU-resident graph pipeline:
 *  - seekdb_cuvs_graph_degree: graph degree of the handle, 0 when the graph is not
 *    exportable (index built by cuvsCagraBuild / loaded from a cuVS file).
 *  - seekdb_cuvs_copy_graph: copies the n x degree uint32 graph to host memory.
 *  - seekdb_cuvs_from_graph: rebuilds a searchable handle from base vectors and a
 *    previously exported graph (no graph construction). */
long  seekdb_cuvs_graph_degree(void* handle);
int   seekdb_cuvs_copy_graph(void* handle, uint32_t* out);
void* seekdb_cuvs_from_graph(const float* base, const uint32_t* graph, long n, long dim,
                             long degree);
#ifdef __cplusplus
}
#endif
#endif
