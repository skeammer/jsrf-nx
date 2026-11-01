#!/bin/sh
# The vector SSE helpers against the lane-wise ones (test_xmm_vector.c).
#   regen/xmm_vector/run.sh [work dir]       CC=, CFLAGS= to change the build
set -e
here=$(cd "$(dirname "$0")" && pwd)
gen=$here/../../jsrf-switch/src/recomp/gen
work=${1:-/tmp/jsrf-xmm-vector}
mkdir -p "$work"
CC=${CC:-cc}
# The generated code's own flags (jsrf-switch/CMakeLists.txt).
FL="-O2 -w -fno-strict-aliasing -fwrapv -ffp-contract=off -DRECOMP_GENERATED_CODE ${CFLAGS:-}"
$CC $FL -I"$gen" -DPFX=lane -DRECOMP_XMM_NO_VECTOR -c "$here/helpers.c" -o "$work/lane.o"
$CC $FL -I"$gen" -DPFX=vec -c "$here/helpers.c" -o "$work/vec.o"
$CC -O1 "$here/test_xmm_vector.c" "$work/lane.o" "$work/vec.o" -lm -o "$work/t"
"$work/t"
