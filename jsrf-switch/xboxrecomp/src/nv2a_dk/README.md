# nv2a_dk — the deko3d renderer

The Switch build's renderer (`RENDERER=deko3d` in `switch/build.sh`): the
pushbuffer executor's back end on deko3d, the console's own GPU interface,
instead of OpenGL over Mesa.

## Shaders

- **Ubershaders** (`shaders/nv2a_vs.glsl`, `shaders/nv2a_fs.glsl`), compiled
  with uam when the shaders change (`shaders/build.sh`) and embedded in the
  NRO (`nv2a_dk_shaders.h`, committed). The vertex shader interprets the
  title's NV2A vertex program (decoded on the CPU, `vp_decode1`); the
  fragment shader evaluates the register combiners. They draw everything,
  slowly.
- **The shader pack** (`SD:/switch/jsrf/dk_shaders.pak`): a dedicated shader
  per vertex program and per combiner setup, from the shader
  generator (`dk_shadergen.c`, through
  `dk_gen.c`) compiled with uam. Used wherever it has the setup; the
  ubershaders cover the rest. Each combiner setup is built twice: with the
  title's alpha test, and with it compiled out, which the renderer takes for
  draws with the test off -- a shader that can discard loses the GPU's early
  depth test, so every hidden pixel would run the whole combiner.

### Built on the console

The NRO builds the pack itself (`dk_compile.c`, with uam linked in as a
library: `xboxrecomp/third_party/uam`), on a thread of its own:

1. every vertex shader and pixel shader the game's XBE ships
   (`dk_xbe_scan.c`; JSRF: 123 vertex programs and 108 combiner setups, some
   370 shaders in all), read from `game/default.xbe`, and what earlier runs
   recorded in `SD:/switch/jsrf/dk_keys.bin` -- **before the game starts**:
   on the first start (after unpacking the disc image, if it boots from one)
   the loading screen's bar fills again, amber, until they are all built.
   Later starts find them in the pack and go straight on;
   `RECOMP_DK_COMPILE_WAIT=0` lets the game start at once and builds them
   beside it;
2. while the game runs, a shader a draw needs that the pack lacks (combiner
   setups D3D builds from texture-stage states are in no file), at once --
   the ubershader draws it for the few frames that takes, then the new one
   takes over. A thin bar along the bottom of the screen shows it when more
   than a few are queued.

Each shader is appended to `dk_shaders.pak` as it is built (to the card every
16 or 5 s), so the work is done once. The pack carries a stamp of the shader
generators' sources (`dk_gen_files.txt`, `nv2a_dk_pack.h`): an update that
changes them makes the console build the pack anew, so a fixed shader bug
never lingers in an old pack. Combiner setups met in play are also recorded
to `dk_keys.bin` (`RECOMP_DK_KEYS=0` stops it). `RECOMP_DK_COMPILE=0` turns
the compiler off.

The log says what it did:

    [DK] compiler up: builds what sdmc:/switch/jsrf/dk_shaders.pak lacks ...
    [DK] compiler: 123 vertex programs and 108 combiner setups from .../default.xbe to build
    [DK] compiler: 50 of <n> built (<t> s)
    [DK] compiler: <n> shaders built in <t> s (0 failed); <n> added to ... this run

### Built on a PC

`tools/dk_pack` makes the same pack from the same keys (`tools/dk_scan`
writes the XBE's shaders as key records). `switch/build.sh` does it when it
finds the game's `default.xbe` next to `jsrf-switch/` (or `$GAME/`), and by
hand, from the repository root:

    docker run --rm -v "$PWD":/workspace -w /workspace/jsrf-switch \
        docker.io/devkitpro/devkita64:20260219 \
        bash switch/dk_pack.sh /workspace/default.xbe /workspace/dk_keys.bin

then copy `switch_sd/switch/jsrf/dk_shaders.pak` to the card. Not needed any
more -- the console builds its own -- but it saves the first start the work.
An existing pack is kept and only new keys are added. File formats:
`nv2a_dk_pack.h`.

## Screen and internal resolution

The swapchain is made at the size the system shows the layer at: 1280x720
in the hand (or on a 720p TV), 1920x1080 docked to a 1080p TV. The renderer
asks every 30 frames (`appletGetDefaultDisplayResolution`) and makes it
again when that changes.

The title draws into surfaces larger than its own by the internal
resolution, `RECOMP_DK_SCALE`:

- `max` (the default) is the screen's height: x1.5, 960x720, in the hand;
  x2.25, 1440x1080, docked -- whatever it costs. Heavy scenes may then take
  longer than a 60 Hz frame (in JSRF's first stage the GPU took 7-8 ms a
  frame at x1 and 14-17 ms at x1.5 in the hand).
- `auto` is dynamic, from x1 up to the screen's height. It starts at the top,
  steps down a quarter as soon as 30 frames average over the GPU budget
  (`RECOMP_DK_GPU_MS`, default 14 of the 16.7 ms frame), and steps up only
  after 600 frames of the game drawing (30 draws or more, so not a loading
  screen) whose average, scaled by the pixel count, still fits in 80% of the
  budget, and whose heaviest 30 frames at this level in the last 3 minutes
  would still fit the budget -- and not within 30 s of stepping down,
  doubled (up to 8 min) whenever a step up is undone within a minute. A
  change waits for the GPU and copies every surface, so it must not see-saw.
- A number fixes it (`1` is the title's own 640x480; at most 4).

The title's coordinates are untouched -- the viewport maps them onto the
larger image, and scissors, clear rectangles and sampled render targets are
converted -- so the shaders and the pack do not change. A surface the title
anti-aliases already counts its supersampling towards the factor. On a
change every surface is copied into one at the new size (depth buffers are
made anew). Fill costs grow with the pixel count: in JSRF's first stage the
GPU took 7-8 ms a frame at x1 and 14-17 ms at x1.5 in the hand.

## Switches

| | |
|---|---|
| `RECOMP_DK_SCALE=max\|auto\|<n>` | internal resolution (see above; default max, the screen's height) |
| `RECOMP_DK_GPU_MS=<ms>` | `auto`'s GPU budget per frame (default 14) |
| `RECOMP_DK_MIPS=0` | sample level 0 only (the title's mip chains are uploaded and used by default) |
| `RECOMP_DK_TRACE=1` | surfaces, depth buffers and textures as they are made |
| `RECOMP_DK_TEX_MB=<n>` | texture memory (default 256) |
| `RECOMP_DK_TEX_EVERY=<n>` | a texture whose bytes passed 8 content checks in a row is checked every `n`-th frame instead of every frame, each on its own phase (default 4; 1 = every frame, as before). A changing one (a movie) stays at every frame; one the executor rewrote is redone at once |
| `RECOMP_DK_DATA_CACHED=0` | the data ring (vertices, indices, texture uploads) written uncached, as before. By default it is CPU-cached and its new bytes are cleaned out before each submit: 0.60 ms per MB against 0.68 in the same streets. The `[perf]` line shows `data copies` and `cleaning` times either way |
| `RECOMP_DK_THREAD=0` | the renderer on the executor's thread, as before. By default it runs on a thread of its own behind the executor (`nv2a_backend_queue.c`): the executor decodes the pushbuffer and queues each call, the renderer's thread records them for the GPU, on two cores. `RECOMP_QUEUE_RING_KB` (8192), `RECOMP_QUEUE_FRAMES` (how many flips may be queued, 2), `RECOMP_QUEUE_SPIN_US` (50) and `RECOMP_QUEUE_PRIO` tune it. `RECOMP_QUEUE_COPY_VERTICES=0`: vertex arrays read when the thread gets to the draw, with the GPU's progress held back until then (default 1: copied when queued, progress passed on at once -- JSRF's D3D waits for pushbuffer room on that progress) |
| `RECOMP_DK_HITCH_MS=<ms>` | a `[hitch]` line for every frame over this long (default 50; e.g. 22 for every frame under ~45 fps). The profile report's "during the hitches" tables cover those frames |
| `RECOMP_DK_FRONT_FLIP=1` | swap the front-face winding |
| `RECOMP_DK_HOLE_DIAG=1` | alpha-tested pixels cyan instead of discarded |
| `RECOMP_DK_DRAWSYNC=1` | every draw submitted and waited for alone (`=<n>`: from n seconds in), so a GPU fault names the draw it stopped on; slow, a diagnostic. Build with `DK_DEBUG=1` to get deko3d's own report too |
| `RECOMP_DK_NODRAW=1` | draws counted, not drawn (phase 1) |
| `RECOMP_DK_HEARTBEAT=1` | a moving square in the corner, to tell a live screen from a hung one |
| `RECOMP_DK_FPS=1` | an FPS counter in the top-left corner: frames shown per second over the last half second, and the longest gap between two frames in that time (`MS`) |
| `RECOMP_DK_COMPILE=0` | no shader compiling on the console (the pack is only read) |
| `RECOMP_DK_COMPILE_WAIT=0` | the first start does not wait on the loading screen for the shaders; they are built beside the game |
| `RECOMP_DK_COMPRESS=0` | render targets and depth buffers without the GPU's framebuffer compression |
| `RECOMP_DK_BORDER=0` | clamp-to-border textures read transparent black instead of the title's border colour (as before) |
| `RECOMP_DK_POLY_OFFSET=0` | ignore the title's polygon offset (as before; shadows and decals then fight the surface under them) |
| `RECOMP_DK_CULL=0` | no face culling (a diagnostic) |
| `RECOMP_DK_CAPTURE=<s>` | every `<s>` seconds, one whole frame's draws written to the log (shaders, combiner key and constants, textures, blend, depth, a vertex; at most 40 frames of 300 draws) |
| `RECOMP_DK_LIGHTING=0` | no fixed-function lighting: lit draws in their vertex colour, as before (to compare) |
| `RECOMP_DK_FOG=0` | the fog unit off: no fog at all (to compare; the fog value used to be taken as the fog factor unconverted) |
| `RECOMP_DK_PIXEL_ALIGN=0` | at an internal resolution, positions are not moved onto the image's pixel centres, as before (2D quads then miss the first column and row: lines along the left and top of some cutscenes) |
| `RECOMP_DK_ZCLAMP=vertex` | depth clamped per vertex in the shaders, as before (geometry next to the camera then lost to what is behind it) |
| `RECOMP_DK_KEYS=0` | do not record new shader keys |
| `RECOMP_DK_DIR=<dir>` | where the pack and keys live (default `sdmc:/switch/jsrf`) |

Build-time: `DK_DEBUG=1 RENDERER=deko3d switch/build.sh` links deko3d's
checking library instead of its release one: it names any misuse in the log
and on screen, at some CPU cost per call.

## Reading the log

Every 10 s, two `[perf] deko3d:` lines: GPU time per frame (timestamps)
with the internal resolution, picture and screen sizes, the renderer's CPU
time and how much of it waits for the GPU, draws per
frame, how many draws the pack covers, syncs (full GPU waits) by cause,
barriers, upload volume per frame, new keys recorded, and the shaders the
console built that were taken into use. Inside `draws`: `data copies` is the
time writing vertices and indices into the data ring, `texture checks` the
sampled-hash checks (how many a frame, and how many `RECOMP_DK_TEX_EVERY`
put off); `cleaning` (second line) is the cached data ring's cache cleaning.
A third, `[perf] pushbuffer:`, gives
the command words the executor read per frame, split by how
`nv2a_pb_exec_run` took them (transform constants, programs, indices, inline
vertices, shadow-only state, the rest word by word). It also gives, per frame:
the executor thread's time in the walk (drawing and presenting included; minus
the renderer's CPU time it is reading the stream and gathering each draw), how
much of that was stalled on NOP traps waiting for the title's PGRAPH handler,
how long it slept between passes and how often, how often new commands turned
up while it watched before a sleep (`RECOMP_EXEC_POLL_US`), and how many times
a guest wait woke it (`nv2a_kick`). With `RECOMP_PB_REST=1`, last, `the rest
by method`: the methods whose words took the slow path, with their share of
all words (off by default: counting them costs the walk).

With the renderer on its own thread the executor's walk no longer includes
the renderer's time, and a fourth line, `[perf] renderer thread:`, says how
much of the time that thread sat idle (waiting for work: the executor or the
game is the limit) and what the executor waited for: the thread to catch up
before a NOP trap or a 2D copy, the frame limit (`two frames ahead`: the
renderer is the limit) or room in the ring; and the queue's volume per frame.
In the per-thread `% of a core` line the renderer's thread is a new entry,
and `[QUEUE] renderer thread on core <n>` at the start says where it went.

Vertex-program constants are pushed by changed rows only. Each raw batch says
which rows the title wrote since the batch before (`vp_const_lo`/`hi`,
`vp_const_gen_prev`), so a draw compares those rows rather than all 192.
