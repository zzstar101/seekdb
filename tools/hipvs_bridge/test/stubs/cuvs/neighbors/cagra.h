#pragma once
#include "../core/c_api.h"
#include <dlpack/dlpack.h>
#include <stdint.h>
#include <stdbool.h>
enum cuvsCagraGraphBuildAlgo { CUVS_CAGRA_GRAPH_BUILD_ALGO_IVF_PQ = 0, NN_DESCENT = 1 };
struct cuvsCagraIndexParams { enum cuvsCagraGraphBuildAlgo build_algo; size_t graph_degree; size_t intermediate_graph_degree; size_t nn_descent_niter; };
struct cuvsCagraSearchParams { size_t itopk_size; uint32_t search_width; };
typedef void* cuvsCagraIndex_t;
typedef struct cuvsCagraIndexParams* cuvsCagraIndexParams_t;
typedef struct cuvsCagraSearchParams* cuvsCagraSearchParams_t;
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t*);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t*);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t);
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t, DLManagedTensor*, cuvsCagraIndex_t);
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t, cuvsCagraIndex_t, DLManagedTensor*, DLManagedTensor*, DLManagedTensor*, cuvsFilter);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t*);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char*, cuvsCagraIndex_t, bool);
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char*, cuvsCagraIndex_t);
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t, int64_t*);
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t, int64_t*);
