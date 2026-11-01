#!/bin/sh
# Differential test of regen/jsrf_reg_locals.py: every lifted function run
# both ways from the same state, everything it does compared.
#
#   regen/difftest/run.sh <original gen dir> <rewritten gen dir> [work dir] [function|all] [seeds]
#
# Host build (gcc, Linux): the plain thread-local path, not the Switch's fast
# one. About 10 minutes to build on two cores; 20 runs each, 30 minutes.
#
# For regen/jsrf_inline_leaves.py: DT_ONLY=<file> (the changed callers) and
# DT_REAL=<file> (python3 regen/jsrf_inline_leaves.py <original> --list),
# so calls to the inlined functions run their real bodies in the original
# (dt_gen.py).
#
# For jsrf_copy_loops.py, jsrf_fp_locals.py and jsrf_simd_locals.py:
# DT_ONLY=<file> (their --list output, and 0x001993A0 for the copy loop).
# Run once with DT_NO_NAN=1 (no NaNs or infinities in the random state: must
# be 0 mismatches) and once without, where a mismatch should only ever be a
# different NaN (which of two NaN operands comes out is not fixed by C).
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$here/../../jsrf-switch
orig=$(cd "$1" && pwd); new=$(cd "$2" && pwd)
work=${3:-/tmp/jsrf-reg-locals-difftest}
python3 "$here/dt_gen.py" "$orig" "$new" "$work"
make -C "$work" -f "$here/Makefile" -j"${JOBS:-2}" HERE="$here" NEW="$new" REPO="$repo"
"$work/dt" "${4:-all}" "${5:-3}"
