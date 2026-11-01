#!/usr/bin/env bash
# Build the deko3d renderer's shader pack from the game's XBE and/or the keys
# the console recorded.
#
# The deko3d build draws everything with two ubershaders and records each
# vertex program and combiner setup it meets to SD:/switch/jsrf/dk_keys.bin.
# This turns those into dedicated shaders (compiled here with uam) and
# writes dk_shaders.pak; put that file next to the NRO and the console uses
# it from the next start. The console now builds the same pack itself on the
# first start (nv2a_dk/dk_compile.c); this only saves it the work. An
# existing pack is kept and only new keys are added (if the same version of
# the shader generators made it).
#
# From the repository root (the folder holding jsrf-switch/), with the
# game's default.xbe and/or the key file copied off the card:
#
#   docker run --rm -v "$PWD":/workspace -w /workspace/jsrf-switch \
#       docker.io/devkitpro/devkita64:20260219 \
#       bash switch/dk_pack.sh /workspace/default.xbe /workspace/dk_keys.bin
#
# An .xbe (any file starting XBEH) is scanned by tools/dk_scan for every
# vertex shader and pixel shader the title ships -- the whole game's, before
# it is played. Combiner setups D3D makes from texture-stage states are in no
# file; those still come from the console's dk_keys.bin.
#
# Writes ../switch_sd/switch/jsrf/dk_shaders.pak (OUT= to change it).
# Environment: UAM (default $DEVKITPRO/tools/bin/uam; installed with
# dkp-pacman when missing), CC (default cc).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
OUT="${OUT:-$REPO/../switch_sd/switch/jsrf/dk_shaders.pak}"
CC="${CC:-cc}"
fail() { echo "error: $*" >&2; exit 1; }

[ "$#" -ge 1 ] || fail "usage: dk_pack.sh [default.xbe] [dk_keys.bin...]"
for k in "$@"; do [ -f "$k" ] || fail "no file $k"; done

UAM="${UAM:-$DEVKITPRO/tools/bin/uam}"
if [ ! -x "$UAM" ]; then
    if command -v uam >/dev/null 2>&1; then
        UAM="$(command -v uam)"
    elif command -v dkp-pacman >/dev/null 2>&1; then
        echo "==> uam not installed; installing it (dkp-pacman -S uam)..."
        dkp-pacman -Sy --noconfirm uam >/dev/null || fail "could not install uam"
        UAM="$DEVKITPRO/tools/bin/uam"
    fi
fi
[ -x "$UAM" ] || fail "uam not found (set UAM=/path/to/uam)"
command -v "$CC" >/dev/null 2>&1 || fail "no C compiler ($CC); set CC="

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# The generators' stamp, as CMake works it out (xboxrecomp/src/nv2a_dk/
# CMakeLists.txt, nv2a_dk_pack.h): the console keeps a pack only if it has it.
GEN_DIR="$REPO/xboxrecomp/src/nv2a_dk"
STAMP=""
while IFS= read -r f; do
    case "$f" in ''|'#'*) continue ;; esac
    STAMP="$STAMP$(sha1sum "$GEN_DIR/$f" | cut -c1-40)"
done < "$GEN_DIR/dk_gen_files.txt"
STAMP="$(printf %s "$STAMP" | sha1sum | cut -c1-16)"
echo "==> Building dk_pack (shader generators' stamp $STAMP)..."
"$CC" -O1 -DDK_GEN_STAMP=0x${STAMP}ull -o "$WORK/dk_pack" "$REPO/tools/dk_pack/dk_pack.c"
KEYS=()
n=0
for k in "$@"; do
    if [ "$(head -c 4 "$k")" = "XBEH" ]; then
        [ -x "$WORK/dk_scan" ] || "$CC" -O1 -o "$WORK/dk_scan" "$REPO/tools/dk_scan/dk_scan.c"
        n=$((n + 1))
        echo "==> Scanning $k for the title's shaders..."
        "$WORK/dk_scan" -o "$WORK/xbe$n.keys" "$k"
        KEYS+=("$WORK/xbe$n.keys")
    else
        KEYS+=("$k")
    fi
done
mkdir -p "$(dirname "$OUT")"
echo "==> Compiling shaders with $UAM..."
set +e
"$WORK/dk_pack" -u "$UAM" -o "$OUT" -w "$WORK/glsl" "${KEYS[@]}"
rc=$?
set -e
if [ "$rc" -ne 0 ] && [ "$rc" -ne 3 ]; then
    fail "dk_pack failed ($rc)"
fi
if [ "$rc" -eq 3 ]; then
    echo "note: some shaders did not compile; the console draws those with the ubershaders"
fi
echo
echo "Copy $OUT to SD:/switch/jsrf/dk_shaders.pak"
