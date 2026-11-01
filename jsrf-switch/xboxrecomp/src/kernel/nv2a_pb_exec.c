/**
 * Execute the title's pushbuffer.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT. This walks
 * the same command stream nv2a_pb_scan.c follows and carries it out: it keeps
 * the method shadow (the register file the hardware draws from), gathers each
 * batch's vertices and indices, and hands clears, batches and flips to the
 * render back end (nv2a_backend.h) -- the deko3d renderer, which runs the
 * vertex program, the combiners and the rest on the Switch's GPU. Software
 * methods, NOP traps and 2D blits are handled here.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE */
#include "xbox_memory_layout.h"   /* xbox_Nv2aFrameCounterFlip */
/* The swizzle decoder the D3D8 layer already uses -- one implementation of
 * Morton order, not a second one that can disagree with it. */
#include "d3d8_swizzle.h"
#include "nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Debug switches, read once. getenv() walks the whole environment, and these
 * were tested per method and per batch -- millions of times a second during a
 * level load, enough to stall the thread that also acknowledges the GPU. */
static int pb_verbose(void)
{
    static int set = -1;

    if (set == -1)
        set = getenv("RECOMP_PB_EXEC_VERBOSE") != NULL;
    return set;
}

void nv2a_pb_exec_report(void);

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

/* Where a DMA-object offset actually lives.
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset inside the colour DMA object,
 * and for a framebuffer that object covers physical memory -- so the offset
 * is a physical address, not a guest VA. Those are the same number in this
 * runtime, which is why treating it as a VA works until it does not: on
 * hardware the image is mapped at VA 0x00010000 from arbitrary physical
 * pages, so a framebuffer at physical 0x84000 does not overlap it. Here it
 * would.
 *
 * The title tells us which it is by where it allocated. Half-Life 2's
 * framebuffer comes from MmAllocateContiguousMemory, which this runtime
 * serves from the window at XBOX_CONTIG_BASE, so physical P is visible at
 * XBOX_CONTIG_BASE + P -- clear of the image, and the same bytes the title's
 * own writes and the framebuffer window reach.
 *
 * So: use the offset as a VA when that is credible, and fall back to the
 * physical mirror exactly when it is not. Titles whose surfaces already sit
 * in ordinary RAM (Wreckless renders to the tiled alias of physical
 * 0x01954000) keep the first path and are unaffected.
 */
static uint32_t dma_resolve(uint32_t offset)
{
    extern int xbox_ContiguousIsPhysical(uint32_t phys);
    extern uint32_t xbox_BusPassthroughVa(uint32_t bus);
    uint32_t tagged_va = xbox_BusPassthroughVa(offset);

    /* In order:
     *   1. a pool or heap VA that MmGetPhysicalAddress tagged: that VA;
     *   2. memory the contiguous arena handed out (its bump allocator from
     *      XBOX_CONTIG_BASE makes that a fact, not a guess): the window.
     *      Before the image test, which only catches an offset that would
     *      land on the title's code: Half-Life 2's colour surface, physical
     *      0x00A6C000, missed the image by 700 KB, read as a VA, and the
     *      clear went 1.2 MB through the guest heap while the real surface
     *      at 0x80A6C000 stayed black;
     *   3. below the XBE base (page zero, the fake TIB): physical, from
     *      before the arena moved above the image;
     *   4. clear of the image: a VA (Wreckless's tiled alias at 0x01954000);
     *   5. on the image but inside the window's size: the window;
     *   6. otherwise as given -- nothing better to offer. */
    if (tagged_va)
        return tagged_va;
    if (xbox_ContiguousIsPhysical(offset))
        return XBOX_CONTIG_BASE + offset;
    if (offset != 0 && offset < 0x10000u)
        return XBOX_CONTIG_BASE + offset;
    if (!surface_hits_image(offset, 1))
        return offset;
    return (uint64_t)offset < XBOX_CONTIG_SIZE ? XBOX_CONTIG_BASE + offset : offset;
}

/* NV097 methods this executor acts on. */
/* Blending. The pair this title programs, read from its own pushbuffer
 * rather than guessed: BLEND_ENABLE written 1168 times and left on,
 * SFACTOR 0x0302 (SRC_ALPHA) and DFACTOR 0x0303 (ONE_MINUS_SRC_ALPHA).
 * ALPHA_TEST_ENABLE is written 390 times and left at zero, so this is
 * blending and not an alpha test. */
#define NV097_SET_BLEND_ENABLE            0x0304
#define NV097_SET_BLEND_FUNC_SFACTOR      0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR      0x0348
#define NV_BLEND_SRC_ALPHA                0x0302
#define NV_BLEND_ONE_MINUS_SRC_ALPHA      0x0303

#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_SET_TEXTURE_OFFSET          0x1B00   /* +i*0x40 */
#define NV097_SET_TEXTURE_FORMAT          0x1B04
#define NV097_SET_TEXTURE_ADDRESS         0x1B08
#define NV097_SET_TEXTURE_CONTROL1        0x1B10
#define NV097_SET_TEXTURE_IMAGE_RECT      0x1B1C
/* The buffer flip. A title double-buffers by telling the GPU which buffer
 * the CRTC reads and which it draws into, advancing the write index and
 * then stalling until the flip has happened. Ignoring these means the
 * stall never clears: Half-Life 2's loader submits its initialisation,
 * asks for a flip, and waits for it in a loop that makes no kernel calls
 * and burns no dispatch, which reads as a hang with no cause.
 *
 * ponytail: the flip completes the moment it is asked for, because there is
 * no scanout to be in the middle of. That makes every frame land instantly
 * and a title that paces itself on the flip runs as fast as it can draw.
 * Pacing wants the vblank clock in the kernel, not a sleep in here. */
#define NV097_SET_FLIP_READ               0x0120
#define NV097_SET_FLIP_WRITE              0x0124
#define NV097_SET_FLIP_MODULO             0x0128
#define NV097_FLIP_INCREMENT_WRITE        0x012C
#define NV097_FLIP_STALL                  0x0130
#define NV097_ARRAY_ELEMENT16             0x1800
/* Draw a run of vertices straight out of the arrays, with no index list:
 * bits 0..23 are the first vertex, bits 24..31 the count minus one. It may
 * appear several times inside one BEGIN_END to draw a longer run. */
#define NV097_DRAW_ARRAYS                 0x1810
#define NV097_INLINE_ARRAY                0x1818
/* Immediate-mode vertices. SET_VERTEX3F/4F carry the position, and writing
 * its last component completes a vertex using whatever the SET_VERTEX_DATA*
 * registers currently hold for the other attributes. This is how Half-Life
 * 2's Xbox loader and the game's own 2D drawing submit every quad -- neither
 * uses INLINE_ARRAY -- so without these the executor saw SET_BEGIN_END pairs
 * with nothing attached and reported `draws 0` while a million and a half
 * textured quads a minute went past it. */
#define NV097_SET_VERTEX3F                0x1500   /* +0..0x08, 3 floats */
#define NV097_SET_VERTEX4F                0x1518   /* +0..0x0C, 4 floats */
#define NV097_SET_VERTEX_DATA2F_M         0x1880   /* + attr*8,  2 floats */
#define NV097_SET_VERTEX_DATA4F_M         0x1A00   /* + attr*16, 4 floats */
#define NV097_SET_VERTEX_DATA4UB          0x1940   /* + attr*4,  D3DCOLOR */
#define NV097_SET_VERTEX_DATA2S           0x1900   /* + attr*4,  2 shorts */
#define NV097_SET_VERTEX_DATA4S_M         0x1980   /* + attr*8,  4 shorts */

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  65536
#define NV_MAX_INLINE   65536           /* dwords of INLINE_ARRAY per batch */

/* Texture stage 0, decoded from what the title programmed.
 *
 * Only stage 0: it is the only one the dashboard configures, and a stage
 * nothing writes to is a stage nothing can be sampled from. The rest arrive
 * as unhandled methods and are counted as such, which is how the next title
 * that needs them will say so. */
typedef struct {
    uint32_t offset;                    /* guest address of texel (0,0)  */
    uint32_t width, height;             /* from IMAGE_RECT               */
    uint32_t pitch;                     /* bytes per row, from CONTROL1  */
    uint32_t color;                     /* NV097 colour-format code      */
    uint32_t addr_u, addr_v;            /* wrap mode per axis            */
    int      valid;
} Texture;

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint32_t   idx[NV_MAX_INDICES];   /* DRAW_ARRAYS starts are 24-bit */
    uint32_t   idx_count;
    /* The lowest and highest of idx[0..idx_count), kept by the whole-run
     * index paths as they fill it (idx_mm set), so raw_batch need not look
     * for them; any other writer clears idx_mm and raw_batch looks. */
    uint32_t   idx_lo, idx_hi;
    int        idx_mm;
    /* INLINE_ARRAY payload: vertices written straight into the pushbuffer
     * instead of into a buffer the title points at. Same vertex format, a
     * different place to read them from. */
    uint32_t   inline_buf[NV_MAX_INLINE];
    uint32_t   inline_count;
    /* Each attribute's current value: what SET_VERTEX_DATA*, SET_VERTEX3F/4F,
     * SET_DIFFUSE_COLOR* and the rest last wrote, and what it reads without
     * an array; attr_const_gen 0 = not initialised yet. */
    float      attr_const[16][4];
    uint32_t   attr_const_gen;
    /* Immediate-mode vertices completed in this batch, and the attributes
     * that vary across them (imm_attr_touch). */
    uint32_t   imm_count;
    uint16_t   imm_populated;
    int        inline_active;
    /* What inline offsets index while inline_active: inline_buf for an
     * INLINE_ARRAY batch, s_imm for an immediate one. */
    const uint32_t *inline_src;
    uint32_t   draws, verts;
    uint32_t color_offset, color_base, pitch, format;
    /* The surface the last batch drew into: by the flip, a double-buffered
     * title has already pointed color_offset at the next one. */
    uint32_t drawn_offset;
    uint32_t drawn_pitch;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t flip_read, flip_write, flip_modulo, flips;
    uint32_t tris_drawn;                /* sent to the back end */
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    Texture  tex;
    /* Fixed-function transform: the composite matrix (world*view*projection
     * *viewport, as D3D uploads it), the viewport offset added after the
     * perspective divide, and which transform unit is active. */
    uint32_t clip_raw_h, clip_raw_v;    /* SET_SURFACE_CLIP_* as sent */
    Nv2aRenderState rs;                 /* handed to a back end with each batch */
    uint32_t zstencil_clear;            /* SET_ZSTENCIL_CLEAR_VALUE */
    float    aa_sx, aa_sy;              /* anti-aliasing scale of the surface */
    float    composite[16];
    float    vp_offset[4];
    uint32_t xform_mode;                /* SET_TRANSFORM_EXECUTION_MODE */
    int      composite_set;
} s_gpu;

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count, last_param; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

/* Every texture-stage register, as the title last set it.
 * Texturing is not implemented yet; knowing which formats and sizes a title
 * actually programs is what decides which ones are worth implementing. */
#define NV_TEX_FIRST 0x1B00
#define NV_TEX_LAST  0x1BFC
static uint32_t s_tex_reg[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];
static uint8_t  s_tex_set[(NV_TEX_LAST - NV_TEX_FIRST) / 4 + 1];

/* Formats whose dimensions come from the format word and whose coordinates
 * arrive normalised, rather than from a pitch and SET_TEXTURE_IMAGE_RECT with
 * coordinates in texels. Swizzled and block-compressed are both in this group,
 * and every place that used to test only for swizzled needs the pair. */
static int tex_size_from_format(uint32_t fmt)
{
    return d3d8_format_is_swizzled(fmt) || d3d8_format_dxt_block_bytes(fmt);
}

static void record_tex_reg(uint32_t method, uint32_t param)
{
    s_tex_reg[(method - NV_TEX_FIRST) / 4] = param;
    s_tex_set[(method - NV_TEX_FIRST) / 4] = 1;
    /* A pitch is a linear texture's property. A swizzled one has no rows and
     * so no pitch, and requiring one here refused every swizzled texture --
     * which is nearly all of them, since swizzled is the Xbox default. That
     * left the title's own textures unsampled and every textured quad drawn in
     * flat vertex colour. */
    s_gpu.tex.valid = s_gpu.tex.offset && s_gpu.tex.width && s_gpu.tex.height
                   && (tex_size_from_format(s_gpu.tex.color)
                       || s_gpu.tex.pitch);
}

/* The last value written to every Kelvin method, as the GPU's register file
 * would hold it. State the executor does not model explicitly (lights,
 * combiners, material colours) is read from here. */
static uint32_t s_reg[0x2000 / 4];

/* For a back end behind a queue (nv2a_pb_reg_dirty): a flag per 64-byte
 * block of s_reg (16 words), set when a word in it changes, cleared by the
 * back end once it has copied the block. All set to begin with. */
#define SHADOW_BLOCKS     (0x2000 / 64)
#define SHADOW_BLOCK(word) ((word) / 16)
static uint8_t s_reg_dirty[SHADOW_BLOCKS] = { [0 ... SHADOW_BLOCKS - 1] = 1 };

uint8_t *nv2a_pb_reg_dirty(void)
{
    return s_reg_dirty;
}
const uint32_t *nv2a_pb_reg_shadow(void) { return s_reg; }

/* A word the title reads the GPU's progress from (the semaphore,
 * PATT_COLOR0). With the back end on a thread of its own, written once that
 * thread has made every call before it (nv2a_backend.h). */
static void progress_store(volatile uint32_t *word, uint32_t value)
{
    if (nv2a_backend_queue_on())
        nv2a_backend_queue_store(word, value);
    else
        *word = value;
}

static uint32_t s_sem_va;
void nv2a_pb_set_semaphore_target(uint32_t guest_va) { s_sem_va = guest_va; }

/* Slot + 1 of each method in s_unhandled: this runs millions of times a
 * second, so no search. The report sorts s_unhandled and rebuilds it. */
static uint16_t s_unhandled_slot[0x2000 / 4];

/* The ranking is only ever printed by nv2a_pb_exec_report, which a quiet log
 * (RECOMP_QUIET=1, the Switch default) skips, so a quiet run does not keep
 * it. Most state methods end up here: it was a few stores on every one. */
int xbox_log_quiet(void);           /* kernel_bridge.c */
static int unhandled_on(void)
{
    static int on = -1;
    if (on < 0)
        on = !xbox_log_quiet() || pb_verbose();
    return on;
}

static void note_unhandled(uint32_t method, uint32_t param)
{
    uint16_t *slot = &s_unhandled_slot[(method & 0x1FFC) / 4];

    if (!unhandled_on())
        return;
    s_gpu.unhandled_total++;
    if (*slot) {
        s_unhandled[*slot - 1].count++;
        s_unhandled[*slot - 1].last_param = param;
        return;
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled[s_unhandled_count].last_param = param;
        s_unhandled_count++;
        *slot = (uint16_t)s_unhandled_count;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array. */
/* A normalised short: -32767..32767 to -1..1 (and -32768 to -1 as well). */
static float snorm16(int16_t value)
{
    float f = (float)value / 32767.0f;
    return f < -1.0f ? -1.0f : f;
}

/* Bits [first, first + bits) of `packed` as a two's-complement number. */
static float signed_field(uint32_t packed, unsigned first, unsigned bits)
{
    int32_t top_aligned = (int32_t)(packed << (32u - first - bits));
    return (float)(top_aligned >> (32u - bits));
}

static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->size || !a->stride)
        return 0;
    if (s_gpu.inline_active) {
        /* The batch arrived as INLINE_ARRAY, so `offset` is a byte offset into
         * the buffered payload rather than a guest address -- and 0 is a legal
         * one there, which is why the offset test is on the other side of this
         * branch. */
        size_t at = (size_t)a->offset + (size_t)index * a->stride;
        if (at + 4 > (size_t)s_gpu.inline_count * 4)
            return 0;
        p = (const uint8_t *)s_gpu.inline_src + at;
    } else {
        if (!a->offset)
            return 0;
        p = mem + a->offset + (size_t)index * a->stride;
    }

    switch (a->type) {
    case 0:                                  /* D3DCOLOR */
        /* A DWORD 0xAARRGGBB, so little-endian bytes are B,G,R,A -- not the
         * component order of every other format here. Returned as R,G,B,A so
         * callers need not know which format the title chose. */
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    case 1:                                  /* S1: normalised short */
    case 5: {                                /* S32K: short as it is */
        const int16_t *shorts = (const int16_t *)p;
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = a->type == 5 ? (float)shorts[i] : snorm16(shorts[i]);
        return 1;
    }
    case 6: {                                /* CMP: a packed 11:11:10 normal */
        uint32_t packed;
        memcpy(&packed, p, 4);
        out[0] = signed_field(packed, 0, 11) / 1023.0f;
        out[1] = signed_field(packed, 11, 11) / 1023.0f;
        out[2] = signed_field(packed, 22, 10) / 511.0f;
        return 1;
    }
    default:
        return 0;
    }
}

/* NV097 fixed-function transform methods (not in nv2a_regs.h's NV097 list). */
#define NV097_SET_COMPOSITE_MATRIX_FIRST 0x0680
#define NV097_SET_COMPOSITE_MATRIX_LAST  0x06BC
#define NV097_SET_VIEWPORT_OFFSET_FIRST  0x0A20
#define NV097_SET_VIEWPORT_OFFSET_LAST   0x0A2C
#define NV097_SET_TRANSFORM_EXEC_MODE    0x1E94
#define NV_XFORM_MODE_PROGRAM            2   /* low two bits; 0 = fixed */

/* The surface rectangle in real pixels.
 *
 * SET_SURFACE_CLIP_* is in logical pixels; with anti-aliasing on, the surface
 * is larger by the AA factor in SET_SURFACE_FORMAT bits 12-15 (0 = 1x1,
 * 1 = 2x1 "center corner 2", 2 = 2x2 "square offset 4"), and so are the
 * coordinates D3D's composite matrix produces. X-Men Legends renders its
 * 640x480 menu into a 1280x960 surface (pitch 5120), which read as an
 * 8-bytes-per-pixel surface and drew nothing until this was applied. */
static void surface_apply_clip(void)
{
    uint32_t aa = (s_gpu.format >> 12) & 0xF;
    uint32_t sx = aa ? 2 : 1, sy = aa == 2 ? 2 : 1;

    s_gpu.aa_sx = (float)sx;
    s_gpu.aa_sy = (float)sy;

    s_gpu.clip_x = (s_gpu.clip_raw_h & 0xFFFF) * sx;
    s_gpu.clip_w = ((s_gpu.clip_raw_h >> 16) & 0xFFFF) * sx;
    s_gpu.clip_y = (s_gpu.clip_raw_v & 0xFFFF) * sy;
    s_gpu.clip_h = ((s_gpu.clip_raw_v >> 16) & 0xFFFF) * sy;
}

/* Vertex programs (SET_TRANSFORM_EXECUTION_MODE = program).
 *
 * The program slots and the constant file, as the title uploads them. The
 * deko3d back end runs the program on the GPU (Nv2aRawBatch.vp_program /
 * vp_consts); the generations tell it when either changed.
 *
 * RECOMP_VP=0: program batches are handed over as if no program ran (A/B). */
#define NV097_SET_TRANSFORM_PROGRAM        0x0B00   /* 32 dwords */
#define NV097_SET_TRANSFORM_CONSTANT       0x0B80   /* 32 dwords */
#define NV097_SET_TRANSFORM_PROGRAM_LOAD   0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START  0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD  0x1EA4
#define VP_SLOTS  136
#define VP_CONSTS 192

static struct {
    uint32_t prog[VP_SLOTS][4];         /* microcode, four dwords a slot */
    float    c[VP_CONSTS][4];           /* the constant file */
    /* Where the next uploaded slot or constant goes, and where the program
     * starts. */
    uint32_t prog_load, const_load, prog_start;
    int      off;                       /* RECOMP_VP=0 */
    uint32_t gen;                       /* bumped by anything a batch reads */
    uint32_t prog_gen, const_gen;       /* contents' versions, for the back end */
    /* Constants written since the last batch: rows [dirty_lo, dirty_hi), and
     * the const_gen that batch carried (Nv2aRawBatch.vp_const_gen_prev). */
    uint32_t dirty_lo, dirty_hi, gen_handed;
} s_vp = { .prog_gen = 1, .const_gen = 1, .dirty_lo = 0, .dirty_hi = VP_CONSTS };

static inline void vp_const_dirty(uint32_t row)
{
    s_vp.dirty_lo = row < s_vp.dirty_lo ? row : s_vp.dirty_lo;
    s_vp.dirty_hi = row + 1 > s_vp.dirty_hi ? row + 1 : s_vp.dirty_hi;
}

/* The transform context shares the vertex-program constant file: on the
 * GPU these methods write constants, at the slots xemu names
 * NV_IGRAPH_XF_XFCTX_*:
 *   SET_EYE_POSITION     0x0A50  c[56]  (XFCTX_EYEP)
 *   SET_FOG_PLANE        0x09D0  c[57]  (XFCTX_FOG)
 *   SET_VIEWPORT_SCALE   0x0AF0  c[58]  (XFCTX_VPSCL)
 *   SET_VIEWPORT_OFFSET  0x0A20  c[59]  (XFCTX_VPOFF)
 * The epilogue the XDK appends to every vertex program scales and offsets
 * oPos with c[58] and c[59]. Dropped, every program put its batch at the
 * origin: the macOS JSRF port lost 714,240 draws that way (fixed 4 Sep), and
 * upstream xboxrecomp fixed the viewport slots in 0e9cff9 ("no 3D anywhere")
 * and the fog plane and eye position in #210. JSRF draws its characters with
 * vertex programs and most of the world with the fixed-function pipeline.
 * RECOMP_VP_XFCTX=0 stops the mirroring, for A/B runs. */
static int vp_xfctx_on(void)
{
    static int on = -1;

    if (on == -1) {
        const char *setting = getenv("RECOMP_VP_XFCTX");
        on = setting == NULL || strcmp(setting, "0") != 0;
    }
    return on;
}

static void vp_xfctx(uint32_t row, uint32_t component, uint32_t param)
{
    if (!vp_xfctx_on() || row >= VP_CONSTS || component > 3)
        return;
    memcpy(&s_vp.c[row][component], &param, 4);
    vp_const_dirty(row);
    s_vp.gen++;
    s_vp.const_gen++;
}

/* The transform-context methods vp_method mirrors (the fourth, the viewport
 * offset, is mirrored with its own handling in nv2a_pb_exec_method). */
static const struct { uint32_t method; uint32_t row; } k_xfctx[] = {
    { 0x0A50, 56 },                     /* SET_EYE_POSITION */
    { 0x09D0, 57 },                     /* SET_FOG_PLANE */
    { 0x0AF0, 58 },                     /* SET_VIEWPORT_SCALE */
};

/* One dword of an upload in progress: SET_TRANSFORM_PROGRAM / CONSTANT are
 * 32-dword windows that fill a slot four dwords at a time, moving on after
 * each fourth. The dword's place in the slot is its place in the window. */
static void vp_upload(uint32_t window_dword, uint32_t param, int constant)
{
    uint32_t lane = window_dword % 4;

    if (constant) {
        if (s_vp.const_load < VP_CONSTS) {
            memcpy(&s_vp.c[s_vp.const_load][lane], &param, 4);
            vp_const_dirty(s_vp.const_load);
        }
        s_vp.const_load += lane == 3;
        s_vp.const_gen++;
    } else {
        if (s_vp.prog_load < VP_SLOTS)
            s_vp.prog[s_vp.prog_load][lane] = param;
        s_vp.prog_load += lane == 3;
        s_vp.prog_gen++;
    }
    s_vp.gen++;
}

/* A vertex-program method: 1 if it was one. */
static int vp_method(uint32_t method, uint32_t param)
{
    size_t k;

    for (k = 0; k < sizeof k_xfctx / sizeof k_xfctx[0]; k++) {
        if (method - k_xfctx[k].method < 0x10) {
            vp_xfctx(k_xfctx[k].row, (method - k_xfctx[k].method) / 4, param);
            return 1;
        }
    }
    if (method - NV097_SET_TRANSFORM_PROGRAM < 0x80) {
        vp_upload((method - NV097_SET_TRANSFORM_PROGRAM) / 4, param, 0);
        return 1;
    }
    if (method - NV097_SET_TRANSFORM_CONSTANT < 0x80) {
        vp_upload((method - NV097_SET_TRANSFORM_CONSTANT) / 4, param, 1);
        return 1;
    }
    switch (method) {
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        s_vp.prog_load = param;
        return 1;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        s_vp.const_load = param;
        return 1;
    case NV097_SET_TRANSFORM_PROGRAM_START:
        s_vp.prog_start = param;
        s_vp.gen++;
        s_vp.prog_gen++;
        return 1;
    default:
        return 0;
    }
}

/* Is this batch transformed by a vertex program? */
static int batch_is_vp(void)
{
    static int checked;

    if (!checked) {
        const char *setting = getenv("RECOMP_VP");
        s_vp.off = setting != NULL && setting[0] == '0';
        checked = 1;
    }
    return !s_vp.off && (s_gpu.xform_mode & 3) == NV_XFORM_MODE_PROGRAM;
}

/* Is this batch transformed by the fixed-function unit? */
static int batch_is_ffp(void)
{
    return s_gpu.composite_set
        && (s_gpu.xform_mode & 3) != NV_XFORM_MODE_PROGRAM
        && !s_gpu.inline_active;
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* ── Render back end (nv2a_backend.h) ───────────────────────── */

static const Nv2aBackend *s_backend;

void nv2a_backend_register(const Nv2aBackend *backend)
{
    s_backend = backend;
}

static void current_surface(Nv2aSurface *out)
{
    out->color_va        = dma_resolve(s_gpu.color_offset);
    out->width           = s_gpu.clip_w;
    out->height          = s_gpu.clip_h;
    out->pitch           = s_gpu.pitch;
    out->bytes_per_pixel = surface_bpp();
    out->aa_sx           = s_gpu.aa_sx > 1.5f ? 2 : 1;
    out->aa_sy           = s_gpu.aa_sy > 1.5f ? 2 : 1;
    out->clip_x          = s_gpu.clip_x;
    out->clip_y          = s_gpu.clip_y;
}

static int sample_texture(const Texture *t, uint32_t u, uint32_t v, uint32_t *argb);

/* RECOMP_TEX_STATS=1: each format and size the back end has decoded, the
 * first time and every 1000th -- which decodes are worth a fast path. */
static void note_decode(const Nv2aTexture *tex)
{
    static int on = -1;
    static struct { uint32_t key, times; } kinds[64];
    uint32_t key, k;

    if (on == -1)
        on = getenv("RECOMP_TEX_STATS") != NULL;
    if (!on)
        return;
    key = (tex->color << 24) ^ (tex->width << 12) ^ tex->height;
    for (k = 0; k < 64; k++)
        if (kinds[k].times == 0 || kinds[k].key == key)
            break;
    if (k == 64)
        return;
    kinds[k].key = key;
    if (kinds[k].times++ % 1000 == 0)
        fprintf(stderr, "  [TEX] decode fmt 0x%02X %ux%u pitch %u x%u\n", tex->color,
                tex->width, tex->height, tex->pitch, kinds[k].times);
}

/* A texture as A8R8G8B8 rows of `width`, for a format the back end cannot
 * sample as stored. 0 if some texel could not be read. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out)
{
    const int linear_argb = tex->color == 0x12, linear_xrgb = tex->color == 0x1E;
    Texture source;
    uint32_t x, y;

    note_decode(tex);

    /* LIN_A8R8G8B8 / LIN_X8R8G8B8 already are the output: whole rows are
     * copied (a movie frame is one of these, every frame), X8 made opaque. */
    if ((linear_argb || linear_xrgb) && tex->pitch >= tex->width * 4) {
        const uint8_t *rows = (const uint8_t *)xbox_GetMemoryOffset() + tex->offset;
        for (y = 0; y < tex->height; y++) {
            uint32_t *out = argb_out + (size_t)y * tex->width;
            memcpy(out, rows + (size_t)y * tex->pitch, (size_t)tex->width * 4);
            if (linear_xrgb)
                for (x = 0; x < tex->width; x++)
                    out[x] |= 0xFF000000u;
        }
        return 1;
    }

    /* Everything else texel by texel, clamped at the edges. */
    memset(&source, 0, sizeof source);
    source.offset = tex->offset;
    source.width = tex->width;
    source.height = tex->height;
    source.pitch = tex->pitch;
    source.color = tex->color;
    source.addr_u = source.addr_v = 3;       /* clamp */
    source.valid = 1;
    for (y = 0; y < tex->height; y++)
        for (x = 0; x < tex->width; x++)
            if (!sample_texture(&source, x, y, argb_out + (size_t)y * tex->width + x))
                return 0;
    return 1;
}

/* CLEAR_SURFACE: the back end clears colour and/or depth and stencil
 * (param is the method's bits: Z 0x1, stencil 0x2, colour 0xF0). */
static void clear_surface(uint32_t param)
{
    Nv2aSurface surface;

    if (s_backend == NULL || s_backend->clear == NULL)
        return;
    if (s_gpu.color_offset == 0 || s_gpu.pitch == 0 || s_gpu.clip_h == 0 || surface_bpp() == 0)
        return;
    current_surface(&surface);
    s_backend->clear(&surface, &s_gpu.rs, param, s_gpu.clear_color, s_gpu.zstencil_clear);
    s_gpu.clears++;
}

/* ── Texture decoding (nv2a_backend_decode_texture) ───────────────────
 *
 * The CPU's reading of a texture, for the formats the back end cannot sample
 * as they are. */

/* One texel, in the title's own format.
 *
 * The codes are the NV097 colour field, which is the Xbox D3DFMT_ enum --
 * src/d3d/d3d8_xbox.h is the table, and it is the table to check against
 * rather than recollection: 0x1E is LIN_X8R8G8B8 and not, as this first read
 * it, a byte-reversed BGRA. Getting that one wrong turned an opaque black
 * render target into a screen of pure blue, which is the kind of wrong that
 * looks like content.
 *
 * Only the linear (LIN_) formats are read. A swizzled texture stores its
 * texels in Morton order rather than in rows, so reading one as if it had a
 * pitch does not give a slightly wrong colour, it gives a different image --
 * and inventing that image is exactly what this is not for. An unsupported
 * format samples nothing and the caller keeps the vertex colour, which is
 * visibly wrong rather than quietly wrong.
 *
 * ponytail: nearest texel, no filtering, whatever SET_TEXTURE_FILTER asked
 * for. Bilinear when a title's output actually depends on it.
 */
/* Off the edge of the texture, the way the title asked for.
 *
 * Refusing to sample instead is not neutral: it hands the caller back the
 * vertex colour, so a pass whose coordinates reach the last texel by half a
 * texel gets a bright line down the edge of the screen. The dashboard's
 * resolve does exactly that -- its last column and last row, 1119 pixels of
 * white on a black frame, from a rounding step at the boundary.
 */
static uint32_t wrap_coord(uint32_t c, uint32_t size, uint32_t mode)
{
    if (!size)
        return 0;
    if (mode == 1)                         /* wrap */
        return c % size;
    return c >= size ? size - 1 : c;       /* clamp, and everything else */
}

static uint32_t expand(uint32_t v, uint32_t bits)
{
    return d3d8_expand_channel(v, bits);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

/* Texel (u, v) of `t` -- a parameter, not s_gpu.tex, so a back end's thread
 * can decode while the executor goes on changing texture state. */
static int sample_texture(const Texture *t, uint32_t u, uint32_t v, uint32_t *argb)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t fmt;

    if (!t->valid)
        return 0;
    u = wrap_coord(u, t->width,  t->addr_u);
    v = wrap_coord(v, t->height, t->addr_v);

    fmt = t->color;
    if (d3d8_format_dxt_block_bytes(fmt))
        return d3d8_dxt_decode_texel(mem + t->offset, fmt, u, v,
                                     t->width, argb);
    if (d3d8_format_is_swizzled(fmt)) {
        /* Morton order: a texel's index is interleaved from x and y instead of
         * v*pitch + u, so index from the base of the image. The switch below
         * casts to each format's own width, which makes that index a texel
         * index for every one of them. */
        fmt = linear_twin(fmt);
        p = mem + t->offset;
        u = swizzle_offset(u, v, t->width, t->height);
    } else {
        p = mem + t->offset + (size_t)v * t->pitch;
    }

    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x1F, 5) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5) << 16)
              | (expand((t >>  5) & 0x3F, 6) <<  8)
              |  expand( t        & 0x1F, 5);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4) << 24)
              | (expand((t >>  8) & 0x0F, 4) << 16)
              | (expand((t >>  4) & 0x0F, 4) <<  8)
              |  expand( t        & 0x0F, 4);
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */

/* Fixed-function vertex lighting (SET_LIGHTING_ENABLE).
 *
 * With lighting on, the diffuse colour the combiners see is computed per
 * vertex from the normal and up to eight lights; the vertex's own colour
 * stream is ignored unless SET_COLOR_MATERIAL routes it in. Using the colour
 * stream anyway is what drew every lit mesh -- characters, streets -- black.
 *
 * XDK D3D pre-multiplies material into the light colours and folds
 * emission + material ambient * global ambient into SCENE_AMBIENT_COLOR, so
 * the hardware sum is simply
 *   colour = scene_ambient + sum_i atten_i * (ambient_i + diffuse_i * max(0, N.L_i))
 * with alpha from SET_MATERIAL_ALPHA. Everything is in eye space.
 *
 * ponytail: normals go through the model-view matrix itself rather than the
 * inverse transpose (exact for rotations and uniform scale), no specular, no
 * spot cones, no back-face colours, no skinning. Add them when a mesh looks
 * wrong rather than dark. */
#define NV_LIGHTS 8

/* An untransformed batch drawn as if it were screen space smears a few pixels
 * into the corner, so the batch has to be classified before it is rasterised.
 *
 * This used to demand that every vertex land inside the surface, which is a
 * different question and the wrong one: geometry that extends past the
 * viewport is ordinary, and clipping it is raster_triangle's job (it clamps
 * its span to the clip rect). The dashboard is exactly the case that exposed
 * it -- a full-screen pass drawn as one oversized triangle, vertices at
 * (-0.5,-0.5), (2*w,-0.5), (-0.5,2*h), all correct and all rejected.
 *
 * What actually separates the two is scale. Object-space positions are model
 * units, a handful either side of the origin; screen-space ones are measured
 * in pixels of a surface hundreds of pixels wide. So: the batch has to be able
 * to touch the surface at all, and it has to be bigger than object space.
 *
 * ponytail: a genuinely tiny screen-space sprite reads as object space and is
 * skipped. It is counted as skipped rather than silently dropped, and the
 * unambiguous answer needs the vertex-program state, which is not tracked yet.
 */
#define OBJECT_SPACE_SPAN 8.0f

/* NV097 primitive types.
 *
 * These are the operand of SET_BEGIN_END, where 0 is END and the list starts
 * at 1. They were each one too low, so every title's geometry was decomposed
 * as the primitive below the one it asked for -- a strip as a fan, a fan as
 * quads, and TRIANGLES, the one case whose vertex count must be a multiple
 * of three, as a strip.
 *
 * The vertex order says which numbering is right without taking a table on
 * trust: a strip arrives in Z order and a fan in cyclic order, and they only
 * line up with the primitive under this one. */
#define NV_PRIM_POINTS         1
#define NV_PRIM_LINES          2
#define NV_PRIM_LINE_LOOP      3
#define NV_PRIM_LINE_STRIP     4
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9
#define NV_PRIM_POLYGON        10

uint32_t nv2a_pb_resolve(uint32_t dma_offset)
{
    return dma_resolve(dma_offset);
}

/* Hand the batch to the back end as raw NV2A state (see Nv2aRawBatch).
 *
 * Vertices are gathered over the index range the batch touches -- a mesh
 * names most of them several times. What the back end cannot read as stored
 * is converted to float4 by fetch_attr (every attribute type, inline arrays
 * too); the rest is described where it lies (raw_direct_attrs). */
static float    *s_raw_attrs;          /* converted vertices, [v][attr][4] */
static uint32_t *s_raw_idx;            /* indices rebased to the lowest */
static uint32_t  s_raw_cap_v, s_raw_cap_i;

/* Attributes described where they lie unless RECOMP_DK_DIRECT=0 (then every
 * one is converted, the slow way, for A/B runs). */
static int raw_direct_on(void)
{
    static int on = -1;

    if ((s_backend->flags & NV2A_BACKEND_RAW_DIRECT) == 0)
        return 0;
    if (on == -1) {
        const char *setting = getenv("RECOMP_DK_DIRECT");
        on = setting == NULL || setting[0] != '0';
    }
    return on;
}

/* Bytes one element of attribute `va` takes as stored, or 0 if the back end
 * cannot read that format directly. */
static uint32_t direct_element_bytes(const VertexAttr *va)
{
    if (va->size > 4)
        return 0;
    switch (va->type) {
    case 0:  return va->size == 4 ? 4u : 0u;   /* D3DCOLOR: B, G, R, A */
    case 1:                                    /* S1: normalised short */
    case 5:  return 2u * va->size;             /* S32K: short as it is */
    case 2:  return 4u * va->size;             /* float */
    case 4:  return va->size;                  /* normalised unsigned byte */
    default: return 0;                         /* CMP and the rest: converted */
    }
}

/* rb->direct[] for each present attribute the back end can read as stored,
 * over gathered vertices first .. first + count - 1, and the mask of them.
 * What fetch_attr would read as zeros (no array, an inline array that runs
 * short) stays with the converter. */
static uint16_t raw_direct_attrs(Nv2aRawBatch *rb, uint16_t present,
                                 uint32_t first, uint32_t count)
{
    const uint8_t *guest = (const uint8_t *)xbox_GetMemoryOffset();
    uint16_t direct = 0;
    uint32_t a;

    for (a = 0; a < NV2A_RAW_ATTRS; a++) {
        const VertexAttr *va = &s_gpu.attr[a];
        uint16_t bit = (uint16_t)(1u << a);
        uint32_t element = (present & bit) ? direct_element_bytes(va) : 0;
        size_t start, end;

        if (element == 0)
            continue;
        start = (size_t)va->offset + (size_t)first * va->stride;
        end = start + (size_t)(count - 1) * va->stride + element;
        if (s_gpu.inline_active) {
            if ((size_t)s_gpu.inline_count * 4 < end)
                continue;
            rb->direct[a].ptr = (const uint8_t *)s_gpu.inline_src + start;
            rb->attr_direct_local |= bit;      /* the executor's memory, not guest */
        } else if (va->offset != 0) {
            rb->direct[a].ptr = guest + start;
        } else {
            continue;
        }
        rb->direct[a].type = va->type;
        rb->direct[a].size = va->size;
        rb->direct[a].stride = va->stride;
        direct |= bit;
    }
    rb->attr_direct = direct;
    return direct;
}

static void attr_const_init(void);
static void attr_const_changed(void);

void (*nv2a_raw_batch_hook)(Nv2aRawBatch *rb, float *attrs);

static inline int pb_fast(void);

/* The lowest and highest index of the batch. The whole-run index paths keep
 * them as they fill the list (idx_mm); word by word (RECOMP_PB_FAST=0) they
 * are found here, so the two can be compared. */
static void batch_index_range(uint32_t *lowest, uint32_t *highest)
{
    const uint32_t *index = s_gpu.idx;
    uint32_t count = s_gpu.idx_count, low = 0xFFFFFFFFu, high = 0, i;

    if (s_gpu.idx_mm && pb_fast()) {
        *lowest = s_gpu.idx_lo;
        *highest = s_gpu.idx_hi;
        return;
    }
    /* In locals, so the compiler sees nothing in the loop writes them and
     * does it a vector at a time. */
    for (i = 0; i < count; i++) {
        low = index[i] < low ? index[i] : low;
        high = index[i] > high ? index[i] : high;
    }
    *lowest = low;
    *highest = high;
}

/* *buffer holds at least `needed` units of `unit` bytes, with room to spare;
 * 0 if memory ran out. */
static int reserve_units(void **buffer, uint32_t *capacity, uint32_t needed, size_t unit)
{
    if (needed <= *capacity)
        return *buffer != NULL;
    free(*buffer);
    *capacity = needed + 1024;
    *buffer = malloc((size_t)*capacity * unit);
    return *buffer != NULL;
}

/* Vertex i's attribute a, in the converted array. */
#define RAW_SLOT(i, a) (s_raw_attrs + ((size_t)(i) * NV2A_RAW_ATTRS + (a)) * 4)

/* Convert to float4 the present attributes not described directly. With
 * nothing direct, an absent or unreadable attribute reads its constant. */
static void convert_attrs(uint16_t present, uint16_t direct, uint32_t first, uint32_t count)
{
    uint32_t i, a;

    if (direct) {
        /* Usually nothing: CMP normals, arrays that run off their buffer. */
        uint16_t left = present & (uint16_t)~direct;
        for (a = 0; left != 0 && a < NV2A_RAW_ATTRS; a++)
            if (left & (1u << a))
                for (i = 0; i < count; i++)
                    fetch_attr(&s_gpu.attr[a], first + i, RAW_SLOT(i, a));
        return;
    }
    for (i = 0; i < count; i++)
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (!(present & (1u << a)) || !fetch_attr(&s_gpu.attr[a], first + i, RAW_SLOT(i, a)))
                memcpy(RAW_SLOT(i, a), s_gpu.attr_const[a], sizeof s_gpu.attr_const[a]);
}

/* The state half of the batch: shadow, programs, surface, transform,
 * texture addresses, the constants that changed since the last batch. */
static void describe_state(Nv2aRawBatch *rb)
{
    uint32_t stage;

    rb->regs = s_reg;
    rb->vp_program = (const uint32_t (*)[4])s_vp.prog;
    rb->vp_slots = VP_SLOTS;
    rb->vp_consts = (const float (*)[4])s_vp.c;
    rb->vp_const_count = VP_CONSTS;
    rb->vp_start = s_vp.prog_start;
    current_surface(&rb->surface);
    rb->color_va = rb->surface.color_va;
    rb->zeta_va = s_gpu.rs.zeta_va;
    rb->xform = batch_is_vp() ? 2u : batch_is_ffp() ? 1u : 0u;
    memcpy(rb->composite, s_gpu.composite, sizeof rb->composite);
    memcpy(rb->vp_offset, s_gpu.vp_offset, sizeof rb->vp_offset);
    rb->aa_sx = s_gpu.aa_sx;
    rb->aa_sy = s_gpu.aa_sy;
    /* SET_TEXTURE_OFFSET and SET_TEXTURE_PALETTE of each stage, resolved
     * here so a back end behind a queue never reads the DMA state. */
    for (stage = 0; stage < 4; stage++) {
        uint32_t image = s_reg[(0x1B00u + 0x40u * stage) / 4];
        uint32_t palette = s_reg[(0x1B20u + 0x40u * stage) / 4] & ~0x3Fu;
        rb->tex_va[stage] = image ? dma_resolve(image) : 0;
        rb->pal_va[stage] = palette ? dma_resolve(palette) : 0;
    }
    memcpy(rb->attr_const, s_gpu.attr_const, sizeof rb->attr_const);
    rb->attr_const_gen = s_gpu.attr_const_gen;
    /* Generations: never 0, which a back end reads as "unknown". */
    rb->vp_prog_gen = s_vp.prog_gen ? s_vp.prog_gen : 1;
    rb->vp_const_gen = s_vp.const_gen ? s_vp.const_gen : 1;
    rb->vp_const_gen_prev = s_vp.gen_handed;
    rb->vp_const_lo = s_vp.dirty_lo < VP_CONSTS ? s_vp.dirty_lo : VP_CONSTS;
    rb->vp_const_hi = s_vp.dirty_hi;
    s_vp.gen_handed = rb->vp_const_gen;
    s_vp.dirty_lo = VP_CONSTS;
    s_vp.dirty_hi = 0;
}

static void raw_batch(void)
{
    Nv2aRawBatch rb;
    uint32_t lowest, highest, vertices, i, a;
    uint16_t present = 0, direct = 0;
    int absolute;

    attr_const_init();
    batch_index_range(&lowest, &highest);
    vertices = highest - lowest + 1;
    if (vertices > 1u << 20)
        return;                                /* not a real batch */
    if (!reserve_units((void **)&s_raw_attrs, &s_raw_cap_v, vertices,
                       NV2A_RAW_ATTRS * 4 * sizeof(float))
        || !reserve_units((void **)&s_raw_idx, &s_raw_cap_i, s_gpu.idx_count, sizeof(uint32_t)))
        return;

    for (a = 0; a < NV2A_RAW_ATTRS && a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size != 0 && s_gpu.attr[a].stride != 0)
            present |= (uint16_t)(1u << a);
    memset(&rb, 0, sizeof rb);
    if (raw_direct_on())
        direct = raw_direct_attrs(&rb, present, lowest, vertices);
    convert_attrs(present, direct, lowest, vertices);

    /* Indices as the title gave them, when the back end rebases them itself
     * (and no hook wants to look at gathered ones). */
    absolute = (s_backend->flags & NV2A_BACKEND_RAW_ABS_INDICES) && nv2a_raw_batch_hook == NULL;
    if (!absolute) {
        const uint32_t *index = s_gpu.idx;
        uint32_t *rebased = s_raw_idx, count = s_gpu.idx_count;
        for (i = 0; i < count; i++)
            rebased[i] = index[i] - lowest;
    }
    rb.prim = s_gpu.prim;
    rb.vertex_count = vertices;
    rb.attr_present = present;
    rb.attrs = s_raw_attrs;
    rb.indices = absolute ? s_gpu.idx : s_raw_idx;
    rb.index_base = absolute ? lowest : 0;
    rb.index_count = s_gpu.idx_count;
    describe_state(&rb);

    if (nv2a_raw_batch_hook != NULL) {
        nv2a_raw_batch_hook(&rb, s_raw_attrs);
        rb.vp_const_gen_prev = 0;              /* it may have changed them: all rows */
    }
    s_backend->draw_raw(&rb);
    s_gpu.tris_drawn += s_gpu.idx_count / 3;
    s_gpu.drawn_offset = s_gpu.color_offset;
    s_gpu.drawn_pitch = s_gpu.pitch;
}

/* Every batch goes to the back end as the NV2A sees it (raw_batch): the
 * deko3d renderer runs the vertex program, the combiners and the rest on
 * the GPU. Without a back end there is nothing to draw with. */
static void raster_batch(void)
{
    s_vp.gen++;
    if (s_gpu.idx_count >= 3 && s_backend && s_backend->draw_raw)
        raw_batch();
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
static void draw_primitive(void)
{
    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    s_gpu.verts += s_gpu.idx_count;
    raster_batch();
}

/* Draw the vertices the title wrote straight into the pushbuffer.
 *
 * INLINE_ARRAY carries no offsets and no indices: the dwords between BEGIN and
 * END *are* the vertex buffer, packed in attribute order using the same
 * SET_VERTEX_DATA_ARRAY_FORMAT registers an ordinary array would use. So the
 * whole batch is describable as a vertex array whose base happens to be that
 * payload, which means synthesising the layout and handing it to the existing
 * path -- rather than a second copy of the topology and rasterisation code.
 *
 * The title's own attribute table is saved and put back: these offsets and
 * strides are ours, and it has not stopped using its.
 *
 * ponytail: each attribute is padded to a whole dword. That is exact for the
 * float and D3DCOLOR formats every inline batch actually uses; a packed
 * sub-dword attribute would need the unpadded layout.
 */
static void draw_inline_array(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t off = 0, a, i, vsize, count;

    memcpy(saved, s_gpu.attr, sizeof saved);

    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        uint32_t bytes;
        if (!s_gpu.attr[a].size)
            continue;
        switch (s_gpu.attr[a].type) {
        case 0:  bytes = 4;                        break;  /* D3DCOLOR   */
        case 2:  bytes = 4 * s_gpu.attr[a].size;   break;  /* float      */
        case 4:  bytes = s_gpu.attr[a].size;       break;  /* ubyte norm */
        case 1:
        case 5:  bytes = 2 * s_gpu.attr[a].size;   break;  /* short      */
        case 6:  bytes = 4;                        break;  /* packed CMP */
        default: bytes = 4 * s_gpu.attr[a].size;   break;
        }
        s_gpu.attr[a].offset = off;
        off += (bytes + 3u) & ~3u;
    }
    vsize = off;
    if (!vsize)
        goto out;

    count = (s_gpu.inline_count * 4) / vsize;
    if (count < 3 || count > NV_MAX_INDICES)
        goto out;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.attr[a].size)
            s_gpu.attr[a].stride = vsize;

    for (i = 0; i < count; i++)
        s_gpu.idx[i] = i;
    s_gpu.idx_count = count;
    s_gpu.idx_mm = 0;

    s_gpu.inline_src = s_gpu.inline_buf;
    s_gpu.inline_active = 1;
    draw_primitive();
    s_gpu.inline_active = 0;

out:
    memcpy(s_gpu.attr, saved, sizeof saved);
    s_gpu.idx_count = 0;
}

/* Immediate-mode vertices, kept the way xemu keeps them (pgraph/vertex.c):
 * a float4 per vertex for each attribute that varies in the batch, attribute
 * major, so attribute a of vertex v is s_imm[a][v]. Allocated with the first
 * immediate vertex; a title that never draws that way never pays for it. */
#define IMM_MAX_VERTS 8192
static float (*s_imm)[IMM_MAX_VERTS][4];

/* Draw the vertices the immediate-mode methods completed.
 *
 * Same trick as draw_inline_array: rather than a second copy of the topology
 * and rasterisation code, describe what was accumulated as ordinary vertex
 * arrays and hand them to the existing path. Each attribute that varies gets
 * a float4 array; the rest get none and read their current value, which is
 * how the NV2A (and xemu) treat an attribute set once around the vertices.
 * JSRF's stencil-shadow darkening quad sets its colour that way -- black at
 * half alpha, after another quad's white -- so taking colour only from
 * SET_VERTEX_DATA4UB slot 3 drew every shadow opaque white.
 *
 * The title's own table is saved and put back -- it has not stopped using it.
 */
static void draw_immediate(void)
{
    VertexAttr saved[NV_VERTEX_ATTRS];
    uint32_t a, i, n = s_gpu.imm_count;

    if (n >= 3 && s_imm && (s_gpu.imm_populated & 1u)) {
        memcpy(saved, s_gpu.attr, sizeof saved);
        memset(s_gpu.attr, 0, sizeof s_gpu.attr);
        /* Offsets are byte offsets into s_imm here, not guest addresses --
         * fetch_attr reads them that way while inline_active is set, which is
         * also why 0 is a legal offset for position. */
        for (a = 0; a < NV_VERTEX_ATTRS; a++) {
            if (!(s_gpu.imm_populated & (1u << a)))
                continue;
            s_gpu.attr[a].type = 2;                     /* float4 */
            s_gpu.attr[a].size = 4;
            s_gpu.attr[a].offset = a * (uint32_t)sizeof *s_imm;
            s_gpu.attr[a].stride = 4 * sizeof(float);
        }
        for (i = 0; i < n && i < NV_MAX_INDICES; i++)
            s_gpu.idx[i] = i;
        s_gpu.idx_count = i;
        s_gpu.idx_mm = 0;

        /* fetch_attr bounds-checks against inline_count dwords. */
        s_gpu.inline_src = (const uint32_t *)s_imm;
        s_gpu.inline_count = NV_VERTEX_ATTRS * IMM_MAX_VERTS * 4;
        s_gpu.inline_active = 1;
        draw_primitive();
        s_gpu.inline_active = 0;

        memcpy(s_gpu.attr, saved, sizeof saved);
        s_gpu.idx_count = 0;
        s_gpu.inline_count = 0;
    }
    /* After the batch a varying attribute's current value is its last
     * vertex's, as xemu leaves it. */
    if (n && s_imm && s_gpu.imm_populated) {
        for (a = 0; a < NV_VERTEX_ATTRS; a++)
            if (s_gpu.imm_populated & (1u << a))
                memcpy(s_gpu.attr_const[a], s_imm[a][n - 1], sizeof s_gpu.attr_const[a]);
        attr_const_changed();
    }
    s_gpu.imm_populated = 0;
}

/* The attributes' current values before anything wrote them: (0, 0, 0, 1),
 * except diffuse, white -- what the back ends assumed for it. Once. */
static void attr_const_init(void)
{
    static const float unset[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t a;

    if (s_gpu.attr_const_gen != 0)
        return;
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        memcpy(s_gpu.attr_const[a], a == 3 ? white : unset, sizeof unset);
    s_gpu.attr_const_gen = 1;
}

/* A current value changed: a new generation for the back end (never 0,
 * which means "not set up yet"). */
static void attr_const_changed(void)
{
    s_gpu.attr_const_gen++;
    if (s_gpu.attr_const_gen == 0)
        s_gpu.attr_const_gen = 1;
}

/* Attribute `a` is about to be written: xemu's
 * pgraph_allocate_inline_buffer_vertices. A write after the batch's first
 * vertex makes the attribute vary, and the vertices already complete keep the
 * value it had; one written only before the first vertex stays constant. */
static void imm_attr_touch(uint32_t a)
{
    uint32_t v;

    if (!s_gpu.imm_count || !s_imm || (s_gpu.imm_populated & (1u << a)))
        return;
    s_gpu.imm_populated |= (uint16_t)(1u << a);
    for (v = 0; v < s_gpu.imm_count; v++)
        memcpy(s_imm[a][v], s_gpu.attr_const[a], sizeof s_imm[a][v]);
}

/* A write to position's last component completed a vertex: xemu's
 * pgraph_finish_inline_buffer_vertex. */
static void imm_finish_vertex(void)
{
    static int warned;
    uint32_t a;

    if (!s_gpu.prim)
        return;
    if (!s_imm && !(s_imm = malloc(NV_VERTEX_ATTRS * sizeof *s_imm)))
        return;
    if (s_gpu.imm_count >= IMM_MAX_VERTS) {
        if (!warned) {
            warned = 1;
            fprintf(stderr, "  [GPU] immediate batch over %u vertices; the rest dropped\n",
                    IMM_MAX_VERTS);
        }
        return;
    }
    for (a = 0; a < NV_VERTEX_ATTRS; a++)
        if (s_gpu.imm_populated & (1u << a))
            memcpy(s_imm[a][s_gpu.imm_count], s_gpu.attr_const[a], sizeof s_imm[a][0]);
    s_gpu.imm_count++;
}

/* How an attribute method converts its parameter (xemu's handlers). */
enum {
    IMM_F,          /* one float component                           */
    IMM_F_W1,       /* one float component, w = 1 (the 3F forms)     */
    IMM_F_Z0W1,     /* one float component, z = 0, w = 1 (2F forms)  */
    IMM_UB,         /* four unsigned bytes / 255                     */
    IMM_S2,         /* two shorts, z = 0, w = 1                      */
    IMM_S4,         /* two shorts per dword, over two dwords         */
    IMM_N3S,        /* normal shorts / 32767, no lower than -1       */
    IMM_FOG,        /* one float into all four                       */
    IMM_W1F         /* weight: x, 0, 0, 1                            */
};

/* The per-attribute methods below SET_VERTEX_DATA*: base, bytes, attribute,
 * conversion. */
static const struct {
    uint16_t base, bytes;
    uint8_t  attr, kind;
} k_imm_methods[] = {
    { 0x1500, 12,  0, IMM_F_W1 },   /* SET_VERTEX3F; z completes a vertex */
    { 0x1518, 16,  0, IMM_F },      /* SET_VERTEX4F; w completes one      */
    { 0x1530, 12,  2, IMM_F },      /* SET_NORMAL3F                       */
    { 0x1540, 12,  2, IMM_N3S },    /* SET_NORMAL3S                       */
    { 0x1550, 16,  3, IMM_F },      /* SET_DIFFUSE_COLOR4F                */
    { 0x1560, 12,  3, IMM_F_W1 },   /* SET_DIFFUSE_COLOR3F                */
    { 0x156C,  4,  3, IMM_UB },     /* SET_DIFFUSE_COLOR4UB               */
    { 0x1570, 16,  4, IMM_F },      /* SET_SPECULAR_COLOR4F               */
    { 0x1580, 12,  4, IMM_F_W1 },   /* SET_SPECULAR_COLOR3F               */
    { 0x158C,  4,  4, IMM_UB },     /* SET_SPECULAR_COLOR4UB              */
    { 0x1590,  8,  9, IMM_F_Z0W1 }, /* SET_TEXCOORD0_2F                   */
    { 0x1598,  4,  9, IMM_S2 },     /* SET_TEXCOORD0_2S                   */
    { 0x15A0, 16,  9, IMM_F },      /* SET_TEXCOORD0_4F                   */
    { 0x15B0,  8,  9, IMM_S4 },     /* SET_TEXCOORD0_4S                   */
    { 0x15B8,  8, 10, IMM_F_Z0W1 }, /* SET_TEXCOORD1_2F, _2S, _4F, _4S    */
    { 0x15C0,  4, 10, IMM_S2 },
    { 0x15C8, 16, 10, IMM_F },
    { 0x15D8,  8, 10, IMM_S4 },
    { 0x15E0,  8, 11, IMM_F_Z0W1 }, /* SET_TEXCOORD2_*                    */
    { 0x15E8,  4, 11, IMM_S2 },
    { 0x15F0, 16, 11, IMM_F },
    { 0x1600,  8, 11, IMM_S4 },
    { 0x1608,  8, 12, IMM_F_Z0W1 }, /* SET_TEXCOORD3_*                    */
    { 0x1610,  4, 12, IMM_S2 },
    { 0x1620, 16, 12, IMM_F },
    { 0x1630,  8, 12, IMM_S4 },
    { 0x1698,  4,  5, IMM_FOG },    /* SET_FOG_COORD                      */
    { 0x169C,  4,  1, IMM_W1F },    /* SET_WEIGHT1F                       */
    { 0x16A0,  8,  1, IMM_F_Z0W1 }, /* SET_WEIGHT2F                       */
    { 0x16B0, 12,  1, IMM_F_W1 },   /* SET_WEIGHT3F                       */
    { 0x16C0, 16,  1, IMM_F },      /* SET_WEIGHT4F                       */
};

/* An attribute method: SET_VERTEX_DATA{2F,4F,2S,4UB,4S}_M or one of
 * k_imm_methods. Sets the attribute's current value with xemu's conversions,
 * and a write to position's last component completes an immediate vertex.
 * Returns 1 if `method` was one of them. */
static int imm_attr_write(uint32_t method, uint32_t param)
{
    union { uint32_t u; float f; } v;
    float lo = (float)(int16_t)(param & 0xFFFF), hi = (float)(int16_t)(param >> 16);
    uint32_t a, k, kind, i, end;
    float *c;

    if (method >= NV097_SET_VERTEX_DATA2F_M
            && method < NV097_SET_VERTEX_DATA4F_M + NV_VERTEX_ATTRS * 16) {
        /* SET_VERTEX_DATA*: one window per format, each attribute's slot in
         * it `per_attr` bytes wide. A write to the slot's last dword of
         * attribute 0 (position) completes a vertex. Highest window first. */
        static const struct {
            uint32_t base, per_attr, kind;
        } k_data[] = {
            { NV097_SET_VERTEX_DATA4F_M, 16, IMM_F },
            { NV097_SET_VERTEX_DATA4S_M,  8, IMM_S4 },
            { NV097_SET_VERTEX_DATA4UB,   4, IMM_UB },
            { NV097_SET_VERTEX_DATA2S,    4, IMM_S2 },
            { NV097_SET_VERTEX_DATA2F_M,  8, IMM_F_Z0W1 },
        };
        uint32_t w = 0, offset;

        while (method < k_data[w].base)
            w++;
        offset = method - k_data[w].base;
        a = offset / k_data[w].per_attr;
        k = offset % k_data[w].per_attr / 4;
        kind = k_data[w].kind;
        end = a == 0 && k == k_data[w].per_attr / 4 - 1;
    } else if (method >= NV097_SET_VERTEX3F && method < 0x16D0) {
        for (i = 0; i < sizeof k_imm_methods / sizeof k_imm_methods[0]; i++)
            if (method >= k_imm_methods[i].base
                    && method < (uint32_t)k_imm_methods[i].base + k_imm_methods[i].bytes)
                break;
        if (i == sizeof k_imm_methods / sizeof k_imm_methods[0])
            return 0;
        a = k_imm_methods[i].attr;
        kind = k_imm_methods[i].kind;
        k = (method - k_imm_methods[i].base) / 4;
        end = method == NV097_SET_VERTEX3F + 8 || method == NV097_SET_VERTEX4F + 12;
    } else {
        return 0;
    }

    v.u = param;
    attr_const_init();
    imm_attr_touch(a);
    c = s_gpu.attr_const[a];
    switch (kind) {
    case IMM_F:      c[k] = v.f;                                      break;
    case IMM_F_W1:   c[k] = v.f; c[3] = 1.0f;                         break;
    case IMM_F_Z0W1: c[k] = v.f; c[2] = 0.0f; c[3] = 1.0f;            break;
    case IMM_UB:
        for (i = 0; i < 4; i++)
            c[i] = (float)((param >> (8 * i)) & 0xFF) / 255.0f;
        break;
    case IMM_S2:     c[0] = lo; c[1] = hi; c[2] = 0.0f; c[3] = 1.0f;  break;
    case IMM_S4:     c[(k % 2) * 2] = lo; c[(k % 2) * 2 + 1] = hi;    break;
    case IMM_N3S:
        c[(k % 2) * 2]     = lo / 32767.0f < -1.0f ? -1.0f : lo / 32767.0f;
        c[(k % 2) * 2 + 1] = hi / 32767.0f < -1.0f ? -1.0f : hi / 32767.0f;
        break;
    case IMM_FOG:    c[0] = c[1] = c[2] = c[3] = v.f;                 break;
    default:         c[0] = v.f; c[1] = c[2] = 0.0f; c[3] = 1.0f;     break;
    }
    attr_const_changed();
    if (end)
        imm_finish_vertex();
    return 1;
}

/* The render state the back end gets with every batch (Nv2aRenderState),
 * caught as its methods go by. 1 if `method` was one of them. */
#define RS_WORD(m, field) case m: rs->field = param; return 1
static int capture_render_state(uint32_t method, uint32_t param)
{
    Nv2aRenderState *rs = &s_gpu.rs;
    float value;

    switch (method) {
    RS_WORD(0x0300, alpha_test_enable);         /* SET_ALPHA_TEST_ENABLE */
    RS_WORD(0x0304, blend_enable);              /* SET_BLEND_ENABLE */
    RS_WORD(0x0308, cull_enable);               /* SET_CULL_FACE_ENABLE */
    RS_WORD(0x030C, depth_test_enable);         /* SET_DEPTH_TEST_ENABLE */
    RS_WORD(0x033C, alpha_func);                /* SET_ALPHA_FUNC */
    RS_WORD(0x0340, alpha_ref);                 /* SET_ALPHA_REF */
    RS_WORD(0x0344, blend_src);                 /* SET_BLEND_FUNC_SFACTOR */
    RS_WORD(0x0348, blend_dst);                 /* SET_BLEND_FUNC_DFACTOR */
    RS_WORD(0x034C, blend_color);               /* SET_BLEND_COLOR */
    RS_WORD(0x0350, blend_eq);                  /* SET_BLEND_EQUATION */
    RS_WORD(0x0354, depth_func);                /* SET_DEPTH_FUNC */
    RS_WORD(0x0358, color_mask);                /* SET_COLOR_MASK */
    RS_WORD(0x035C, depth_write);               /* SET_DEPTH_MASK */
    RS_WORD(0x039C, cull_face);                 /* SET_CULL_FACE */
    RS_WORD(0x03A0, front_face);                /* SET_FRONT_FACE */
    case 0x0394:                                /* SET_CLIP_MIN */
    case 0x0398:                                /* SET_CLIP_MAX */
        memcpy(&value, &param, sizeof value);
        if (method == 0x0394)
            rs->depth_min = value;
        else
            rs->depth_max = value;
        return 1;
    case 0x0214:                                /* SET_SURFACE_ZETA_OFFSET */
        rs->zeta_va = param != 0 ? dma_resolve(param) : 0;
        return 1;
    case 0x1D8C:                                /* SET_ZSTENCIL_CLEAR_VALUE */
        s_gpu.zstencil_clear = param;
        return 1;
    default:
        return 0;
    }
}
#undef RS_WORD

/* ── Software-method traps, with the title's PGRAPH handler wrapped ──────
 *
 * A NOP(n) the executor cannot carry out itself is a trap: PGRAPH posts it
 * (TRAPPED_ADDR/DATA, NSOURCE, PGRAPH_INTR, PMC_INTR bit 12), raises IRQ 3 and
 * stalls until the driver's handler has taken it.
 *
 * Telling "taken" from the handler's own PGRAPH_FIFO writes (0 on entry, 1 on
 * exit) loses traps once the handler also runs from the title's busy-waits --
 * D3D calls it whenever PGRAPH_INTR is non-zero. A call that began just
 * before a trap was posted saw no NSOURCE, did nothing, and its exit write
 * read as the trap handled; the fence event was never set and the game slept
 * in BlockOnTime. So a project wraps the handler (JSRF: jsrf_manual_overrides.c)
 * in nv2a_pb_trap_lock/unlock, the lock a trap is posted under: a call sees
 * no trap or a whole one, and reports taking it (nv2a_pb_trap_taken). */
static volatile LONG s_trap_hooked;    /* a wrapper is in use */
static volatile LONG s_trap_lock;      /* 1: held */
static volatile LONG s_trap_taken;     /* the posted trap was taken */

/* NV2A registers the trap protocol touches, as offsets from 0xFD000000. */
#define NV_PMC_INTR_0          0x000100u
#define NV_PMC_INTR_EN_0       0x000140u
#define NV_PGRAPH_INTR         0x400100u
#define NV_PGRAPH_NSOURCE      0x400108u
#define NV_PGRAPH_TRAPPED_ADDR 0x400704u
#define NV_PGRAPH_TRAPPED_DATA 0x400708u
#define NV_PMC_INTR_PGRAPH     0x1000u
#define NV_REG(nv, offset)     ((nv)[(offset) / 4])

/* The interrupt words are written from several threads (the executor, the
 * ack threads, the vblank tick, the wrapper): a plain |= or &= can undo
 * another thread's change. */
#define NV_OR(p, v)  InterlockedOr((volatile LONG *)(p), (LONG)(v))
#define NV_AND(p, v) InterlockedAnd((volatile LONG *)(p), (LONG)(v))

static volatile uint32_t *nv2a_regs(void)
{
    return (volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + 0xFD000000u);
}

/* What the executor sleeps on while the driver works (a trap taken, a flip
 * retired), instead of spinning a core. Auto-reset; the waits time out each
 * millisecond, so a lost signal costs a millisecond and not a hang. */
static HANDLE s_driver_event;

static HANDLE driver_event(void)
{
    HANDLE made;

    if (s_driver_event != NULL)
        return s_driver_event;
    made = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (InterlockedCompareExchangePointer((PVOID volatile *)&s_driver_event, made, NULL) != NULL)
        CloseHandle(made);
    return s_driver_event;
}

static int trap_lock_try(void)
{
    return InterlockedCompareExchange(&s_trap_lock, 1, 0) == 0;
}

static void trap_lock_wait(void)
{
    while (!trap_lock_try())
        Sleep(0);
}

static int trap_was_taken(void)
{
    return InterlockedCompareExchange(&s_trap_taken, 0, 0) != 0;
}

void nv2a_pb_trap_lock(void)
{
    InterlockedExchange(&s_trap_hooked, 1);
    trap_lock_wait();
}

void nv2a_pb_trap_unlock(void)
{
    InterlockedExchange(&s_trap_lock, 0);
}

/* For the ack thread (xbox_memory_layout.c), which clears PGRAPH_INTR and
 * PMC_INTR bit 12 whenever no trap is held. It used to read "held" once per
 * pass, so a trap posted after that read was wiped: the wrapper then saw
 * NSOURCE, reported the trap taken, and the handler -- which acts only on
 * PGRAPH_INTR -- had done nothing. JSRF slept in BlockOnTime for good (1 Nov
 * log, 434 s). Under this lock the ack sees a whole trap or none. Unlike
 * nv2a_pb_trap_lock it does not mark the trap hooked. */
int nv2a_pb_trap_trylock(void)
{
    return trap_lock_try();
}

/* Under the lock, after a handler call that found NSOURCE set: the trap is
 * acknowledged, so clear what the write-1-to-clear would have. */
void nv2a_pb_trap_taken(void)
{
    volatile uint32_t *nv = nv2a_regs();

    NV_REG(nv, NV_PGRAPH_NSOURCE) = 0;
    NV_AND(&NV_REG(nv, NV_PGRAPH_INTR), ~1u);
    NV_AND(&NV_REG(nv, NV_PMC_INTR_0), ~NV_PMC_INTR_PGRAPH);
    InterlockedExchange(&s_trap_taken, 1);
    SetEvent(driver_event());
}

/* The interrupt bits of a pending trap, set (again). */
static void trap_raise(volatile uint32_t *nv)
{
    NV_OR(&NV_REG(nv, NV_PGRAPH_INTR), 1u);
    NV_OR(&NV_REG(nv, NV_PMC_INTR_0), NV_PMC_INTR_PGRAPH);
}

static void trap_deliver_hooked(volatile uint32_t *nv, uint32_t method,
                                uint32_t subch, uint32_t param)
{
    extern void xbox_set_irq_line(uint32_t vector, int level);
    extern void xbox_Nv2aHoldInterrupts(int on);
    static unsigned delivered;
    ULONGLONG next_warning = GetTickCount64() + 2000;

    xbox_Nv2aHoldInterrupts(1);        /* the ack thread leaves PGRAPH alone */

    /* Post it whole, under the lock. */
    trap_lock_wait();
    InterlockedExchange(&s_trap_taken, 0);
    NV_REG(nv, NV_PGRAPH_TRAPPED_ADDR) = method | (subch << 16);
    NV_REG(nv, NV_PGRAPH_TRAPPED_DATA) = param;
    NV_REG(nv, NV_PGRAPH_NSOURCE) = 1;
    trap_raise(nv);
    nv2a_pb_trap_unlock();
    xbox_set_irq_line(3, 1);

    /* PGRAPH stalls until it is taken, however long that is. */
    while (!trap_was_taken()) {
        /* Level-triggered, as on the hardware. The ack thread may have read
         * "not held" just before the hold went up and cleared the bits after
         * it; raised again under the lock, so a trap already taken never is. */
        if (trap_lock_try()) {
            if (!trap_was_taken())
                trap_raise(nv);
            nv2a_pb_trap_unlock();
        }
        if (GetTickCount64() > next_warning) {
            fprintf(stderr, "  [PB] NOP(%u) trap still pending after 2 s "
                    "(PMC_INTR %08X EN %08X)\n", param, NV_REG(nv, NV_PMC_INTR_0),
                    NV_REG(nv, NV_PMC_INTR_EN_0));
            next_warning = GetTickCount64() + 2000;
        }
        WaitForSingleObject(driver_event(), 1);
    }
    xbox_set_irq_line(3, 0);
    if ((++delivered & 1023) == 0)
        fprintf(stderr, "  [PB] NOP trap handled, %u so far\n", delivered);
    xbox_Nv2aHoldInterrupts(0);
}

/* THE 2D ENGINE'S RECTANGLE COPY (NV062 surfaces + NV09F image blit).
 *
 * XDK D3D copies rectangles between surfaces and textures -- CopyRects --
 * with the NV2A's 2D classes on subchannels other than the 3D one: NV062
 * names the colour format, the two pitches and the source and destination
 * offsets, NV09F the operation, the two corner points and the size, and
 * writing the size runs the copy. The executor dropped every method on those
 * subchannels, so no copy ever happened: in JSRF's garage the graffiti picked
 * did not appear on the preview or the walls until the menu was opened again
 * and loaded it from the disc.
 *
 * The classes are told apart by methods only one of them has (NV062's
 * SET_OFFSET_DESTIN 0x30C; NV09F's SET_OPERATION 0x2FC and
 * SET_CONTEXT_SURFACES 0x19C): the two share 0x300-0x308. SRCCOPY is done
 * in guest memory, row by row; other operations (blends through beta) are
 * logged and skipped, as is a copy that touches a surface the renderer keeps
 * on the GPU (Nv2aBackend.surface_at), whose guest bytes are stale.
 * RECOMP_PB_BLIT=0 drops the methods again, as before. */
static struct {
    int      sub_surf, sub_blit;            /* -1: not seen yet */
    uint32_t reg[8][5];                     /* 0x2FC..0x30C per subchannel */
    uint32_t done, skipped;
} s_2d = { -1, -1, {{0}}, 0, 0 };

static int blit_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_BLIT");
        on = !(e && *e == '0');
    }
    return on;
}

static uint32_t blit_bpp(uint32_t fmt)
{
    switch (fmt) {
    case 0x1: return 1;                                 /* Y8 */
    case 0x2: case 0x3: case 0x4: case 0x5: return 2;   /* 1555, 565, Y16 */
    case 0x6: case 0x7: case 0x8: case 0x9: case 0xA: case 0xB: return 4;
    default:  return 0;
    }
}

/* [va, va + bytes) inside guest RAM (above the XBE base, below 128 MB) or the
 * contiguous window: what a copy may read or write. */
static int blit_range_ok(uint32_t va, uint64_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    return (va >= 0x10000u && end <= 0x08000000u)
        || (va >= XBOX_CONTIG_BASE && end <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE);
}

static void blit_2d(uint32_t size)
{
    const uint32_t *sf = s_2d.reg[s_2d.sub_surf], *bl = s_2d.reg[s_2d.sub_blit];
    uint32_t fmt = sf[1], pitch = sf[2], op = bl[0];
    uint32_t in = bl[1], out = bl[2];
    uint32_t w = size & 0xFFFF, h = size >> 16, bpp = blit_bpp(fmt);
    uint32_t sp = pitch & 0xFFFF, dp = pitch >> 16;
    uint32_t sx = in & 0xFFFF, sy = in >> 16, dx = out & 0xFFFF, dy = out >> 16;
    uint32_t src = dma_resolve(sf[3]), dst = dma_resolve(sf[4]);
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    const char *why = NULL;
    uint32_t r;

    if (!bpp)
        why = "colour format unknown";
    else if (op != 3)
        why = "operation not SRCCOPY";
    else if (!w || !h || w > 4096 || h > 4096 || !sp || !dp || sp > 65535 || dp > 65535)
        why = "size or pitch out of range";
    else if (!src || !dst)
        why = "no address";
    else if (!blit_range_ok(src + sy * sp, (uint64_t)(h - 1) * sp + (uint64_t)(sx + w) * bpp)
             || !blit_range_ok(dst + dy * dp, (uint64_t)(h - 1) * dp + (uint64_t)(dx + w) * bpp))
        why = "outside guest memory";
    else if (s_backend && s_backend->surface_at
             && (s_backend->surface_at(src + sy * sp, (h - 1) * sp + (sx + w) * bpp)
                 || s_backend->surface_at(dst + dy * dp, (h - 1) * dp + (dx + w) * bpp)))
        why = "a render target (kept on the GPU)";
    if (why) {
        if (s_2d.skipped++ < 16)
            fprintf(stderr, "  [2D] copy %ux%u skipped: %s (format %X, op %u, src 0x%08X+%u,%u"
                            " pitch %u, dst 0x%08X+%u,%u pitch %u)\n", w, h, why, fmt, op,
                    src, sx, sy, sp, dst, dx, dy, dp);
        return;
    }
    /* Bottom up when the destination rows lie below the source's in the
     * same memory, so no source row is overwritten before it is read. */
    for (r = 0; r < h; r++) {
        uint32_t k = (dst + dy * dp > src + sy * sp) ? h - 1 - r : r;
        memmove(mem + dst + (dy + k) * dp + dx * bpp, mem + src + (sy + k) * sp + sx * bpp,
                (size_t)w * bpp);
    }
    /* Textures made from the bytes just written are stale. The back end's
     * own check samples a texture's bytes, and a copy that changes the
     * middle of a picture on a transparent ground can miss every sample. */
    {
        int refreshed = (s_backend && s_backend->written)
            ? s_backend->written(dst + dy * dp, (h - 1) * dp + (dx + w) * bpp) : -1;
        if (s_2d.done++ < 16 || (s_2d.done & 1023) == 0)
            fprintf(stderr, "  [2D] copy %ux%u, %u bytes a pixel: 0x%08X (%u,%u) -> 0x%08X (%u,%u)"
                            " (%u so far; %d textures to make again)\n", w, h, bpp, src, sx, sy,
                    dst, dx, dy, s_2d.done, refreshed);
    }
}

/* A method on a 2D subchannel: 1 when it was the copy engine's. */
static int blit_method(uint32_t subch, uint32_t method, uint32_t param)
{
    if (subch >= 8 || !blit_on())
        return 0;
    if (method == 0x030C && s_2d.sub_surf != (int)subch) {
        s_2d.sub_surf = (int)subch;
        fprintf(stderr, "  [2D] subchannel %u is the 2D surfaces (NV062)\n", subch);
    }
    if ((method == 0x02FC || method == 0x019C) && s_2d.sub_blit != (int)subch) {
        s_2d.sub_blit = (int)subch;
        fprintf(stderr, "  [2D] subchannel %u is the image blit (NV09F)\n", subch);
    }
    if (method >= 0x02FC && method <= 0x030C)
        s_2d.reg[subch][(method - 0x02FC) / 4] = param;
    if (method == 0x0308 && (int)subch == s_2d.sub_blit && s_2d.sub_surf >= 0) {
        blit_2d(param);
        return 1;
    }
    return (int)subch == s_2d.sub_surf || (int)subch == s_2d.sub_blit;
}

/* ── Taking a header's run whole (nv2a_pb_exec_run) ──────────────────
 *
 * The walk used to hand every word to nv2a_pb_exec_method on its own: the
 * shadow write, the special cases, the switch, then a chain of range tests
 * that most state methods fall all the way through to note_unhandled. In
 * JSRF's busiest streets (700-900 draws a frame, handheld) reading the stream
 * that way was half the executor thread, more than the drawing, and most of
 * the words were bulk data: transform constants, index lists. So a header's
 * run is now taken whole. Index lists, inline vertices and transform
 * program/constant uploads go straight into place; a state method found to
 * be shadow-only (it fell through every handler to note_unhandled) is
 * remembered as such and goes straight to the shadow after that. Everything
 * else is still nv2a_pb_exec_method, word by word, in stream order.
 * RECOMP_PB_FAST=0 puts every word back through nv2a_pb_exec_method. */
static int s_pb_fast = -1;
static int pb_fast_init(void)
{
    const char *e = getenv("RECOMP_PB_FAST");
    s_pb_fast = !(e && *e == '0') && !pb_verbose();
    return s_pb_fast;
}
static inline int pb_fast(void)
{
    return __builtin_expect(s_pb_fast >= 0, 1) ? s_pb_fast : pb_fast_init();
}

/* s_plain[m / 4]: 3D method m reaches only the shadow and note_unhandled.
 * Which handler takes a method depends on the method alone, except for the
 * ones excluded where this is set (nv2a_pb_exec_method's default case). */
static uint8_t s_plain[0x2000 / 4];

/* Words the walk handed over, by how they were taken, for the [perf] line. */
static struct { uint64_t words, consts, prog, idx, inl, plain; } s_ws;

void nv2a_pb_exec_words(uint64_t out[6])
{
    out[0] = s_ws.words;  out[1] = s_ws.consts; out[2] = s_ws.prog;
    out[3] = s_ws.idx;    out[4] = s_ws.inl;    out[5] = s_ws.plain;
}

/* The words none of those fast paths took ("the rest" on the [perf] line),
 * by 3D method, with RECOMP_PB_REST=1: the next thing to make cheaper is
 * whichever leads. Subchannel 0 only, below 0x2000; anything else counts
 * under 0x2000. Off by default: in JSRF's streets no method leads (none
 * over 1% of the words), and the counting was 7% of the walk's own time. */
static uint32_t s_rest[0x2000 / 4 + 1];
static int s_rest_on = -1;

static inline void rest_note(uint32_t subch, uint32_t m)
{
    if (__builtin_expect(s_rest_on > 0, 0))
        s_rest[subch == 0 && m < 0x2000u ? m / 4 : 0x2000 / 4]++;
}

/* The `max` methods with the most such words since the last call (method,
 * words), most first; returns how many, and clears the counts. */
int nv2a_pb_exec_rest_top(uint32_t method[], uint32_t words[], int max)
{
    int n = 0, i, j;
    if (s_rest_on < 0) {
        const char *e = getenv("RECOMP_PB_REST");
        s_rest_on = e && e[0] == '1';
    }
    if (!s_rest_on)
        return 0;
    for (i = 0; i <= 0x2000 / 4; i++) {
        uint32_t c = s_rest[i];
        if (!c)
            continue;
        s_rest[i] = 0;
        if (n == max && words[n - 1] >= c)
            continue;
        j = n < max ? n++ : max - 1;    /* the slot it takes: a new one, or the last */
        for (; j > 0 && words[j - 1] < c; j--) {
            words[j] = words[j - 1];
            method[j] = method[j - 1];
        }
        words[j] = c;
        method[j] = (uint32_t)i * 4;
    }
    return n;
}

/* The shadow is the 3D class's register file: methods to the other
 * subchannels (2D surfaces, blits) share its offsets and must not land in
 * it. NV062's SET_OFFSET_SOURCE is 0x0308 = SET_CULL_FACE_ENABLE: a blit
 * before NFSU2's loading screen (main menu -> Career / Quick Race) switched
 * culling on with the menu car's FRONT/CW state and culled the whole
 * screen. */
static inline void shadow_write(uint32_t method, uint32_t param)
{
    uint32_t word = (method & 0x1FFCu) / 4;

    if (s_reg[word] == param)
        return;
    s_reg[word] = param;
    s_reg_dirty[SHADOW_BLOCK(word)] = 1;
}

/* Time the executor spends stalled on NOP traps (waiting for the title's
 * PGRAPH handler), for the [perf] line. */
static uint64_t s_trap_us, s_trap_n;
static void trap_timed(const LARGE_INTEGER *t0)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t1;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t1);
    s_trap_us += (uint64_t)((t1.QuadPart - t0->QuadPart) * 1000000 / f.QuadPart);
    s_trap_n++;
}

void nv2a_pb_exec_traps(uint64_t out[2])
{
    out[0] = s_trap_us;
    out[1] = s_trap_n;
}

/* The fixed-function transform's state the back end gets with a batch: the
 * composite matrix, the viewport offset (also c[59] for vertex programs,
 * see vp_xfctx) and which transform unit runs. 1 if `method` was one. */
static int capture_transform(uint32_t method, uint32_t param)
{
    uint32_t index;

    if (method - NV097_SET_COMPOSITE_MATRIX_FIRST
            <= NV097_SET_COMPOSITE_MATRIX_LAST - NV097_SET_COMPOSITE_MATRIX_FIRST) {
        index = (method - NV097_SET_COMPOSITE_MATRIX_FIRST) / 4;
        memcpy(&s_gpu.composite[index], &param, sizeof param);
        s_gpu.composite_set = 1;
        return 1;
    }
    if (method - NV097_SET_VIEWPORT_OFFSET_FIRST
            <= NV097_SET_VIEWPORT_OFFSET_LAST - NV097_SET_VIEWPORT_OFFSET_FIRST) {
        index = (method - NV097_SET_VIEWPORT_OFFSET_FIRST) / 4;
        memcpy(&s_gpu.vp_offset[index], &param, sizeof param);
        vp_xfctx(59, index, param);
        return 1;
    }
    if (method == NV097_SET_TRANSFORM_EXEC_MODE) {
        s_gpu.xform_mode = param;
        return 1;
    }
    return 0;
}

#define NV_PGRAPH_FIFO 0x400720u

/* The software-method trap, for a title whose handler is not wrapped (and
 * for the first trap of one whose wrapper has not run yet).
 *
 * The status registers are plain memory here, and the handler acknowledges
 * by writing the status back -- write-one-to-clear on hardware, a no-op on
 * RAM. What it does visibly is bracket its work with PGRAPH_FIFO: 0 on
 * entry, 1 on the way out. So a sentinel 2 goes there; the handler writing
 * 0 over it means it is in and has the status; then it is given up to 2 ms
 * to write the 1, and the executor clears what the hardware would have.
 *
 * PGRAPH never drops a trap: one given up on is a fence event nobody sets,
 * and the title sleeps for good (NFSU2's attract movie froze on its first
 * frame whenever a raised IRQL held the ISR off a moment). So the wait lasts
 * as long as it takes, with a line every 2 s. */
static void trap_deliver_fifo(volatile uint32_t *nv, uint32_t method,
                              uint32_t subch, uint32_t param)
{
    extern void xbox_set_irq_line(uint32_t vector, int level);
    extern void xbox_Nv2aHoldInterrupts(int on);
    extern int xbox_IrqlRaisedCount(void);
    static unsigned delivered, reported;
    ULONGLONG next_warning = GetTickCount64() + 2000, grace_end;

    xbox_Nv2aHoldInterrupts(1);
    NV_REG(nv, NV_PGRAPH_TRAPPED_ADDR) = method | (subch << 16);
    NV_REG(nv, NV_PGRAPH_TRAPPED_DATA) = param;
    NV_REG(nv, NV_PGRAPH_NSOURCE) = 1;
    NV_REG(nv, NV_PGRAPH_FIFO) = 2;
    trap_raise(nv);
    xbox_set_irq_line(3, 1);

    while (NV_REG(nv, NV_PGRAPH_FIFO) == 2) {
        /* Level-triggered: raised until taken, which also undoes an ack
         * thread that cleared the bits just before the hold went up. */
        trap_raise(nv);
        if (GetTickCount64() > next_warning) {
            fprintf(stderr, "  [PB] NOP(%u) trap still pending after 2 s "
                    "(PMC_INTR %08X EN %08X)\n", param, NV_REG(nv, NV_PMC_INTR_0),
                    NV_REG(nv, NV_PMC_INTR_EN_0));
            next_warning = GetTickCount64() + 2000;
        }
        Sleep(0);
    }

    /* In, so the interrupt is taken: the PMC source and the line go now.
     * D3D's DPC runs the handler again while PMC_INTR bit 12 is up, and it
     * then found the same trap still in PGRAPH_INTR/NSOURCE and queued the
     * same flip again -- thousands of times per trap in NFSU2, until D3D's
     * flip targets were millions of vblanks ahead and PersistDisplay, which
     * waits for pending flips to retire, never returned. */
    NV_AND(&NV_REG(nv, NV_PMC_INTR_0), ~NV_PMC_INTR_PGRAPH);
    xbox_set_irq_line(3, 0);
    /* The way-out 1 is brief (the DPC calls the handler straight back while
     * the status stays set), so it is spun for, not slept for. */
    grace_end = GetTickCount64() + 2;
    while (NV_REG(nv, NV_PGRAPH_FIFO) != 1 && GetTickCount64() < grace_end)
        ;
    delivered++;
    if (reported < 8 || (delivered & 1023) == 0) {
        reported++;
        fprintf(stderr, "  [PB] NOP(%u) trap handled, %u handled so far "
                "(PMC_INTR %08X EN %08X PGRAPH_INTR %08X irql-raised %d)\n",
                param, delivered, NV_REG(nv, NV_PMC_INTR_0), NV_REG(nv, NV_PMC_INTR_EN_0),
                NV_REG(nv, NV_PGRAPH_INTR), xbox_IrqlRaisedCount());
    }
    NV_AND(&NV_REG(nv, NV_PGRAPH_INTR), ~1u);
    NV_REG(nv, NV_PGRAPH_NSOURCE) = 0;
    NV_AND(&NV_REG(nv, NV_PMC_INTR_0), ~NV_PMC_INTR_PGRAPH);
    NV_REG(nv, NV_PGRAPH_FIFO) = 1;
    xbox_set_irq_line(3, 0);
    xbox_Nv2aHoldInterrupts(0);
}

/* NO_OPERATION with a non-zero parameter: PGRAPH raises a software-method
 * interrupt and stalls until the driver has handled it. Stock XDK D3D posts
 * NOP(5) after a WAIT_FOR_IDLE and sleeps on an event its PGRAPH handler
 * sets, so without the trap that sleep never ends. */
static void trap_deliver(uint32_t subch, uint32_t method, uint32_t param)
{
    volatile uint32_t *nv = nv2a_regs();
    LARGE_INTEGER started;

    /* The handler must see a GPU that has done everything before the trap:
     * a back end on its own thread catches up first. */
    nv2a_backend_queue_sync(0);
    QueryPerformanceCounter(&started);
    if (InterlockedCompareExchange(&s_trap_hooked, 0, 0))
        trap_deliver_hooked(nv, method, subch, param);
    else
        trap_deliver_fifo(nv, method, subch, param);
    trap_timed(&started);
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    static int inited;

    if (subch == 0) {
        shadow_write(method, param);
        if (method < 0x2000u && s_plain[method / 4]) {
            note_unhandled(method, param);
            return;
        }
    }
    /* BACK_END_WRITE_SEMAPHORE_RELEASE. The title's pointer already names the
     * semaphore word; SET_SEMAPHORE_OFFSET is not added (in X-Men Legends it
     * held 0xFF000000 at times, which put the write outside guest memory). */
    if (method == 0x1D70 && s_sem_va) {
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        progress_store((volatile uint32_t *)(mem + s_sem_va), param);
        return;
    }
    /* NO_OPERATION with a non-zero parameter is a trap (trap_deliver). */
    if (subch == 0 && method == 0x0100 && param) {
        trap_deliver(subch, method, param);
        return;
    }
    /* The pattern object's SET_MONOCHROME_COLOR0 (0x0310) latches into
     * NV_PGRAPH_PATT_COLOR0 (0xFD400B10), and stock XDK D3D uses exactly that
     * as a progress marker: every fence it inserts is a pattern-colour write
     * carrying (sequence & 0x1F) << 2 ahead of the semaphore release, and
     * its fence wait then polls PATT_COLOR0 until those bits match the
     * semaphore -- i.e. until PGRAPH itself has got that far. With nothing
     * latching the register the wait never ends (NFSU2 froze on its loading
     * screen). 0x0310 on the 3D class (subchannel 0) is an unrelated state
     * method, hence the subchannel test. */
    if (subch != 0 && method == 0x0310) {
        uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
        progress_store((volatile uint32_t *)(mem + 0xFD400B10u), param);
        return;
    }
    if (!inited) {
        inited = 1;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (pb_verbose()) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        /* RECOMP_PB_2D=1 (from nfsu2-sw 77b9a83): every method on another
         * subchannel -- the 2D classes, surface-to-surface blits -- which the
         * executor drops. The first 4000. */
        static int trace = -1;
        static unsigned traced;
        if (trace == -1) {
            const char *setting = getenv("RECOMP_PB_2D");
            trace = setting != NULL && setting[0] != '\0' && setting[0] != '0';
        }
        if (trace && traced < 4000) {
            traced++;
            fprintf(stderr, "  [2D] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
        }
        if (blit_method(subch, method, param))
            return;
        note_unhandled(method, param);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_raw_h = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_raw_v = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        surface_apply_clip();
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_CLEAR_SURFACE:
        clear_surface(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
            s_gpu.idx_lo = 0xFFFFFFFFu;
            s_gpu.idx_hi = 0;
            s_gpu.idx_mm = 1;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
            s_gpu.imm_populated = 0;
        } else {
            /* Three ways a batch can have arrived, and only one is in use at
             * a time: vertices completed by SET_VERTEX4F, a payload written
             * with INLINE_ARRAY, or indices into the title's own arrays. */
            if (s_gpu.imm_count)
                draw_immediate();
            else if (s_gpu.inline_count)
                draw_inline_array();
            else
                draw_primitive();
            s_gpu.prim = 0;
            s_gpu.inline_count = 0;
            s_gpu.imm_count = 0;
        }
        break;

    case NV097_INLINE_ARRAY:
        /* Vertex data, not a pointer to it. Buffered rather than decoded here
         * because the format is only fully known at END. */
        if (s_gpu.prim && s_gpu.inline_count < NV_MAX_INLINE)
            s_gpu.inline_buf[s_gpu.inline_count++] = param;
        break;

    case NV097_DRAW_ARRAYS: {
        /* The method this title actually draws with, and the reason the
         * executor reported zero draws while geometry was being submitted the
         * whole time: BEGIN_END arrived, END arrived, and in between came a
         * run description rather than the index list the draw path wanted, so
         * every batch ended with idx_count == 0 and was dropped in silence.
         *
         * Expanded into indices because that is what the rasteriser consumes,
         * and an implicit run is just the indices start..start+count-1. */
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = start + i;
        s_gpu.idx_mm = 0;
        break;
    }

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
            s_gpu.idx_mm = 0;
        }
        break;
    case 0x1808:                            /* ARRAY_ELEMENT32: one index */
        if (s_gpu.prim && s_gpu.idx_count < NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = param;
            s_gpu.idx_mm = 0;
        }
        break;
    case NV097_SET_TEXTURE_OFFSET:
        /* A texture offset is a DMA-object offset, exactly like a surface or a
         * vertex array offset -- physical, and reachable only through the
         * contiguous window when it names contiguous memory. */
        s_gpu.tex.offset = dma_resolve(param);
        record_tex_reg(method, param);
        break;

    case NV097_SET_FLIP_READ:
        s_gpu.flip_read = param;
        return;

    case NV097_SET_FLIP_WRITE:
        s_gpu.flip_write = param;
        return;

    case NV097_SET_FLIP_MODULO:
        s_gpu.flip_modulo = param;
        return;

    case NV097_FLIP_INCREMENT_WRITE:
        s_gpu.flip_write = s_gpu.flip_modulo
                         ? (s_gpu.flip_write + 1) % s_gpu.flip_modulo
                         : s_gpu.flip_write + 1;
        s_gpu.flips++;
        /* Verbose runs: the ranked unhandled-method report every 600 flips,
         * so a run that is killed rather than exits still leaves one. */
        if (pb_verbose() && s_gpu.flips % 600 == 0)
            nv2a_pb_exec_report();
        return;

    case NV097_FLIP_STALL:
        /* PGRAPH holds here until the buffer it is about to draw into is no
         * longer the one being scanned out. Nothing here reports a flip
         * retiring (D3D does it with PGRAPH_INCREMENT on a vblank), so the
         * stall ends at once, with the buffer just finished being read. */
        s_gpu.flip_read = s_gpu.flip_write;
        /* And this is a completed swap, which is what a title's own swap
         * counter counts -- see xbox_Nv2aFrameCounterFlip. Behind a back
         * end on its own thread, once that has the frame's draws. */
        if (nv2a_backend_queue_on())
            nv2a_backend_queue_call(xbox_Nv2aFrameCounterFlip);
        else
            xbox_Nv2aFrameCounterFlip();
        if (s_backend && s_backend->flip)
            s_backend->flip();
        if (pb_verbose()) {
            static unsigned n;
            if (n++ < 8) {
                fprintf(stderr, "  [GPU] flip %u: read=%u write=%u\n",
                        s_gpu.flips, s_gpu.flip_read, s_gpu.flip_write);
                fflush(stderr);
            }
        }
        return;

    case NV097_SET_TEXTURE_FORMAT:
        s_gpu.tex.color = (param >> 8) & 0xFF;
        /* A swizzled texture carries its own dimensions here, as log2 in
         * BASE_SIZE_U/V (nv2a_regs.h: 0x00F00000 / 0x0F000000). It has to:
         * SET_TEXTURE_IMAGE_RECT describes a linear image, and a title that
         * only uses swizzled textures never sends one -- this title sends it
         * once and sets a format 3,176 times. Without this the width and
         * height stayed zero and nothing was ever sampled. */
        if (tex_size_from_format((param >> 8) & 0xFF)) {
            s_gpu.tex.width  = 1u << ((param >> 20) & 0xF);
            s_gpu.tex.height = 1u << ((param >> 24) & 0xF);
        }
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_ADDRESS:
        /* Four bits per axis. 1 is wrap, 3 is clamp-to-edge; the rest (mirror,
         * border) fall back to clamp, which is wrong at an edge rather than
         * wrong everywhere. */
        s_gpu.tex.addr_u =  param        & 0xF;
        s_gpu.tex.addr_v = (param >>  8) & 0xF;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_CONTROL1:
        /* Pitch lives in the top half. Only meaningful for a linear format; a
         * swizzled texture has no pitch because it has no rows. */
        s_gpu.tex.pitch = param >> 16;
        record_tex_reg(method, param);
        break;

    case NV097_SET_TEXTURE_IMAGE_RECT:
        s_gpu.tex.width  = param >> 16;
        s_gpu.tex.height = param & 0xFFFF;
        record_tex_reg(method, param);
        break;

    default:
        if (capture_render_state(method, param) || capture_transform(method, param)
                || vp_method(method, param))
            break;
        if (method >= NV_TEX_FIRST && method <= NV_TEX_LAST)
            record_tex_reg(method, param);
        if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            /* Resolved here, once, so every consumer -- the rasteriser's
             * attribute reads and the diagnostics alike -- sees the same
             * address. A vertex array offset is a DMA-object offset exactly
             * like a surface offset: physical, and addressable only through
             * the window when it names contiguous memory. */
            s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4].offset =
                dma_resolve(param);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            if (a->size == 7)                   /* SIZE_3W */
                a->size = 3;
            a->stride = (param >> 8)  & 0xFF;
        } else if (!imm_attr_write(method, param)) {
            /* Shadow-only, and it always will be: every test above is on the
             * method alone. Not the semaphore release or NOP (taken before
             * the switch only with a semaphore registered / a parameter),
             * and not the texture registers (record_tex_reg ran as well). */
            if (method < 0x2000u && method != 0x1D70 && method != 0x0100
             && !(method >= NV_TEX_FIRST && method <= NV_TEX_LAST) && pb_fast())
                s_plain[method / 4] = 1;
            note_unhandled(method, param);
        }
        break;
    }
}

/* SET_TRANSFORM_PROGRAM / SET_TRANSFORM_CONSTANT words of an increasing run
 * starting at `method`, as vp_method takes them one at a time (nothing ahead
 * of it in nv2a_pb_exec_method touches 0x0B00-0x0BFF). Returns how many of
 * the run's words it took: up to the end of the two ranges. */
static uint32_t vp_upload_run(uint32_t method, const uint32_t *p, uint32_t n)
{
    uint32_t i = 0, kp = 0, k = (NV097_SET_TRANSFORM_CONSTANT + 0x80 - method) / 4, load;

    if (k > n)
        k = n;
    if (method < NV097_SET_TRANSFORM_CONSTANT) {    /* program words first */
        kp = (NV097_SET_TRANSFORM_CONSTANT - method) / 4;
        if (kp > k)
            kp = k;
        load = s_vp.prog_load;
        for (; i < kp; i++) {
            uint32_t slot = (method + i * 4 - NV097_SET_TRANSFORM_PROGRAM) / 4;
            shadow_write(method + i * 4, p[i]);
            if (load < VP_SLOTS)
                s_vp.prog[load][slot % 4] = p[i];
            load += slot % 4 == 3;
        }
        s_vp.prog_load = load;
        s_vp.prog_gen += kp;
        s_ws.prog += kp;
    }
    load = s_vp.const_load;
    {
        uint32_t first = VP_CONSTS, last = 0;     /* rows written */
        for (; i < k; i++) {
            uint32_t slot = (method + i * 4 - NV097_SET_TRANSFORM_CONSTANT) / 4;
            shadow_write(method + i * 4, p[i]);
            if (load < VP_CONSTS) {
                memcpy(&s_vp.c[load][slot % 4], &p[i], 4);
                if (first == VP_CONSTS)
                    first = load;
                last = load;
            }
            load += slot % 4 == 3;
        }
        if (first < VP_CONSTS) {
            vp_const_dirty(first);
            vp_const_dirty(last);
        }
    }
    s_vp.const_load = load;
    s_vp.const_gen += k - kp;
    s_ws.consts += k - kp;
    s_vp.gen += k;
    return k;
}

/* A non-increasing run of one method: the shadow ends on the last word and
 * its block is dirty if any word changed it, as word by word. */
static inline void shadow_run(uint32_t method, const uint32_t *p, uint32_t n)
{
    uint32_t word = (method & 0x1FFCu) / 4, value = s_reg[word], differed = 0, i;

    for (i = 0; i < n; i++) {
        differed |= value ^ p[i];
        value = p[i];
    }
    s_reg[word] = value;
    if (differed != 0)
        s_reg_dirty[SHADOW_BLOCK(word)] = 1;
}

/* A header's run: `n` parameter words at p for `method` on `subch`, every
 * word to the same method when noninc. The same as nv2a_pb_exec_method on
 * each word in turn (see "Taking a header's run whole" above). */
void nv2a_pb_exec_run(uint32_t subch, uint32_t method, int noninc,
                      const uint32_t *p, uint32_t n)
{
    uint32_t i = 0;

    s_ws.words += n;
    if (subch == 0 && n && pb_fast()) {
        if (noninc) {
            switch (method) {
            case NV097_ARRAY_ELEMENT16:         /* two 16-bit indices a word */
                if (s_gpu.prim) {
                    uint32_t k = (NV_MAX_INDICES - s_gpu.idx_count) / 2, *d;
                    uint32_t lo = s_gpu.idx_lo, hi = s_gpu.idx_hi;
                    if (k > n) k = n;
                    d = s_gpu.idx + s_gpu.idx_count;
                    /* Locals throughout, and the range found on the way, so
                     * raw_batch does not walk the indices again for it. */
                    for (i = 0; i < k; i++) {
                        uint32_t a = p[i] & 0xFFFFu, b = p[i] >> 16;
                        uint32_t mn = a < b ? a : b, mx = a < b ? b : a;
                        d[2 * i]     = a;
                        d[2 * i + 1] = b;
                        lo = mn < lo ? mn : lo;
                        hi = mx > hi ? mx : hi;
                    }
                    s_gpu.idx_lo = lo;
                    s_gpu.idx_hi = hi;
                    s_gpu.idx_count += 2 * k;
                }
                shadow_run(method, p, n);
                s_ws.idx += n;
                return;
            case 0x1808:                        /* ARRAY_ELEMENT32 */
                if (s_gpu.prim) {
                    uint32_t k = NV_MAX_INDICES - s_gpu.idx_count, *d;
                    uint32_t lo = s_gpu.idx_lo, hi = s_gpu.idx_hi;
                    if (k > n) k = n;
                    d = s_gpu.idx + s_gpu.idx_count;
                    for (i = 0; i < k; i++) {
                        d[i] = p[i];
                        lo = p[i] < lo ? p[i] : lo;
                        hi = p[i] > hi ? p[i] : hi;
                    }
                    s_gpu.idx_lo = lo;
                    s_gpu.idx_hi = hi;
                    s_gpu.idx_count += k;
                }
                shadow_run(method, p, n);
                s_ws.idx += n;
                return;
            case NV097_INLINE_ARRAY:
                if (s_gpu.prim) {
                    uint32_t k = NV_MAX_INLINE - s_gpu.inline_count;
                    if (k > n) k = n;
                    memcpy(s_gpu.inline_buf + s_gpu.inline_count, p, (size_t)k * 4);
                    s_gpu.inline_count += k;
                }
                shadow_run(method, p, n);
                s_ws.inl += n;
                return;
            default:
                break;
            }
        } else if (method >= NV097_SET_TRANSFORM_PROGRAM
                && method < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
            i = vp_upload_run(method, p, n);
        }
        for (; i < n; i++) {
            uint32_t m = noninc ? method : method + i * 4;
            if (m < 0x2000u && s_plain[m / 4]) {
                shadow_write(m, p[i]);
                if (unhandled_on())
                    note_unhandled(m, p[i]);
                s_ws.plain++;
            } else {
                rest_note(0, m);
                nv2a_pb_exec_method(0, m, p[i]);
            }
        }
        return;
    }
    for (; i < n; i++) {
        rest_note(subch, noninc ? method : method + i * 4);
        nv2a_pb_exec_method(subch, noninc ? method : method + i * 4, p[i]);
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    extern int xbox_log_quiet(void);    /* kernel_bridge.c: RECOMP_QUIET */

    if (xbox_log_quiet())
        return;
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u, %u indices\n", s_gpu.draws, s_gpu.verts);

    fprintf(stderr, "[GPU] %u triangles sent to the back end; draw surface 0x%08X -> 0x%08X,"
                    " clear surface 0x%08X -> 0x%08X\n", s_gpu.tris_drawn, s_gpu.drawn_offset,
            dma_resolve(s_gpu.drawn_offset), s_gpu.color_offset, dma_resolve(s_gpu.color_offset));

    if (getenv("RECOMP_TEX_STATE")) {
        uint32_t k;
        for (k = 0; k < sizeof s_tex_set / sizeof s_tex_set[0]; k++)
            if (s_tex_set[k])
                fprintf(stderr, "  [TEX] 0x%04X = 0x%08X\n",
                        (unsigned)(NV_TEX_FIRST + k * 4), s_tex_reg[k]);
    }

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm.
     *
     * RECOMP_PB_UNHANDLED_ALL lists every one instead. Ten is the right
     * default -- the tail is a long list of state registers nobody needs to
     * read -- but when a title stops and the question is which method it
     * stopped on, the answer is as likely to be the one seen twice as the
     * one seen a thousand times, and ten hides it. */
    {
        int shown = getenv("RECOMP_PB_UNHANDLED_ALL") ? s_unhandled_count : 10;
    for (i = 0; i < shown && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        {
            /* The value as well as the count. A method nobody decoded is
             * a guess until you see what it carried: screen coordinates,
             * a 0..1 texcoord and a packed colour are told apart at a
             * glance, and that is what says which vertex encoding a title
             * is using. */
            union { uint32_t u; float f; } v;
            v.u = s_unhandled[i].last_param;
            fprintf(stderr, "  [GPU]   0x%04X x%-8u last=0x%08X (%.4f)\n",
                    s_unhandled[i].method, s_unhandled[i].count,
                    v.u, v.f);
        }
    }
    }
    for (i = 0; i < s_unhandled_count; i++)       /* the sort moved them */
        s_unhandled_slot[(s_unhandled[i].method & 0x1FFC) / 4] = (uint16_t)(i + 1);
    fflush(stderr);
}
