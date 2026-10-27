#!/usr/bin/env bash
set -Eeuo pipefail

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BRIDGE_DIR=$(dirname "$TEST_DIR")
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/seekdb-bridge-mock.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT

CC=${CC:-cc}
CXX=${CXX:-c++}
COMMON_FLAGS=(-Wall -Wextra -Werror -pedantic -g -O1 -D__HIP_PLATFORM_AMD__
              -I"$TEST_DIR/mock_include" -I"$BRIDGE_DIR")
if [[ "${SANITIZE:-0}" == 1 ]]; then
  COMMON_FLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

# hipcc compiles the .c bridge as C++, so exercise both the C ABI source and the
# production language mode. The mock queues copies/build/search until sync.
for language in c c++; do
  if [[ "$language" == c ]]; then
    "$CC" -std=c11 "${COMMON_FLAGS[@]}" -c "$BRIDGE_DIR/seekdb_cuvs_bridge.c" \
      -o "$BUILD_DIR/bridge.o"
  else
    "$CXX" -x c++ -std=c++11 "${COMMON_FLAGS[@]}" -c "$BRIDGE_DIR/seekdb_cuvs_bridge.c" \
      -o "$BUILD_DIR/bridge.o"
  fi
  "$CXX" -std=c++11 "${COMMON_FLAGS[@]}" "$TEST_DIR/test_bridge.cpp" \
    "$BUILD_DIR/bridge.o" -o "$BUILD_DIR/test_bridge"
  "$BUILD_DIR/test_bridge"
done
