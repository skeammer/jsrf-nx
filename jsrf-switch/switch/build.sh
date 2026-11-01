#!/usr/bin/env bash
# Builds the Switch NRO and stages an SD card layout under switch_sd/:
#
#   switch/<home>/<home>.nro       jsrf.nro for the main build
#   switch/<home>/save/            made on the first start
#   switch/<home>/jsrf_env.txt     KEY=VALUE settings (a commented template)
#   switch/jsrf/game/              the extracted disc, if JSRF_GAME_SRC has it
#
# Run it in devkitPro's container (see the README). Settings, all optional:
#   JSRF_HOME_NAME  SD folder, sdmc:/switch/<name> (default jsrf). Any other
#                   name is a side-by-side build: <name>/<name>.nro, titled
#                   "Jet Set Radio Future (test)", with its own settings,
#                   saves and shader cache, sharing /switch/jsrf/game.
#   JSRF_GEN_DIR    the lifted code (default src/recomp/gen)
#   XBOXRECOMP_DIR  the runtime (default xboxrecomp/)
#   JSRF_GAME_SRC   an extracted disc to stage (default ../game_files)
#   SD_ROOT         staging SD root (default ../switch_sd)
#   BUILD_DIR       build directory (default build-switch-dk)
#   JOBS            parallel compiles (default: one per 2 GB of free memory)
#   DK_DEBUG        1: deko3d's checking library, which names a misuse in the
#                   log and on screen (slower)
#   FAST_TLS        0: plain __thread accesses in the lifted code
#   DK_PACK         1: build the shader pack here (switch/dk_pack.sh), from the
#                   default.xbe in JSRF_GAME_SRC or beside jsrf-switch/ and a
#                   dk_keys.bin beside jsrf-switch/, instead of letting the
#                   console build it on its first start
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
runtime="${XBOXRECOMP_DIR:-$here/xboxrecomp}"
generated="${JSRF_GEN_DIR:-$here/src/recomp/gen}"
game_src="${JSRF_GAME_SRC:-$here/../game_files}"
sd="${SD_ROOT:-$here/../switch_sd}"
build="${BUILD_DIR:-$here/build-switch-dk}"
home="${JSRF_HOME_NAME:-jsrf}"

if [ "$home" = jsrf ]; then
    title="Jet Set Radio Future"
    nro=jsrf.nro
else
    title="Jet Set Radio Future (test)"
    nro="$home.nro"
fi

die() { echo "error: $*" >&2; exit 1; }
[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || die "no devkitPro at $DEVKITPRO"
[ -f "$DEVKITPRO/portlibs/switch/include/SDL2/SDL.h" ] || die "switch-sdl2 is not installed"
[ -f "$DEVKITPRO/libnx/include/deko3d.h" ] || die "deko3d is not installed (dkp-pacman -S deko3d)"
[ -f "$generated/recomp_funcs.h" ] || die "no lifted code in $generated (see its README)"

options=("-DJSRF_SWITCH_APP_NAME=$title" "-DJSRF_HOME_NAME=$home")
if [ "${DK_DEBUG:-0}" = 1 ] && [ -f "$DEVKITPRO/libnx/lib/libdeko3dd.a" ]; then
    options+=(-DJSRF_DEKO3D_DEBUG=ON)
else
    [ "${DK_DEBUG:-0}" = 1 ] && echo "note: no libdeko3dd.a, so DK_DEBUG=1 is ignored" >&2
    options+=(-DJSRF_DEKO3D_DEBUG=OFF)
fi
if [ "${FAST_TLS:-1}" = 0 ]; then options+=(-DJSRF_FAST_TLS=OFF); else options+=(-DJSRF_FAST_TLS=ON); fi

if [ -z "${JOBS:-}" ]; then
    free_mb=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)
    JOBS=$(( free_mb / 2000 ))
    [ "$JOBS" -ge 1 ] || JOBS=1
    [ "$JOBS" -le "$(nproc)" ] || JOBS=$(nproc)
fi

echo "==> Configuring ($build)"
cmake -S "$here" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_DIR="$runtime" \
    -DJSRF_GEN_DIR="$generated" \
    "${options[@]}"
echo "==> Building, $JOBS at a time"
ninja -C "$build" -j"$JOBS"

dest="$sd/switch/$home"
mkdir -p "$dest/save" "$sd/switch/jsrf/game"
cp "$build/jsrf_recomp.nro" "$dest/$nro"
if [ -f "$game_src/default.xbe" ]; then
    echo "==> Staging the game files"
    rsync -a --size-only "$game_src/" "$sd/switch/jsrf/game/"
fi

cat > "$dest/jsrf_env.txt" << 'EOF'
# Jet Set Radio Future: settings, one KEY=VALUE a line ('#' starts a comment).
RECOMP_VBLANK=1
RECOMP_AC97_READY=plain
RECOMP_USB=1
RECOMP_PB_EXEC=1
RECOMP_QUIET=1
RECOMP_USB_TRACE=1
RECOMP_INPUT_DIAG=1
# An FPS counter in the top-left corner (and the longest frame of the last
# half second, in ms):
# RECOMP_DK_FPS=1
# A [hitch] log line for every frame over this many ms (default 50):
# RECOMP_DK_HITCH_MS=22
# The sampling profiler (prof.bin; switch/prof_report.sh makes the tables):
# RECOMP_NX_PROFILE=1
# Delete the disc image once it has been unpacked into game/:
# JSRF_ISO_DELETE=1
# Docked, the CPU runs at 1785 MHz and the GPU at 921.6 (the system's docked
# clocks are 1020 / 768); handheld keeps the system's. 0 turns that off:
# JSRF_DOCK_BOOST=0
# Fixed clocks in both modes, if wanted:
# JSRF_CPU_MHZ=1785
# JSRF_GPU_MHZ=921.6
# JSRF_MEM_MHZ=1600
# Audio balance in dB: music is in the 2D bins, sound effects in the 3D ones
# (the log's "[APU] levels" line every 10 s shows both). The defaults:
# RECOMP_APU_2D_DB=6
# RECOMP_APU_3D_DB=-6
# Loading: 0 does the big file reads and writes on the game's thread again
# (by default beside it, so the loading screen keeps moving):
# RECOMP_BG_IO=0
# For A/B runs -- the graphics thread: commands word by word (RECOMP_PB_FAST=0),
# how long it watches for new ones before sleeping (us, default 300); the
# renderer: on the executor's thread (RECOMP_DK_THREAD=0), vertex data written
# uncached (RECOMP_DK_DATA_CACHED=0), settled textures checked every frame
# (RECOMP_DK_TEX_EVERY=1), vertex arrays read at the draw
# (RECOMP_QUEUE_COPY_VERTICES=0); vertex programs: no transform-context
# mirroring (RECOMP_VP_XFCTX=0), IEEE multiplies (RECOMP_VSH_MUL_ZERO=0):
# RECOMP_PB_FAST=0
# RECOMP_EXEC_POLL_US=0
# RECOMP_DK_THREAD=0
# RECOMP_DK_DATA_CACHED=0
# RECOMP_DK_TEX_EVERY=1
# RECOMP_QUEUE_COPY_VERTICES=0
# RECOMP_VP_XFCTX=0
# RECOMP_VSH_MUL_ZERO=0
# Methods on subchannels other than 0 (2D classes, blits) in the log, the
# first 4000:
# RECOMP_PB_2D=1
EOF

# The shader pack: by default the console builds it on its first start. With
# DK_PACK=1 it is built here, so the console is spared that minute or two.
pack_note="shader pack: built by the console on its first start (DK_PACK=1 builds it here)"
if [ "${DK_PACK:-0}" = 0 ]; then
    rm -f "$dest/dk_shaders.pak"        # an old one copied over would replace the console's
else
    inputs=()
    for xbe in "$game_src/default.xbe" "$here/../default.xbe"; do
        if [ -f "$xbe" ]; then inputs+=("$xbe"); break; fi
    done
    [ -f "$here/../dk_keys.bin" ] && inputs+=("$here/../dk_keys.bin")
    if [ "${#inputs[@]}" -eq 0 ]; then
        pack_note="no default.xbe in $game_src or beside jsrf-switch/: no shader pack built"
    elif OUT="$dest/dk_shaders.pak" bash "$here/switch/dk_pack.sh" "${inputs[@]}"; then
        pack_note="dk_shaders.pak, $(stat -c %s "$dest/dk_shaders.pak") bytes"
    else
        pack_note="dk_shaders.pak NOT rebuilt (see above)"
    fi
fi

echo
echo "Staged $dest"
echo "  $nro $(stat -c %s "$dest/$nro") bytes"
echo "  $pack_note"
[ -f "$sd/switch/jsrf/game/default.xbe" ] && echo "  game files: $(du -sh "$sd/switch/jsrf/game" | cut -f1)"
true
