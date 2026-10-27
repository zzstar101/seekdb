/* Test-only runtime surface, with deferred operations on explicit streams. */
#ifndef SEEKDB_TEST_CUDA_RUNTIME_H
#define SEEKDB_TEST_CUDA_RUNTIME_H
#include <stddef.h>
typedef void *cudaStream_t;
typedef enum { cudaSuccess = 0, cudaErrorUnknown = 999 } cudaError_t;
typedef enum { cudaMemcpyDefault = 4 } cudaMemcpyKind;
#ifdef __cplusplus
extern "C" {
#endif
cudaError_t cudaMemcpyAsync(void *, const void *, size_t, cudaMemcpyKind, cudaStream_t);
#ifdef __cplusplus
}
#endif
#endif
