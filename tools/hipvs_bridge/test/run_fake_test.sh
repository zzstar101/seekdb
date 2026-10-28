#!/usr/bin/env bash
# Offline concurrency test for the bridge.  Needs only a C compiler: the GPU is
# replaced by test/fake_cuvs.c, which turns a shared-buffer overwrite into a
# wrong answer.  No AMD hardware, no hipVS install, no seekdb build.
#
#   ./run_fake_test.sh
#
# The script builds two variants:
#   1. the bridge as shipped            -> must report 0 mismatches
#   2. the same bridge with the lock
#      removed by sed                   -> must report mismatches, which proves
#                                          the test can see the defect at all
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BRIDGE_DIR=$(cd "$HERE/.." && pwd)
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== build shipped bridge"
"$CC" -O2 -pthread -std=gnu11 -Wall \
  -I"$HERE/stubs" -I"$BRIDGE_DIR" \
  "$HERE/fake_cuvs.c" "$HERE/test_concurrent_search.c" "$BRIDGE_DIR/seekdb_cuvs_bridge.c" \
  -o "$TMP/shipped" -lm
echo "== run (expect 0 mismatches)"
set +e
"$TMP/shipped"
rc_shipped=$?
set -e

echo "== build bridge with the per-handle lock removed"
sed -e 's/pthread_mutex_lock(&h->mu)/(0)/' \
    -e 's/pthread_mutex_unlock(&h->mu)/(0)/' \
    "$BRIDGE_DIR/seekdb_cuvs_bridge.c" > "$TMP/bridge_nolock.c"
"$CC" -O2 -pthread -std=gnu11 -w \
  -I"$HERE/stubs" -I"$BRIDGE_DIR" \
  "$HERE/fake_cuvs.c" "$HERE/test_concurrent_search.c" "$TMP/bridge_nolock.c" \
  -o "$TMP/nolock" -lm
echo "== run (expect mismatches: the test must be able to fail)"
set +e
"$TMP/nolock"
rc_nolock=$?
set -e

echo
if [ "$rc_shipped" -eq 0 ] && [ "$rc_nolock" -ne 0 ]; then
  echo "PASS: serialised bridge is clean, lock-free variant is caught"
  exit 0
fi
if [ "$rc_shipped" -ne 0 ] && [ "$rc_nolock" -ne 0 ]; then
  echo "FAIL: the bridge under test loses queries under concurrency"
  exit 1
fi
echo "FAIL: unexpected result (shipped rc=$rc_shipped, nolock rc=$rc_nolock)"
exit 1
