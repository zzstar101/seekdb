#pragma once

#include <stdint.h>
#include <cuvs/core/c_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exact kNN graph (self excluded) of a host fp32 row-major dataset, computed on
 * the GPU (fp16 WMMA distance tiles + sampled-threshold top-k). d_knn: device
 * uint32 [n x k], each row sorted by distance. Requires k <= 256. 0 on success. */
int sk_cagra_knn_build(cuvsResources_t res, const float* h_base, int64_t n, int64_t dim, int k,
                       uint32_t* d_knn);

/* CAGRA graph optimisation (rank-based detour pruning + reverse-edge merge) on
 * the GPU. d_knn: device uint32 [n x k] sorted by distance; d_graph: device
 * uint32 [n x out_degree]. Requires out_degree <= k <= 256. */
int sk_cagra_optimize(cuvsResources_t res, uint32_t* d_knn, int64_t n, int k,
                      int out_degree, uint32_t* d_graph);

#ifdef __cplusplus
}
#endif
