#pragma once
#include <stddef.h>
#define cudaSuccess 0
#define cudaMemcpyDefault 4
int cudaMemcpy(void*, const void*, size_t, int);
