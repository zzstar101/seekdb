#pragma once
#include <stdint.h>
typedef struct { int device_type; int device_id; } DLDevice;
typedef struct { int code; int bits; int lanes; } DLDataType;
typedef struct { void* data; DLDevice device; int ndim; DLDataType dtype; int64_t* shape; int64_t* strides; uint64_t byte_offset; } DLTensor;
typedef struct { DLTensor dl_tensor; } DLManagedTensor;
#define kDLCUDA 2
#define kDLCPU 1
#define kDLFloat 2
#define kDLUInt 3
#define kDLInt 4
