#!/usr/bin/env bash
set -Eeuo pipefail

TEST_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd "$TEST_DIR/../../.." && pwd)
TEST_TMP=$(mktemp -d)
trap 'rm -rf "$TEST_TMP"' EXIT

"${CXX:-g++}" -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -I"$REPO_DIR" "$TEST_DIR/worker_test.cpp" \
  -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join \
  -o "$TEST_TMP/worker_test"
"$TEST_TMP/worker_test"
