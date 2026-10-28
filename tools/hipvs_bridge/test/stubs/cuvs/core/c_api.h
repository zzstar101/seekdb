#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int cuvsError_t;
#define CUVS_SUCCESS 0
typedef void* cuvsResources_t;
typedef struct { int type; uintptr_t addr; } cuvsFilter;
#define NO_FILTER 0
const char* cuvsGetLastErrorText(void);
cuvsError_t cuvsResourcesCreate(cuvsResources_t*);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t);
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void**, size_t);
cuvsError_t cuvsRMMFree(cuvsResources_t, void*, size_t);
