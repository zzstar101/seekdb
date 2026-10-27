#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HIPVS_ROOT=${HIPVS_ROOT:-/opt/hipvs}
HIPVS_LIB_DIR=${HIPVS_LIB_DIR:-$HIPVS_ROOT/lib}
ROCM_ROOT=${ROCM_ROOT:-/opt/rocm}
HIPCC=${HIPCC:-$ROCM_ROOT/bin/hipcc}
ARCHIVER=${AR:-ar}
OUTPUT=${1:-$SCRIPT_DIR/libseekdb_cuvs_bridge.so}

[[ -x "$HIPCC" ]] || { echo "hipcc is not executable: $HIPCC" >&2; exit 2; }
[[ -f "$HIPVS_ROOT/include/cuvs/core/c_api.h" ]] || {
  echo "hipVS headers are missing under $HIPVS_ROOT/include" >&2
  exit 2
}
[[ -f "$HIPVS_LIB_DIR/libcuvs_c.so" ]] || {
  echo "libcuvs_c.so is missing under $HIPVS_LIB_DIR" >&2
  exit 2
}

mkdir -p "$(dirname "$OUTPUT")"

compile_args=(
  -x c++
  -fPIC
  -O2
  -D__HIP_PLATFORM_AMD__
  -I"$HIPVS_ROOT/include"
  -I"$ROCM_ROOT/include"
  "$SCRIPT_DIR/seekdb_cuvs_bridge.c"
)

if [[ "$OUTPUT" == *.a ]]; then
  OBJECT=${OUTPUT%.a}.o
  "$HIPCC" "${compile_args[@]}" -c -o "$OBJECT"
  "$ARCHIVER" rcs "$OUTPUT" "$OBJECT"
else
  "$HIPCC" "${compile_args[@]}" \
    -shared \
    -L"$HIPVS_LIB_DIR" \
    -Wl,-soname,libseekdb_cuvs_bridge.so \
    -Wl,-rpath,"$HIPVS_LIB_DIR:$ROCM_ROOT/lib" \
    -lcuvs_c \
    -o "$OUTPUT"
fi

echo "built $OUTPUT"
