/*
 * nv2a_dk.c -- a deko3d renderer for the pushbuffer executor (Nintendo Switch).
 *
 * Registered as the executor's back end (nv2a_backend.h), the Switch's
 * renderer. The executor decodes the pushbuffer and keeps the
 * method shadow; this file turns that into deko3d commands. deko3d is the
 * console's own GPU interface: command lists are built in memory and handed
 * to the GPU, with no driver in between, which is where OpenGL over Mesa
 * spent most of a frame (and stalled for seconds compiling shaders).
 *
 * Shaders are two ubershaders compiled at build time (uam; shaders/): the
 * vertex shader interprets the title's NV2A vertex program, decoded here
 * once per program version and read from a uniform buffer; the fragment
 * shader evaluates the texture stages and register combiners from their
 * registers. Their semantics are the shader generator's (dk_shadergen.c),
 * kept alike so a picture that differs points at the renderer, not at the
 * decode. Where the shader pack (dk_shaders.pak) has a dedicated shader for
 * a program or combiner setup, that draws instead, much faster; the console
 * builds the pack itself (dk_compile.c: the game's XBE on the first start,
 * and whatever a draw meets that the pack lacks), on its own thread.
 *
 * Phases (each tried on the console before the next):
 *   1  the display: device, swapchain, surfaces and depth buffers by guest
 *      address, clears, presenting, the loading screen          (done)
 *   2  geometry through both ubershaders, textures (DXT as BC, the rest
 *      decoded), render targets sampled as textures, blend/depth/stencil/
 *      cull state                                                <- this file
 *   3  performance: specialised shaders for common setups, fewer pushes
 *   4  what the GL renderer does not do either: bump/cube texture modes
 *
 * Memory, and when it may be reused:
 *   - command memory and the data ring (vertices, indices, texture uploads)
 *     come in DK_FRAMES slices, one per frame in flight; a slice is reused
 *     after its frame's fence;
 *   - uniforms are pushed into the command stream (dkCmdBufPushConstants),
 *     so one buffer serves every draw;
 *   - a texture whose contents change gets a new image and descriptor; the
 *     old ones are released DK_FRAMES frames later (or after a sync);
 *   - surfaces and depth buffers are freed only after the queue is idle.
 *
 * Rules (the Halo CE Switch port's measurements, port/switch/DEKO3D.md, and
 * deko3d's own):
 *   - deko3d ends the program on any failure it reports, so every size and
 *     limit is checked before asking it for anything, and a queue in error
 *     state is never used again (queue_failed);
 *   - the queue keeps state between command lists, so each frame binds what
 *     every draw relies on;
 *   - one thread at a time: the loader until the executor's first call, then
 *     the executor (nv2a_backend.h: every call is on the NV2A poll thread).
 *
 * The screen: the swapchain is 1280x720 in the hand and 1920x1080 docked to
 * a 1080p TV, made again when that changes (screen_poll). The title draws
 * at an internal resolution (RECOMP_DK_SCALE, default max: the screen's
 * height, x1.5 = 960x720 in the hand and x2.25 = 1440x1080 docked; auto
 * moves between x1 and that by GPU time -- dyn_note); surfaces are carried
 * over on a change.
 *
 * Runtime switches:
 *   RECOMP_DK_SCALE=max|auto|<n> internal resolution: max (default) the
 *                           screen's height, auto dynamic up to it, a
 *                           number fixed (1 = the title's 640x480; up to 4)
 *   RECOMP_DK_GPU_MS=<ms>   auto's GPU budget per frame (default 14)
 *   RECOMP_DK_MIPS=0        level 0 only (mip chains are uploaded and used)
 *   RECOMP_DK_COMPILE=0     no shader compiling on the console (the pack is
 *                           only read)
 *   RECOMP_DK_COMPILE_WAIT=0 the first start does not wait for the shaders
 *                           (they are built beside the game)
 *   RECOMP_DK_COMPRESS=0    render targets and depth buffers uncompressed
 *   RECOMP_DK_BORDER=0      clamp-to-border reads transparent black, not the
 *                           title's border colour
 *   RECOMP_DK_POLY_OFFSET=0 the title's polygon offset ignored (as before)
 *   RECOMP_DK_CULL=0        no face culling (diagnostic)
 *   RECOMP_DK_CAPTURE=<s>   every <s> seconds a frame's draws to the log
 *   RECOMP_DK_LIGHTING=0    no fixed-function lighting (lit draws in their
 *                           vertex colour, as before)
 *   RECOMP_DK_FOG=0         the fog unit off: no fog at all (to compare)
 *   RECOMP_DK_TEXGEN=0      no generated texture coordinates and no texture
 *                           matrices for fixed-function draws (as before)
 *   RECOMP_DK_PIXEL_ALIGN=0 positions not moved onto the image's pixel centres
 *                           at an internal resolution (as before)
 *   RECOMP_DK_ZCLAMP=vertex depth clamped per vertex, as before, not only per
 *                           pixel
 *   RECOMP_DK_HEARTBEAT=1   a moving square in the corner (live vs hung)
 *   RECOMP_DK_TRACE=1       surfaces, depth buffers and textures as made
 *   RECOMP_DK_TEX_MB=<n>    texture memory (default 256)
 *   RECOMP_DK_FRONT_FLIP=1  swap the front-face winding (if culling is wrong)
 *   RECOMP_DK_HOLE_DIAG=1   alpha-tested pixels cyan instead of discarded
 *   RECOMP_DK_DRAWSYNC=1    every draw waited for alone (=<n>: from n seconds
 *                           in), so a GPU fault names its draw (slow)
 *   RECOMP_DK_NODRAW=1      phase 1 behaviour: draws counted, not drawn
 *   RECOMP_VSH_MUL_ZERO=0   IEEE multiplies in vertex programs (A/B)
 */
#include "nv2a_dk.h"
#include "nv2a_dk_shaders.h"
#include "nv2a_dk_pack.h"
#include "dk_compile.h"
#include "dk_font.h"
#include "../kernel/nv2a_backend.h"

#include <switch.h>
#include <deko3d.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── The device, the queue, command memory ─────────────────────────── */

#define DK_FRAMES        3              /* frames the CPU may run ahead */
#define CMD_CHUNK        (1u << 20)     /* command memory added at a time */
#define CMD_CHUNKS       8              /* per frame */
#define CMD_GUARD        4096u          /* each chunk's tail, never handed out (cmd_add_mem) */
#define DATA_SLICE       (24u << 20)    /* vertices, indices, uploads per frame */
#define SWAP_IMAGES      3
#define SCREEN_POLL      30             /* frames between screen-size checks */

static DkDevice    s_dev;
static DkQueue     s_queue;
static DkCmdBuf    s_cmd;
static DkMemBlock  s_cmdmem;            /* DK_FRAMES x CMD_CHUNKS x CMD_CHUNK */
static DkFence     s_fence[DK_FRAMES];
static int         s_fence_set[DK_FRAMES];
static unsigned    s_slot;              /* frame slot being recorded */
static unsigned    s_chunk;             /* chunks of it handed to s_cmd */

static DkMemBlock  s_datamem;           /* DK_FRAMES x DATA_SLICE */
static uint8_t    *s_data_cpu;
static DkGpuAddr   s_data_gpu;
static uint32_t    s_data_off;          /* in this frame's slice */
/* The data ring (vertices, indices, texture uploads) in CPU-cached memory,
 * its new bytes cleaned out to memory before each submit, instead of written
 * uncached (RECOMP_DK_DATA_CACHED=0). In the busy streets the ring takes 3-4
 * MB a frame; written uncached that was 0.68 ms per MB, cached and cleaned
 * 0.60, and the index loop no longer stalls behind the vertex copies. */
static int         s_data_cached = -1;
static uint32_t    s_data_clean;        /* this slice is clean up to here */
static uint64_t    s_data_clean_ticks;  /* spent cleaning, for the perf line */

/* GPU time per frame: a timestamp at the top of the pipe when a frame's
 * commands start and one at its end, per frame slot, read back once the
 * slot's fence has passed. */
static DkMemBlock s_tsmem;
static uint64_t  *s_ts_cpu;
static DkGpuAddr  s_ts_gpu;
static int        s_ts_set[4];

/* The swapchain is made at the size the system shows the layer at: 1280x720
 * in the hand, 1920x1080 docked to a 1080p TV (screen_want), and made again
 * when that changes. */
static DkMemBlock  s_fbmem;
static DkImage     s_fb[SWAP_IMAGES];
static DkSwapchain s_swap;
static uint32_t    s_fb_w = 1280, s_fb_h = 720;

/* Internal resolution (RECOMP_DK_SCALE): surfaces and depth buffers are made
 * larger than the title's by this factor, and the viewport, scissors and
 * clears follow; the title's coordinates (u_surf, texcoords) stay its own.
 * "auto" fills the screen's height with the title's 480 lines: x1.5 in the
 * hand, x2.25 docked. */
static float       s_scale = 1.0f;      /* surfaces are made at this */
static float       s_scale_want = 1.0f; /* taken up at the next frame_begin */
enum { DYN_AUTO, DYN_FIXED, DYN_MAX };
static int         s_scale_mode = -1;   /* DYN_*, -1 unread */
static float       s_scale_fixed = 1.0f;    /* DYN_FIXED */
static float       s_scale_ceil;        /* the screen's height / 480 */
static float       s_dyn;               /* DYN_AUTO's level */
static double      s_dyn_budget_ms = 14.0;  /* RECOMP_DK_GPU_MS */
static struct {
    uint64_t ns, up_ns;
    uint32_t n, up_n, since_change, since_down, since_up, changes;
    uint32_t hold;                      /* frames after a step down before one up */
    uint32_t peak_age;                  /* frames since peak_ms was set */
    double   peak_ms;                   /* the heaviest 30 frames at this level, <= 3 min old */
} s_dyn_st;
static uint32_t    s_slot_draws[4];     /* draws in the frame each slot holds */
static uint32_t    s_draws_at_begin;

static int         s_up;                /* 1 device up, -1 failed */
static int         s_trace = -1, s_heartbeat = -1, s_nodraw = -1;
static int         s_show_fps = -1;     /* RECOMP_DK_FPS=1: a counter in the corner */
static int         s_tex_every = -1;    /* RECOMP_DK_TEX_EVERY: see tex_get */
#define TEX_SETTLED 8
static uint32_t    s_frames, s_draws, s_draws_skipped, s_clears;
static const uint32_t *s_regs;          /* the executor's method shadow */

/* For the [perf] report (nv2a_dk_perf): where a frame's time goes. */
enum { SYNC_DATA, SYNC_SURF, SYNC_DEPTH, SYNC_EVICT, SYNC_CMD, SYNC_DEFER, SYNC_N };
static struct {
    uint64_t t_draw, t_clear, t_flip, t_fence, t_sync;  /* CPU ticks */
    uint64_t t_copy, t_check;           /* in t_draw: data ring writes, texture checks */
    uint32_t checks, checks_skipped;    /* texture content checks done / put off */
    uint64_t gpu_ns;
    uint32_t gpu_frames;
    uint32_t syncs[SYNC_N];
    uint32_t bar_full, bar_frag, bar_desc;
    uint64_t b_vtx, b_idx, b_tex, b_push;
    uint32_t verts, draws, spec_vs, spec_fs, spec_fs_noat, uber_vs_vp;
    uint32_t surfs, depths, texs;
    uint32_t built;                     /* shaders the console's compiler added */
    uint32_t lit, lit_push;             /* lit fixed-function draws, light pushes */
    uint32_t texgen, texmat;            /* fixed-function draws: generated coordinates, a texture matrix only */
    uint32_t fogged;                    /* draws with the fog unit on */
    /* Draws by alpha test, for the early depth test: off (or ALWAYS); on
     * but unable to fail (>= 0, <= 255); on without depth writes; on with
     * them. And the commonest func/ref pairs among those it can fail. */
    uint32_t at_off, at_triv, at_nozw, at_zw, at_prove;
    uint32_t af_key[8], af_n[8];
} s_st;

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

/* The host's log onto the card now (switch_nx.c); absent elsewhere. */
void xbox_log_sync(void) __attribute__((weak));

static void log_sync(void)
{
    fflush(stderr);
    if (xbox_log_sync)
        xbox_log_sync();
}

/* With deko3d's checking library (NV2A_DK_CHECKED) every error comes here
 * first, named; deko3d then shows it on screen and ends the program. The
 * release library ends it without a word (0x367 = DkResult_Fail), so the
 * bring-up logs each step before taking it. */
static void dk_debug(void *user, const char *context, DkResult result, const char *message)
{
    (void)user;
    fprintf(stderr, "  [DK] deko3d %s %s: result %d: %s\n",
            result == DkResult_Success ? "warning in" : "ERROR in",
            context ? context : "?", (int)result, message ? message : "");
    log_sync();
}

static void step(const char *what)
{
    fprintf(stderr, "  [DK] bring-up: %s\n", what);
    log_sync();
}

static int rc_ok(const char *what, Result rc)
{
    if (R_SUCCEEDED(rc))
        return 1;
    fprintf(stderr, "  [DK] bring-up: %s FAILED: 0x%X (%04u-%04u)\n", what, rc,
            2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    log_sync();
    return 0;
}

/* What dkDeviceCreate does first, asked of the system directly, so that a
 * refusal is logged with its result instead of ending the program. The
 * services are reference-counted: deko3d's own initialisation finds them
 * open. Also the default window, which the swapchain configures. */
static int preflight(void)
{
    NvAddressSpace as;
    iova_t iova = 0;
    NvMap map;
    void *page;
    NWindow *win;
    u64 total = 0, used = 0;
    u32 ww = 0, wh = 0;

    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    fprintf(stderr, "  [DK] bring-up: process memory %lu MB total, %lu MB used\n",
            (unsigned long)(total >> 20), (unsigned long)(used >> 20));
    step("nv services");
    if (!rc_ok("nvInitialize", nvInitialize())
        || !rc_ok("nvMapInit", nvMapInit())
        || !rc_ok("nvFenceInit", nvFenceInit())
        || !rc_ok("nvGpuInit", nvGpuInit()))
        return 0;

    step("GPU address space");
    memset(&as, 0, sizeof as);
    if (!rc_ok("nvAddressSpaceCreate", nvAddressSpaceCreate(&as, 0x10000)))
        return 0;
    if (!rc_ok("nvAddressSpaceAlloc (4 GB code segment)",
               nvAddressSpaceAlloc(&as, false, 0x100000000ull, &iova))) {
        nvAddressSpaceClose(&as);
        return 0;
    }
    nvAddressSpaceFree(&as, iova, 0x100000000ull);
    nvAddressSpaceClose(&as);

    step("a page of heap for the GPU");
    page = aligned_alloc(0x1000, 0x1000);
    if (!page) {
        fprintf(stderr, "  [DK] bring-up: no 4 KB of heap left\n");
        log_sync();
        return 0;
    }
    memset(&map, 0, sizeof map);
    if (!rc_ok("nvMapCreate", nvMapCreate(&map, page, 0x1000, 0x10000, NvKind_Pitch, false))) {
        free(page);
        return 0;
    }
    nvMapClose(&map);
    free(page);

    step("default window");
    win = nwindowGetDefault();
    if (!win || !nwindowIsValid(win)) {
        fprintf(stderr, "  [DK] bring-up: the default window is not set up\n");
        log_sync();
        return 0;
    }
    if (rc_ok("nwindowGetDimensions", nwindowGetDimensions(win, &ww, &wh)))
        fprintf(stderr, "  [DK] bring-up: default window %ux%u\n", ww, wh);
    return 1;
}

/* s_cmd ran out of memory: give it the next chunk of this frame's slice.
 * flush_if_low keeps it from ever reaching the last one.
 *
 * Every chunk is handed out short of its last CMD_GUARD bytes. deko3d's
 * dkCmdBufBindVtxBuffers reserves 5 words a buffer and writes 6 (two
 * commands of a header and a 2-word address each; still so upstream), and
 * its writer does not check past the reservation: with 80 to 95 words left
 * in a chunk, binding our 16 buffers ran up to 16 words past the chunk's
 * end -- into the next chunk, which the next command then took and wrote
 * over from its start. The GPU read the first chunk's last command, a
 * VERTEX_ARRAY_LIMIT, and took the next chunk's first header for its
 * address. 99th Street, three seconds in, every time: the first frame heavy
 * enough (about a thousand draws) to fill a chunk, and the same commands
 * every run so the same boundary. deko3d's checking library reported "GPU
 * rejected command list" one run and "GPU method error, [b197:7da] =
 * 0x200205f2" (an INDEX_ARRAY_START header as that address) the next. The
 * overrun now lands in the guard, which nothing else writes. */
static void cmd_add_mem(void *user, DkCmdBuf cmd, size_t min)
{
    uint32_t off;
    (void)user;
    if (min > CMD_CHUNK - CMD_GUARD || s_chunk >= CMD_CHUNKS) {
        fprintf(stderr, "  [DK] command memory exhausted (%u chunks, %zu wanted)\n",
                s_chunk, min);
        fflush(stderr);
        s_chunk = CMD_CHUNKS - 1;       /* reuse the last: wrong, but no abort */
    }
    off = (s_slot * CMD_CHUNKS + s_chunk++) * CMD_CHUNK;
    dkCmdBufAddMemory(cmd, s_cmdmem, off, CMD_CHUNK - CMD_GUARD);
}

/* Record from the start of this frame's slice. dkCmdBufClear alone does not
 * do it: it rewinds to the start of the chunk added last -- the previous
 * frame's, which the GPU may not have read yet (the first console run: frame
 * 2 written over frame 1's commands, "GPU rejected command list"). */
static void cmd_rewind(void)
{
    dkCmdBufClear(s_cmd);
    dkCmdBufAddMemory(s_cmd, s_cmdmem, s_slot * CMD_CHUNKS * CMD_CHUNK, CMD_CHUNK - CMD_GUARD);
    s_chunk = 1;
}

static void gpu_map_dump(void);

/* deko3d ends the program when a queue in error state is used again, so
 * every use asks first. A faulted queue switches the renderer off: the game
 * runs on (sound, input, the log) and deko3d's report is already logged --
 * with its checking library (DK_DEBUG=1) only: the release library says
 * nothing, so the line says which to build. Then the map of the renderer's
 * GPU memory, so a page fault's address names what was being read. */
static int queue_failed(const char *where)
{
    if (!dkQueueIsInErrorState(s_queue))
        return 0;
    if (s_up > 0) {
#if defined(NV2A_DK_CHECKED)
        fprintf(stderr, "  [DK] the GPU stopped the queue (seen in %s) after %u frames,"
                        " %u draws: the renderer is off from here; deko3d's report is above\n",
                where, s_frames, s_draws);
#else
        fprintf(stderr, "  [DK] the GPU stopped the queue (seen in %s) after %u frames,"
                        " %u draws: the renderer is off from here. The release deko3d"
                        " library does not say why: build with DK_DEBUG=1 for its report"
                        " (the faulting address)\n", where, s_frames, s_draws);
#endif
        gpu_map_dump();
        log_sync();
    }
    s_up = -1;
    return 1;
}

static void release_deferred(int all);
static void data_clean(void);
static void frame_state(void);
static void pack_take(void);
static void rescale_all(void);
static void dyn_note(uint64_t ns, uint32_t draws);

/* Submit what is recorded so far, wait for the GPU to finish everything,
 * and go on recording from the start of this frame's slice. Before freeing
 * an image, and when a slice runs low. Everything the GPU held is free after
 * it, so the data slice starts over too. */
static void sync_gpu_why(int why)
{
    uint64_t t0 = armGetSystemTick();
    DkCmdList list = dkCmdBufFinishList(s_cmd);
    s_st.syncs[why]++;
    if (queue_failed("sync"))
        return;
    data_clean();
    dkQueueSubmitCommands(s_queue, list);
    dkQueueWaitIdle(s_queue);
    if (queue_failed("sync"))
        return;
    cmd_rewind();
    s_data_off = 0;
    s_data_clean = 0;
    release_deferred(1);
    s_st.t_sync += armGetSystemTick() - t0;
}
#define sync_gpu() sync_gpu_why(SYNC_CMD)

static void flush_if_low(void)
{
    if (s_chunk >= CMD_CHUNKS - 1)
        sync_gpu_why(SYNC_CMD);
}

/* RECOMP_DK_CAPTURE=<s>: every <s> seconds one whole frame's draws are
 * written to the log -- shaders, combiner key and constants, textures,
 * blend, depth, lighting, a vertex -- to find what a wrong-looking screen
 * is made of (at most 40 frames, 300 draws each). */
static uint32_t s_cap_period = ~0u, s_caps, s_cap_draws;
static int      s_cap;

/* GPU time per render target: a timestamp each time the target changes
 * (bind_targets), in the timestamp block after the frame ones, so a frame
 * whose GPU time is over RECOMP_DK_GPU_SLOW_MS (default 16; 0 = off) gets a
 * [gpu] line naming the targets that took it. A timestamp is written once
 * the work before it is done: pass k runs from its own to the next one (the
 * last to the frame's end). Pass 0 is the frame's start up to the first
 * bind. */
#define TS_PASS_OFF 256u
#define TS_PASS_MAX 48u
typedef struct { uint32_t va, w, h, pw, ph, draws; } DkPass;
static DkPass   s_pass[4][TS_PASS_MAX];
static uint32_t s_npass[4], s_pass_frame[4], s_pass_end_draws[4];
static int      s_gpu_slow_ms = -1;
static uint32_t s_gpu_slow_lines;

static void pass_mark(uint32_t va, uint32_t w, uint32_t h, uint32_t pw, uint32_t ph)
{
    uint32_t n = s_npass[s_slot];
    DkPass *p;
    if (!s_ts_cpu || n >= TS_PASS_MAX)
        return;
    if (n > 0)
        dkCmdBufReportCounter(s_cmd, DkCounter_Timestamp,
                              s_ts_gpu + TS_PASS_OFF + (s_slot * TS_PASS_MAX + n) * 16u);
    p = &s_pass[s_slot][n];
    p->va = va; p->w = w; p->h = h; p->pw = pw; p->ph = ph; p->draws = s_draws;
    s_npass[s_slot] = n + 1;
}

static void pass_report(int slot, uint64_t frame_ns)
{
    struct { uint32_t va, w, h, pw, ph, draws, n; uint64_t ns; } agg[TS_PASS_MAX];
    uint32_t k, j, na = 0, n = s_npass[slot];
    char line[1024];
    int len;

    if (s_gpu_slow_ms < 0) {
        const char *e = getenv("RECOMP_DK_GPU_SLOW_MS");
        s_gpu_slow_ms = e ? atoi(e) : 16;
        if (s_gpu_slow_ms > 0)
            fprintf(stderr, "  [DK] [gpu] lines for frames whose GPU time is over %d ms, by"
                            " render target (RECOMP_DK_GPU_SLOW_MS; 0 = off)\n", s_gpu_slow_ms);
    }
    if (s_gpu_slow_ms <= 0 || n == 0 || frame_ns <= (uint64_t)s_gpu_slow_ms * 1000000ull
        || s_gpu_slow_lines >= 300)
        return;
    for (k = 0; k < n; k++) {
        const DkPass *p = &s_pass[slot][k];
        uint64_t a = k ? s_ts_cpu[(TS_PASS_OFF / 8) + (slot * TS_PASS_MAX + k) * 2 + 1]
                       : s_ts_cpu[slot * 4 + 1];
        uint64_t b = k + 1 < n ? s_ts_cpu[(TS_PASS_OFF / 8) + (slot * TS_PASS_MAX + k + 1) * 2 + 1]
                               : s_ts_cpu[slot * 4 + 3];
        uint32_t d = (k + 1 < n ? s_pass[slot][k + 1].draws : s_pass_end_draws[slot]) - p->draws;
        uint64_t ns = b > a ? dkTimestampToNs(b - a) : 0;
        for (j = 0; j < na && agg[j].va != p->va; j++)
            ;
        if (j == na) {
            agg[na].va = p->va; agg[na].w = p->w; agg[na].h = p->h;
            agg[na].pw = p->pw; agg[na].ph = p->ph;
            agg[na].draws = 0; agg[na].n = 0; agg[na].ns = 0;
            na++;
        }
        agg[j].draws += d; agg[j].n++; agg[j].ns += ns;
    }
    /* Longest first. */
    for (k = 1; k < na; k++)
        for (j = k; j > 0 && agg[j].ns > agg[j - 1].ns; j--) {
            __typeof__(agg[0]) t = agg[j]; agg[j] = agg[j - 1]; agg[j - 1] = t;
        }
    len = snprintf(line, sizeof line, "[gpu] frame %u: %.1f ms, %u passes%s:", s_pass_frame[slot],
                   frame_ns / 1e6, n, n >= TS_PASS_MAX ? " (the rest in the last)" : "");
    for (k = 0; k < na && k < 8 && len < (int)sizeof line - 96; k++) {
        if (agg[k].va == 0 && agg[k].w == 0)
            len += snprintf(line + len, sizeof line - len, " start %.1f ms %u draws;",
                            agg[k].ns / 1e6, agg[k].draws);
        else
            len += snprintf(line + len, sizeof line - len,
                            " 0x%08X %ux%u (%ux%u) %.1f ms %u draws %u binds;",
                            agg[k].va, agg[k].w, agg[k].h, agg[k].pw, agg[k].ph,
                            agg[k].ns / 1e6, agg[k].draws, agg[k].n);
    }
    fprintf(stderr, "%s\n", line);
    s_gpu_slow_lines++;
}

/* Start recording the next frame: its slices must be free of the GPU. */
static void frame_begin(void)
{
    uint64_t t0 = armGetSystemTick();
    if (s_cap_period == ~0u) {
        const char *e = getenv("RECOMP_DK_CAPTURE");
        s_cap_period = e && atoi(e) > 0 ? (uint32_t)atoi(e) * 60u : 0u;
    }
    s_cap = s_cap_period && s_frames && s_frames % s_cap_period == 0 && s_caps < 40;
    if (s_cap) {
        s_caps++;
        s_cap_draws = 0;
        fprintf(stderr, "  [CAP] frame %u (capture %u of 40)\n", s_frames, s_caps);
    }
    s_slot = (s_slot + 1) % DK_FRAMES;
    if (s_fence_set[s_slot])
        dkFenceWait(&s_fence[s_slot], -1);
    s_fence_set[s_slot] = 0;
    s_st.t_fence += armGetSystemTick() - t0;
    if (s_ts_cpu && s_ts_set[s_slot]) {
        /* {value, timestamp} pairs: [slot*4 + 1] start, [slot*4 + 3] end */
        uint64_t a = s_ts_cpu[s_slot * 4 + 1], b = s_ts_cpu[s_slot * 4 + 3];
        if (b > a && b - a < dkNsToTimestamp(2000000000ull)) {
            uint64_t ns = dkTimestampToNs(b - a);
            s_st.gpu_ns += ns;
            s_st.gpu_frames++;
            dyn_note(ns, s_slot_draws[s_slot]);    /* may ask for a new scale */
            pass_report(s_slot, ns);
        }
        s_ts_set[s_slot] = 0;
    }
    s_npass[s_slot] = 0;
    s_draws_at_begin = s_draws;
    cmd_rewind();
    s_data_off = 0;
    s_data_clean = 0;
    release_deferred(0);
    if (s_scale_want != s_scale)
        rescale_all();                  /* docked or undocked: before any draw */
    frame_state();
    pack_take();                        /* shaders the console just built */
    if (s_ts_cpu) {
        dkCmdBufReportCounter(s_cmd, DkCounter_TimestampPipelineTop, s_ts_gpu + s_slot * 32);
        s_pass_frame[s_slot] = s_frames;
        pass_mark(0, 0, 0, 0, 0);       /* pass 0: up to the first bind */
    }
}

/* `size` bytes of this frame's data slice. May sync (then everything
 * allocated earlier and not yet referenced by recorded commands is gone):
 * allocate a draw's data in one call. */
static uint8_t *data_alloc(uint32_t size, uint32_t align, DkGpuAddr *gpu)
{
    uint32_t at;
    if (size > DATA_SLICE)
        return NULL;
    at = align_up(s_data_off, align);
    if (at + size > DATA_SLICE) {
        sync_gpu_why(SYNC_DATA);
        if (s_up <= 0)
            return NULL;
        at = 0;
    }
    s_data_off = at + size;
    at += s_slot * DATA_SLICE;
    *gpu = s_data_gpu + at;
    return s_data_cpu + at;
}

/* Before a submit: what the CPU wrote into this slice since the last one
 * goes out of its cache, so the GPU reads it (cached data ring only). */
static void data_clean(void)
{
    if (s_data_cached > 0 && s_data_off > s_data_clean) {
        uint64_t t0 = armGetSystemTick();
        dkMemBlockFlushCpuCache(s_datamem, s_slot * DATA_SLICE + s_data_clean,
                                s_data_off - s_data_clean);
        s_data_clean_ticks += armGetSystemTick() - t0;
    }
    s_data_clean = s_data_off;
}

/* ── Shaders, uniform buffers, descriptors ─────────────────────────── */

typedef struct {
    uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
} DkshHeader;
typedef struct {
    uint32_t type, entrypoint, num_gprs, constbuf1_off, constbuf1_sz, per_warp_scratch_sz;
} DkshProgramHeader;

static DkMemBlock s_codemem;
static DkShader   s_vs, s_fs, s_vs_fixed;   /* s_vs_fixed: batches without a program */
/* s_vs_fixed built with NV2A_TEXGEN: only for the fixed-function draws that
 * generate texture coordinates or use a texture matrix (tex_push), so the
 * rest keep the smaller shader. */
static DkShader   s_vs_fixed_tg;

/* The uniform buffers, at fixed offsets in one block. */
#define UBO_CONSTS     0x0000u          /* vertex: c[192] */
#define UBO_CONSTS_SZ  0x0C00u
#define UBO_PROG       0x0C00u          /* vertex: decoded program */
#define UBO_PROG_SZ    0x0900u
#define UBO_VSD        0x1600u          /* vertex: per-draw */
#define UBO_VSD_SZ     0x0200u
#define UBO_FSD        0x1800u          /* fragment: per-draw */
#define UBO_FSD_SZ     0x0300u
#define UBO_LIGHT      0x1B00u          /* vertex: fixed-function lighting */
#define UBO_LIGHT_SZ   0x0300u
#define UBO_TEX        0x1E00u          /* vertex: texgen, texture matrices */
#define UBO_TEX_SZ     0x0200u
#define UBO_TOTAL      0x2000u
static DkMemBlock s_ubomem;
static DkGpuAddr  s_ubo;

typedef struct {                        /* VsDraw, std140 */
    float    surf[4];
    float    m[16];
    float    vpoff[4];
    float    aa[4];
    uint32_t mode[4];
    uint32_t flags[4];
    float    attr[16][4];
    float    fogp[4];                   /* SET_FOG_PARAMS 0, 1 */
    uint32_t fogm[4];                   /* FOG_ENABLE, FOG_MODE, FOG_GEN_MODE */
    float    fogpl[4];                  /* SET_FOG_PLANE */
} UboVsDraw;
_Static_assert(sizeof(UboVsDraw) <= UBO_VSD_SZ, "VsDraw fits its slot");

typedef struct {                        /* VsLight, std140 (nv2a_vs_fixed.glsl) */
    float    mv[4][4];
    float    scene[4];
    uint32_t ctl[4];
    float    amb[8][4], dif[8][4], dir[8][4], pos[8][4], att[8][4];
} UboVsLight;
_Static_assert(sizeof(UboVsLight) <= UBO_LIGHT_SZ, "VsLight fits its slot");

typedef struct {                        /* VsTex, std140 (nv2a_vs_fixed.glsl) */
    float    mat[16][4];                /* SET_TEXTURE_MATRIX, 4 rows a stage */
    uint32_t gen[4][4];                 /* per stage S T R Q: 0, 1 normal, 2 reflection, 3 sphere */
    uint32_t ctl[4];                    /* matrix mask, normalize, generating stages */
} UboVsTex;
_Static_assert(sizeof(UboVsTex) <= UBO_TEX_SZ, "VsTex fits its slot");
_Static_assert(UBO_TEX + UBO_TEX_SZ <= UBO_TOTAL, "VsTex inside the block");

typedef struct {                        /* FsDraw, std140 */
    float    tscale[4][4];
    float    c0[8][4];
    float    c1[8][4];
    float    fc0[4], fc1[4], fog[4];
    uint32_t cicw[8], cocw[8], aicw[8], aocw[8];
    uint32_t ctl[4];
    uint32_t ctl2[4];
} UboFsDraw;

static float     s_sh_consts[192][4];   /* what the GPU's buffers hold */
static uint32_t  s_sh_prog[136][4];
static UboVsDraw s_sh_vsd;
static UboFsDraw s_sh_fsd;
static uint32_t  s_prog_gen_pushed, s_const_gen_pushed, s_prog_versions;
static uint32_t  s_prog_lo, s_prog_hi, s_prog_writes_c;
/* The pack's shaders for the current program and combiners (NULL: the
 * ubershader), and what is bound. */
static const DkShader *s_vs_spec, *s_fs_spec, *s_cur_vs, *s_cur_fs;
static uint64_t  s_vs_key;              /* the current program's (0: none) */
/* The same setup with the alpha test compiled out (DK_KEY_FS_NOAT), for
 * draws with the test off: no discard keeps the early depth test. */
static const DkShader *s_fs_spec_noat;
static uint32_t  s_fs_words[DK_FS_KEY_WORDS];
static int       s_fs_words_valid;


/* Image descriptors: textures (with room for ones released late), then one
 * per surface, then the 1x1 stand-in. Samplers: a table of combinations. */
#define TEX_MAX       1024              /* texture cache entries */
#define TEXDESC_N     2048              /* their descriptors */
#define DK_MAX_SURF   32
#define DK_MAX_DEPTH  16
#define DESC_SURF0    TEXDESC_N
#define DESC_DUMMY    (TEXDESC_N + DK_MAX_SURF)
#define IMGDESC_N     (DESC_DUMMY + 1)
#define SAMP_N        128
static DkMemBlock s_descmem;
static DkImageDescriptor   *s_imgdesc;
static DkSamplerDescriptor *s_sampdesc;
static DkGpuAddr  s_imgdesc_gpu, s_sampdesc_gpu;
static int        s_desc_dirty;         /* descriptors written: invalidate before the next draw */
static int        s_upload_dirty;       /* copies into images: barrier before the next draw */

static uint32_t   s_samp_key[SAMP_N];
static uint32_t   s_samp_border[SAMP_N];    /* ARGB, for clamp-to-border */
static int        s_samp_n;

static DkMemBlock s_dummymem;
static DkImage    s_dummy;

static int shader_load(DkShader *sh, const uint8_t *blob, uint32_t *code_at)
{
    const DkshHeader *h = (const DkshHeader *)blob;
    DkShaderMaker m;
    if (h->magic != 0x48534B44u)
        return 0;
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(s_codemem) + *code_at, blob + h->control_sz, h->code_sz);
    dkShaderMakerDefaults(&m, s_codemem, *code_at);
    m.control = blob;
    dkShaderInitialize(sh, &m);
    *code_at = align_up(*code_at + h->code_sz, DK_SHADER_CODE_ALIGNMENT);
    return 1;
}

static uint32_t shader_scratch(const uint8_t *blob)
{
    const DkshHeader *h = (const DkshHeader *)blob;
    const DkshProgramHeader *p = (const DkshProgramHeader *)(blob + h->programs_off);
    return p->per_warp_scratch_sz;
}

/* ── The shader pack and the key file (nv2a_dk_pack.h) ─────────────── */

#define PACK_MAX       4096
#define PACK_HASH      8192             /* power of two */
#define PACK_SCRATCH_MAX (64u << 10)    /* per warp: more is skipped */
/* Code memory for shaders the console builds while it runs (dk_compile.c):
 * JSRF's whole pack is 1.1 MB of code. */
#define PACK_GROW      (4u << 20)
typedef struct {
    uint64_t key;
    uint32_t kind, off;                 /* off: DKSH module in s_pack_buf */
    DkShader sh;
} PackShader;
static uint8_t    *s_pack_buf;
static PackShader *s_pk;                /* PACK_MAX entries: the compiler adds to it */
static uint32_t    s_npk, s_pk_vs, s_pk_fs;
static int32_t     s_pk_slot[PACK_HASH];    /* index + 1 */
static uint32_t    s_code_at, s_code_cap;   /* code memory used and its size */
static uint32_t    s_queue_scratch;     /* the queue's scratch per warp */

static const char *dk_dir(void)
{
#ifndef JSRF_SD_HOME
#define JSRF_SD_HOME "sdmc:/switch/jsrf"
#endif
    const char *d = getenv("RECOMP_DK_DIR");
    return d && *d ? d : JSRF_SD_HOME;
}

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *p;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *size = p ? (uint32_t)n : 0;
    return p;
}

/* Read dk_shaders.pak: entries whose modules check out. Returns the code
 * bytes they need; *scratch_max their largest scratch. */
static uint32_t pack_read(uint32_t *scratch_max)
{
    char path[256];
    uint32_t size, at = 12, i, count, code = 0;
    uint8_t *p;

    s_pk = (PackShader *)calloc(PACK_MAX, sizeof *s_pk);
    if (!s_pk)
        return 0;
    snprintf(path, sizeof path, "%s/dk_shaders.pak", dk_dir());
    p = read_file(path, &size);
    if (!p) {
        fprintf(stderr, "  [DK] no shader pack (%s) yet: the ubershaders draw until the"
                        " console has built it\n", path);
        return 0;
    }
    if (size < 12 || ((uint32_t *)p)[0] != DK_PACK_MAGIC || ((uint32_t *)p)[1] != DK_PACK_VERSION) {
        fprintf(stderr, "  [DK] %s is not a version %u shader pack; ignored\n", path, DK_PACK_VERSION);
        free(p);
        return 0;
    }
    count = ((uint32_t *)p)[2];
    {
        /* Made by these generators? (nv2a_dk_pack.h) Else its shaders may
         * carry a bug since fixed: the console builds the pack anew. */
        uint64_t stamp = 0;
        if (count && size >= 28 && ((uint32_t *)p)[3] == DK_PACK_STAMP)
            memcpy(&stamp, p + 20, 8);
        if (DK_GEN_STAMP && stamp != DK_GEN_STAMP && dk_compile_enabled()) {
            fprintf(stderr, "  [DK] %s was made by another version of the shader generators"
                            " (stamp %016llx, this NRO's %016llx): not used; the console builds"
                            " it anew\n", path, (unsigned long long)stamp,
                    (unsigned long long)DK_GEN_STAMP);
            free(p);
            return 0;
        }
    }
    for (i = 0; i < count && s_npk < PACK_MAX && at + 16 <= size; i++) {
        uint32_t kind, n;
        uint64_t key;
        const DkshHeader *h;
        const DkshProgramHeader *ph;
        memcpy(&kind, p + at, 4);
        memcpy(&n, p + at + 4, 4);
        memcpy(&key, p + at + 8, 8);
        if (at + 16 + n > size)
            break;
        h = (const DkshHeader *)(p + at + 16);
        ph = (const DkshProgramHeader *)(p + at + 16 + h->programs_off);
        if (kind <= 1 && n >= 256 && !((at + 16) & 3) && h->magic == 0x48534B44u
            && h->control_sz + h->code_sz <= n && h->programs_off + sizeof *ph <= h->control_sz
            && ph->type == kind && ph->per_warp_scratch_sz <= PACK_SCRATCH_MAX) {
            PackShader *e = &s_pk[s_npk++];
            e->key = key;
            e->kind = kind;
            e->off = at + 16;
            code += align_up(h->code_sz, DK_SHADER_CODE_ALIGNMENT);
            if (ph->per_warp_scratch_sz > *scratch_max)
                *scratch_max = ph->per_warp_scratch_sz;
            if (kind) s_pk_fs++; else s_pk_vs++;
        }
        at += 16 + n;
    }
    s_pack_buf = p;
    fprintf(stderr, "  [DK] shader pack: %u vertex + %u fragment shaders (%u KB of code)\n",
            s_pk_vs, s_pk_fs, code >> 10);
    return code;
}

static void pack_insert(uint32_t i)
{
    uint32_t slot = (uint32_t)(s_pk[i].key ^ (s_pk[i].key >> 29)) & (PACK_HASH - 1);
    while (s_pk_slot[slot])
        slot = (slot + 1) & (PACK_HASH - 1);
    s_pk_slot[slot] = (int32_t)i + 1;
}

/* The pack's shaders into code memory, and the lookup table. */
static void pack_load(uint32_t *code_at)
{
    uint32_t i;
    for (i = 0; i < s_npk; i++) {
        PackShader *e = &s_pk[i];
        if (shader_load(&e->sh, s_pack_buf + e->off, code_at))
            pack_insert(i);
    }
    free(s_pack_buf);                   /* deko3d copied what it keeps */
    s_pack_buf = NULL;
}

static const DkShader *pack_find(uint32_t kind, uint64_t key)
{
    uint32_t slot = (uint32_t)(key ^ (key >> 29)) & (PACK_HASH - 1);
    while (s_pk_slot[slot]) {
        PackShader *e = &s_pk[s_pk_slot[slot] - 1];
        if (e->key == key && e->kind == kind)
            return &e->sh;
        slot = (slot + 1) & (PACK_HASH - 1);
    }
    return NULL;
}

/* One shader the console built (dk_compile.c), into spare code memory and
 * the lookup table. 0 if it cannot be used this run: its scratch is more
 * than the queue was made with, or the spare code memory is used up (the
 * next start reads it from the pack with the rest). */
static int pack_add(uint32_t kind, uint64_t key, const uint8_t *blob, uint32_t n)
{
    static int said_scratch, said_full;
    const DkshHeader *h = (const DkshHeader *)blob;
    const DkshProgramHeader *ph;
    PackShader *e;
    if (kind > 1 || n < 256 || h->magic != 0x48534B44u || h->control_sz > n
        || h->code_sz > n - h->control_sz || h->programs_off + sizeof *ph > h->control_sz)
        return 0;
    ph = (const DkshProgramHeader *)(blob + h->programs_off);
    if (ph->type != kind || pack_find(kind, key))
        return 0;
    if (ph->per_warp_scratch_sz > s_queue_scratch) {
        if (!said_scratch++)
            fprintf(stderr, "  [DK] a built shader needs %u bytes of scratch per warp (the queue"
                            " has %u): used from the next start\n",
                    ph->per_warp_scratch_sz, s_queue_scratch);
        return 0;
    }
    if (s_npk >= PACK_MAX || s_code_at + align_up(h->code_sz, DK_SHADER_CODE_ALIGNMENT) > s_code_cap) {
        if (!said_full++)
            fprintf(stderr, "  [DK] no room for more built shaders this run (%u loaded, %u KB of"
                            " code): the next start loads them from the pack\n",
                    s_npk, s_code_at >> 10);
        return 0;
    }
    e = &s_pk[s_npk];
    e->key = key;
    e->kind = kind;
    e->off = 0;
    if (!shader_load(&e->sh, blob, &s_code_at))
        return 0;
    pack_insert(s_npk++);
    if (kind) s_pk_fs++; else s_pk_vs++;
    return 1;
}

/* At the start of a frame: what the compiler has finished since the last,
 * taken into use. The draws that were waiting for one look it up again. */
static void pack_take(void)
{
    uint32_t kind, size, n = 0;
    uint64_t key;
    uint8_t *blob;
    while (n < 64 && dk_compile_take(&kind, &key, &blob, &size)) {
        if (pack_add(kind, key, blob, size)) {
            n++;
            if (kind == 0 && key == s_vs_key)
                s_vs_spec = pack_find(0, key);
        }
        free(blob);
    }
    if (!n)
        return;
    s_fs_words_valid = 0;               /* the combiners are looked up again */
    s_st.built += n;
    /* New code: nothing has run from those addresses, but the shader caches
     * are told anyway, as deko3d asks after code memory is written. */
    dkCmdBufBarrier(s_cmd, DkBarrier_None, DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
}

/* Programs and combiner setups already asked of the compiler this run, so
 * each is asked once (seen_add below is for the key file, and holds keys
 * from earlier runs too). */
#define ASKED_N 8192
static uint64_t s_asked[ASKED_N];
static uint32_t s_nasked;

static int asked_add(uint64_t key)
{
    uint32_t slot = (uint32_t)(key ^ (key >> 31)) & (ASKED_N - 1);
    while (s_asked[slot]) {
        if (s_asked[slot] == key)
            return 0;
        slot = (slot + 1) & (ASKED_N - 1);
    }
    if (s_nasked >= ASKED_N * 3 / 4)
        return 0;
    s_asked[slot] = key;
    s_nasked++;
    return 1;
}

/* The compiler started: the pack's keys, and where the pack, the key file
 * and the game's XBE are (JSRF_GAME_DIR / RECOMP_GAME_DIR as the game reads
 * them, else <dk_dir>/game). */
static void compiler_start(void)
{
    char pack[256], keys[256], xbe[256];
    const char *g = getenv("JSRF_GAME_DIR");
    uint64_t *have = (uint64_t *)malloc((s_npk ? s_npk : 1) * sizeof *have);
    uint32_t i;
    if (!have)
        return;
    for (i = 0; i < s_npk; i++)
        have[i] = s_pk[i].key;
    if (!g || !*g)
        g = getenv("RECOMP_GAME_DIR");
    snprintf(pack, sizeof pack, "%s/dk_shaders.pak", dk_dir());
    snprintf(keys, sizeof keys, "%s/dk_keys.bin", dk_dir());
    if (g && *g)
        snprintf(xbe, sizeof xbe, "%s/default.xbe", g);
    else
        snprintf(xbe, sizeof xbe, "%s/game/default.xbe", dk_dir());
    dk_compile_start(have, s_npk, pack, keys, xbe);
    free(have);
}

/* Keys met that the pack lacks, appended to dk_keys.bin for dk_pack. Each
 * is written once: the set holds the file's keys from earlier runs too. */
#define SEEN_N 16384
static uint64_t  s_seen[SEEN_N];
static uint32_t  s_nseen;
static uint32_t *s_keybuf;
static uint32_t  s_keylen, s_keycap, s_keys_new;
static int       s_keys_on = -1;

static int seen_add(uint64_t key)
{
    uint32_t slot = (uint32_t)(key ^ (key >> 31)) & (SEEN_N - 1);
    while (s_seen[slot]) {
        if (s_seen[slot] == key)
            return 0;
        slot = (slot + 1) & (SEEN_N - 1);
    }
    if (s_nseen >= SEEN_N * 3 / 4)
        return 0;                       /* full: stop recording */
    s_seen[slot] = key;
    s_nseen++;
    return 1;
}

static void keys_load(void)
{
    char path[256];
    uint32_t size, n, at = 0, *w;
    uint8_t *p;
    uint32_t i;

    if (s_keys_on < 0) {
        const char *e = getenv("RECOMP_DK_KEYS");
        s_keys_on = !(e && *e == '0');
    }
    for (i = 0; i < s_npk; i++)
        seen_add(s_pk[i].key);
    snprintf(path, sizeof path, "%s/dk_keys.bin", dk_dir());
    p = read_file(path, &size);
    if (!p)
        return;
    w = (uint32_t *)p;
    n = size / 4;
    while (at + 3 <= n) {
        if (w[at] == DK_KEY_VS && w[at + 2] <= 136 && w[at + 1] < 136 && at + 3 + w[at + 2] * 4 <= n) {
            static uint32_t prog[136][4];
            uint32_t start = w[at + 1], cnt = w[at + 2];
            if (start + cnt <= 136) {
                memcpy(prog[start], &w[at + 3], (size_t)cnt * 16);
                seen_add(dk_vs_key(start, cnt, (const uint32_t (*)[4])prog));
            }
            at += 3 + cnt * 4;
        } else if (w[at] == DK_KEY_FS && w[at + 1] == DK_FS_KEY_WORDS
                   && at + 2 + DK_FS_KEY_WORDS <= n) {
            seen_add(dk_key_hash(DK_KEY_FS, &w[at + 2], DK_FS_KEY_WORDS));
            at += 2 + DK_FS_KEY_WORDS;
        } else {
            break;
        }
    }
    free(p);
    fprintf(stderr, "  [DK] %s: %u keys recorded before\n", path, s_nseen - s_npk);
}

static void key_put(const uint32_t *w, uint32_t n)
{
    if (s_keylen + n > s_keycap) {
        uint32_t cap = (s_keylen + n) * 2 + 1024;
        uint32_t *nb = (uint32_t *)realloc(s_keybuf, cap * 4);
        if (!nb)
            return;
        s_keybuf = nb;
        s_keycap = cap;
    }
    memcpy(s_keybuf + s_keylen, w, (size_t)n * 4);
    s_keylen += n;
    s_keys_new++;
}

static void keys_flush(void)
{
    char path[256];
    FILE *f;
    if (!s_keylen)
        return;
    snprintf(path, sizeof path, "%s/dk_keys.bin", dk_dir());
    f = fopen(path, "ab");
    if (f) {
        fwrite(s_keybuf, 4, s_keylen, f);
        fclose(f);
    }
    s_keylen = 0;
}

static void key_vs(uint32_t start, uint32_t n, const uint32_t (*prog)[4], uint64_t key)
{
    uint32_t hdr[3] = { DK_KEY_VS, start, n };
    if (!s_keys_on || !seen_add(key))
        return;
    key_put(hdr, 3);
    key_put(&prog[start][0], n * 4);
}

static void key_fs(const uint32_t *k, uint64_t key)
{
    uint32_t hdr[2] = { DK_KEY_FS, DK_FS_KEY_WORDS };
    if (!s_keys_on || !seen_add(key))
        return;
    key_put(hdr, 2);
    key_put(k, DK_FS_KEY_WORDS);
}

static DkMemBlock block(uint32_t size, uint32_t flags)
{
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, s_dev, align_up(size, DK_MEMBLOCK_ALIGNMENT));
    mm.flags = flags;
    return dkMemBlockCreate(&mm);
}

/* ── The screen and the internal resolution ────────────────────────── */

/* The size the system shows the layer at. Asked of the system each time
 * (appletGetOperationMode's cached answer needs the applet messages
 * pumped, which nothing does under deko3d): 1920x1080 docked to a 1080p
 * TV, 1280x720 in the hand or on a 720p TV. */
static void screen_want(uint32_t *w, uint32_t *h)
{
    s32 dw = 0, dh = 0;
    int big;
    if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&dw, &dh)) && dh > 0)
        big = dh >= 1080;
    else
        big = appletGetOperationMode() == AppletOperationMode_Console;
    *w = big ? 1920 : 1280;
    *h = big ? 1080 : 720;
}

/* RECOMP_DK_SCALE: "auto" (default) dynamic, from x1 up to the screen's
 * height, held where the GPU keeps up (dyn_note); "max" the screen's height
 * whatever it costs; a number, fixed. */
static void scale_mode_read(void)
{
    const char *e;
    if (s_scale_mode >= 0)
        return;
    e = getenv("RECOMP_DK_SCALE");
    /* The screen's height unless asked otherwise: 720 lines in the hand,
     * 1080 docked, whatever the GPU time (the dynamic mode dropped to the
     * title's 480 lines and stayed there after one heavy scene). */
    s_scale_mode = DYN_MAX;
    if (e && *e && !strcmp(e, "auto")) {
        s_scale_mode = DYN_AUTO;
    } else if (e && *e && strcmp(e, "max") != 0) {
        double v = strtod(e, NULL);
        if (v < 1.0) v = 1.0;
        if (v > 4.0) v = 4.0;
        s_scale_fixed = (float)v;
        s_scale_mode = DYN_FIXED;
    }
    e = getenv("RECOMP_DK_GPU_MS");
    if (e && atof(e) >= 4.0 && atof(e) <= 33.0)
        s_dyn_budget_ms = atof(e);
}

static const char *scale_mode_name(void)
{
    return s_scale_mode == DYN_FIXED ? "fixed (RECOMP_DK_SCALE)"
         : s_scale_mode == DYN_MAX   ? "the screen's height"
         : "dynamic, up to the screen's height (RECOMP_DK_SCALE=auto)";
}

static void dyn_restart(void)
{
    s_dyn_st.ns = s_dyn_st.up_ns = 0;
    s_dyn_st.n = s_dyn_st.up_n = 0;
    s_dyn_st.since_change = 0;
    s_dyn_st.peak_ms = 0.0;
}

/* The internal resolution wanted on a screen `screen_h` lines tall. A new
 * height (docked, undocked) starts the dynamic level at the top again. */
static float scale_for_screen(uint32_t screen_h)
{
    float top = (float)screen_h / 480.0f;
    scale_mode_read();
    if (top < 1.0f)
        top = 1.0f;
    if (top != s_scale_ceil) {
        s_scale_ceil = top;
        s_dyn = top;
        dyn_restart();
    }
    if (s_scale_mode == DYN_FIXED)
        return s_scale_fixed;
    if (s_scale_mode == DYN_MAX)
        return top;
    if (s_dyn < 1.0f || s_dyn > top)
        s_dyn = top;
    return s_dyn;
}

/* The steps the dynamic resolution moves in: quarters from x1 to the top,
 * and the top itself. 0 when there is none. */
static float dyn_below(float s)
{
    float l;
    if (s <= 1.0f)
        return 0.0f;
    l = floorf((s - 0.001f) * 4.0f) / 4.0f;
    return l < 1.0f ? 1.0f : l;
}

static float dyn_above(float s)
{
    float l = floorf(s * 4.0f + 0.001f) / 4.0f + 0.25f;
    if (s >= s_scale_ceil - 0.001f)
        return 0.0f;
    return l > s_scale_ceil ? s_scale_ceil : l;
}

static void dyn_step(float to, double ms, const char *why)
{
    s_dyn_st.changes++;
    if (s_dyn_st.changes <= 40)
        fprintf(stderr, "  [DK] dynamic resolution: GPU %.1f ms/frame %s -> x%.2f\n", ms, why,
                (double)to);
    s_dyn = to;
    s_scale_want = to;                  /* frame_begin makes the surfaces again */
    dyn_restart();
}

/* One frame's GPU time (and its draw count) for the dynamic resolution.
 *
 * Down a step as soon as 30 frames average over the budget (default 14 ms
 * of the 16.7 ms frame). Up a step only on evidence: 600 frames of the game
 * drawing (30 draws or more -- not a loading screen) whose average, scaled
 * by the pixel count, would be under 80% of the budget, AND the heaviest 30
 * frames seen at this level in the last 3 minutes would still fit it, and
 * not within the hold after a step down: 30 s,
 * doubled (up to 8 min) whenever a step down comes within a minute of a step
 * up. A change costs a GPU wait and a copy of every surface, so it must not
 * see-saw: on 20 Oct, judged on the average alone, it went x1 -> x1.25 at
 * 6.7 ms and back within 16 s on a 14.3 ms stretch, three times. */
static void dyn_note(uint64_t ns, uint32_t draws)
{
    double ms;
    if (s_scale_mode != DYN_AUTO || s_up <= 0)
        return;
    if (!s_dyn_st.hold) {
        s_dyn_st.hold = 1800;
        s_dyn_st.since_up = 1u << 30;   /* the start is not a step up */
    }
    if (ns > 40000000ull)
        ns = 40000000ull;               /* one stall is not a trend */
    s_dyn_st.since_change++;
    s_dyn_st.since_down++;
    s_dyn_st.since_up++;
    if (s_dyn_st.since_change < 10)
        return;                         /* the frames around a change */
    s_dyn_st.ns += ns;
    s_dyn_st.n++;
    if (draws >= 30) {
        s_dyn_st.up_ns += ns;
        s_dyn_st.up_n++;
    }
    if (s_dyn_st.n >= 30) {
        ms = (double)s_dyn_st.ns / s_dyn_st.n / 1e6;
        s_dyn_st.ns = 0;
        s_dyn_st.n = 0;
        s_dyn_st.peak_age += 30;
        if (ms >= s_dyn_st.peak_ms * 0.9) {     /* a new peak, or the old one again */
            if (ms > s_dyn_st.peak_ms)
                s_dyn_st.peak_ms = ms;
            s_dyn_st.peak_age = 0;
        } else if (s_dyn_st.peak_age > 3 * 3600) {
            s_dyn_st.peak_ms = ms;      /* not seen for 3 min: forget it */
            s_dyn_st.peak_age = 0;
        }
        if (ms > s_dyn_budget_ms && dyn_below(s_dyn) > 0.0f) {
            if (s_dyn_st.since_up < 3600 && s_dyn_st.hold < 8 * 1800)
                s_dyn_st.hold *= 2;     /* it went up too soon: wait longer next time */
            s_dyn_st.since_down = 0;
            dyn_step(dyn_below(s_dyn), ms, "over budget");
            return;
        }
    }
    if (s_dyn_st.up_n >= 600 && s_dyn_st.since_down >= s_dyn_st.hold) {
        float up = dyn_above(s_dyn);
        double r2 = up > 0.0f ? (double)(up / s_dyn) * (up / s_dyn) : 0.0;
        double peak = s_dyn_st.peak_ms;
        ms = (double)s_dyn_st.up_ns / s_dyn_st.up_n / 1e6;
        s_dyn_st.up_ns = 0;
        s_dyn_st.up_n = 0;
        if (up > 0.0f && ms * r2 < s_dyn_budget_ms * 0.8 && peak * r2 < s_dyn_budget_ms) {
            s_dyn_st.since_up = 0;
            dyn_step(up, ms, "with room");
        }
    }
}

/* Physical size of `v` title pixels at factor `sc` (an image's extent), and
 * of an edge at `v` (scissors, clears): the first pixel whose centre is past
 * it, as the rasteriser decides coverage, so rectangles that meet in the
 * title's pixels meet in ours. Both are v at x1. */
static uint32_t phys_size(uint32_t v, float sc)
{
    return (uint32_t)ceil((double)v * sc - 1e-3);
}

static uint32_t phys_edge(uint32_t v, float sc)
{
    double p = ceil((double)v * sc - 0.5 - 1e-3);
    return p > 0.0 ? (uint32_t)p : 0u;
}

/* One axis's factor for a surface `v` real pixels long with anti-aliasing
 * factor `aa`: the title's supersampling counts towards the internal
 * resolution (a 2x surface at "x2.25" is made x1.125), never below its own
 * size, and no image over 4096. */
static float axis_scale(float g, uint32_t aa, uint32_t v)
{
    float sc = g / (float)(aa ? aa : 1);
    if (sc < 1.0f) sc = 1.0f;
    if (v && (double)v * sc > 4096.0) sc = 4096.0f / (float)v;
    return sc;
}

static void swapchain_drop(void)
{
    if (s_swap) dkSwapchainDestroy(s_swap);
    if (s_fbmem) dkMemBlockDestroy(s_fbmem);
    s_swap = NULL;
    s_fbmem = NULL;
}

static int swapchain_make(uint32_t w, uint32_t h)
{
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    DkSwapchainMaker sm;
    DkImage const *imgs[SWAP_IMAGES];
    uint32_t fb_size, fb_align, i;

    dkImageLayoutMakerDefaults(&lm, s_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    dkImageLayoutInitialize(&layout, &lm);
    fb_align = dkImageLayoutGetAlignment(&layout);
    if (fb_align < DK_MEMBLOCK_ALIGNMENT)
        fb_align = DK_MEMBLOCK_ALIGNMENT;
    fb_size = align_up((uint32_t)dkImageLayoutGetSize(&layout), fb_align);
    s_fbmem = block(SWAP_IMAGES * fb_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!s_fbmem)
        return 0;
    for (i = 0; i < SWAP_IMAGES; i++) {
        dkImageInitialize(&s_fb[i], &layout, s_fbmem, i * fb_size);
        imgs[i] = &s_fb[i];
    }
    dkSwapchainMakerDefaults(&sm, s_dev, nwindowGetDefault(), imgs, SWAP_IMAGES);
    s_swap = dkSwapchainCreate(&sm);
    if (!s_swap)
        return 0;
    dkSwapchainSetSwapInterval(s_swap, 1);
    s_fb_w = w;
    s_fb_h = h;
    fprintf(stderr, "  [DK] screen %ux%u (%s), %u MB of swapchain images\n", w, h,
            h >= 1080 ? "docked, 1080p" : "handheld or a 720p TV",
            SWAP_IMAGES * fb_size >> 20);
    return 1;
}

/* Called between frames (present): a new size for the layer -- docked or
 * undocked -- means a new swapchain, and with RECOMP_DK_SCALE=auto a new
 * internal resolution, which frame_begin takes up. */
static void screen_poll(void)
{
    uint32_t w, h;
    screen_want(&w, &h);
    if (w != s_fb_w || h != s_fb_h) {
        if (queue_failed("screen change"))
            return;
        dkQueueWaitIdle(s_queue);       /* nothing in flight reads the old images */
        if (queue_failed("screen change"))
            return;
        swapchain_drop();
        if (!swapchain_make(w, h)) {
            fprintf(stderr, "  [DK] no swapchain at %ux%u: the renderer is off\n", w, h);
            s_up = -1;
            return;
        }
        log_sync();
    }
    s_scale_want = scale_for_screen(s_fb_h);
}

/* ── Bring-up ──────────────────────────────────────────────────────── */

static int device_up(void)
{
    DkDeviceMaker dm;
    DkCmdBufMaker cm;
    DkQueueMaker qm;
    uint32_t code_at = 0, scratch, pack_code;
    const DkshHeader *hv = (const DkshHeader *)s_dksh_vs, *hf = (const DkshHeader *)s_dksh_fs;
    const DkshHeader *hx = (const DkshHeader *)s_dksh_vs_fixed;
    const DkshHeader *ht = (const DkshHeader *)s_dksh_vs_fixed_tg;

    if (s_up)
        return s_up > 0;
    s_up = -1;
    if (s_trace < 0)
        s_trace = getenv("RECOMP_DK_TRACE") != NULL;
    if (s_heartbeat < 0) {
        /* Off now that the game draws; on to tell a live screen from a hung one. */
        const char *e = getenv("RECOMP_DK_HEARTBEAT");
        s_heartbeat = e && e[0] == '1';
    }
    if (s_nodraw < 0)
        s_nodraw = getenv("RECOMP_DK_NODRAW") != NULL;
    if (s_show_fps < 0) {
        const char *e = getenv("RECOMP_DK_FPS");
        s_show_fps = e && e[0] == '1';
    }
    if (s_tex_every < 0) {
        const char *e = getenv("RECOMP_DK_TEX_EVERY");
        s_tex_every = e ? atoi(e) : 4;
        if (s_tex_every < 1)
            s_tex_every = 1;
    }

#if defined(NV2A_DK_CHECKED)
    step("deko3d's checking library is linked (JSRF_DEKO3D_DEBUG)");
#endif
    if (!preflight()) {
        fprintf(stderr, "  [DK] deko3d not started: the system refused a step above."
                        " The game runs on without a picture.\n");
        log_sync();
        return 0;
    }

    step("dkDeviceCreate");
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = dk_debug;
    /* Rows top-first, as the guest's surfaces are. deko3d's NDC y points
     * up (GL); the shaders turn y over (u_surf.w = -1, nv2a_clip) so row 0
     * is the top -- without that the first console run drew everything
     * upside down and every winding turned over (RECOMP_DK_FRONT_FLIP=1
     * "fixed" culling). DkDeviceFlags_YAxisPointsDown would do the same, but
     * the devkitPro image's deko3d predates it. */
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft;
    s_dev = dkDeviceCreate(&dm);
    if (!s_dev)
        return 0;

    step("command memory (24 MB) and data ring (72 MB)");
    s_cmdmem = block(DK_FRAMES * CMD_CHUNKS * CMD_CHUNK,
                     DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (s_data_cached < 0) {
        /* On by default: 0.60 ms per MB against 0.68 uncached, cleaning
         * included, in the same streets (2026-10-26). */
        const char *e = getenv("RECOMP_DK_DATA_CACHED");
        s_data_cached = !(e && e[0] == '0');
    }
    s_datamem = block(DK_FRAMES * DATA_SLICE,
                      (s_data_cached > 0 ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached)
                      | DkMemBlockFlags_GpuCached);
    s_data_cpu = (uint8_t *)dkMemBlockGetCpuAddr(s_datamem);
    s_data_gpu = dkMemBlockGetGpuAddr(s_datamem);

    step("dkCmdBufCreate");
    dkCmdBufMakerDefaults(&cm, s_dev);
    cm.cbAddMem = cmd_add_mem;
    s_cmd = dkCmdBufCreate(&cm);

    step("shaders");
    if (hv->magic != 0x48534B44u || hf->magic != 0x48534B44u || hx->magic != 0x48534B44u
        || ht->magic != 0x48534B44u) {
        fprintf(stderr, "  [DK] the embedded shaders are not DKSH modules\n");
        log_sync();
        return 0;
    }
    /* The vertex shaders keep register files in local memory: the queue's
     * scratch must hold the largest per warp, or the GPU faults. */
    scratch = shader_scratch(s_dksh_vs);
    if (shader_scratch(s_dksh_fs) > scratch)
        scratch = shader_scratch(s_dksh_fs);
    pack_code = pack_read(&scratch);
    if (!s_pk)
        return 0;
    /* The ubershaders, the pack, and room for what the console builds. */
    s_code_cap = align_up(hv->code_sz, DK_SHADER_CODE_ALIGNMENT)
               + align_up(hf->code_sz, DK_SHADER_CODE_ALIGNMENT)
               + align_up(hx->code_sz, DK_SHADER_CODE_ALIGNMENT)
               + align_up(ht->code_sz, DK_SHADER_CODE_ALIGNMENT) + pack_code + PACK_GROW;
    s_codemem = block(s_code_cap + DK_SHADER_CODE_UNUSABLE_SIZE,
                      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached
                      | DkMemBlockFlags_Code);
    if (!shader_load(&s_vs, s_dksh_vs, &code_at) || !shader_load(&s_fs, s_dksh_fs, &code_at)
        || !shader_load(&s_vs_fixed, s_dksh_vs_fixed, &code_at)
        || !shader_load(&s_vs_fixed_tg, s_dksh_vs_fixed_tg, &code_at))
        return 0;
    pack_load(&code_at);
    s_code_at = code_at;
    keys_load();
    scratch = align_up(scratch, DK_PER_WARP_SCRATCH_MEM_ALIGNMENT);
    if (scratch < 4 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT)
        scratch = 4 * DK_PER_WARP_SCRATCH_MEM_ALIGNMENT;
    s_queue_scratch = scratch;
    compiler_start();
    fprintf(stderr, "  [DK] shaders: vertex %u bytes, fragment %u bytes, %u bytes"
                    " of scratch per warp\n", hv->code_sz, hf->code_sz, scratch);

    step("dkQueueCreate");
    dkQueueMakerDefaults(&qm, s_dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_EnableZcull;
    qm.perWarpScratchMemorySize = scratch;
    s_queue = dkQueueCreate(&qm);

    step("uniform buffers and descriptors");
    s_tsmem = block(4096, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached);
    s_ts_cpu = (uint64_t *)dkMemBlockGetCpuAddr(s_tsmem);
    s_ts_gpu = dkMemBlockGetGpuAddr(s_tsmem);
    memset(s_ts_cpu, 0, 4096);
    s_ubomem = block(UBO_TOTAL, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    s_ubo = dkMemBlockGetGpuAddr(s_ubomem);
    memset(dkMemBlockGetCpuAddr(s_ubomem), 0, UBO_TOTAL);
    s_descmem = block(IMGDESC_N * sizeof(DkImageDescriptor) + SAMP_N * sizeof(DkSamplerDescriptor),
                      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    s_imgdesc = (DkImageDescriptor *)dkMemBlockGetCpuAddr(s_descmem);
    s_sampdesc = (DkSamplerDescriptor *)(s_imgdesc + IMGDESC_N);
    s_imgdesc_gpu = dkMemBlockGetGpuAddr(s_descmem);
    s_sampdesc_gpu = s_imgdesc_gpu + IMGDESC_N * sizeof(DkImageDescriptor);
    memset(s_imgdesc, 0, IMGDESC_N * sizeof(DkImageDescriptor) + SAMP_N * sizeof(DkSamplerDescriptor));

    step("swapchain");
    {
        uint32_t w, h;
        screen_want(&w, &h);
        if (!swapchain_make(w, h))
            return 0;
    }
    s_scale = s_scale_want = scale_for_screen(s_fb_h);

    s_slot = 0;
    s_chunk = 0;
    s_up = 1;
    fprintf(stderr, "  [DK] deko3d up: %ux%u swapchain, %u images, %u MB command memory;"
                    " internal resolution x%.2f (%s); data ring %s\n",
            s_fb_w, s_fb_h, SWAP_IMAGES, DK_FRAMES * CMD_CHUNKS * CMD_CHUNK >> 20,
            (double)s_scale, scale_mode_name(),
            s_data_cached > 0 ? "CPU-cached (RECOMP_DK_DATA_CACHED=1)" : "uncached");
    log_sync();
    return 1;
}

/* ── Texture memory: one heap, first fit, released late ────────────── */

typedef struct { uint32_t off, size; } Span;
#define HEAP_SPANS 4096
static DkMemBlock s_heapmem;
static uint32_t   s_heap_size, s_heap_used;
static Span       s_free[HEAP_SPANS];
static int        s_nfree;

static void heap_init(void)
{
    const char *e = getenv("RECOMP_DK_TEX_MB");
    uint32_t mb = e && atoi(e) >= 16 && atoi(e) <= 1024 ? (uint32_t)atoi(e) : 256;
    s_heap_size = mb << 20;
    s_heapmem = block(s_heap_size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    s_free[0].off = 0;
    s_free[0].size = s_heap_size;
    s_nfree = 1;
    fprintf(stderr, "  [DK] texture memory %u MB\n", mb);
}

static int heap_alloc(uint32_t size, uint32_t align, uint32_t *out)
{
    int i;
    size = align_up(size, 256);
    for (i = 0; i < s_nfree; i++) {
        uint32_t a = align_up(s_free[i].off, align);
        uint32_t end = s_free[i].off + s_free[i].size;
        if (a + size > end || a < s_free[i].off)
            continue;
        if (a > s_free[i].off) {
            /* Keep the gap in front; what follows the block becomes a span
             * of its own. */
            uint32_t tail = end - (a + size);
            s_free[i].size = a - s_free[i].off;
            if (tail) {
                if (s_nfree == HEAP_SPANS)
                    return 0;
                memmove(&s_free[i + 2], &s_free[i + 1], (size_t)(s_nfree - i - 1) * sizeof(Span));
                s_free[i + 1].off = a + size;
                s_free[i + 1].size = tail;
                s_nfree++;
            }
        } else {
            s_free[i].off += size;
            s_free[i].size -= size;
            if (!s_free[i].size) {
                memmove(&s_free[i], &s_free[i + 1], (size_t)(s_nfree - i - 1) * sizeof(Span));
                s_nfree--;
            }
        }
        *out = a;
        s_heap_used += size;
        return 1;
    }
    return 0;
}

static void heap_free(uint32_t off, uint32_t size)
{
    int i, j;
    size = align_up(size, 256);
    s_heap_used -= size;
    for (i = 0; i < s_nfree && s_free[i].off < off; i++) { }
    /* Join the span before and/or after. */
    if (i > 0 && s_free[i - 1].off + s_free[i - 1].size == off) {
        s_free[i - 1].size += size;
        if (i < s_nfree && off + size == s_free[i].off) {
            s_free[i - 1].size += s_free[i].size;
            memmove(&s_free[i], &s_free[i + 1], (size_t)(s_nfree - i - 1) * sizeof(Span));
            s_nfree--;
        }
        return;
    }
    if (i < s_nfree && off + size == s_free[i].off) {
        s_free[i].off = off;
        s_free[i].size += size;
        return;
    }
    if (s_nfree == HEAP_SPANS)
        return;                         /* leaked: a full span list */
    for (j = s_nfree; j > i; j--)
        s_free[j] = s_free[j - 1];
    s_free[i].off = off;
    s_free[i].size = size;
    s_nfree++;
}

/* Texture descriptors: a slot is reused only once the GPU is done with it. */
static uint16_t s_desc_free[TEXDESC_N];
static int      s_ndesc_free;

typedef struct { uint32_t off, size, desc, frame; } Deferred;
#define DEFER_N 4096
static Deferred s_defer[DEFER_N];
static int      s_ndefer;

static void defer_release(uint32_t off, uint32_t size, uint32_t desc)
{
    if (s_ndefer == DEFER_N)
        sync_gpu_why(SYNC_DEFER);       /* releases everything */
    s_defer[s_ndefer].off = off;
    s_defer[s_ndefer].size = size;
    s_defer[s_ndefer].desc = desc;
    s_defer[s_ndefer].frame = s_frames;
    s_ndefer++;
}

/* Frame F's draws may use what was released in F; F is done once frame
 * F + DK_FRAMES begins (frame_begin waited for its fence). */
static void release_deferred(int all)
{
    int i, n = 0;
    for (i = 0; i < s_ndefer; i++) {
        Deferred *d = &s_defer[i];
        if (all || s_frames >= d->frame + DK_FRAMES) {
            if (d->size)
                heap_free(d->off, d->size);
            if (d->desc < TEXDESC_N && s_ndesc_free < TEXDESC_N)
                s_desc_free[s_ndesc_free++] = (uint16_t)d->desc;
        } else {
            s_defer[n++] = *d;
        }
    }
    s_ndefer = n;
}

static void write_image_desc(uint32_t idx, const DkImage *img)
{
    DkImageView v;
    dkImageViewDefaults(&v, img);
    dkImageDescriptorInitialize(&s_imgdesc[idx], &v, false, false);
    s_desc_dirty = 1;
}

/* ── Surfaces and depth buffers, by guest address ──────────────────── */

/* A surface's w x h are the title's real (anti-aliased) pixels; its image is
 * pw x ph, larger by the internal resolution (scx, scy; axis_scale). The
 * title's coordinates -- u_surf, texcoords, SURFACE_CLIP, clear rectangles --
 * are in w x h; the viewport maps them onto the image, scissors and clears
 * are converted (phys_edge). */
typedef struct {
    uint32_t   va, w, h;                /* real (anti-aliased) pixels */
    uint32_t   aa_sx, aa_sy, used;
    uint32_t   pw, ph;                  /* the image */
    float      scx, scy;                /* pw ~ w * scx */
    int        written;                 /* drawn into since the last barrier */
    DkMemBlock mem;
    DkImage    img;
} DkSurf;

typedef struct {
    uint32_t   va, w, h, used;          /* the image's size: its colour surface's pw x ph */
    DkMemBlock mem;
    DkImage    img;
} DkDepth;

static DkSurf  s_surf[DK_MAX_SURF];
static DkDepth s_depth[DK_MAX_DEPTH];
static DkSurf *s_last;                  /* last surface drawn or cleared */
static DkSurf *s_bound;
static DkDepth *s_bound_depth;

/* Every block of GPU memory the renderer holds, by GPU address, for a page
 * fault's address (queue_failed). deko3d's own (the queue's scratch, its
 * command ring) are not listed: an address outside all of these is one of
 * those. */
static void gpu_map_one(const char *what, DkMemBlock mb, uint32_t va, uint32_t w, uint32_t h)
{
    unsigned long long lo, hi;
    if (!mb)
        return;
    lo = (unsigned long long)dkMemBlockGetGpuAddr(mb);
    hi = lo + dkMemBlockGetSize(mb);
    if (w)
        fprintf(stderr, "  [DK]   0x%010llX-0x%010llX %s 0x%08X %ux%u\n", lo, hi, what, va, w, h);
    else
        fprintf(stderr, "  [DK]   0x%010llX-0x%010llX %s\n", lo, hi, what);
}

static void gpu_map_dump(void)
{
    int i;
    fprintf(stderr, "  [DK] the renderer's GPU memory:\n");
    gpu_map_one("command memory", s_cmdmem, 0, 0, 0);
    gpu_map_one("data ring (vertices, indices)", s_datamem, 0, 0, 0);
    gpu_map_one("uniforms", s_ubomem, 0, 0, 0);
    gpu_map_one("descriptors", s_descmem, 0, 0, 0);
    gpu_map_one("shader code", s_codemem, 0, 0, 0);
    gpu_map_one("timestamps", s_tsmem, 0, 0, 0);
    gpu_map_one("swapchain", s_fbmem, 0, 0, 0);
    gpu_map_one("texture heap", s_heapmem, 0, 0, 0);
    gpu_map_one("dummy texture", s_dummymem, 0, 0, 0);
    for (i = 0; i < DK_MAX_SURF; i++)
        gpu_map_one("surface", s_surf[i].mem, s_surf[i].va, s_surf[i].pw, s_surf[i].ph);
    for (i = 0; i < DK_MAX_DEPTH; i++)
        gpu_map_one("depth", s_depth[i].mem, s_depth[i].va, s_depth[i].w, s_depth[i].h);
}

/* Render targets and depth buffers with the GPU's framebuffer compression
 * (DkImageFlags_HwCompression, as deko3d's own examples make theirs): the
 * colour and depth units read and write compressed tiles, the texture units
 * and the 2D engine (rescaling, presenting) understand them, so only memory
 * traffic changes -- the cost that grows with the internal resolution
 * (1440x1080 docked is five times the title's pixels). Surfaces are never
 * read by the CPU. RECOMP_DK_COMPRESS=0 makes them plain, to compare. */
static uint32_t rt_flags(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_COMPRESS");
        on = !(e && *e == '0');
        fprintf(stderr, "  [DK] render targets and depth buffers %s\n",
                on ? "compressed (RECOMP_DK_COMPRESS=0: plain)" : "plain (RECOMP_DK_COMPRESS=0)");
    }
    return on ? DkImageFlags_HwCompression : 0;
}

/* A memory block holding one image of `layout`, at the alignment it needs. */
static DkMemBlock image_alloc(DkImage *img, DkImageLayout const *layout)
{
    DkMemBlock mem;
    uint32_t align = dkImageLayoutGetAlignment(layout);
    uint64_t size = dkImageLayoutGetSize(layout);
    uint32_t off;
    DkGpuAddr gpu;

    if (align < DK_MEMBLOCK_ALIGNMENT)
        align = DK_MEMBLOCK_ALIGNMENT;
    if (size == 0 || size > (256u << 20))
        return NULL;
    mem = block((uint32_t)size + align, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    if (!mem)
        return NULL;
    gpu = dkMemBlockGetGpuAddr(mem);
    off = (uint32_t)((align - (gpu % align)) % align);
    dkImageInitialize(img, layout, mem, off);
    return mem;
}

static void bind_targets(DkSurf *s, DkDepth *d)
{
    DkImageView cv, dv;
    dkImageViewDefaults(&cv, &s->img);
    if (d)
        dkImageViewDefaults(&dv, &d->img);
    dkCmdBufBindRenderTarget(s_cmd, &cv, d ? &dv : NULL);
    if (s != s_bound)
        pass_mark(s->va, s->w, s->h, s->pw, s->ph);
    s_bound = s;
    s_bound_depth = d;
}

static int s_rs_valid;                  /* apply_state's viewport and scissor are set */

static void full_rect(uint32_t w, uint32_t h)
{
    s_rs_valid = 0;
    DkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    DkScissor sc = { 0, 0, w, h };
    dkCmdBufSetViewports(s_cmd, 0, &vp, 1);
    dkCmdBufSetScissors(s_cmd, 0, &sc, 1);
}

static DkSurf *surf_find(uint32_t va)
{
    int i;
    for (i = 0; i < DK_MAX_SURF; i++)
        if (s_surf[i].mem && s_surf[i].va == va)
            return &s_surf[i];
    return NULL;
}

/* A colour image for surface `s` (w, h, aa set) at internal factor `g`:
 * fills in pw, ph, scx, scy, mem, img. */
static int surf_image(DkSurf *s, float g)
{
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    s->scx = axis_scale(g, s->aa_sx, s->w);
    s->scy = axis_scale(g, s->aa_sy, s->h);
    s->pw = phys_size(s->w, s->scx);
    s->ph = phys_size(s->h, s->scy);
    if (!s->pw || !s->ph || s->pw > 4096 || s->ph > 4096)
        return 0;
    dkImageLayoutMakerDefaults(&lm, s_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | rt_flags();
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = s->pw;
    lm.dimensions[1] = s->ph;
    dkImageLayoutInitialize(&layout, &lm);
    s->mem = image_alloc(&s->img, &layout);
    return s->mem != NULL;
}

static DkSurf *surf_get(uint32_t va, uint32_t w, uint32_t h, uint32_t aa_sx, uint32_t aa_sy)
{
    DkSurf *s = surf_find(va), *victim = NULL;
    int i;

    /* Keep a surface while the clip fits (NFSU2's split screen clips it to
     * half and back every frame); grow it when it does not. */
    if (s && s->w == w && s->h >= h) {
        s->used = s_frames;
        return s;
    }
    if (s && s->w == w && s->h > h)
        h = s->h;
    if (!s) {
        for (i = 0; i < DK_MAX_SURF; i++) {
            if (!s_surf[i].mem) { victim = &s_surf[i]; break; }
            if (!victim || s_surf[i].used < victim->used) victim = &s_surf[i];
        }
        s = victim;
    }
    if (w > 4096 || h > 4096 || !w || !h)
        return NULL;
    if (s->mem) {
        sync_gpu_why(SYNC_SURF);        /* the GPU may still render into or sample it */
        if (s_up <= 0)
            return NULL;
        if (s_last == s) s_last = NULL;
        if (s_bound == s) s_bound = NULL;
        dkMemBlockDestroy(s->mem);
    }
    memset(s, 0, sizeof *s);

    s->va = va; s->w = w; s->h = h; s->used = s_frames;
    s->aa_sx = aa_sx; s->aa_sy = aa_sy;
    if (!surf_image(s, s_scale)) {
        memset(s, 0, sizeof *s);
        return NULL;
    }
    s_st.surfs++;
    write_image_desc(DESC_SURF0 + (uint32_t)(s - s_surf), &s->img);

    /* New storage is undefined: start black, as the GL renderer does. */
    bind_targets(s, NULL);
    full_rect(s->pw, s->ph);
    dkCmdBufClearColorFloat(s_cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    if (s_trace)
        fprintf(stderr, "  [DK] surface 0x%08X %ux%u (aa %ux%u), image %ux%u\n",
                va, w, h, aa_sx, aa_sy, s->pw, s->ph);
    return s;
}

/* Depth buffers belong to the title's zeta address, not to a colour surface
 * (NFSU2's light flares switch colour target but keep the scene's Z). */
static DkDepth *depth_get(uint32_t va, uint32_t w, uint32_t h, int *fresh)
{
    DkDepth *d, *victim = NULL;
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    int i;

    for (i = 0; i < DK_MAX_DEPTH; i++) {
        if (s_depth[i].mem && s_depth[i].va == va && s_depth[i].w == w && s_depth[i].h == h) {
            s_depth[i].used = s_frames;
            return &s_depth[i];
        }
        if (!victim || (victim->mem && (!s_depth[i].mem || s_depth[i].used < victim->used)))
            victim = &s_depth[i];
    }
    d = victim;
    if (d->mem) {
        sync_gpu_why(SYNC_DEPTH);
        if (s_up <= 0)
            return NULL;
        if (s_bound_depth == d) s_bound_depth = NULL;
        dkMemBlockDestroy(d->mem);
    }
    memset(d, 0, sizeof *d);
    dkImageLayoutMakerDefaults(&lm, s_dev);
    lm.flags = DkImageFlags_UsageRender | rt_flags();
    lm.format = DkImageFormat_Z24S8;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    dkImageLayoutInitialize(&layout, &lm);
    d->mem = image_alloc(&d->img, &layout);
    if (!d->mem)
        return NULL;
    d->va = va; d->w = w; d->h = h; d->used = s_frames;
    *fresh = 1;
    s_st.depths++;
    if (s_trace)
        fprintf(stderr, "  [DK] depth 0x%08X %ux%u\n", va, w, h);
    return d;
}

static DkSurf *surf_bind(const Nv2aSurface *sf, uint32_t zeta_va)
{
    uint32_t bpp = sf->bytes_per_pixel ? sf->bytes_per_pixel : 4;
    uint32_t w = sf->pitch ? sf->pitch / bpp : sf->width;
    uint32_t h = sf->clip_y + sf->height;
    DkSurf *s;
    DkDepth *d = NULL;
    int fresh = 0;

    if (w < sf->clip_x + sf->width) w = sf->clip_x + sf->width;
    if (!w || !h || !sf->color_va)
        return NULL;
    s = surf_get(sf->color_va, w, h, sf->aa_sx ? sf->aa_sx : 1, sf->aa_sy ? sf->aa_sy : 1);
    if (!s)
        return NULL;
    if (zeta_va)
        d = depth_get(zeta_va, s->pw, s->ph, &fresh);
    if (s_up <= 0)
        return NULL;
    if (s_bound != s || s_bound_depth != d)
        bind_targets(s, d);
    if (fresh) {                        /* new storage is undefined */
        full_rect(s->pw, s->ph);
        dkCmdBufClearDepthStencil(s_cmd, true, 1.0f, 0xFF, 0);
    }
    return s;
}

/* The internal resolution changed (docked or undocked with "auto"): every
 * surface is made again at s_scale_want, its picture carried over by the 2D
 * engine, under the same descriptor; depth buffers are dropped (the next
 * use makes and clears one). At frame_begin, with nothing of the frame
 * recorded yet: once the GPU is idle, then the copies, then idle again
 * before the old images go. */
static void rescale_all(void)
{
    DkMemBlock old[DK_MAX_SURF + DK_MAX_DEPTH];
    int nold = 0, i, kept = 0;

    if (s_up <= 0 || queue_failed("rescale"))
        return;
    dkQueueWaitIdle(s_queue);
    if (queue_failed("rescale"))
        return;
    s_scale = s_scale_want;
    for (i = 0; i < DK_MAX_SURF; i++) {
        DkSurf *s = &s_surf[i], n;
        DkImageView sv, nv;
        DkImageRect sr, nr;
        if (!s->mem)
            continue;
        n = *s;
        n.mem = NULL;
        old[nold++] = s->mem;
        if (!surf_image(&n, s_scale)) {
            /* No memory for it: dropped; the next use makes it again. */
            if (s_last == s) s_last = NULL;
            memset(s, 0, sizeof *s);
            continue;
        }
        sr.x = sr.y = sr.z = 0; sr.width = s->pw; sr.height = s->ph; sr.depth = 1;
        nr.x = nr.y = nr.z = 0; nr.width = n.pw; nr.height = n.ph; nr.depth = 1;
        dkImageViewDefaults(&sv, &s->img);
        dkImageViewDefaults(&nv, &n.img);
        dkCmdBufBlitImage(s_cmd, &sv, &sr, &nv, &nr,
                          DkBlitFlag_FilterLinear | DkBlitFlag_ModeBlit, 0);
        *s = n;
        write_image_desc(DESC_SURF0 + (uint32_t)i, &s->img);
        kept++;
    }
    for (i = 0; i < DK_MAX_DEPTH; i++) {
        if (s_depth[i].mem)
            old[nold++] = s_depth[i].mem;
        memset(&s_depth[i], 0, sizeof s_depth[i]);
    }
    s_bound = NULL;                     /* the next draw binds the new images */
    s_bound_depth = NULL;
    s_rs_valid = 0;
    if (kept) {
        sync_gpu_why(SYNC_SURF);        /* the copies are done */
        if (s_up <= 0)
            return;
    }
    for (i = 0; i < nold; i++)
        dkMemBlockDestroy(old[i]);
    fprintf(stderr, "  [DK] internal resolution x%.2f (%ux%u for 640x480); %d surfaces"
                    " carried over\n", (double)s_scale, phys_size(640, s_scale),
            phys_size(480, s_scale), kept);
    log_sync();
}

/* ── Textures ──────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, color, w, h, pitch, hash, used, checked;
    uint32_t levels;                    /* mip levels in the image */
    uint32_t bytes;                     /* guest bytes it was made from, from va */
    uint32_t desc;                      /* image descriptor */
    uint32_t off, size;                 /* in the heap */
    int      live;
    int      stale;                     /* guest bytes rewritten (dk_written) */
    uint32_t same;                      /* content checks in a row it passed */
    uint8_t  amin;                      /* its alpha: 0 may be 0, 1 above 0 everywhere,
                                         * 2 255 everywhere (alpha_proves) */
    DkImage  img;
} DkTex;

static DkTex    s_tex[TEX_MAX];
#define TEX_BUCKETS 2048
static uint16_t s_tex_head[TEX_BUCKETS], s_tex_next[TEX_MAX];
static uint32_t *s_decode;
static size_t    s_decode_cap;
static uint32_t  s_tex_made, s_tex_uploads;
static uint64_t  s_tex_bytes_all;        /* uploaded since the start (hitch_check) */

static uint32_t tex_bucket(uint32_t va, uint32_t color, uint32_t w, uint32_t h)
{
    uint32_t k = va ^ (color << 24) ^ (w << 12) ^ (h << 4);
    k ^= k >> 15; k *= 0x2C1B3C6Du; k ^= k >> 12;
    return k & (TEX_BUCKETS - 1);
}

static void tex_index_add(DkTex *t)
{
    uint32_t b = tex_bucket(t->va, t->color, t->w, t->h);
    uint16_t i = (uint16_t)(t - s_tex);
    s_tex_next[i] = s_tex_head[b];
    s_tex_head[b] = (uint16_t)(i + 1);
}

static void tex_index_del(DkTex *t)
{
    uint16_t *at = &s_tex_head[tex_bucket(t->va, t->color, t->w, t->h)];
    uint16_t i = (uint16_t)(t - s_tex);
    while (*at) {
        if (*at == i + 1) {
            *at = s_tex_next[i];
            return;
        }
        at = &s_tex_next[*at - 1];
    }
}

/* Swizzled (0x00-0x0B, 0x19, 0x1A, 0x27-0x3F except linear) and DXT carry
 * their size in the format word; linear images use IMAGE_RECT. */
static int tex_size_from_format(uint32_t color)
{
    if (color >= 0x0C && color <= 0x0F)
        return 1;
    switch (color) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x1B: case 0x1C: case 0x1D:
    case 0x1E: case 0x1F: case 0x20: case 0x24: case 0x25: case 0x26:
    case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x34: case 0x35:
    case 0x36: case 0x37: case 0x40: case 0x41: case 0x43: case 0x44:
    case 0x45: case 0x46:
        return 0;
    default:
        return 1;
    }
}

/* About 250 words sampled across the bytes, and the last one. The stride is
 * bytes / 251, not / 256: a power-of-two texture's size over 256 is a whole
 * number of rows, and every sample of a linear image then fell in its first
 * column -- for a picture on a transparent ground (a graffiti design), a
 * column that never changes, so a new picture in the same memory passed for
 * the old one. A prime divisor walks the samples across the columns. */
static uint32_t bytes_hash(uint32_t va, uint32_t bytes)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset() + va;
    uint32_t h = 2166136261u ^ bytes, step, i;

    if (!bytes)
        return h;
    step = bytes / 251;
    if (step < 4) step = 4;
    step &= ~3u;
    for (i = 0; i + 4 <= bytes; i += step) {
        uint32_t w;
        memcpy(&w, mem + i, 4);
        h = (h ^ w) * 16777619u;
    }
    memcpy(&i, mem + bytes - 4, 4);
    return (h ^ i) * 16777619u;
}

static uint32_t tex_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (color == 0x0C) return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F) return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    if (pitch) return pitch * h;
    return w * h * 4;                   /* upper bound for swizzled formats */
}

/* MIP LEVELS. The title's textures carry their mip chains -- each level
 * swizzled on its own, straight after the one before -- and the renderer
 * uploaded level 0 only and sampled it at every distance: a distant wall
 * read texels spread over the whole image, missing the texture cache on
 * nearly every one, and shimmered. RECOMP_DK_MIPS=0 goes back to that. */
static int s_mips = -1;

static int mips_on(void)
{
    if (s_mips < 0) {
        const char *e = getenv("RECOMP_DK_MIPS");
        s_mips = !(e && *e == '0');
    }
    return s_mips;
}

static int tex_is_dxt(uint32_t c) { return c == 0x0C || c == 0x0E || c == 0x0F; }

/* Bytes per texel of a swizzled format whose levels this renderer uploads
 * (DXT aside); 0 for linear and unknown ones, which get one level. */
static uint32_t swz_bpp(uint32_t c)
{
    switch (c) {
    case 0x00: case 0x01: case 0x0B: case 0x19:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x1A: case 0x27: case 0x28: case 0x29:
        return 2;
    case 0x06: case 0x07: case 0x3A: case 0x3B: case 0x3C:
        return 4;
    default:
        return 0;
    }
}

static uint32_t lvl_dim(uint32_t v, uint32_t l) { return v >> l ? v >> l : 1u; }

/* Bytes of level `l` of a w x h texture (swizzled or DXT). */
static uint32_t level_bytes(uint32_t color, uint32_t w, uint32_t h, uint32_t l)
{
    uint32_t lw = lvl_dim(w, l), lh = lvl_dim(h, l);
    return tex_is_dxt(color) ? tex_bytes(color, lw, lh, 0) : lw * lh * swz_bpp(color);
}

/* The levels to upload: SET_TEXTURE_FORMAT's count (bits 16-19), within the
 * chain down to 1x1; 1 for linear textures and with RECOMP_DK_MIPS=0. */
static uint32_t tex_levels(uint32_t color, uint32_t format, uint32_t w, uint32_t h,
                           uint32_t pitch)
{
    uint32_t n = (format >> 16) & 0xF, full = 1, m = w > h ? w : h;
    if (!mips_on() || pitch || !(tex_is_dxt(color) || swz_bpp(color)))
        return 1;
    while (m > 1) {
        m >>= 1;
        full++;
    }
    if (n < 1)
        n = 1;
    return n < full ? n : full;
}

/* Xbox swizzle: texel (u, v) of a power-of-two image sits at the index made
 * by interleaving the coordinate bits, u in the even positions and v in the
 * odd ones, until the smaller dimension runs out. */
static uint32_t swizzle_index(uint32_t u, uint32_t v, uint32_t w, uint32_t h)
{
    uint32_t out = 0, bit = 0, mw = w - 1, mh = h - 1, m;

    for (m = 1; mw >= m || mh >= m; m <<= 1) {
        if (mw >= m) { if (u & m) out |= 1u << bit; bit++; }
        if (mh >= m) { if (v & m) out |= 1u << bit; bit++; }
    }
    return out;
}

/* SZ_I8_A8R8G8B8: 8-bit indices, swizzled, into an A8R8G8B8 palette named
 * by SET_TEXTURE_PALETTE (length code in [3:2]: 256, 128, 64, 32 entries). */
static int decode_indexed(uint32_t va, uint32_t w, uint32_t h, uint32_t pal_reg,
                          uint32_t pal_va, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t entries = 256u >> ((pal_reg >> 2) & 3);
    const uint8_t *idx = mem + va;
    uint32_t x, y;

    if (!(pal_reg & ~0x3Fu) || !pal_va)
        return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t i = idx[swizzle_index(x, y, w, h)];
            uint32_t c;
            if (i >= entries)
                i = 0;
            memcpy(&c, mem + pal_va + i * 4, 4);
            out[y * w + x] = c;
        }
    return 1;
}

/* Make room in the heap: release the least recently used textures not used
 * this frame, then wait for the GPU so the memory is reusable now. */
static void tex_evict(uint32_t want)
{
    uint32_t freed = 0;
    while (freed < want + (s_heap_size >> 3)) {
        DkTex *lru = NULL;
        int i;
        for (i = 0; i < TEX_MAX; i++) {
            DkTex *c = &s_tex[i];
            if (c->live && c->used != s_frames && (!lru || c->used < lru->used))
                lru = c;
        }
        if (!lru)
            break;
        defer_release(lru->off, lru->size, lru->desc);
        freed += lru->size;
        tex_index_del(lru);
        memset(lru, 0, sizeof *lru);
    }
    sync_gpu_why(SYNC_EVICT);
}

/* Storage for a w x h image of `fmt`, and its descriptor; NULL-image on
 * failure (the caller samples the stand-in). */
static int tex_storage(DkTex *t, DkImageFormat fmt)
{
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    uint32_t size, align, off;

    dkImageLayoutMakerDefaults(&lm, s_dev);
    lm.format = fmt;
    lm.dimensions[0] = t->w;
    lm.dimensions[1] = t->h;
    lm.mipLevels = t->levels ? t->levels : 1;
    dkImageLayoutInitialize(&layout, &lm);
    size = (uint32_t)dkImageLayoutGetSize(&layout);
    align = dkImageLayoutGetAlignment(&layout);
    if (align < 256)
        align = 256;
    if (!size || size > s_heap_size / 2 || !s_ndesc_free)
        return 0;
    if (!heap_alloc(size, align, &off)) {
        tex_evict(size);
        if (s_up <= 0 || !heap_alloc(size, align, &off))
            return 0;
    }
    if (!s_ndesc_free) {
        heap_free(off, size);
        return 0;
    }
    t->off = off;
    t->size = size;
    t->desc = s_desc_free[--s_ndesc_free];
    dkImageInitialize(&t->img, &layout, s_heapmem, off);
    write_image_desc(t->desc, &t->img);
    return 1;
}

/* Copy `bytes` (rows of `row` bytes, 0: packed) into level `l` of t's image. */
static void tex_upload(DkTex *t, const void *src, uint32_t bytes, uint32_t row, uint32_t l)
{
    DkGpuAddr gpu;
    uint8_t *dst = data_alloc(bytes, 256, &gpu);
    DkCopyBuf cb;
    DkImageView v;
    DkImageRect r = { 0, 0, 0, lvl_dim(t->w, l), lvl_dim(t->h, l), 1 };

    if (!dst)
        return;
    memcpy(dst, src, bytes);
    s_st.b_tex += bytes;
    s_tex_bytes_all += bytes;
    s_st.texs++;
    cb.addr = gpu;
    cb.rowLength = row;
    cb.imageHeight = 0;
    dkImageViewDefaults(&v, &t->img);
    v.mipLevelOffset = (uint8_t)l;
    v.mipLevelCount = 1;
    dkCmdBufCopyBufferToImage(s_cmd, &cb, &v, &r, 0);
    s_upload_dirty = 1;
    s_tex_uploads++;
}

/* WHERE A TEXTURE'S ALPHA CAN BE 0 (alpha_proves): the lowest alpha of
 * every texel the image holds, all its levels, as DkTex.amin -- 0 one may
 * be 0, 1 all above 0, 2 all 255. DXT is read block by block: a BC1 block
 * in its three-colour mode has a transparent code, BC2 has explicit 4-bit
 * alpha, BC3 interpolates between its two ends (the six-value mode adds 0
 * and 255 codes). A block's unused texels (a 2x2 level) count as well,
 * which only errs towards "may be 0". */
static uint8_t s_tex_amin;              /* the texture tex_get last returned */
static uint8_t s_stage_amin[4];         /* each bound stage's (bind_stage) */

static uint8_t amin_class(uint32_t a)   /* 0..255 */
{
    return a == 0 ? 0 : a == 255 ? 2 : 1;
}

static uint8_t dxt_amin(uint32_t color, const uint8_t *p, uint32_t bytes)
{
    uint32_t bs = color == 0x0C ? 8u : 16u, i, lo = 255;
    for (i = 0; i + bs <= bytes && lo; i += bs, p += bs) {
        if (color == 0x0C) {
            uint32_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8;
            uint32_t idx = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24, k;
            if (c0 > c1)
                continue;                       /* four colours, opaque */
            for (k = 0; k < 16; k++)
                if (((idx >> (2 * k)) & 3) == 3) { lo = 0; break; }
        } else if (color == 0x0E) {
            uint32_t k;
            for (k = 0; k < 8; k++) {
                uint32_t a = p[k] & 0xF, b = p[k] >> 4;
                if (a * 17 < lo) lo = a * 17;
                if (b * 17 < lo) lo = b * 17;
            }
        } else {
            uint32_t a0 = p[0], a1 = p[1], k, m = 255;
            uint64_t idx = 0;
            for (k = 0; k < 6; k++)
                idx |= (uint64_t)p[2 + k] << (8 * k);
            for (k = 0; k < 16; k++) {
                uint32_t c = (uint32_t)(idx >> (3 * k)) & 7, v;
                if (c == 0) v = a0;
                else if (c == 1) v = a1;
                else if (a0 > a1) v = a1;               /* between the two: at least a1 */
                else if (c == 6) v = 0;
                else if (c == 7) v = 255;
                else v = a0 < a1 ? a0 : a1;
                if (v < m) m = v;
            }
            if (m < lo) lo = m;
        }
    }
    return amin_class(lo);
}

static uint8_t bgra_amin(const uint32_t *px, size_t n)
{
    uint32_t lo = 255;
    size_t i;
    for (i = 0; i < n && lo; i++) {
        uint32_t a = px[i] >> 24;
        if (a < lo) lo = a;
    }
    return amin_class(lo);
}

/* The image descriptor for the texture at `va`, made or refreshed. */
static uint32_t tex_get(uint32_t va, uint32_t color, uint32_t w, uint32_t h, uint32_t pitch,
                        uint32_t levels, uint32_t pal_reg, uint32_t pal_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    DkTex *t = NULL, *victim = NULL;
    uint32_t i, hash;
    Nv2aTexture nt;

    s_tex_amin = 0;

    for (i = s_tex_head[tex_bucket(va, color, w, h)]; i; i = s_tex_next[i - 1]) {
        DkTex *c = &s_tex[i - 1];
        if (c->live && c->va == va && c->color == color && c->w == w && c->h == h
            && c->levels == levels) {
            t = c;
            break;
        }
    }
    /* Once per frame per texture is enough to notice an update. And a
     * texture whose bytes have not changed in TEX_SETTLED checks is looked at
     * every s_tex_every-th frame (RECOMP_DK_TEX_EVERY, default 4; 1 = every
     * frame), each on its own phase so the checks spread over the frames:
     * the sampled hash reads ~250 scattered words, which in the busy streets
     * was 4% of the renderer thread. A texture that changes (a movie) keeps
     * being checked every frame; one the executor rewrote is marked stale. */
    if (t && !t->stale) {
        if (t->checked == s_frames) {
            t->used = s_frames;
            s_tex_amin = t->amin;
            return t->desc;
        }
        /* Not a paletted one: a palette animates on its own. */
        if (s_tex_every > 1 && t->same >= TEX_SETTLED && t->pitch == pitch && color != 0x0B
            && s_frames - t->checked < (uint32_t)s_tex_every
            && (s_frames + (uint32_t)(t - s_tex)) % (uint32_t)s_tex_every != 0) {
            t->used = s_frames;
            s_st.checks_skipped++;
            s_tex_amin = t->amin;
            return t->desc;
        }
    }
    /* One level: as before. A chain: every level, exactly. */
    uint32_t bytes = levels > 1 ? 0 : tex_bytes(color, w, h, pitch), lv;
    for (lv = 0; levels > 1 && lv < levels; lv++)
        bytes += level_bytes(color, w, h, lv);
    {
        uint64_t t0 = armGetSystemTick();
        hash = bytes_hash(va, bytes);
        if (color == 0x0B && pal_va)    /* the palette is content too */
            hash ^= bytes_hash(pal_va, 1024) * 31u;
        s_st.t_check += armGetSystemTick() - t0;
        s_st.checks++;
    }
    if (t && t->hash == hash && t->pitch == pitch && !t->stale) {
        t->used = t->checked = s_frames;
        if (t->same < TEX_SETTLED)
            t->same++;
        s_tex_amin = t->amin;
        return t->desc;
    }
    if (t) {
        /* Changed contents (a movie frame, a rewritten texture): new storage,
         * so draws still queued with the old picture keep it. */
        defer_release(t->off, t->size, t->desc);
        tex_index_del(t);
        memset(t, 0, sizeof *t);
    } else {
        for (i = 0; i < TEX_MAX; i++) {
            DkTex *c = &s_tex[i];
            if (!c->live) { victim = c; break; }
            if (!victim || c->used < victim->used) victim = c;
        }
        t = victim;
        if (t->live) {
            defer_release(t->off, t->size, t->desc);
            tex_index_del(t);
            memset(t, 0, sizeof *t);
        }
    }
    t->va = va; t->color = color; t->w = w; t->h = h; t->pitch = pitch;
    t->levels = levels; t->bytes = bytes;
    t->hash = hash; t->used = t->checked = s_frames;
    s_tex_made++;
    if (s_trace)
        fprintf(stderr, "  [DK] texture 0x%08X format 0x%02X %ux%u pitch %u, %u levels\n",
                va, color, w, h, pitch, levels);

    if (color == 0x0C || color == 0x0E || color == 0x0F) {
        /* DXT: the GPU samples it as it is (BC1/2/3), no decode. */
        DkImageFormat f = color == 0x0C ? DkImageFormat_RGBA_BC1
                        : color == 0x0E ? DkImageFormat_RGBA_BC2 : DkImageFormat_RGBA_BC3;
        uint32_t l, off = 0;
        if (!tex_storage(t, f))
            return DESC_DUMMY;
        for (l = 0; l < levels; l++) {
            uint32_t n = level_bytes(color, w, h, l);
            tex_upload(t, mem + va + off, n, 0, l);
            off += n;
        }
        t->amin = dxt_amin(color, mem + va, off);
    } else if (color == 0x12 && pitch && !(pitch & 3) && pitch / 4 >= w) {
        /* Linear A8R8G8B8 (movie frames): BGRA bytes as they are. */
        if (!tex_storage(t, DkImageFormat_BGRA8_Unorm))
            return DESC_DUMMY;
        tex_upload(t, mem + va, pitch * h, pitch, 0);
    } else {
        int ok = 0;
        uint32_t l, lva = va;
        uint8_t am = 2;
        if ((size_t)w * h > s_decode_cap) {
            free(s_decode);
            s_decode_cap = (size_t)w * h;
            s_decode = (uint32_t *)malloc(s_decode_cap * 4);
            if (!s_decode)
                s_decode_cap = 0;
        }
        /* Level by level, each decoded into s_decode and copied out before
         * the next (the copy takes it into the data ring at once). */
        for (l = 0; l < levels; l++) {
            uint32_t lw = lvl_dim(w, l), lh = lvl_dim(h, l);
            ok = 0;
            if (s_decode && (color == 0x06 || color == 0x07)) {
                /* Swizzled A8R8G8B8 / X8R8G8B8: un-Morton in a loop. */
                const uint32_t *src = (const uint32_t *)(mem + lva);
                uint32_t x, y, fill = color == 0x07 ? 0xFF000000u : 0;
                for (y = 0; y < lh; y++)
                    for (x = 0; x < lw; x++)
                        s_decode[(size_t)y * lw + x] = src[swizzle_index(x, y, lw, lh)] | fill;
                ok = 1;
            } else if (s_decode && color == 0x0B) {
                ok = decode_indexed(lva, lw, lh, pal_reg, pal_va, s_decode);
            } else if (s_decode) {
                memset(&nt, 0, sizeof nt);
                nt.offset = lva; nt.width = lw; nt.height = lh; nt.pitch = pitch; nt.color = color;
                nt.addr_u = nt.addr_v = 1;
                ok = nv2a_backend_decode_texture(&nt, s_decode);
            }
            if (!ok)
                break;
            if (l == 0 && !tex_storage(t, DkImageFormat_BGRA8_Unorm))
                return DESC_DUMMY;
            tex_upload(t, s_decode, lw * lh * 4, 0, l);
            if (am) {
                uint8_t a = bgra_amin(s_decode, (size_t)lw * lh);
                if (a < am) am = a;
            }
            if (levels > 1)
                lva += level_bytes(color, w, h, l);
        }
        if (!ok && l == 0) {
            /* Remembered as the stand-in, so it is not retried every draw
             * (only when its bytes change). */
            if (s_trace)
                fprintf(stderr, "  [DK] texture 0x%08X format 0x%02X not decodable\n", va, color);
            t->desc = DESC_DUMMY;
            t->off = t->size = 0;
            t->live = 1;
            tex_index_add(t);
            return DESC_DUMMY;
        }
        /* A level past the first that would not decode stays undefined;
         * the sampler never reaches it if the title set the count right. */
        t->amin = ok ? am : 0;
    }
    t->live = 1;
    tex_index_add(t);
    s_tex_amin = t->amin;
    return t->desc;
}

static DkWrapMode wrap_mode(uint32_t m)
{
    switch (m) {
    case 2:  return DkWrapMode_MirroredRepeat;
    case 3:  return DkWrapMode_ClampToEdge;
    case 4:  return DkWrapMode_ClampToBorder;
    case 5:  return DkWrapMode_ClampToEdge;
    default: return DkWrapMode_Repeat;
    }
}

/* The sampler descriptor for a stage's address, filter, control0 and
 * border colour registers. SET_TEXTURE_FILTER's min mode (bits 16-23): 1
 * point and 2 linear without mips; 3/4 point/linear from the nearest level,
 * 5/6 between two levels (7, convolution, as linear); mag (24-27): 1 point.
 * CONTROL0's LOD clamps (min 18-29, max 6-17, 4.8 fixed) bound the levels, a
 * zero max read as no clamp. Without mips (RECOMP_DK_MIPS=0) level 0 only.
 *
 * Address mode 4 clamps to SET_TEXTURE_BORDER_COLOR (ARGB). It was clamped
 * to transparent black, as the GL and Vulkan renderers do: JSRF projects
 * its shadows with a texture clamped to a white border, so wherever a
 * receiving polygon reached past the shadow texture it was shaded dark --
 * large dark shapes on walls and floors that came and went as the player
 * moved. RECOMP_DK_BORDER=0 goes back to black, to compare. */
static int border_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_BORDER");
        on = !(e && *e == '0');
    }
    return on;
}

static uint32_t sampler_get(uint32_t addr, uint32_t filter, uint32_t ctl0, uint32_t border)
{
    uint32_t minm = (filter >> 16) & 0xFF;
    uint32_t mip = !mips_on() ? 0u : (minm == 3 || minm == 4) ? 1u : (minm == 5 || minm == 6) ? 2u : 0u;
    uint32_t lo = (ctl0 >> 18) & 0xFFF, hi = (ctl0 >> 6) & 0xFFF;
    uint32_t lo_l = lo >> 8 > 15 ? 15u : lo >> 8;
    uint32_t hi_l = !hi || hi >> 8 > 15 ? 15u : hi >> 8;
    uint32_t key = (addr & 0xF0F) | ((((filter >> 24) & 0xF) == 1) << 16)
                 | ((minm == 1 || minm == 3 || minm == 5) << 17) | (mip << 18)
                 | (mip ? (lo_l << 20) | (hi_l << 24) : 0u) | 0x80000000u;
    DkSampler smp;
    int i;

    if (((addr & 0xF) != 4 && ((addr >> 8) & 0xF) != 4) || !border_on())
        border = 0;                     /* only clamp-to-border reads it */
    for (i = 0; i < s_samp_n; i++)
        if (s_samp_key[i] == key && s_samp_border[i] == border)
            return (uint32_t)i;
    if (s_samp_n == SAMP_N)
        return 0;
    dkSamplerDefaults(&smp);
    smp.wrapMode[0] = wrap_mode(addr & 0xF);
    smp.wrapMode[1] = wrap_mode((addr >> 8) & 0xF);
    smp.borderColor[0].value_f = (float)((border >> 16) & 0xFF) / 255.0f;
    smp.borderColor[1].value_f = (float)((border >> 8) & 0xFF) / 255.0f;
    smp.borderColor[2].value_f = (float)(border & 0xFF) / 255.0f;
    smp.borderColor[3].value_f = (float)(border >> 24) / 255.0f;
    if (border && s_trace)
        fprintf(stderr, "  [DK] sampler %d: border colour 0x%08X\n", s_samp_n, border);
    smp.magFilter = (key & (1u << 16)) ? DkFilter_Nearest : DkFilter_Linear;
    smp.minFilter = (key & (1u << 17)) ? DkFilter_Nearest : DkFilter_Linear;
    smp.mipFilter = mip == 1 ? DkMipFilter_Nearest : mip == 2 ? DkMipFilter_Linear
                  : DkMipFilter_None;
    if (mip) {
        smp.lodClampMin = (float)lo_l;
        smp.lodClampMax = hi_l >= 15 ? 1000.0f : (float)(hi_l < lo_l ? lo_l : hi_l);
    }
    dkSamplerDescriptorInitialize(&s_sampdesc[s_samp_n], &smp);
    s_samp_key[s_samp_n] = key;
    s_samp_border[s_samp_n] = border;
    s_desc_dirty = 1;
    return (uint32_t)s_samp_n++;
}

/* A surface kept taller than its clip and sampled as a texture of the clip's
 * size: texcoords cover the w x h texels at the top left. */
static void rt_scale(uint32_t w, uint32_t h, const DkSurf *s, float scale[2])
{
    uint32_t lw = s->w / (s->aa_sx ? s->aa_sx : 1), lh = s->h / (s->aa_sy ? s->aa_sy : 1);
    if (w < lw) scale[0] *= (float)w / (float)lw;
    if (h < lh) scale[1] *= (float)h / (float)lh;
    /* The image is w * scx rounded up: the picture fills all but that
     * fraction of a pixel (1 whenever the factor comes out whole). */
    scale[0] *= (float)((double)s->w * s->scx / (double)s->pw);
    scale[1] *= (float)((double)s->h * s->scy / (double)s->ph);
}

static int s_rt_barrier;                /* a sampled surface was drawn into */

/* Stage i's texture handle and texcoord scale, from the method shadow. */
static DkResHandle bind_stage(const Nv2aRawBatch *b, int i, float scale[2])
{
    const uint32_t *regs = b->regs;
    uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
    uint32_t control0 = regs[base + 3];
    uint32_t format = regs[base + 1];
    uint32_t color = (format >> 8) & 0xFF;
    uint32_t va, w, h, pitch = 0, desc, samp;
    DkSurf *rt;

    scale[0] = scale[1] = 1.0f;
    s_stage_amin[i] = 2;                /* the stand-in is (0, 0, 0, 1) */
    if (!(control0 & 0x40000000u) || !regs[base] || !b->tex_va[i])
        return dkMakeTextureHandle(DESC_DUMMY, 0);
    va = b->tex_va[i];
    if (tex_size_from_format(color)) {
        w = 1u << ((format >> 20) & 0xF);
        h = 1u << ((format >> 24) & 0xF);
    } else {
        uint32_t rect = regs[base + 7];
        w = rect >> 16;
        h = rect & 0xFFFF;
        pitch = regs[base + 4] >> 16;
        if (w) scale[0] = 1.0f / (float)w;
        if (h) scale[1] = 1.0f / (float)h;
    }
    if (!w || !h || w > 4096 || h > 4096)
        return dkMakeTextureHandle(DESC_DUMMY, 0);
    /* SET_TEXTURE_BORDER_COLOR (0x1B24) is the stage's tenth word. */
    samp = sampler_get(regs[base + 2], regs[base + 5], control0, regs[base + 9]);
    rt = surf_find(va);
    if (rt) {
        /* The image the title samples is the logical one; the surface holds
         * it at the anti-aliased size, and larger again by the internal
         * resolution. Normalised coordinates cover all three. */
        rt_scale(w, h, rt, scale);
        {
            /* Which render targets the title samples, and how (the first
             * 24 combinations): a projected shadow shows up here. */
            static uint32_t seen[24];
            static int nseen;
            uint32_t mode = (regs[0x1E70 / 4] >> (i * 5)) & 0x1F;
            uint32_t k = (w * 2654435761u) ^ (h * 40503u) ^ (regs[base + 2] * 2246822519u)
                       ^ (mode << 27) ^ regs[base + 9];
            int j;
            for (j = 0; j < nseen && seen[j] != k; j++)
                ;
            if (j == nseen && nseen < 24) {
                seen[nseen++] = k;
                fprintf(stderr, "  [DK] surface 0x%08X (%ux%u, image %ux%u) sampled by stage %d:"
                                " address 0x%04X, border 0x%08X (%s), stage mode %u\n",
                        rt->va, w, h, rt->pw, rt->ph, i, regs[base + 2] & 0xFFFF,
                        regs[base + 9], (format >> 3) & 1 ? "colour" : "texture", mode);
            }
        }
        if (rt->written)
            s_rt_barrier = 1;
        rt->used = s_frames;
        desc = DESC_SURF0 + (uint32_t)(rt - s_surf);
        s_stage_amin[i] = 0;            /* whatever was drawn there */
    } else {
        desc = tex_get(va, color, w, h, pitch, tex_levels(color, format, w, h, pitch),
                       regs[base + 8], b->pal_va[i]);
        s_stage_amin[i] = desc == DESC_DUMMY ? 2 : s_tex_amin;
    }
    /* Clamped to the border colour on an axis: that colour is sampled too. */
    {
        uint32_t ad = regs[base + 2];
        if ((ad & 0xF) == 4 || ((ad >> 8) & 0xF) == 4) {
            /* sampler_get: transparent black without RECOMP_DK_BORDER */
            uint8_t a = border_on() ? amin_class(regs[base + 9] >> 24) : 0;
            if (a < s_stage_amin[i])
                s_stage_amin[i] = a;
        }
    }
    return dkMakeTextureHandle(desc, samp);
}

/* ── Per-frame state: what every draw relies on ────────────────────── */

static void frame_state(void)
{
    DkBufExtents vs[5], fs;
    DkShader const *sh[2] = { &s_vs, &s_fs };

    dkCmdBufBindShaders(s_cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, sh, 2);
    s_cur_vs = &s_vs;
    s_cur_fs = &s_fs;
    vs[0].addr = s_ubo + UBO_CONSTS; vs[0].size = UBO_CONSTS_SZ;
    vs[1].addr = s_ubo + UBO_PROG;   vs[1].size = UBO_PROG_SZ;
    vs[2].addr = s_ubo + UBO_VSD;    vs[2].size = UBO_VSD_SZ;
    vs[3].addr = s_ubo + UBO_LIGHT;  vs[3].size = UBO_LIGHT_SZ;
    vs[4].addr = s_ubo + UBO_TEX;    vs[4].size = UBO_TEX_SZ;
    fs.addr = s_ubo + UBO_FSD;       fs.size = UBO_FSD_SZ;
    dkCmdBufBindUniformBuffers(s_cmd, DkStage_Vertex, 0, vs, 5);
    dkCmdBufBindUniformBuffers(s_cmd, DkStage_Fragment, 0, &fs, 1);
    dkCmdBufBindImageDescriptorSet(s_cmd, s_imgdesc_gpu, IMGDESC_N);
    dkCmdBufBindSamplerDescriptorSet(s_cmd, s_sampdesc_gpu, SAMP_N);
}

/* The 1x1 texture unbound stages sample: GL's unbound unit, (0,0,0,1). */
static void dummy_up(void)
{
    DkImageLayoutMaker lm;
    DkImageLayout layout;
    DkCopyBuf cb;
    DkImageView v;
    DkImageRect r = { 0, 0, 0, 1, 1, 1 };
    DkGpuAddr gpu;
    uint8_t *p;
    static const uint32_t black = 0xFF000000u;

    dkImageLayoutMakerDefaults(&lm, s_dev);
    lm.format = DkImageFormat_BGRA8_Unorm;
    lm.dimensions[0] = 1;
    lm.dimensions[1] = 1;
    dkImageLayoutInitialize(&layout, &lm);
    s_dummymem = image_alloc(&s_dummy, &layout);
    if (!s_dummymem)
        return;
    write_image_desc(DESC_DUMMY, &s_dummy);
    p = data_alloc(256, 256, &gpu);
    if (!p)
        return;
    memcpy(p, &black, 4);
    cb.addr = gpu; cb.rowLength = 0; cb.imageHeight = 0;
    dkImageViewDefaults(&v, &s_dummy);
    dkCmdBufCopyBufferToImage(s_cmd, &cb, &v, &r, 0);
    s_upload_dirty = 1;
}

static void renderer_up(void)
{
    int i;
    heap_init();
    for (i = 0; i < TEXDESC_N; i++)
        s_desc_free[i] = (uint16_t)(TEXDESC_N - 1 - i);
    s_ndesc_free = TEXDESC_N;
    sampler_get(0x101, 0x02020000, 0, 0);   /* sampler 0: repeat, linear */
    dummy_up();
    /* The uniform buffers start zero, as their shadows do. */
}

/* ── The loading screen and presenting ─────────────────────────────── */

static volatile int s_progress = -1;
static volatile int s_progress_shaders; /* the bar is the shader build's (its own colour) */
static volatile uint32_t s_build_done, s_build_total;   /* its count, for the caption */
static volatile int s_loader_stop;
static pthread_t    s_loader;
static int          s_loader_running;

void nv2a_dk_set_progress(int permille)
{
    s_progress = permille;
}

static void build_progress(uint32_t done, uint32_t total)
{
    s_build_done = done;
    s_build_total = total;
    s_progress_shaders = 1;
    s_progress = (int)((uint64_t)done * 1000 / (total ? total : 1));
}

void nv2a_dk_build_shaders(const char *xbe_path)
{
    if (s_up <= 0)
        return;                         /* no renderer: no compiler either */
    dk_compile_wait(xbe_path, build_progress);
    s_progress = -1;
    s_progress_shaders = 0;
}

/* A filled rectangle on the bound target (top-left coordinates). */
static void rect(int x, int y, int w, int h, float r, float g, float b)
{
    DkScissor sc;
    if (w <= 0 || h <= 0)
        return;
    sc.x = (uint32_t)(x < 0 ? 0 : x);
    sc.y = (uint32_t)(y < 0 ? 0 : y);
    sc.width = (uint32_t)w;
    sc.height = (uint32_t)h;
    dkCmdBufSetScissors(s_cmd, 0, &sc, 1);
    dkCmdBufClearColorFloat(s_cmd, 0, DkColorMask_RGBA, r, g, b, 1.0f);
}

/* `s` in dk_font.h's 5x7 font, `sc` screen pixels a font pixel, top-left
 * at (x, y): a cleared rectangle per run of set pixels in a row. */
static void draw_text(const char *s, int x, int y, int sc, float r, float g, float b)
{
    for (; *s; s++, x += (DK_FONT_COLS + 1) * sc) {
        const uint8_t *gl = dk_font_glyph(*s);
        int row;
        if (!gl)
            continue;
        for (row = 0; row < DK_FONT_ROWS; row++) {
            int col = 0;
            while (col < DK_FONT_COLS) {
                int run = 0;
                while (col + run < DK_FONT_COLS && (gl[row] >> (DK_FONT_COLS - 1 - col - run) & 1))
                    run++;
                if (run) {
                    rect(x + col * sc, y + row * sc, run * sc, sc, r, g, b);
                    col += run;
                } else {
                    col++;
                }
            }
        }
    }
}

/* `s` centred on the screen with its top at y. */
static void draw_caption(const char *s, int y, int sc, float r, float g, float b)
{
    int w = (int)strlen(s) * (DK_FONT_COLS + 1) * sc - sc;
    draw_text(s, ((int)s_fb_w - w) / 2, y, sc, r, g, b);
}

/* Background and a bar: filled to s_progress, or a moving segment. */
static void draw_loading(unsigned frame)
{
    const int W = (int)s_fb_w, H = (int)s_fb_h;
    int bar_w = W * 9 / 32, bar_h = H / 180 > 2 ? H / 180 : 2;
    int bar_x = (W - bar_w) / 2, bar_y = H * 3 / 5;
    int p = s_progress;

    rect(0, 0, W, H, 0.08f, 0.10f, 0.14f);
    rect(bar_x, bar_y, bar_w, bar_h, 0.14f, 0.16f, 0.18f);
    if (p >= 0) {
        /* A caption over the bar and a note under it; a font pixel is
         * 3 screen pixels in the hand, 4 docked. */
        char line[96];
        int sc = H / 240 > 2 ? H / 240 : 2, sc2 = sc * 2 / 3 > 1 ? sc * 2 / 3 : 1;
        int shaders = s_progress_shaders;
        if (p > 1000) p = 1000;
        rect(bar_x, bar_y - bar_h, bar_w, bar_h * 3, 0.14f, 0.16f, 0.18f);
        if (shaders)                    /* building the shader pack: amber */
            rect(bar_x, bar_y - bar_h, bar_w * p / 1000, bar_h * 3, 0.96f, 0.62f, 0.16f);
        else                            /* unpacking the disc image */
            rect(bar_x, bar_y - bar_h, bar_w * p / 1000, bar_h * 3, 0.85f, 0.87f, 0.90f);
        if (shaders)
            snprintf(line, sizeof line, "BUILDING SHADERS  %d%%  (%u / %u)", p / 10,
                     (unsigned)s_build_done, (unsigned)s_build_total);
        else
            snprintf(line, sizeof line, "UNPACKING GAME FILES  %d%%", p / 10);
        draw_caption(line, bar_y - bar_h - 4 * sc - DK_FONT_ROWS * sc, sc, 0.85f, 0.87f, 0.90f);
        draw_caption(shaders ? "FIRST START ONLY - LATER STARTS SKIP THIS"
                             : "FIRST START ONLY",
                     bar_y + 2 * bar_h + 4 * sc2, sc2, 0.50f, 0.53f, 0.58f);
    } else {
        int seg = bar_w / 5;
        int pos = (int)(frame * 6u % (unsigned)(bar_w + seg)) - seg;
        int x0 = bar_x + (pos < 0 ? 0 : pos), x1 = bar_x + pos + seg;
        if (x1 > bar_x + bar_w) x1 = bar_x + bar_w;
        rect(x0, bar_y, x1 - x0, bar_h, 0.85f, 0.87f, 0.90f);
    }
}

/* Record the picture into a swapchain image, submit the frame, present. */
static void present(int show_loading)
{
    int slot;
    DkImageView fbv;
    DkCmdList list;

    if (s_up <= 0 || queue_failed("present"))
        return;
    if (s_ts_cpu) {
        dkCmdBufReportCounter(s_cmd, DkCounter_Timestamp, s_ts_gpu + s_slot * 32 + 16);
        s_ts_set[s_slot] = 1;
        s_pass_end_draws[s_slot] = s_draws;
    }
    s_slot_draws[s_slot] = s_draws - s_draws_at_begin;
    /* Everything drawn this frame is finished before the 2D engine reads it. */
    dkCmdBufBarrier(s_cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
    slot = dkQueueAcquireImage(s_queue, s_swap);
    dkImageViewDefaults(&fbv, &s_fb[slot]);
    dkCmdBufBindRenderTarget(s_cmd, &fbv, NULL);
    s_bound = NULL;
    s_bound_depth = NULL;
    full_rect(s_fb_w, s_fb_h);
    if (show_loading || !s_last) {
        draw_loading(s_frames);
    } else {
        /* The logical aspect (surface / anti-aliasing factor; 16:9 for a
         * widescreen title), letterboxed. Rows are top-first in both. The
         * whole image: at "auto" a 480-line picture is the screen's height,
         * so the 2D engine copies it 1:1. */
        DkSurf *s = s_last;
        DkImageView sv;
        DkImageRect src = { 0, 0, 0, s->pw, s->ph, 1 }, dst;
        float lw = (float)s->w / (float)s->aa_sx, lh = (float)s->h / (float)s->aa_sy;
        float scale, fw = (float)s_fb_w, fh = (float)s_fb_h;
        if (xbox_video_widescreen() && lw < lh * 1.5f)
            lw = lh * 16.0f / 9.0f;
        scale = fw / lw < fh / lh ? fw / lw : fh / lh;
        dst.width = (uint32_t)(lw * scale + 0.5f);
        dst.height = (uint32_t)(lh * scale + 0.5f);
        if (dst.width > s_fb_w) dst.width = s_fb_w;
        if (dst.height > s_fb_h) dst.height = s_fb_h;
        dst.x = (s_fb_w - dst.width) / 2;
        dst.y = (s_fb_h - dst.height) / 2;
        dst.z = 0;
        dst.depth = 1;
        dkCmdBufClearColorFloat(s_cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
        dkImageViewDefaults(&sv, &s->img);
        dkCmdBufBlitImage(s_cmd, &sv, &src, &fbv, &dst,
                          DkBlitFlag_FilterLinear | DkBlitFlag_ModeBlit, 0);
    }
    if (s_heartbeat) {
        /* A square that moves every frame: a hung picture stops it. */
        int x = 8 + (int)(s_frames % 60u) * 4;
        rect(x, 8, 12, 12, 0.95f, 0.55f, 0.10f);
        full_rect(s_fb_w, s_fb_h);
    }
    if (s_show_fps > 0 && !show_loading) {
        /* RECOMP_DK_FPS=1: frames shown per second over the last half
         * second, and the longest gap between two of them in that time --
         * a stutter shows there while the average still reads high. */
        static uint64_t t_win, t_prev, worst;
        static uint32_t f_win;
        static char line[40];
        uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
        if (t_prev && now - t_prev > worst)
            worst = now - t_prev;
        t_prev = now;
        if (!t_win) {
            t_win = now;
            f_win = s_frames;
        } else if (now - t_win >= freq / 2) {
            double secs = (double)(now - t_win) / (double)freq;
            snprintf(line, sizeof line, "%.0f FPS  %.0f MS", (double)(s_frames - f_win) / secs,
                     (double)worst * 1000.0 / (double)freq);
            t_win = now;
            f_win = s_frames;
            worst = 0;
        }
        if (line[0]) {
            int sc = s_fb_h >= 1080 ? 4 : 3;
            int w = (int)strlen(line) * (DK_FONT_COLS + 1) * sc + sc;
            rect(8, 8, w + 2 * sc, (DK_FONT_ROWS + 2) * sc, 0.0f, 0.0f, 0.0f);
            draw_text(line, 8 + 2 * sc, 8 + sc, sc, 1.0f, 0.86f, 0.25f);
            full_rect(s_fb_w, s_fb_h);
        }
    }
    {
        /* The console building shaders (dk_compile.c): a thin bar along the
         * bottom edge, filled as they are done. Not for one or two. */
        uint32_t done, total;
        if (!show_loading && dk_compile_progress(&done, &total) && total >= 4) {
            int h = (int)s_fb_h / 240 > 3 ? (int)s_fb_h / 240 : 3;
            int w = (int)((uint64_t)s_fb_w * done / total);
            rect(w, (int)s_fb_h - h, (int)s_fb_w - w, h, 0.10f, 0.11f, 0.13f);
            rect(0, (int)s_fb_h - h, w, h, 0.85f, 0.87f, 0.90f);
            full_rect(s_fb_w, s_fb_h);
        }
    }
    dkCmdBufSignalFence(s_cmd, &s_fence[s_slot], false);
    list = dkCmdBufFinishList(s_cmd);
    if (queue_failed("present"))
        return;
    s_fence_set[s_slot] = 1;
    data_clean();
    dkQueueSubmitCommands(s_queue, list);
    dkQueuePresentImage(s_queue, s_swap, slot);
    s_frames++;
    if (s_frames == 1 || s_frames == 60) {
        fprintf(stderr, "  [DK] frame %u presented\n", s_frames);
        log_sync();
    }
    if (s_frames % SCREEN_POLL == 0) {
        screen_poll();                  /* docked or undocked */
        if (s_up <= 0)
            return;
    }
    frame_begin();
}

static void *loader_main(void *arg)
{
    (void)arg;
    step("loading screen: first frame");
    frame_begin();
    while (!s_loader_stop && s_up > 0)
        present(1);
    if (s_up > 0 && !queue_failed("loading screen"))
        dkQueueWaitIdle(s_queue);
    return NULL;
}

void nv2a_dk_loader_start(void)
{
    if (getenv("NFSU2_LOADER") && strcmp(getenv("NFSU2_LOADER"), "0") == 0)
        return;
    /* The device on this (the main) thread, before anything else runs: a
     * few milliseconds, and a failure is logged in order. */
    if (!device_up()) {
        fprintf(stderr, "  [DK] loading screen: deko3d did not come up\n");
        log_sync();
        return;
    }
    s_loader_running = pthread_create(&s_loader, NULL, loader_main, NULL) == 0;
}

/* The executor's first call: take the display over from the loader. */
static int ready(void)
{
    static int took;
    if (!took) {
        took = 1;
        if (s_loader_running) {
            s_loader_stop = 1;
            pthread_join(s_loader, NULL);
            s_loader_running = 0;
            fprintf(stderr, "  [DK] loading screen handed over after %u frames\n", s_frames);
        }
        if (device_up()) {
            frame_begin();
            renderer_up();
            fprintf(stderr, "  [DK] renderer up (phase 2: geometry, textures, combiners)%s\n",
                    s_nodraw ? "; RECOMP_DK_NODRAW: draws counted, not drawn" : "");
            log_sync();
        }
    }
    return s_up > 0;
}

/* ── The vertex program, decoded for the ubershader ────────────────── */

static uint32_t fld(const uint32_t *t, int dw, int pos, int bits)
{
    return (t[dw] >> pos) & ((1u << bits) - 1u);
}

/* One 128-bit NV2A instruction into nv2a_vs.glsl's layout. */
static void vp_decode1(const uint32_t *t, uint32_t out[4])
{
    uint32_t mac = fld(t, 1, 21, 4), ilu = fld(t, 1, 25, 3);
    uint32_t rd = fld(t, 3, 20, 4);
    uint32_t creg = (fld(t, 2, 0, 2) << 2) | fld(t, 3, 30, 2);
    uint32_t ld = mac ? 1 : rd;                 /* a paired ILU writes R1 */

    out[0] = mac | (ilu << 4) | (fld(t, 3, 0, 1) << 7)
           | (fld(t, 2, 26, 2) << 8) | (fld(t, 2, 11, 2) << 10) | (fld(t, 3, 28, 2) << 12)
           | (fld(t, 1, 8, 1) << 14) | (fld(t, 2, 25, 1) << 15) | (fld(t, 2, 10, 1) << 16)
           | (fld(t, 3, 2, 1) << 17) | (fld(t, 3, 11, 1) << 18)
           | (fld(t, 3, 1, 1) << 20) | (rd << 21) | (ld << 25);
    out[1] = fld(t, 2, 28, 4) | (fld(t, 2, 13, 4) << 4) | (creg << 8)
           | (fld(t, 1, 9, 4) << 12) | (fld(t, 1, 13, 8) << 16) | (fld(t, 3, 3, 8) << 24);
    out[2] = fld(t, 1, 0, 8) | (fld(t, 2, 17, 8) << 8) | (fld(t, 2, 2, 8) << 16)
           | (fld(t, 3, 24, 4) << 24) | (fld(t, 3, 16, 4) << 28);
    out[3] = fld(t, 3, 12, 4);
}

/* Decode the program from its first slot to FINAL and push what changed. */
static void vp_push_program(const Nv2aRawBatch *b)
{
    uint32_t pc, lo = b->vp_start, hi, end = b->vp_slots < 136 ? b->vp_slots : 136;
    uint32_t dec[136][4];
    int writes_c = 0;

    if (s_prog_gen_pushed == b->vp_prog_gen)
        return;
    s_prog_gen_pushed = b->vp_prog_gen;
    s_prog_versions++;
    if (lo >= end) {
        s_prog_lo = s_prog_hi = 0;
        s_vs_spec = NULL;
        s_vs_key = 0;
        return;
    }
    for (pc = lo; pc < end; pc++) {
        const uint32_t *t = b->vp_program[pc];
        vp_decode1(t, dec[pc]);
        if (fld(t, 3, 12, 4) && !fld(t, 3, 11, 1))
            writes_c = 1;
        if (fld(t, 3, 0, 1)) {
            pc++;
            break;
        }
    }
    hi = pc;
    s_prog_lo = lo;
    s_prog_hi = hi;
    s_prog_writes_c = (uint32_t)writes_c;
    {
        uint64_t key = dk_vs_key(lo, hi - lo, b->vp_program);
        s_vs_key = key;
        s_vs_spec = pack_find(0, key);
        if (!s_vs_spec) {
            /* The ubershader draws it until the compiler is done (a few
             * frames); then pack_take puts this one in its place. */
            if (asked_add(key))
                dk_compile_request_vs(lo, hi - lo, &b->vp_program[lo][0]);
            key_vs(lo, hi - lo, b->vp_program, key);
        }
    }
    if (memcmp(&s_sh_prog[lo], &dec[lo], (size_t)(hi - lo) * 16)) {
        memcpy(&s_sh_prog[lo], &dec[lo], (size_t)(hi - lo) * 16);
        dkCmdBufPushConstants(s_cmd, s_ubo + UBO_PROG, UBO_PROG_SZ, lo * 16, (hi - lo) * 16,
                              &s_sh_prog[lo]);
        s_st.b_push += (hi - lo) * 16;
    }
}

/* The 192 constants: only the rows that changed, as runs (a draw usually
 * changes a matrix or two). */
static void vp_push_consts(const Nv2aRawBatch *b)
{
    uint32_t n = b->vp_const_count < 192 ? b->vp_const_count : 192, r = 0;

    if (s_const_gen_pushed == b->vp_const_gen || !b->vp_consts)
        return;
    /* The GPU's buffer holds the batch before this one's constants: only
     * the rows the title wrote since can differ (192 compares a draw were
     * most of the work, for a dozen rows changed). */
    if (b->vp_const_gen_prev && s_const_gen_pushed == b->vp_const_gen_prev) {
        r = b->vp_const_lo;
        if (b->vp_const_hi < n)
            n = b->vp_const_hi;
    }
    s_const_gen_pushed = b->vp_const_gen;
    while (r < n) {
        uint32_t e;
        if (!memcmp(s_sh_consts[r], b->vp_consts[r], 16)) { r++; continue; }
        for (e = r + 1; e < n && memcmp(s_sh_consts[e], b->vp_consts[e], 16); e++) { }
        memcpy(s_sh_consts[r], b->vp_consts[r], (size_t)(e - r) * 16);
        dkCmdBufPushConstants(s_cmd, s_ubo + UBO_CONSTS, UBO_CONSTS_SZ, r * 16, (e - r) * 16,
                              s_sh_consts[r]);
        s_st.b_push += (e - r) * 16;
        r = e;
    }
}

/* ── Render state ──────────────────────────────────────────────────── */

/* A colour byte as 0..1: (float)b / 255.0f, from a table made with that
 * same expression -- the fragment uniforms convert 66 of them a draw, and a
 * division each was a measurable share of it. */
static float s_u8f[256];
static int   s_u8f_ok;
static float byte_f(uint32_t v, int shift)
{
    if (__builtin_expect(!s_u8f_ok, 0)) {
        int i;
        for (i = 0; i < 256; i++)
            s_u8f[i] = (float)i / 255.0f;
        s_u8f_ok = 1;
    }
    return s_u8f[(v >> shift) & 0xFF];
}

static void argb_vec4(uint32_t v, float out[4])
{
    out[0] = byte_f(v, 16); out[1] = byte_f(v, 8);
    out[2] = byte_f(v, 0);  out[3] = byte_f(v, 24);
}

static uint32_t zmax_of(const uint32_t *r)
{
    return (((r[0x208 / 4] >> 4) & 0xF) == 1) ? 0xFFFFu : 0xFFFFFFu;
}

static DkBlendFactor blend_factor(uint32_t v)
{
    if (v == 0) return DkBlendFactor_Zero;
    if (v == 1) return DkBlendFactor_One;
    if (v >= 0x300 && v <= 0x308) return (DkBlendFactor)(DkBlendFactor_SrcColor + (v - 0x300));
    switch (v) {
    case 0x8001: return DkBlendFactor_ConstColor;
    case 0x8002: return DkBlendFactor_InvConstColor;
    case 0x8003: return DkBlendFactor_ConstAlpha;
    case 0x8004: return DkBlendFactor_InvConstAlpha;
    default:     return DkBlendFactor_One;
    }
}

static DkBlendOp blend_op(uint32_t v)
{
    switch (v) {
    case 0x800A: return DkBlendOp_Sub;
    case 0x800B: return DkBlendOp_RevSub;
    case 0x8007: return DkBlendOp_Min;
    case 0x8008: return DkBlendOp_Max;
    default:     return DkBlendOp_Add;  /* also the signed variants */
    }
}

static DkCompareOp compare_op(uint32_t v, DkCompareOp dflt)
{
    return v >= 0x200 && v <= 0x207 ? (DkCompareOp)(v - 0x200 + 1) : dflt;
}

static DkStencilOp stencil_op(uint32_t v)
{
    switch (v) {
    case 0x0000: return DkStencilOp_Zero;
    case 0x1E01: return DkStencilOp_Replace;
    case 0x1E02: return DkStencilOp_Incr;
    case 0x1E03: return DkStencilOp_Decr;
    case 0x150A: return DkStencilOp_Invert;
    case 0x8507: return DkStencilOp_IncrWrap;
    case 0x8508: return DkStencilOp_DecrWrap;
    default:     return DkStencilOp_Keep;
    }
}

typedef struct {
    uint32_t blend, bsrc, bdst, beq, bcolor;
    uint32_t depth, dfunc, dmask;
    uint32_t stencil, smask, sfunc, sref, sread, sop[3];
    uint32_t cull, cullface, front, cm;
    uint32_t sc[4];
    uint32_t vp[2];
    uint32_t poff, pfactor, pbias, zmax;    /* polygon offset: modes, float bits */
    float    pscale;                        /* the surface's internal factor */
} DkRState;
static DkRState s_rs;
static int      s_front_flip = -1;

/* A rectangle in the surface's real pixels as a scissor on its image. */
static void phys_rect(const DkSurf *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                      uint32_t sc[4])
{
    x0 = phys_edge(x0, s->scx); x1 = phys_edge(x1, s->scx);
    y0 = phys_edge(y0, s->scy); y1 = phys_edge(y1, s->scy);
    if (x1 > s->pw) x1 = s->pw;
    if (y1 > s->ph) y1 = s->ph;
    sc[0] = 0; sc[1] = 0; sc[2] = s->pw; sc[3] = s->ph;
    if (x1 <= x0 || y1 <= y0)
        return;
    sc[0] = x0; sc[1] = y0; sc[2] = x1 - x0; sc[3] = y1 - y0;
}

/* SET_SURFACE_CLIP as a scissor (NFSU2's split screen: each view stops at
 * its half). Full surface otherwise. */
static void surface_scissor(const uint32_t *r, const DkSurf *s, uint32_t sc[4])
{
    uint32_t x0 = (r[0x200 / 4] & 0xFFFF) * s->aa_sx, x1 = x0 + (r[0x200 / 4] >> 16) * s->aa_sx;
    uint32_t y0 = (r[0x204 / 4] & 0xFFFF) * s->aa_sy, y1 = y0 + (r[0x204 / 4] >> 16) * s->aa_sy;

    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 <= x0 || y1 <= y0)
        x0 = y0 = 0, x1 = s->w, y1 = s->h;
    phys_rect(s, x0, y0, x1, y1, sc);
}

/* The title's real pixels onto the surface's image (u_surf stays in the
 * title's: the vertex shaders are unchanged by the internal resolution). */
static void surf_viewport(const DkSurf *s)
{
    DkViewport vp = { 0.0f, 0.0f, (float)((double)s->w * s->scx),
                      (float)((double)s->h * s->scy), 0.0f, 1.0f };
    dkCmdBufSetViewports(s_cmd, 0, &vp, 1);
}

/* RECOMP_DK_FOG=0: the fog unit off -- no fog anywhere (to compare). */
static int fog_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_FOG");
        on = !(e && *e == '0');
        if (!on)
            fprintf(stderr, "  [DK] fog off (RECOMP_DK_FOG=0)\n");
    }
    return on;
}

/* RECOMP_DK_PIXEL_ALIGN=0: positions not shifted onto the image's pixel
 * centres at an internal resolution (to compare; nv2a_vs.glsl, nv2a_clip). */
static int pixel_align_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_PIXEL_ALIGN");
        on = !(e && *e == '0');
        if (!on)
            fprintf(stderr, "  [DK] pixel centres not aligned (RECOMP_DK_PIXEL_ALIGN=0)\n");
    }
    return on;
}

/* RECOMP_DK_ZCLAMP=vertex: depth clamped per vertex in the vertex shaders,
 * as before (nv2a_vs.glsl, nv2a_clip), instead of only per pixel by the
 * rasterizer. To compare. */
static int zclamp_vertex(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_ZCLAMP");
        on = e && !strcmp(e, "vertex");
        if (on)
            fprintf(stderr, "  [DK] depth clamped per vertex (RECOMP_DK_ZCLAMP=vertex)\n");
    }
    return on;
}

/* RECOMP_DK_CULL=0: no face culling (a diagnostic: geometry missing next
 * to the camera -- culled, or clipped?). */
static int cull_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_CULL");
        on = !(e && *e == '0');
        if (!on)
            fprintf(stderr, "  [DK] face culling off (RECOMP_DK_CULL=0)\n");
    }
    return on;
}

/* RECOMP_DK_POLY_OFFSET=0: polygon offset ignored, as before (to compare). */
static int poff_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_POLY_OFFSET");
        on = !(e && *e == '0');
    }
    return on;
}

static void apply_state(const uint32_t *r, int has_depth, const DkSurf *s)
{
    DkRState want;

    memset(&want, 0, sizeof want);
    surface_scissor(r, s, want.sc);
    want.vp[0] = s->pw;
    want.vp[1] = s->ph;
    if (r[0x304 / 4]) {
        want.blend = 1; want.bsrc = r[0x344 / 4]; want.bdst = r[0x348 / 4];
        want.beq = r[0x350 / 4]; want.bcolor = r[0x34C / 4];
    }
    if (has_depth && r[0x30C / 4]) {
        want.depth = 1; want.dfunc = r[0x354 / 4];
    }
    want.dmask = has_depth && r[0x35C / 4];
    if (has_depth && r[0x32C / 4]) {
        want.stencil = 1; want.smask = r[0x360 / 4]; want.sfunc = r[0x364 / 4];
        want.sref = r[0x368 / 4]; want.sread = r[0x36C / 4];
        want.sop[0] = r[0x370 / 4]; want.sop[1] = r[0x374 / 4]; want.sop[2] = r[0x378 / 4];
    }
    if (r[0x308 / 4] && cull_on()) {
        want.cull = 1; want.cullface = r[0x39C / 4]; want.front = r[0x3A0 / 4];
    }
    want.cm = r[0x358 / 4];
    /* Polygon offset (SET_POLY_OFFSET_{POINT,LINE,FILL}_ENABLE 0x330-0x338,
     * SCALE_FACTOR 0x384, BIAS 0x388, floats): JSRF draws its shadows and
     * decals over the ground with it; without it they fought the ground for
     * depth and flickered in and out as the camera moved. */
    if (has_depth && (r[0x330 / 4] || r[0x334 / 4] || r[0x338 / 4]) && poff_on()) {
        want.poff = (r[0x330 / 4] ? DkPolygonFlag_Point : 0u) | (r[0x334 / 4] ? DkPolygonFlag_Line : 0u)
                  | (r[0x338 / 4] ? DkPolygonFlag_Fill : 0u);
        want.pfactor = r[0x384 / 4];
        want.pbias = r[0x388 / 4];
        want.zmax = zmax_of(r);
        want.pscale = (float)s->scx;
    }
    if (s_rs_valid && !memcmp(&want, &s_rs, sizeof want))
        return;
    s_rs = want;
    s_rs_valid = 1;
    if (s_front_flip < 0)
        s_front_flip = getenv("RECOMP_DK_FRONT_FLIP") != NULL;

    {
        DkScissor sc = { want.sc[0], want.sc[1], want.sc[2], want.sc[3] };
        surf_viewport(s);
        dkCmdBufSetScissors(s_cmd, 0, &sc, 1);
    }
    {
        DkRasterizerState rs;
        dkRasterizerStateDefaults(&rs);
        /* The NV2A clamps depth instead of clipping at the near and far
         * planes (NFSU2's loading screen sits at z = 1.0). */
        rs.depthClampEnable = 1;
        if (want.poff) {
            /* The NV2A's units are its own depth steps (z in 0..zmax); the
             * image is 24-bit, so a 16-bit surface's step is 256 of ours.
             * The slope term follows the pixel, which the internal
             * resolution makes smaller: scaled back up by its factor. */
            float factor, bias;
            memcpy(&factor, &want.pfactor, 4);
            memcpy(&bias, &want.pbias, 4);
            if (want.zmax == 0xFFFFu)
                bias *= 256.0f;
            rs.depthBiasEnableMask = want.poff;
            dkCmdBufSetDepthBias(s_cmd, bias, 0.0f, factor * (want.pscale > 0.0f ? want.pscale : 1.0f));
        }
        if (want.cull) {
            rs.cullMode = want.cullface == 0x404 ? DkFace_Front
                        : want.cullface == 0x408 ? DkFace_FrontAndBack : DkFace_Back;
            /* GL's CW/CCW, as the Vulkan renderer maps them (deko3d's
             * upper-left origin is Vulkan's). */
            rs.frontFace = ((want.front == 0x901) != s_front_flip) ? DkFrontFace_CCW : DkFrontFace_CW;
        } else {
            rs.cullMode = DkFace_None;
        }
        dkCmdBufBindRasterizerState(s_cmd, &rs);
    }
    {
        DkColorState cs;
        DkColorWriteState cw;
        DkBlendState bs;
        uint32_t cm = want.cm, mask = 0;
        dkColorStateDefaults(&cs);
        dkColorStateSetBlendEnable(&cs, 0, want.blend != 0);
        dkCmdBufBindColorState(s_cmd, &cs);
        if (cm & 0x00010000u) mask |= DkColorMask_R;
        if (cm & 0x00000100u) mask |= DkColorMask_G;
        if (cm & 0x00000001u) mask |= DkColorMask_B;
        if (cm & 0x01000000u) mask |= DkColorMask_A;
        dkColorWriteStateDefaults(&cw);
        dkColorWriteStateSetMask(&cw, 0, mask);
        dkCmdBufBindColorWriteState(s_cmd, &cw);
        if (want.blend) {
            float bc[4];
            dkBlendStateDefaults(&bs);
            bs.colorBlendOp = bs.alphaBlendOp = blend_op(want.beq);
            bs.srcColorBlendFactor = bs.srcAlphaBlendFactor = blend_factor(want.bsrc);
            bs.dstColorBlendFactor = bs.dstAlphaBlendFactor = blend_factor(want.bdst);
            dkCmdBufBindBlendStates(s_cmd, 0, &bs, 1);
            argb_vec4(want.bcolor, bc);
            dkCmdBufSetBlendConst(s_cmd, bc[0], bc[1], bc[2], bc[3]);
        }
    }
    {
        DkDepthStencilState ds;
        dkDepthStencilStateDefaults(&ds);
        /* As the GL renderer: with the test off nothing is written either. */
        ds.depthTestEnable = want.depth;
        ds.depthWriteEnable = want.dmask;
        ds.depthCompareOp = compare_op(want.dfunc, DkCompareOp_Lequal);
        ds.stencilTestEnable = want.stencil;
        if (want.stencil) {
            ds.stencilFrontFailOp = ds.stencilBackFailOp = stencil_op(want.sop[0]);
            ds.stencilFrontDepthFailOp = ds.stencilBackDepthFailOp = stencil_op(want.sop[1]);
            ds.stencilFrontPassOp = ds.stencilBackPassOp = stencil_op(want.sop[2]);
            ds.stencilFrontCompareOp = ds.stencilBackCompareOp =
                compare_op(want.sfunc, DkCompareOp_Always);
            dkCmdBufSetStencil(s_cmd, DkFace_FrontAndBack, (uint8_t)(want.smask & 0xFF),
                               (uint8_t)(want.sref & 0xFF), (uint8_t)(want.sread & 0xFF));
        }
        dkCmdBufBindDepthStencilState(s_cmd, &ds);
    }
}

/* ── Vertices ──────────────────────────────────────────────────────── */

/* The byte ranges the direct attributes span, merged where they overlap
 * (interleaved attributes share one), each from a 4-aligned start; of_run[a]
 * says which range attribute a is in. As the GL renderer. */
typedef struct { const uint8_t *lo, *hi; uint32_t at; } VRun;

static uint32_t attr_elem(uint32_t type, uint32_t size)
{
    switch (type) {
    case 0:  return 4;
    case 2:  return 4 * size;
    case 4:  return size;
    default: return 2 * size;
    }
}

static uint32_t vertex_runs(const Nv2aRawBatch *b, VRun *run, int *of_run)
{
    uint32_t nv = b->vertex_count, nrun = 0, a, i;

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(b->attr_direct & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv - 1) * b->direct[a].stride
           + attr_elem(b->direct[a].type, b->direct[a].size);
        for (i = 0; i < nrun; i++)
            if (lo <= run[i].hi && run[i].lo <= hi)
                break;
        if (i == nrun) {
            run[nrun].lo = lo;
            run[nrun].hi = hi;
            nrun++;
        } else {
            if (lo < run[i].lo) run[i].lo = lo;
            if (hi > run[i].hi) run[i].hi = hi;
        }
        of_run[a] = (int)i;
    }
    for (i = 0; i + 1 < nrun; ) {
        uint32_t j;
        for (j = i + 1; j < nrun; j++)
            if (run[j].lo <= run[i].hi && run[i].lo <= run[j].hi)
                break;
        if (j == nrun) { i++; continue; }
        if (run[j].lo < run[i].lo) run[i].lo = run[j].lo;
        if (run[j].hi > run[i].hi) run[i].hi = run[j].hi;
        for (a = 0; a < NV2A_RAW_ATTRS; a++) {
            if (!(b->attr_direct & (1u << a)))
                continue;
            if (of_run[a] == (int)j)
                of_run[a] = (int)i;
            else if (of_run[a] == (int)(nrun - 1))
                of_run[a] = (int)j;
        }
        run[j] = run[nrun - 1];
        nrun--;
    }
    return nrun;
}

static DkVtxAttribSize vsize(uint32_t bits, uint32_t n)
{
    static const DkVtxAttribSize s32[4] = { DkVtxAttribSize_1x32, DkVtxAttribSize_2x32,
                                            DkVtxAttribSize_3x32, DkVtxAttribSize_4x32 };
    static const DkVtxAttribSize s16[4] = { DkVtxAttribSize_1x16, DkVtxAttribSize_2x16,
                                            DkVtxAttribSize_3x16, DkVtxAttribSize_4x16 };
    static const DkVtxAttribSize s8[4]  = { DkVtxAttribSize_1x8, DkVtxAttribSize_2x8,
                                            DkVtxAttribSize_3x8, DkVtxAttribSize_4x8 };
    if (n < 1) n = 1;
    if (n > 4) n = 4;
    return bits == 32 ? s32[n - 1] : bits == 16 ? s16[n - 1] : s8[n - 1];
}

/* Vertex and index data for one draw, in one data allocation, and the
 * attribute/buffer state that reads it. Returns the index count, 0 to skip. */
static uint32_t upload_draw(const Nv2aRawBatch *b, DkIdxFormat *ifmt)
{
    VRun run[NV2A_RAW_ATTRS];
    int of_run[NV2A_RAW_ATTRS];
    uint32_t slot[NV2A_RAW_ATTRS], np = 0, nrun, a, i, v, bytes = 0, pack_at, idx_at, isz;
    uint32_t nv = b->vertex_count, n = b->index_count;
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    DkVtxAttribState as[NV2A_RAW_ATTRS];
    DkVtxBufferState bs[NV2A_RAW_ATTRS];
    DkBufExtents ext[NV2A_RAW_ATTRS];
    DkGpuAddr gpu;
    uint8_t *dst;

    if (!nv || !n)
        return 0;
    nrun = vertex_runs(b, run, of_run);
    for (i = 0; i < nrun; i++) {
        run[i].at = bytes;
        bytes += align_up((uint32_t)(run[i].hi - run[i].lo), 16);
    }
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (conv & (1u << a))
            slot[np++] = a;
    pack_at = bytes;
    bytes += nv * np * 16;
    idx_at = align_up(bytes, 16);
    *ifmt = nv <= 65536 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint32;
    isz = *ifmt == DkIdxFormat_Uint16 ? 2 : 4;
    bytes = idx_at + n * isz;
    dst = data_alloc(bytes, 256, &gpu);
    if (!dst)
        return 0;
    s_st.b_vtx += idx_at;
    s_st.b_idx += bytes - idx_at;

    uint64_t t_copy0 = armGetSystemTick();
    for (i = 0; i < nrun; i++)
        memcpy(dst + run[i].at, run[i].lo, (size_t)(run[i].hi - run[i].lo));
    if (np && b->attrs_packed) {
        memcpy(dst + pack_at, b->attrs, (size_t)nv * np * 16);    /* a queue packed them */
    } else if (np) {
        float *out = (float *)(dst + pack_at);
        for (v = 0; v < nv; v++)
            for (i = 0; i < np; i++)
                memcpy(out + ((size_t)v * np + i) * 4,
                       b->attrs + ((size_t)v * NV2A_RAW_ATTRS + slot[i]) * 4, 16);
    }
    {
        /* Rebased here, on the way into the ring (NV2A_BACKEND_RAW_ABS_INDICES):
         * the executor no longer makes a rebased copy first. */
        /* Eight at a time with NEON: the scalar loop was the single
         * biggest line of the renderer's per-draw work (it stays scalar:
         * the compiler cannot tell the two buffers apart). */
        const uint32_t *__restrict ix = b->indices, base = b->index_base;
        i = 0;
        if (isz == 2) {
            uint16_t *__restrict o = (uint16_t *)(dst + idx_at);
#if defined(__aarch64__)
            uint32x4_t vb = vdupq_n_u32(base);
            for (; i + 8 <= n; i += 8) {
                uint32x4_t a = vsubq_u32(vld1q_u32(ix + i), vb);
                uint32x4_t c = vsubq_u32(vld1q_u32(ix + i + 4), vb);
                vst1q_u16(o + i, vcombine_u16(vmovn_u32(a), vmovn_u32(c)));
            }
#endif
            for (; i < n; i++)
                o[i] = (uint16_t)(ix[i] - base);
        } else {
            uint32_t *__restrict o = (uint32_t *)(dst + idx_at);
#if defined(__aarch64__)
            uint32x4_t vb = vdupq_n_u32(base);
            for (; i + 8 <= n; i += 8) {
                vst1q_u32(o + i, vsubq_u32(vld1q_u32(ix + i), vb));
                vst1q_u32(o + i + 4, vsubq_u32(vld1q_u32(ix + i + 4), vb));
            }
#endif
            for (; i < n; i++)
                o[i] = ix[i] - base;
        }
    }
    s_st.t_copy += armGetSystemTick() - t_copy0;

    /* One buffer binding per attribute, so a big stride or offset never
     * meets the attribute state's 14-bit offset. */
    memset(as, 0, sizeof as);
    memset(bs, 0, sizeof bs);
    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        as[a].bufferId = a;
        ext[a].addr = gpu;
        ext[a].size = 16;
        if (!(b->attr_present & (1u << a))) {
            as[a].isFixed = 1;          /* the shader reads u_attr[a] instead */
            as[a].size = DkVtxAttribSize_4x32;
            as[a].type = DkVtxAttribType_Float;
            continue;
        }
        if (b->attr_direct & (1u << a)) {
            uint32_t t = b->direct[a].type, sz = b->direct[a].size;
            uint32_t off = run[of_run[a]].at + (uint32_t)(b->direct[a].ptr - run[of_run[a]].lo);
            ext[a].addr = gpu + off;
            ext[a].size = (nv - 1) * b->direct[a].stride + attr_elem(t, sz);
            bs[a].stride = b->direct[a].stride;
            switch (t) {
            case 0:  as[a].size = DkVtxAttribSize_4x8; as[a].type = DkVtxAttribType_Unorm;
                     as[a].isBgra = 1; break;                     /* D3DCOLOR */
            case 1:  as[a].size = vsize(16, sz); as[a].type = DkVtxAttribType_Snorm; break;
            case 2:  as[a].size = vsize(32, sz); as[a].type = DkVtxAttribType_Float; break;
            case 4:  as[a].size = vsize(8, sz);  as[a].type = DkVtxAttribType_Unorm; break;
            default: as[a].size = vsize(16, sz); as[a].type = DkVtxAttribType_Sscaled; break;
            }
        } else {
            for (i = 0; i < np && slot[i] != a; i++) { }
            ext[a].addr = gpu + pack_at + i * 16;
            ext[a].size = nv * np * 16 - i * 16;
            bs[a].stride = np * 16;
            as[a].size = DkVtxAttribSize_4x32;
            as[a].type = DkVtxAttribType_Float;
        }
    }
    dkCmdBufBindVtxAttribState(s_cmd, as, NV2A_RAW_ATTRS);
    dkCmdBufBindVtxBufferState(s_cmd, bs, NV2A_RAW_ATTRS);
    dkCmdBufBindVtxBuffers(s_cmd, 0, ext, NV2A_RAW_ATTRS);
    dkCmdBufBindIdxBuffer(s_cmd, *ifmt, gpu + idx_at);
    return n;
}

/* ── The back end ──────────────────────────────────────────────────── */

static int mul_zero_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VSH_MUL_ZERO");
        on = !(e && e[0] == '0' && e[1] == '\0');
    }
    return on;
}

static void dk_clear_body(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                          uint32_t flags, uint32_t argb, uint32_t zstencil);

static void dk_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    uint64_t t0 = armGetSystemTick();
    dk_clear_body(sf, rs, flags, argb, zstencil);
    s_st.t_clear += armGetSystemTick() - t0;
}

static void dk_clear_body(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                          uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    DkSurf *s;
    DkScissor sc;

    if (!ready())
        return;
    flush_if_low();
    s = surf_bind(sf, rs ? rs->zeta_va : 0);
    if (!s)
        return;
    s_rs_valid = 0;                     /* viewport, scissor and masks change here */
    sc.x = 0; sc.y = 0; sc.width = s->pw; sc.height = s->ph;
    if (s_regs) {
        /* The clear rectangle is in logical pixels, like SURFACE_CLIP; the
         * surface is larger by the anti-aliasing factor, its image by the
         * internal resolution. */
        uint32_t hz = s_regs[0x1D98 / 4], vt = s_regs[0x1D9C / 4];
        uint32_t x0 = (hz & 0xFFFF) * s->aa_sx, x1 = ((hz >> 16) + 1) * s->aa_sx;
        uint32_t y0 = (vt & 0xFFFF) * s->aa_sy, y1 = ((vt >> 16) + 1) * s->aa_sy;
        if (x1 > s->w) x1 = s->w;
        if (y1 > s->h) y1 = s->h;
        if (x1 > x0 && y1 > y0) {
            uint32_t r4[4];
            phys_rect(s, x0, y0, x1, y1, r4);
            sc.x = r4[0]; sc.y = r4[1]; sc.width = r4[2]; sc.height = r4[3];
        }
    }
    surf_viewport(s);
    dkCmdBufSetScissors(s_cmd, 0, &sc, 1);
    if (flags & 0xF0) {
        uint32_t mask = ((flags & 0x10) ? DkColorMask_R : 0) | ((flags & 0x20) ? DkColorMask_G : 0)
                      | ((flags & 0x40) ? DkColorMask_B : 0) | ((flags & 0x80) ? DkColorMask_A : 0);
        dkCmdBufClearColorFloat(s_cmd, 0, mask, byte_f(argb, 16), byte_f(argb, 8),
                                byte_f(argb, 0), byte_f(argb, 24));
    }
    if ((flags & 0x3) && s_bound_depth) {
        uint32_t zmax = s_regs ? zmax_of(s_regs) : 0xFFFFFFu;
        uint32_t z = zmax == 0xFFFFu ? (zstencil & 0xFFFF) : (zstencil >> 8);
        dkCmdBufClearDepthStencil(s_cmd, (flags & 0x1) != 0, (float)((double)z / (double)zmax),
                                  (flags & 0x2) ? 0xFF : 0, (uint8_t)(zstencil & 0xFF));
    }
    s->written = 1;
    s_clears++;
    s_last = s;
}

/* FIXED-FUNCTION LIGHTING. A draw without a vertex program and with
 * SET_LIGHTING_ENABLE on gets its colour from the lights and the material,
 * not from its vertices: nv2a_vs_fixed.glsl computes it from these
 * registers (the CPU path's lit_color in nv2a_pb_exec.c, on the GPU). The
 * raw batches carry the vertex colour only, and every lit draw used to come
 * out in it -- white where the vertices have none: JSRF's pause map was a
 * white silhouette, its text on it unreadable, the settings screen white.
 * RECOMP_DK_LIGHTING=0: as before (to compare). */
static int light_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_LIGHTING");
        on = !(e && *e == '0');
        if (!on)
            fprintf(stderr, "  [DK] fixed-function lighting off (RECOMP_DK_LIGHTING=0)\n");
    }
    return on;
}

static UboVsLight s_sh_light;
static int        s_sh_light_valid;

static void reg_vec(const uint32_t *r, uint32_t reg, uint32_t n, float *out)
{
    memcpy(out, &r[reg / 4], n * 4);
}

/* The lighting registers into VsLight, pushed when they changed. */
static void light_push(const uint32_t *r)
{
    UboVsLight l;
    uint32_t i;
    memset(&l, 0, sizeof l);
    reg_vec(r, 0x0480, 16, &l.mv[0][0]);        /* SET_MODEL_VIEW_MATRIX0 */
    reg_vec(r, 0x0A10, 3, l.scene);             /* SET_SCENE_AMBIENT_COLOR */
    reg_vec(r, 0x03B4, 1, &l.scene[3]);         /* SET_MATERIAL_ALPHA */
    l.ctl[0] = r[0x03BC / 4];                   /* SET_LIGHT_ENABLE_MASK */
    l.ctl[1] = r[0x0298 / 4];                   /* SET_COLOR_MATERIAL */
    for (i = 0; i < 8; i++) {
        uint32_t b = 0x1000u + i * 0x80u;
        if (!((l.ctl[0] >> (2 * i)) & 3))
            continue;
        reg_vec(r, b + 0x00, 3, l.amb[i]);      /* AMBIENT_COLOR */
        reg_vec(r, b + 0x24, 1, &l.amb[i][3]);  /* LOCAL_RANGE */
        reg_vec(r, b + 0x0C, 3, l.dif[i]);      /* DIFFUSE_COLOR */
        reg_vec(r, b + 0x34, 3, l.dir[i]);      /* INFINITE_DIRECTION */
        reg_vec(r, b + 0x5C, 3, l.pos[i]);      /* LOCAL_POSITION */
        reg_vec(r, b + 0x68, 3, l.att[i]);      /* LOCAL_ATTENUATION */
    }
    if (s_sh_light_valid && !memcmp(&l, &s_sh_light, sizeof l))
        return;
    s_sh_light = l;
    s_sh_light_valid = 1;
    dkCmdBufPushConstants(s_cmd, s_ubo + UBO_LIGHT, UBO_LIGHT_SZ, 0, sizeof l, &l);
    s_st.b_push += sizeof l;
    s_st.lit_push++;
}

/* TEXTURE COORDINATE GENERATION AND TEXTURE MATRICES, for draws without a
 * vertex program (nv2a_vs_fixed.glsl, texcoords). The T&L unit makes a
 * stage's coordinates from the eye-space normal when SET_TEXGEN_S..Q asks
 * (normal, reflection and sphere maps) and runs each stage's coordinates
 * through its SET_TEXTURE_MATRIX when SET_TEXTURE_MATRIX_ENABLE says so.
 * Neither was done: those draws sampled at the vertex's own coordinates,
 * which for JSRF's reflective glass and panels are not where the reflection
 * is (upstream JSRF Unleashed 0.5.5: dark bars on Dogenzaka Hill's
 * footbridge, glass in stripes). The eye-linear and object-linear modes are
 * not made; the first draw asking says so. RECOMP_DK_TEXGEN=0: as before. */
static int texgen_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_TEXGEN");
        on = !(e && *e == '0');
        fprintf(stderr, "  [DK] texgen and texture matrices %s (RECOMP_DK_TEXGEN)\n",
                on ? "on" : "off");
    }
    return on;
}

static UboVsTex s_sh_tex;
static int      s_sh_tex_valid;

/* VsTex from the registers, pushed when it changed. Returns 1 when the draw
 * uses a texture matrix or generated coordinates, 2 when the generated ones
 * need the model-view (VsLight) as well, 0 for neither. */
static int tex_push(const uint32_t *r)
{
    UboVsTex t;
    uint32_t st, c;
    int any = 0;

    memset(&t, 0, sizeof t);
    for (st = 0; st < 4; st++) {
        if (r[(0x0420 + 4 * st) / 4]) {             /* SET_TEXTURE_MATRIX_ENABLE */
            t.ctl[0] |= 1u << st;
            reg_vec(r, 0x06C0 + 0x40 * st, 16, &t.mat[4 * st][0]);
        }
        for (c = 0; c < 4; c++) {
            uint32_t mode = r[(0x03C0 + 0x10 * st + 4 * c) / 4], g = 0;
            switch (mode) {
            case 0x0000: break;
            case 0x8511: g = c < 3 ? 1u : 0u; break;    /* NORMAL_MAP */
            case 0x8512: g = c < 3 ? 2u : 0u; break;    /* REFLECTION_MAP */
            case 0x2402: g = c < 2 ? 3u : 0u; break;    /* SPHERE_MAP */
            default: {
                static int said;
                if (said++ < 4)
                    fprintf(stderr, "  [DK] texgen mode %04X (stage %u, %c) is not made: "
                            "the vertex's own coordinate is used\n", mode, st, "STRQ"[c]);
                break;
            }
            }
            t.gen[st][c] = g;
            if (g)
                t.ctl[2] |= 1u << st;
        }
    }
    t.ctl[1] = r[0x03A4 / 4] ? 1u : 0u;                 /* SET_NORMALIZATION_ENABLE */
    if (!t.ctl[0] && !t.ctl[2])
        return 0;
    any = t.ctl[2] ? 2 : 1;
    if (!s_sh_tex_valid || memcmp(&t, &s_sh_tex, sizeof t)) {
        s_sh_tex = t;
        s_sh_tex_valid = 1;
        dkCmdBufPushConstants(s_cmd, s_ubo + UBO_TEX, UBO_TEX_SZ, 0, sizeof t, &t);
        s_st.b_push += sizeof t;
    }
    if (t.ctl[2])
        s_st.texgen++;
    else
        s_st.texmat++;
    return any;
}

/* One draw of a captured frame (RECOMP_DK_CAPTURE), two or three lines. */
static void cap_draw(const Nv2aRawBatch *b, const uint32_t *r, const DkSurf *s,
                     const UboFsDraw *fd, const uint32_t *k, const DkShader *vs,
                     const DkShader *fs)
{
    char line[1400];
    int at = 0, i;
    if (++s_cap_draws > 300)
        return;
#define CAP(...) do { if (at < (int)sizeof line) \
        at += snprintf(line + at, sizeof line - (size_t)at, __VA_ARGS__); } while (0)
    CAP("  [CAP] d%u prim %u n %u idx %u xf %u attrs %04X tgt 0x%08X%s | vs %s %016llx | fs %s",
        s_cap_draws, b->prim, b->vertex_count, b->index_count, b->xform, b->attr_present,
        s->va, b->zeta_va ? "+z" : "",
        b->xform != 2 ? "fixed" : vs == &s_vs ? "uber" : "pack",
        (unsigned long long)(b->xform == 2 ? s_vs_key : 0ull),
        fs == &s_fs ? "uber" : fs == s_fs_spec_noat ? "pack-noat" : "pack");
    CAP(" | blend %u %X/%X eq %X | depth %u f %X w %u | atest %X | cull %u | cmask %08X"
        " | light %u mask %04X cmat %X | fog %u mode %X gen %u p %08X %08X col %08X",
        r[0x304 / 4], r[0x344 / 4], r[0x348 / 4], r[0x350 / 4], r[0x30C / 4], r[0x354 / 4],
        r[0x35C / 4], fd->ctl2[1], r[0x308 / 4], r[0x358 / 4], r[0x314 / 4], r[0x3BC / 4],
        r[0x298 / 4], r[0x2A4 / 4], r[0x29C / 4], r[0x2A0 / 4], r[0x9C0 / 4], r[0x9C4 / 4],
        r[0x2A8 / 4]);
    for (i = 0; i < 4; i++) {
        uint32_t base = (0x1B00u + (uint32_t)i * 0x40u) / 4;
        if (!(r[base + 3] & 0x40000000u) || !b->tex_va[i])
            continue;
        CAP(" | t%d 0x%08X%s fmt %08X rect %08X pitch %u addr %04X filt %08X ctl0 %08X pal %08X/0x%08X sc %g,%g",
            i, b->tex_va[i], surf_find(b->tex_va[i]) ? "(rt)" : "", r[base + 1], r[base + 7],
            r[base + 4] >> 16, r[base + 2] & 0xFFFF, r[base + 5], r[base + 3], r[base + 8],
            b->pal_va[i], fd->tscale[i][0], fd->tscale[i][1]);
    }
    fprintf(stderr, "%s\n", line);
    at = 0;
    CAP("  [CAP]    key");
    for (i = 0; i < (int)DK_FS_KEY_WORDS; i++)
        CAP(" %08X", k[i]);
    CAP(" | c0");
    for (i = 0; i < 8; i++)
        CAP(" %08X", r[0xA60 / 4 + i]);
    CAP(" | c1");
    for (i = 0; i < 8; i++)
        CAP(" %08X", r[0xA80 / 4 + i]);
    CAP(" | fc %08X %08X fog %08X", r[0x1E20 / 4], r[0x1E24 / 4], r[0x2A8 / 4]);
    fprintf(stderr, "%s\n", line);
    at = 0;
    CAP("  [CAP]    v0");
    for (i = 0; i < NV2A_RAW_ATTRS && i < 16; i++) {
        if (!(b->attr_present & (1u << i)))
            continue;
        if ((b->attr_direct & (1u << i)) && b->direct[i].ptr) {
            const uint8_t *p = b->direct[i].ptr;
            CAP(" a%d(t%u s%u)", i, b->direct[i].type, b->direct[i].size);
            if (b->direct[i].type == 2) {
                float f[4] = { 0, 0, 0, 1 };
                memcpy(f, p, (b->direct[i].size > 4 ? 4 : b->direct[i].size) * 4);
                CAP("=%g,%g,%g,%g", f[0], f[1], f[2], f[3]);
            } else {
                CAP("=%02X%02X%02X%02X", p[0], p[1], p[2], p[3]);
            }
        } else if (b->attrs) {
            const float *f = b->attrs + i * 4;
            CAP(" a%d=%g,%g,%g,%g", i, f[0], f[1], f[2], f[3]);
        }
    }
    CAP(" | const d %g,%g,%g,%g", b->attr_const[3][0], b->attr_const[3][1], b->attr_const[3][2],
        b->attr_const[3][3]);
    fprintf(stderr, "%s\n", line);
#undef CAP
}

/* RECOMP_DK_DRAWSYNC=1 (=<n>: from n seconds after the renderer starts):
 * every draw submitted and waited for on its own, so that when the GPU
 * rejects or faults on something, the draw it was set up with is named --
 * prim, counts, shaders, combiner key, textures, state (cap_draw's lines) --
 * not just the frame. The GPU idles between draws, so the game runs slower:
 * a diagnostic for a GPU fault, not a setting. If the fault goes away with
 * it, the commands themselves are sound and something overlaps in time. */
static uint32_t s_ds_k[DK_FS_KEY_WORDS];
static const DkShader *s_ds_vs, *s_ds_fs;

static int drawsync_on(void)
{
    static int state = -1;              /* -1 unread, 0 off, 1 on */
    static uint64_t from;
    if (state < 0) {
        const char *e = getenv("RECOMP_DK_DRAWSYNC");
        int sec = e ? atoi(e) : 0;
        state = e && *e && !(e[0] == '0' && !e[1]);
        if (state) {
            from = armGetSystemTick() + (uint64_t)(sec > 1 ? sec : 0) * armGetSystemTickFreq();
            fprintf(stderr, "  [DK] RECOMP_DK_DRAWSYNC: every draw submitted and waited for"
                            " alone%s (slow; to name a draw the GPU stops on)\n",
                    sec > 1 ? ", from the time set" : "");
        }
    }
    return state && armGetSystemTick() >= from;
}

/* AN ALPHA TEST THAT CANNOT FAIL, PROVED (RECOMP_DK_AT_PROVE=0: off).
 *
 * JSRF leaves the alpha test on for nearly every draw as GREATER 0 --
 * "drop what is fully transparent". A shader that can discard runs before
 * the depth test, so with the test in it the GPU shades every pixel of
 * every surface, hidden or not; character select was 20-27 ms of GPU a
 * frame that way. Where the alpha the combiners produce is above 0 at
 * every pixel, the test passes everywhere, and the setup's build without
 * it (DK_KEY_FS_NOAT) draws the very same pixels with the depth test
 * first.
 *
 * Whether it is: the combiners' alpha path as dk_shadergen.c makes it, followed
 * with what each value can be -- 0, 0 or more, above 0, 1 (about), or
 * anything -- from the textures (DkTex.amin, the border colour, a render
 * target counting as anything), the constants (known), the vertex colours
 * (read from the batch, only when needed) and the fog (0 or more). The
 * final combiner's G input must come out above 0. Anything not followed
 * counts as "may be 0" and keeps the test. */
enum { AV_Z, AV_NN, AV_P, AV_1, AV_U };     /* 0, >=0, >0, ~1, anything */

static int av_pos(int v) { return v == AV_P || v == AV_1; }

static int av_const(float x)
{
    return x <= 0.0f ? AV_Z : x >= 1.0f ? AV_1 : AV_P;
}

static int av_map(int v, uint32_t map)
{
    switch (map) {
    case 0: return v == AV_U ? AV_NN : v;                               /* unsigned */
    case 1: return v == AV_Z ? AV_1 : v == AV_1 ? AV_Z : AV_NN;         /* unsigned invert */
    case 2: return v == AV_1 ? AV_1 : AV_U;                             /* expand normal */
    case 3: return v == AV_Z ? AV_1 : AV_U;                             /* expand negate */
    case 4: return v == AV_1 ? AV_P : AV_U;                             /* half-bias normal */
    case 5: return v == AV_Z ? AV_P : AV_U;                             /* half-bias negate */
    case 6: return v;                                                   /* signed identity */
    default: return v == AV_Z ? AV_Z : AV_U;                            /* signed negate */
    }
}

static int av_mul(int a, int b)
{
    if (a == AV_Z || b == AV_Z) return AV_Z;
    if (a == AV_U || b == AV_U) return AV_U;
    if (a == AV_1) return b;
    if (b == AV_1) return a;
    return a == AV_P && b == AV_P ? AV_P : AV_NN;
}

static int av_add(int a, int b)
{
    if (a == AV_U || b == AV_U) return AV_U;
    if (a == AV_Z) return b;
    if (b == AV_Z) return a;
    return av_pos(a) || av_pos(b) ? AV_P : AV_NN;
}

static int av_either(int a, int b)              /* the mux: one or the other */
{
    if (a == AV_U || b == AV_U) return AV_U;
    if (a == b) return a;
    if (av_pos(a) && av_pos(b)) return AV_P;
    return AV_NN;
}

static int av_scale(int v, uint32_t op)         /* then clamped to [-1, 1] */
{
    switch (op) {
    case 0: return v;
    case 2: case 4: case 6: return v == AV_1 ? AV_P : v;
    case 1: case 3: return v == AV_1 ? AV_P : AV_U;                      /* - 0.5 */
    default: return AV_U;
    }
}

/* The alpha of vertex attribute a over the batch's vertices (AV_*). */
static int av_vertex(const Nv2aRawBatch *b, int a)
{
    uint32_t bit = 1u << a, v, n = b->vertex_count, lo = 2;
    if (!(b->attr_present & bit))
        return av_const(b->attr_const[a][3]);
    if (n > 8192)
        return AV_NN;
    if (b->attr_direct & bit) {
        const uint8_t *p = b->direct[a].ptr;
        uint32_t st = b->direct[a].stride, ty = b->direct[a].type, sz = b->direct[a].size;
        if (!p)
            return AV_NN;
        if ((ty == 0 || ty == 4) && sz == 4) {           /* D3DCOLOR / unsigned bytes */
            for (v = 0; v < n && lo; v++, p += st) {
                uint32_t x = p[3];
                if (x == 0) lo = 0; else if (x < 255) lo = 1;
            }
        } else if (ty == 2 && sz < 4) {
            return AV_1;                                 /* no alpha: reads 1 */
        } else if (ty == 2) {
            for (v = 0; v < n && lo; v++, p += st) {
                float x;
                memcpy(&x, p + 12, 4);
                if (!(x > 0.0f)) lo = 0; else if (x < 1.0f) lo = 1;
            }
        } else {
            return AV_NN;
        }
    } else if (b->attrs && !b->attrs_packed) {
        for (v = 0; v < n && lo; v++) {
            float x = b->attrs[((size_t)v * 16 + (size_t)a) * 4 + 3];
            if (!(x > 0.0f)) lo = 0; else if (x < 1.0f) lo = 1;
        }
    } else {
        return AV_NN;
    }
    return lo == 2 ? AV_1 : lo == 1 ? AV_P : AV_NN;      /* interpolated, then clamped */
}

static int av_read(uint32_t b, int stage, const int *reg, const UboFsDraw *fd, int final,
                   uint32_t *used)
{
    uint32_t r = b & 0xF;
    int use_alpha = (b >> 4) & 1, v;
    const float *c = NULL;

    if (final && r == 1) c = fd->fc0;
    else if (final && r == 2) c = fd->fc1;
    else if (r == 1) c = fd->c0[(fd->ctl[0] & (1u << 12)) ? stage : 0];
    else if (r == 2) c = fd->c1[(fd->ctl[0] & (1u << 16)) ? stage : 0];
    if (c)
        v = av_const(use_alpha ? c[3] : c[2]);
    else if (r == 0 || (!final && r >= 14) || (final && use_alpha && r >= 14))
        v = AV_Z;                                        /* ZERO; V1R0 and EF have alpha 0 */
    else if (!use_alpha)
        v = AV_U;                                        /* a blue channel */
    else {
        v = reg[r];
        if (r == 4 || r == 5)
            *used |= 1u << (r - 4);
    }
    return v;
}

/* 1 when the final alpha is above 0 at every pixel; *used says whether the
 * vertex colours' alpha was read (bit 0 V0, bit 1 V1). */
static int av_final(const UboFsDraw *fd, const int st[4], int v0, int v1, uint32_t *used)
{
    int reg[16], i;
    uint32_t prog = fd->ctl[3], count = fd->ctl[0] & 0xFF, cw1 = fd->ctl[2];

    for (i = 0; i < 16; i++)
        reg[i] = AV_U;
    reg[0] = AV_Z;
    reg[3] = AV_NN;                                      /* FOG.a: the factor */
    reg[4] = v0;
    reg[5] = v1;
    for (i = 0; i < 4; i++) {
        uint32_t mode = (prog >> (5 * i)) & 0x1F;
        reg[8 + i] = mode == 0 || mode == 3 || mode == 5 ? AV_Z
                   : mode == 4 ? AV_NN
                   : (mode == 15 || mode == 16) && i == 0 ? AV_Z
                   : st[i];
    }
    reg[12] = reg[8];                                    /* R0.a starts as T0.a */
    reg[13] = AV_Z;
    if (count > 8)
        count = 8;
    for (i = 0; i < (int)count; i++) {
        uint32_t aicw = fd->aicw[i], aocw = fd->aocw[i], cocw = fd->cocw[i], op = (aocw >> 15) & 7;
        int aA = av_map(av_read(aicw >> 24, i, reg, fd, 0, used), (aicw >> 29) & 7);
        int aB = av_map(av_read(aicw >> 16, i, reg, fd, 0, used), (aicw >> 21) & 7);
        int aC = av_map(av_read(aicw >> 8, i, reg, fd, 0, used), (aicw >> 13) & 7);
        int aD = av_map(av_read(aicw, i, reg, fd, 0, used), (aicw >> 5) & 7);
        int aAB = av_mul(aA, aB), aCD = av_mul(aC, aD);
        int aS = (aocw & (1u << 14)) ? av_either(aAB, aCD) : av_add(aAB, aCD);
        uint32_t d;
        aAB = av_scale(aAB, op);
        aCD = av_scale(aCD, op);
        aS = av_scale(aS, op);
#define AV_DST(x) ((x) == 4 || (x) == 5 || ((x) >= 8 && (x) <= 13))
        if (AV_DST(d = (aocw >> 4) & 0xF)) reg[d] = aAB;
        if (AV_DST(d = aocw & 0xF)) reg[d] = aCD;
        if (AV_DST(d = (aocw >> 8) & 0xF)) reg[d] = aS;
        if ((cocw & (1u << 19)) && AV_DST(d = (cocw >> 4) & 0xF)) reg[d] = AV_U;
        if ((cocw & (1u << 18)) && AV_DST(d = cocw & 0xF)) reg[d] = AV_U;
#undef AV_DST
    }
    {
        uint32_t g = (cw1 >> 8) & 0xFF;
        int v = av_read(g, 0, reg, fd, 1, used);
        v = av_map(v, (g >> 5) & 1 ? 1u : 0u);           /* clamped, perhaps inverted */
        return av_pos(v);
    }
}

static int at_prove_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DK_AT_PROVE");
        on = !(e && *e == '0');
        fprintf(stderr, "  [DK] alpha tests proved unable to fail are dropped: %s"
                        " (RECOMP_DK_AT_PROVE)\n", on ? "on" : "off");
    }
    return on;
}

/* Bumped whenever the fragment uniforms change (the draw's fd differs from
 * the one pushed): the first pass of the proof depends on nothing else but
 * the stages' textures, so consecutive draws of a setup reuse it. */
static uint32_t s_fsd_gen = 1;

/* GREATER 0 that passes at every pixel of this draw (see above). */
static int alpha_proves(const Nv2aRawBatch *b, const uint32_t *r, const UboFsDraw *fd, int lit)
{
    static uint32_t m_gen;
    static int m_st[4], m_res;
    static uint32_t m_used;
    int st[4], i, v0 = AV_NN, v1 = AV_NN;
    uint32_t used = 0;

    for (i = 0; i < 4; i++)
        st[i] = s_stage_amin[i] == 2 ? AV_1 : s_stage_amin[i] == 1 ? AV_P : AV_NN;
    if (m_gen == s_fsd_gen && !memcmp(m_st, st, sizeof st)) {
        used = m_used;
        if (m_res)
            return 1;
    } else {
        int res = av_final(fd, st, AV_NN, AV_NN, &used);
        m_gen = s_fsd_gen;
        memcpy(m_st, st, sizeof st);
        m_res = res;
        m_used = used;
        if (res)
            return 1;
    }
    if (!used || b->xform == 2)
        return 0;                       /* a program's colours: not followed */
    if (lit) {
        uint32_t f = (r[0x298 / 4] >> 4) & 3;           /* SET_COLOR_MATERIAL diffuse */
        float ma;
        memcpy(&ma, &r[0x3B4 / 4], 4);                  /* SET_MATERIAL_ALPHA */
        if (used & 1)
            v0 = f == 0 ? av_const(ma) : f == 1 ? av_vertex(b, 3) : f == 2 ? av_vertex(b, 4)
               : AV_1;
        if (used & 2)
            v1 = av_vertex(b, 4);
    } else {
        if (used & 1) v0 = av_vertex(b, 3);
        if (used & 2) v1 = av_vertex(b, 4);
    }
    return av_final(fd, st, v0, v1, &used);
}

typedef struct {
    uint32_t k[DK_FS_KEY_WORDS];
    const DkShader *spec, *noat;
} FsMemo;
#define FS_MEMO 256
static FsMemo s_fs_memo[FS_MEMO];

static uint32_t fs_memo_slot(const uint32_t *k)
{
    uint32_t h = 0x9E3779B9u, i;
    for (i = 0; i < DK_FS_KEY_WORDS; i++)
        h = (h ^ k[i]) * 0x85EBCA6Bu + (h >> 15);
    return (h ^ (h >> 16)) & (FS_MEMO - 1);
}

static void dk_draw_raw_body(const Nv2aRawBatch *b)
{
    const uint32_t *r = b->regs;
    DkSurf *s;
    DkResHandle tex[4];
    float tscale[4][2];
    UboVsDraw vd;
    UboFsDraw fd;
    DkIdxFormat ifmt;
    uint32_t n, i;
    int has_depth, lit, texed;

    if (!ready())
        return;
    s_regs = r;
    if (b->prim < 1 || b->prim > 10 || !b->index_count) {
        s_draws_skipped++;
        return;
    }
    flush_if_low();
    s = surf_bind(&b->surface, b->zeta_va);
    if (!s)
        return;
    s_last = s;
    if (s_nodraw) {
        s_draws_skipped++;
        return;
    }

    /* Textures first: their uploads go ahead of the draw in the stream. */
    s_rt_barrier = 0;
    for (i = 0; i < 4; i++)
        tex[i] = bind_stage(b, (int)i, tscale[i]);
    if (s_up <= 0)
        return;
    if (s_rt_barrier || s_upload_dirty) {
        /* A surface drawn into this frame is sampled now, or images were
         * just written by the copy engine: finish that first. */
        dkCmdBufBarrier(s_cmd, s_upload_dirty ? DkBarrier_Full : DkBarrier_Fragments,
                        DkInvalidateFlags_Image | (s_desc_dirty ? DkInvalidateFlags_Descriptors : 0));
        if (s_upload_dirty) s_st.bar_full++; else s_st.bar_frag++;
        if (s_rt_barrier)
            for (i = 0; i < DK_MAX_SURF; i++)
                s_surf[i].written = 0;
        s_upload_dirty = 0;
        s_desc_dirty = 0;
    } else if (s_desc_dirty) {
        dkCmdBufBarrier(s_cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        s_desc_dirty = 0;
        s_st.bar_desc++;
    }
    dkCmdBufBindTextures(s_cmd, DkStage_Fragment, 0, tex, 4);

    /* The vertex stage's uniforms. */
    if (b->xform == 2)
        vp_push_program(b);
    vp_push_consts(b);
    memset(&vd, 0, sizeof vd);
    vd.surf[0] = 2.0f / (float)s->w;
    vd.surf[1] = 2.0f / (float)s->h;
    vd.surf[2] = 1.0f / (float)zmax_of(r);
    vd.surf[3] = -1.0f;                 /* NDC y up: rows top-first (device_up) */
    memcpy(vd.m, b->composite, sizeof vd.m);
    memcpy(vd.vpoff, b->vp_offset, sizeof vd.vpoff);
    vd.aa[0] = b->aa_sx > 0 ? b->aa_sx : 1.0f;
    vd.aa[1] = b->aa_sy > 0 ? b->aa_sy : 1.0f;
    if (pixel_align_on()) {
        /* The title's pixel centres onto the image's (nv2a_vs.glsl,
         * nv2a_clip): 0 at x1. */
        vd.aa[2] = 0.5f / s->scx - 0.5f;
        vd.aa[3] = 0.5f / s->scy - 0.5f;
    }
    vd.mode[0] = b->xform;
    vd.mode[1] = b->attr_present;
    vd.mode[2] = b->xform == 2 ? s_prog_lo : 0;
    vd.mode[3] = b->xform == 2 ? s_prog_hi : 0;
    lit = b->xform == 1 && r[0x314 / 4] && light_on();
    /* The fog unit (nv2a_vs.glsl, nv2a_fog). A draw without a program and
     * with fog from the eye distance or the fog plane needs eye space: the
     * model-view in VsLight. RECOMP_DK_FOG=0: no fog at all, to compare. */
    if (r[0x2A4 / 4] && fog_on()) {
        uint32_t gen = r[0x2A0 / 4];
        vd.fogm[0] = 1;
        vd.fogm[1] = r[0x29C / 4];
        vd.fogm[2] = gen;
        memcpy(vd.fogp, &r[0x9C0 / 4], 8);
        memcpy(vd.fogpl, &r[0x9D0 / 4], 16);
        if (b->xform == 1 && gen >= 1 && gen <= 3 && !lit)
            light_push(r);
        s_st.fogged++;
    }
    if (lit) {
        light_push(r);
        s_st.lit++;
    }
    texed = b->xform == 1 && texgen_on() ? tex_push(r) : 0;
    if (texed == 2 && !lit)
        light_push(r);                  /* the model-view, for the normal */
    vd.flags[0] = (mul_zero_on() ? 1u : 0u) | (s_prog_writes_c ? 2u : 0u)
                | (zclamp_vertex() ? 4u : 0u) | (lit ? 8u : 0u) | (texed ? 16u : 0u);
    memcpy(vd.attr, b->attr_const, sizeof vd.attr);
    if (memcmp(&vd, &s_sh_vsd, sizeof vd)) {
        s_sh_vsd = vd;
        dkCmdBufPushConstants(s_cmd, s_ubo + UBO_VSD, UBO_VSD_SZ, 0, sizeof vd, &vd);
        s_st.b_push += sizeof vd;
    }

    /* The fragment stage's. */
    memset(&fd, 0, sizeof fd);
    for (i = 0; i < 4; i++) {
        fd.tscale[i][0] = tscale[i][0];
        fd.tscale[i][1] = tscale[i][1];
    }
    for (i = 0; i < 8; i++) {
        argb_vec4(r[0xA60 / 4 + i], fd.c0[i]);
        argb_vec4(r[0xA80 / 4 + i], fd.c1[i]);
        fd.cicw[i] = r[0xAC0 / 4 + i];
        fd.cocw[i] = r[0x1E40 / 4 + i];
        fd.aicw[i] = r[0x260 / 4 + i];
        fd.aocw[i] = r[0xAA0 / 4 + i];
    }
    argb_vec4(r[0x1E20 / 4], fd.fc0);
    argb_vec4(r[0x1E24 / 4], fd.fc1);
    {
        uint32_t fc = r[0x2A8 / 4];     /* R in the low byte */
        fd.fog[0] = byte_f(fc, 0); fd.fog[1] = byte_f(fc, 8);
        fd.fog[2] = byte_f(fc, 16); fd.fog[3] = byte_f(fc, 24);
    }
    fd.ctl[0] = r[0x1E60 / 4];
    fd.ctl[1] = r[0x288 / 4];
    fd.ctl[2] = r[0x28C / 4];
    fd.ctl[3] = r[0x1E70 / 4];
    fd.ctl2[0] = r[0x1E78 / 4];
    fd.ctl2[1] = r[0x300 / 4] ? r[0x33C / 4] : 0x207;
    fd.ctl2[2] = r[0x340 / 4] & 0xFF;
    {
        static int hole = -1;
        if (hole < 0)
            hole = getenv("RECOMP_DK_HOLE_DIAG") != NULL;
        fd.ctl2[3] = hole ? 1u : 0u;
    }
    if (memcmp(&fd, &s_sh_fsd, sizeof fd)) {
        s_sh_fsd = fd;
        s_fsd_gen++;
        dkCmdBufPushConstants(s_cmd, s_ubo + UBO_FSD, UBO_FSD_SZ, 0, sizeof fd, &fd);
        s_st.b_push += sizeof fd;
    }

    /* The pack's shaders where it has them. The combiner key is
     * dk_shadergen.h's DkCombinerKey, word for word. */
    {
        uint32_t k[DK_FS_KEY_WORDS];
        const DkShader *vs, *fs;
        memcpy(k, fd.cicw, 32);
        memcpy(k + 8, fd.cocw, 32);
        memcpy(k + 16, fd.aicw, 32);
        memcpy(k + 24, fd.aocw, 32);
        k[32] = fd.ctl[0];              /* control */
        k[33] = fd.ctl[1];              /* final CW0 */
        k[34] = fd.ctl[2];              /* final CW1 */
        k[35] = fd.ctl[3];              /* shader stage program */
        k[36] = fd.ctl2[0];             /* other stage input */
        if (!s_fs_words_valid || memcmp(k, s_fs_words, sizeof k)) {
            /* The setups a frame switches between, remembered with both
             * their builds: the two keys' byte-wise hashes were the second
             * biggest part of the per-draw work. Only a setup the pack has
             * both builds of is kept, so one the console compiles later is
             * still looked up until it is there. */
            FsMemo *m = &s_fs_memo[fs_memo_slot(k)];
            memcpy(s_fs_words, k, sizeof k);
            s_fs_words_valid = 1;
            if (m->spec && !memcmp(m->k, k, sizeof k)) {
                s_fs_spec = m->spec;
                s_fs_spec_noat = m->noat;
            } else {
                uint64_t key = dk_key_hash(DK_KEY_FS, k, DK_FS_KEY_WORDS);
                s_fs_spec = pack_find(1, key);
                s_fs_spec_noat = pack_find(1, dk_key_hash(DK_KEY_FS_NOAT, k, DK_FS_KEY_WORDS));
                if ((!s_fs_spec || !s_fs_spec_noat) && asked_add(key))
                    dk_compile_request_fs(k);   /* both builds; the compiler skips one it has */
                if (!s_fs_spec)
                    key_fs(k, key);
                if (s_fs_spec && s_fs_spec_noat) {
                    memcpy(m->k, k, sizeof k);
                    m->spec = s_fs_spec;
                    m->noat = s_fs_spec_noat;
                }
            }
        }
        vs = b->xform != 2 ? (texed ? &s_vs_fixed_tg : &s_vs_fixed)
                           : s_vs_spec ? s_vs_spec : &s_vs;
        /* Alpha test off, or one that cannot fail -- ALWAYS, >= 0, <= 255
         * (the shader clamps alpha to 0..255 first): the build without it,
         * where the pack has one, so the GPU keeps its early depth test. A
         * pack from before it has none, and the one with the test then draws
         * it as before. RECOMP_DK_EZ_TEST=1 (a measurement, not a fix: cut-out
         * edges are lost) uses it for every draw. */
        {
            static int ez_test = -1;
            uint32_t af = fd.ctl2[1], aref = fd.ctl2[2];
            int triv = (af == 0x206 && aref == 0) || (af == 0x203 && aref == 0xFF);
            int zw = b->zeta_va != 0 && s_bound_depth != NULL && r[0x30C / 4] && r[0x35C / 4];
            if (ez_test < 0) {
                const char *e = getenv("RECOMP_DK_EZ_TEST");
                ez_test = e && *e == '1';
                if (ez_test)
                    fprintf(stderr, "  [DK] RECOMP_DK_EZ_TEST=1: every draw without the alpha test"
                                    " (a measurement: cut-out edges are drawn solid)\n");
            }
            if (!triv && af == 0x204 && aref == 0 && s_fs_spec_noat && at_prove_on()
                && alpha_proves(b, r, &fd, lit))
                triv = 2;
            if (af == 0x207) s_st.at_off++;
            else if (triv == 2) s_st.at_prove++;
            else if (triv) s_st.at_triv++;
            else {
                uint32_t key = af << 8 | aref, j;
                if (zw) s_st.at_zw++; else s_st.at_nozw++;
                for (j = 0; j < 8 && s_st.af_n[j] && s_st.af_key[j] != key; j++)
                    ;
                if (j < 8) { s_st.af_key[j] = key; s_st.af_n[j]++; }
            }
            fs = (af == 0x207 || triv || ez_test) && s_fs_spec_noat ? s_fs_spec_noat
               : s_fs_spec ? s_fs_spec : &s_fs;
        }
        if (fs == s_fs_spec_noat) s_st.spec_fs_noat++;
        if (s_cap)
            cap_draw(b, r, s, &fd, k, vs, fs);
        memcpy(s_ds_k, k, sizeof s_ds_k);  /* for RECOMP_DK_DRAWSYNC's report */
        s_ds_vs = vs;
        s_ds_fs = fs;
        if (vs != s_cur_vs || fs != s_cur_fs) {
            DkShader const *sh[2] = { vs, fs };
            dkCmdBufBindShaders(s_cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, sh, 2);
            s_cur_vs = vs;
            s_cur_fs = fs;
        }
        if (b->xform == 2) {
            if (vs != &s_vs) s_st.spec_vs++;
            else s_st.uber_vs_vp++;
        }
        if (fs != &s_fs) s_st.spec_fs++;
    }

    has_depth = b->zeta_va != 0 && s_bound_depth != NULL;
    apply_state(r, has_depth, s);
    n = upload_draw(b, &ifmt);
    if (!n || s_up <= 0)
        return;
    dkCmdBufDrawIndexed(s_cmd, (DkPrimitive)(b->prim - 1), n, 1, 0, 0, 0);
    s->written = 1;
    s_draws++;
    s_st.draws++;
    s_st.verts += b->vertex_count;
    if (drawsync_on()) {
        uint32_t frame = s_frames, nd = s_draws;
        sync_gpu_why(SYNC_CMD);
        if (s_up <= 0) {
            fprintf(stderr, "  [DK] RECOMP_DK_DRAWSYNC: the GPU stopped on draw %u (frame %u):"
                            " these commands were that draw and what it set up\n", nd, frame);
            s_cap_draws = 0;
            cap_draw(b, r, s, &fd, s_ds_k, s_ds_vs, s_ds_fs);
            log_sync();
        }
    }
}

static void dk_draw_raw(const Nv2aRawBatch *b)
{
    uint64_t t0 = armGetSystemTick();
    dk_draw_raw_body(b);
    s_st.t_draw += armGetSystemTick() - t0;
}

/* A frame over 50 ms (a hitch, the screen held; RECOMP_DK_HITCH_MS sets
 * the time): what the renderer did in it -- textures made and uploaded,
 * draws, its own time -- so a stall of its own shows apart from the game's
 * (then the renderer's time is small; RECOMP_NX_PROFILE=1's report has
 * "during the hitches" tables of where every thread was). As the GL
 * renderer's [hitch] line; at most 30 lines per 10 s (1200 under 50 ms).
 *
 * The second half is the game's file I/O in the same frame, from every
 * thread (xbox_io_totals, the kernel's file bridges): how long the host spent
 * in reads, writes, opens and the rest -- directory scans, attribute queries,
 * deletes. A loading screen that holds still for seconds is either the SD
 * card or the game's own work on what it read, and this tells them apart. */
extern void xbox_io_totals(uint64_t out[10]);

static void hitch_check(void)
{
    static uint64_t last, window, r_last, bytes_last, io_last[10];
    static uint32_t lines, tex_last, draws_last, ms_over = 0, cap;
    uint64_t now = armGetSystemTick(), freq = armGetSystemTickFreq();
    if (!ms_over) {
        /* RECOMP_DK_HITCH_MS=<ms>: which frames count (default 50). Lower,
         * e.g. 22, every frame under ~45 fps gets a line, and the profile
         * report's "during the hitches" tables are then what every thread
         * did in the dips, wherever they happened. */
        const char *e = getenv("RECOMP_DK_HITCH_MS");
        ms_over = e && atoi(e) >= 17 ? (uint32_t)atoi(e) : 50u;
        cap = ms_over >= 50 ? 30u : 1200u;
        if (ms_over != 50)
            fprintf(stderr, "  [DK] [hitch] lines for frames over %u ms (RECOMP_DK_HITCH_MS)\n", ms_over);
    }
    uint64_t rt = s_st.t_draw + s_st.t_clear + s_st.t_flip;
    uint64_t io[10], d[10];
    int i;
    xbox_io_totals(io);
    for (i = 0; i < 10; i++)
        d[i] = io[i] - io_last[i];
    if (rt < r_last)
        r_last = 0;                     /* the [perf] report restarted the counters */
    if (last && now - last > freq * ms_over / 1000) {
        if (now - window > freq * 10) {
            window = now;
            lines = 0;
        }
        if (lines++ < cap)
            fprintf(stderr, "[hitch] frame %u: %llu ms -- %u textures made, %.1f MB uploaded,"
                            " %u draws, renderer %llu ms%s; file I/O %llu ms: %llu reads"
                            " %.1f MB %llu ms, %llu writes %.1f MB %llu ms, %llu opens"
                            " %llu ms, %llu other %llu ms\n", s_frames,
                    (unsigned long long)((now - last) * 1000 / freq), s_tex_made - tex_last,
                    (double)(s_tex_bytes_all - bytes_last) / 1048576.0, s_draws - draws_last,
                    (unsigned long long)((rt - r_last) * 1000 / freq),
                    s_cap ? " (a captured frame)" : "",
                    (unsigned long long)((d[2] + d[5] + d[7] + d[9]) / 1000),
                    (unsigned long long)d[0], (double)d[1] / 1048576.0,
                    (unsigned long long)(d[2] / 1000),
                    (unsigned long long)d[3], (double)d[4] / 1048576.0,
                    (unsigned long long)(d[5] / 1000),
                    (unsigned long long)d[6], (unsigned long long)(d[7] / 1000),
                    (unsigned long long)d[8], (unsigned long long)(d[9] / 1000));
    }
    last = now;
    r_last = rt;
    tex_last = s_tex_made;
    bytes_last = s_tex_bytes_all;
    draws_last = s_draws;
    memcpy(io_last, io, sizeof(io_last));
}

static void dk_flip(void)
{
    static uint32_t last_report;
    uint64_t t0 = armGetSystemTick();
    if (!ready())
        return;
    present(0);
    hitch_check();
    if (s_keylen && (s_frames % 120) == 0)
        keys_flush();
    s_st.t_flip += armGetSystemTick() - t0;
    s_rs_valid = 0;                     /* present set its own viewport and scissor */
    if (s_trace && s_frames - last_report >= 600) {
        last_report = s_frames;
        fprintf(stderr, "  [DK] frame %u: %u draws, %u skipped, %u clears, %u textures made"
                        " (%u uploads), %u program versions, texture memory %u/%u MB\n",
                s_frames, s_draws, s_draws_skipped, s_clears, s_tex_made, s_tex_uploads,
                s_prog_versions, s_heap_used >> 20, s_heap_size >> 20);
    }
}

/* Guest bytes [va, va + bytes) overlap a surface drawn on the GPU, whose
 * guest memory is never written back: the executor's 2D copies leave it be.
 * Its extent counted at 4 bytes a pixel, so a 16-bit surface's is generous. */
static int dk_surface_at(uint32_t va, uint32_t bytes)
{
    int i;
    for (i = 0; i < DK_MAX_SURF; i++) {
        const DkSurf *s = &s_surf[i];
        uint64_t lo = s->va, hi = lo + (uint64_t)s->w * s->h * 4u;
        if (s->mem && va < hi && (uint64_t)va + bytes > lo)
            return 1;
    }
    return 0;
}

/* The executor wrote guest bytes [va, va + bytes) itself (a 2D copy -- in
 * JSRF's garage, the graffiti design just picked going into the preview's
 * texture): every texture made from those bytes is made again at its next
 * use, whatever its sampled hash says. */
static int dk_written(uint32_t va, uint32_t bytes)
{
    int i, n = 0;
    for (i = 0; i < TEX_MAX; i++) {
        DkTex *t = &s_tex[i];
        if (t->live && va < (uint64_t)t->va + t->bytes && (uint64_t)va + bytes > t->va) {
            t->stale = 1;
            n++;
        }
    }
    return n;
}

static const Nv2aBackend s_backend_dk = {
    dk_clear,
    NULL,              /* draw: everything arrives through draw_raw */
    dk_flip,
    dk_draw_raw,
    /* Attributes as the title stored them, uploaded as they are; the
     * executor converts only what the GPU cannot read (CMP normals). And
     * the indices as given, rebased while they are copied into the ring.
     * Behind the queue, the converted ones come packed as the ring wants. */
    NV2A_BACKEND_RAW_DIRECT | NV2A_BACKEND_RAW_ABS_INDICES | NV2A_BACKEND_RAW_PACKED,
    dk_surface_at,
    dk_written,
};

/* By default on a thread of its own, behind the executor
 * (nv2a_backend_queue): the executor decodes, this records, on two cores.
 * RECOMP_DK_THREAD=0: on the executor's thread, as before. */
void nv2a_dk_install(void)
{
    const char *e = getenv("RECOMP_DK_THREAD");
    const Nv2aBackend *be = &s_backend_dk;
    if (!(e && e[0] == '0' && e[1] == '\0'))
        be = nv2a_backend_queue(&s_backend_dk);
    nv2a_backend_register(be);
    fprintf(stderr, "  [DK] NV2A deko3d renderer registered (phase 2: vertex-program and"
                    " combiner ubershaders, built with the NRO), %s\n",
            be != &s_backend_dk ? "on a thread of its own (RECOMP_DK_THREAD=0: on the executor's)"
                                : "on the executor's thread");
}

/* Three lines for the platform's [perf] report, then the counters restart. */
void nv2a_dk_perf(char *buf, size_t n, double secs)
{
    extern void nv2a_pb_exec_words(uint64_t out[6]);
    extern uint64_t nv2a_exec_busy_us(void);
    extern uint32_t nv2a_kick_count(void);
    extern uint64_t nv2a_exec_sleep_us(void);
    extern void nv2a_pb_exec_traps(uint64_t out[2]);
    extern uint32_t nv2a_exec_poll_hits(void), nv2a_exec_sleeps(void);
    extern int nv2a_pb_exec_rest_top(uint32_t method[], uint32_t words[], int max);
    static uint32_t last_frames, last_kicks, last_hits, last_sleeps;
    uint32_t hits = nv2a_exec_poll_hits(), sleeps = nv2a_exec_sleeps();
    static uint64_t last_w[6], last_busy, last_sleep, last_trap[2];
    uint32_t kicks = nv2a_kick_count();
    uint64_t sleep_us = nv2a_exec_sleep_us(), trap[2];
    uint64_t w[6], dw[6], busy = nv2a_exec_busy_us();
    uint32_t frames = s_frames - last_frames, i, syncs = 0;
    double f = frames ? (double)frames : 1.0;
    double tick_ms = 1000.0 / 19200000.0, words, rest;
    int len;
    last_frames = s_frames;
    for (i = 0; i < SYNC_N; i++)
        syncs += s_st.syncs[i];
    nv2a_pb_exec_words(w);
    nv2a_pb_exec_traps(trap);
    for (i = 0; i < 6; i++) {
        dw[i] = w[i] - last_w[i];
        last_w[i] = w[i];
    }
    words = dw[0] ? (double)dw[0] : 1.0;
    len = snprintf(buf, n,
             "[perf] deko3d: GPU %.1f ms/frame at x%.2f%s (picture %ux%u, screen %ux%u);"
             " renderer CPU %.1f ms/frame (draws %.1f [data copies %.1f, texture checks %.2f:"
             " %.0f/frame, %.0f put off], clears %.1f,"
             " present %.1f, of which waiting for the GPU %.1f + syncs %.1f); %u draws (%.0f/frame,"
             " %.0f vertices/draw), pack shaders on %u%% of vertex-program draws and %u%% of draws'"
             " combiners (%u%% of draws without the alpha test: early depth)\n"
             "[perf] deko3d: %u syncs (data %u surface %u depth %u evict %u cmd %u defer %u),"
             " barriers %u full %u fragment %u descriptor; per frame: vertices %.0f KB, indices"
             " %.0f KB, textures %.0f KB, uniforms %.0f KB (data ring %s, cleaning %.2f ms);"
             " made %u surfaces %u depth %u textures;"
             " %u new keys recorded (%u pending); %u shaders built on the console taken into use;"
             " %u lit draws (%u light changes), %u fogged, %u texgen + %u texture-matrix draws",
             s_st.gpu_frames ? s_st.gpu_ns / 1e6 / s_st.gpu_frames : 0.0,
             (double)s_scale, s_scale_mode == DYN_AUTO ? " dynamic" : "",
             s_last ? s_last->pw : 0u, s_last ? s_last->ph : 0u, s_fb_w, s_fb_h,
             (double)(s_st.t_draw + s_st.t_clear + s_st.t_flip) * tick_ms / f,
             (double)s_st.t_draw * tick_ms / f, (double)s_st.t_copy * tick_ms / f,
             (double)s_st.t_check * tick_ms / f, s_st.checks / f, s_st.checks_skipped / f,
             (double)s_st.t_clear * tick_ms / f,
             (double)s_st.t_flip * tick_ms / f, (double)s_st.t_fence * tick_ms / f,
             (double)s_st.t_sync * tick_ms / f,
             s_st.draws, s_st.draws / f, s_st.draws ? (double)s_st.verts / s_st.draws : 0.0,
             (s_st.spec_vs + s_st.uber_vs_vp) ? 100u * s_st.spec_vs / (s_st.spec_vs + s_st.uber_vs_vp) : 0u,
             s_st.draws ? 100u * s_st.spec_fs / s_st.draws : 0u,
             s_st.draws ? 100u * s_st.spec_fs_noat / s_st.draws : 0u,
             syncs, s_st.syncs[SYNC_DATA], s_st.syncs[SYNC_SURF], s_st.syncs[SYNC_DEPTH],
             s_st.syncs[SYNC_EVICT], s_st.syncs[SYNC_CMD], s_st.syncs[SYNC_DEFER],
             s_st.bar_full, s_st.bar_frag, s_st.bar_desc,
             s_st.b_vtx / 1024.0 / f, s_st.b_idx / 1024.0 / f, s_st.b_tex / 1024.0 / f,
             s_st.b_push / 1024.0 / f, s_data_cached > 0 ? "cached" : "uncached",
             (double)s_data_clean_ticks * tick_ms / f, s_st.surfs, s_st.depths, s_st.texs,
             s_keys_new, s_keylen, s_st.built, s_st.lit, s_st.lit_push, s_st.fogged, s_st.texgen, s_st.texmat);
    /* What the executor thread reads per frame, and its time in the walk
     * with everything under it (the renderer's share is the line above):
     * the rest is reading the stream and gathering each draw. */
    rest = (double)dw[0] - (double)dw[1] - (double)dw[2] - (double)dw[3]
         - (double)dw[4] - (double)dw[5];       /* read while it counts: >= 0 */
    if (len > 0 && (size_t)len < n)
        snprintf(buf + len, n - (size_t)len,
                 "\n[perf] pushbuffer: %.0f words/frame (transform constants %.0f%%, programs %.0f%%,"
                 " indices %.0f%%, inline vertices %.0f%%, shadow-only state %.0f%%, the rest %.0f%%);"
                 " executor per frame: %.1f ms in the walk (%.1f ms of it stalled on %.1f NOP traps),"
                 " %.1f ms asleep (%.1f sleeps; new commands seen %.1f times while watching"
                 " before a sleep); woken %u times by a guest wait",
                 dw[0] / f, 100.0 * dw[1] / words, 100.0 * dw[2] / words, 100.0 * dw[3] / words,
                 100.0 * dw[4] / words, 100.0 * dw[5] / words,
                 rest > 0.0 ? 100.0 * rest / words : 0.0,
                 (double)(busy - last_busy) / 1000.0 / f,
                 (double)(trap[0] - last_trap[0]) / 1000.0 / f, (double)(trap[1] - last_trap[1]) / f,
                 (double)(sleep_us - last_sleep) / 1000.0 / f, (double)(sleeps - last_sleeps) / f,
                 (double)(hits - last_hits) / f, kicks - last_kicks);
    {
        /* Which methods "the rest" is: the slow path's words, by method. */
        uint32_t m[6], c[6];
        int k, nt = nv2a_pb_exec_rest_top(m, c, 6);
        size_t at = strlen(buf);
        if (nt > 0 && at + 40 < n)
            at += (size_t)snprintf(buf + at, n - at, "; the rest by method:");
        for (k = 0; k < nt && at + 24 < n; k++)
            at += (size_t)snprintf(buf + at, n - at, " %04X %.0f%%", m[k], 100.0 * c[k] / words);
    }
    {
        /* Draws by alpha test (s_st.at_*), for the early depth test. */
        uint32_t tot = s_st.at_off + s_st.at_triv + s_st.at_nozw + s_st.at_zw + s_st.at_prove, j;
        size_t at = strlen(buf);
        if (tot && at + 240 < n) {
            at += (size_t)snprintf(buf + at, n - at,
                     "\n[perf] alpha test: off %u%%, on but cannot fail %u%%, proved above 0 (dropped)"
                     " %u%%, on without depth writes %u%%, on with depth writes %u%%; can fail,"
                     " by func/ref:",
                     100u * s_st.at_off / tot, 100u * s_st.at_triv / tot,
                     100u * s_st.at_prove / tot,
                     100u * s_st.at_nozw / tot, 100u * s_st.at_zw / tot);
            for (j = 0; j < 8 && s_st.af_n[j] && at + 24 < n; j++)
                at += (size_t)snprintf(buf + at, n - at, " %03X/%u %u%%", s_st.af_key[j] >> 8,
                                       s_st.af_key[j] & 0xFF, 100u * s_st.af_n[j] / tot);
        }
    }
    {
        /* RECOMP_DK_THREAD: is the renderer thread starved, or the executor
         * held back by it? */
        size_t at = strlen(buf);
        if (nv2a_backend_queue_on() && at + 2 < n) {
            buf[at++] = '\n';
            buf[at] = '\0';
            if (!nv2a_backend_queue_report(buf + at, (unsigned long)(n - at), f))
                buf[at - 1] = '\0';
        }
    }
    last_hits = hits;
    last_sleeps = sleeps;
    last_busy = busy;
    last_kicks = kicks;
    last_sleep = sleep_us;
    last_trap[0] = trap[0];
    last_trap[1] = trap[1];
    (void)secs;
    memset(&s_st, 0, sizeof s_st);
    s_data_clean_ticks = 0;
    s_keys_new = 0;
}

uint32_t nv2a_dk_frame_count(void) { return s_frames; }
uint64_t nv2a_dk_texture_bytes(void) { return s_heap_used; }
uint32_t nv2a_dk_program_count(void) { return s_prog_versions; }
uint32_t nv2a_dk_draws_skipped(void) { return s_draws_skipped; }
uint32_t nv2a_dk_draw_count(void) { return s_draws; }
