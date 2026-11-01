#!/bin/sh
# The per-site indirect-call cache (RECOMP_ICALL_LOOKUP, recomp_types.h, and
# recomp_icall_resolve, src/recomp_manual.c) under four threads, then a build
# with it off. Host gcc; no game data.
#   sh regen/icall_cache/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/icall_cache_test.$$
mkdir -p "$out"
# recomp_icall_resolve as recomp_manual.c has it, without the rest of that file
awk '/^#if defined\(__GNUC__\) && !defined\(RECOMP_ICALL_FEEDBACK\)/{f=1} f{print} f&&/^#endif/{exit}' \
    "$root/jsrf-switch/src/recomp_manual.c" > "$out/resolve.inc"
inc="-I$out -I$root/jsrf-switch/src/recomp/gen -I$root/jsrf-switch/xboxrecomp/include -I$root/jsrf-switch/xboxrecomp/src"
gcc -O2 -pthread $inc "$here/test_icall_cache.c" -o "$out/t"
gcc -O2 -pthread -DRECOMP_NO_ICALL_CACHE $inc "$here/test_icall_cache.c" -o "$out/t_off"
"$out/t" && "$out/t_off"
rm -rf "$out"
