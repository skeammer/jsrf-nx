#!/usr/bin/env bash
# Turn the Switch sampling profiler's output into per-thread function tables.
#
# On the console: RECOMP_NX_PROFILE=1 in SD:/switch/jsrf/jsrf_env.txt, play
# the part that is slow for a minute, then copy SD:/switch/jsrf/prof.bin and
# jsrf_log.txt next to jsrf-switch/. From the repository root:
#
#   docker run --rm -v "$PWD":/workspace -w /workspace/jsrf-switch \
#       docker.io/devkitpro/devkita64:20260219 \
#       bash switch/prof_report.sh /workspace/prof.bin /workspace/jsrf_log.txt
#
# Writes ../prof_report.txt. Needs the ELF of the build that made prof.bin
# (BUILD_DIR, default build-switch-dk; ELF= to name it directly). FROM=<s>
# and TO=<s> (docker run -e FROM=80 -e TO=150 ...) keep only the samples
# between those log times, e.g. the busy stretch of a run. LINES=fn[,fn...]
# adds where inside those functions the samples were, as source lines.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
ELF="${ELF:-${BUILD_DIR:-$REPO/build-switch-dk}/jsrf_recomp.elf}"
OUT="${OUT:-$REPO/../prof_report.txt}"
CC="${CC:-cc}"
fail() { echo "error: $*" >&2; exit 1; }

[ "$#" -ge 1 ] || fail "usage: prof_report.sh prof.bin [jsrf_log.txt]"
[ -f "$1" ] || fail "no $1"
[ -f "$ELF" ] || fail "no ELF at $ELF (the build that made prof.bin)"
NM="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm"
[ -x "$NM" ] || NM="$(command -v aarch64-none-elf-nm || true)"
[ -n "$NM" ] || fail "aarch64-none-elf-nm not found"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
"$NM" -n --defined-only "$ELF" > "$WORK/syms.txt"
"$CC" -O1 -o "$WORK/prof_report" "$REPO/tools/prof_report/prof_report.c"
"$WORK/prof_report" "$1" "$WORK/syms.txt" "${2:-}" > "$WORK/report.txt"
# LINES=fn[,fn...]: the "@0x<addr>" lines become file:line (and the inlined
# function) where the ELF has line tables.
A2L="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-addr2line"
[ -x "$A2L" ] || A2L="$(command -v aarch64-none-elf-addr2line || true)"
if [ -n "${LINES:-}" ] && [ -n "$A2L" ]; then
    # One addr2line for every address: started once per address, it read the
    # whole ELF's line tables each time, and a report took minutes.
    addrs="$(grep -o '^@0x[0-9a-f]*' "$WORK/report.txt" | sed 's/^@//' | sort -u || true)"
    : > "$WORK/a2l.txt"
    if [ -n "$addrs" ]; then
        # shellcheck disable=SC2086
        "$A2L" -e "$ELF" -a -f -i -p $addrs > "$WORK/a2l.txt" 2>/dev/null || true
    fi
    awk -v A="$WORK/a2l.txt" '
        function key(a) { sub(/^@?0x0*/, "", a); sub(/:$/, "", a); return a }
        BEGIN {
            while ((getline l < A) > 0) {
                if (l ~ /^0x/) { k = key(substr(l, 1, index(l, " ") - 1)); map[k] = substr(l, index(l, " ") + 1) }
                else if (k != "") map[k] = map[k] " " l
            }
        }
        /^@0x/ { a = $1; rest = substr($0, length($1) + 2); sub(/^@/, "", a)
                 print "  " a " " rest "  " map[key(a)]; next }
        { print }' "$WORK/report.txt" > "$OUT"
else
    cp "$WORK/report.txt" "$OUT"
fi
echo "Wrote $OUT"
head -40 "$OUT"
