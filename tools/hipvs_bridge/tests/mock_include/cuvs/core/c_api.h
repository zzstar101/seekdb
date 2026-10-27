/* Minimal test-only cuVS C API. */
#ifndef SEEKDB_TEST_CUVS_CORE_H
#define SEEKDB_TEST_CUVS_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <cuda_runtime.h>
#include <dlpack/dlpack.h>
typedef enum { CUVS_ERROR, CUVS_SUCCESS } cuvsError_t;
typedef uintptr_t cuvsResources_t;
#ifdef __cplusplus
extern "C" {
#endif
const char *cuvsGetLastErrorText(void);
cuvsError_t cuvsResourcesCreate(cuvsResources_t *);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t);
cuvsError_t cuvsStreamGet(cuvsResources_t, cudaStream_t *);
cuvsError_t cuvsStreamSync(cuvsResources_t);
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void **, size_t);
cuvsError_t cuvsRMMFree(cuvsResources_t, void *, size_t);
#ifdef __cplusplus
}
#endif
#endif
