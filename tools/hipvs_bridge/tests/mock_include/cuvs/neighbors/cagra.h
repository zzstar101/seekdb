/* Minimal test-only CAGRA C API. */
#ifndef SEEKDB_TEST_CAGRA_H
#define SEEKDB_TEST_CAGRA_H
#include <cuvs/core/c_api.h>
typedef struct MockIndexParams *cuvsCagraIndexParams_t;
typedef struct MockSearchParams *cuvsCagraSearchParams_t;
typedef struct MockIndex *cuvsCagraIndex_t;
typedef enum { NO_FILTER = 0 } cuvsFilterType;
typedef struct { cuvsFilterType type; uintptr_t addr; } cuvsFilter;
#ifdef __cplusplus
extern "C" {
#endif
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t);
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t,
                         DLManagedTensor *, cuvsCagraIndex_t);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t,
                          cuvsCagraIndex_t, DLManagedTensor *, DLManagedTensor *,
                          DLManagedTensor *, cuvsFilter);
#ifdef __cplusplus
}
#endif
#endif
