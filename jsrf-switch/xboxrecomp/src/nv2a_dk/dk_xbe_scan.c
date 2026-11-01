/* dk_xbe_scan.c -- every shader a title ships in its XBE, as shader-pack
 * keys. Shared by tools/dk_scan (PC) and the console's compiler
 * (dk_compile.c), so both build the same pack.
 *
 *   vertex shaders   u16 type 0x2078 ("x "; 0x7778 read/write), u16 count,
 *                    then count x 4 words of NV2A microcode, the last one
 *                    FINAL (bit 0 of its fourth word) and no other. The
 *                    console's key is (start slot, count, the words); JSRF
 *                    loads them all at slot 0. On 20 Oct 2026: 123 in
 *                    JSRF's XBE, all 29 of the title's recorded ones among
 *                    them (2 more are D3D's own, without a header).
 *   pixel shaders    'PSB0', then a D3DPIXELSHADERDEF (60 words: alpha
 *                    inputs 0-7, final ABCD 8, EFG 9, constants 10-25,
 *                    alpha outputs 26-33, RGB inputs 34-41, compare 42,
 *                    final constants 43-44, RGB outputs 45-52, combiner
 *                    count 53, texture modes 54, dot mapping 55, input
 *                    texture 56, ...). D3D changes two things on the way to
 *                    the GPU, both seen in JSRF's recorded keys: a zero final
 *                    combiner becomes 0x0000000C / 0x00001C80 (r0, r0.a),
 *                    and a stage with no texture bound gets mode 0 -- so each
 *                    definition yields one setup per subset of its textured
 *                    stages. All 6 of JSRF's recorded pixel-shader setups are
 *                    among them.
 *
 * Combiner setups D3D builds from texture-stage states are made at run time
 * and are in no file. */
#include <string.h>

#include "dk_xbe_scan.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void scan_vs(const uint8_t *d, size_t size, dk_scan_vs_fn fn, void *ctx)
{
    size_t o;
    for (o = 0; o + 4 <= size; o += 4) {
        uint16_t type = rd16(d + o), n = rd16(d + o + 2);
        uint32_t w[136 * 4], j, finals = 0;
        if ((type != 0x2078 && type != 0x7778) || n < 1 || n > 136 || o + 4 + (size_t)n * 16 > size)
            continue;
        for (j = 0; j < (uint32_t)n * 4; j++)
            w[j] = rd32(d + o + 4 + j * 4);
        for (j = 0; j < n; j++)
            finals += w[j * 4 + 3] & 1;
        if (!(w[(n - 1) * 4 + 3] & 1) || finals != 1)
            continue;                    /* not a program: FINAL ends it, once */
        fn(ctx, 0, n, w);                /* start slot 0 */
    }
}

/* One D3DPIXELSHADERDEF as the GPU sees it with the stages in `bound` (bit
 * per stage) textured, in Nv2aPshKey's word order. */
static void emit_fs(const uint32_t *def, unsigned bound, dk_scan_fs_fn fn, void *ctx)
{
    uint32_t k[37], modes = 0, s;
    for (s = 0; s < 4; s++)
        if (bound & (1u << s))
            modes |= def[54] & (0x1Fu << (s * 5));
    memcpy(k, def + 34, 32);             /* RGB inputs */
    memcpy(k + 8, def + 45, 32);         /* RGB outputs */
    memcpy(k + 16, def + 0, 32);         /* alpha inputs */
    memcpy(k + 24, def + 26, 32);        /* alpha outputs */
    k[32] = def[53];                     /* combiner count -> control */
    k[33] = def[8] | def[9] ? def[8] : 0x0000000Cu;
    k[34] = def[8] | def[9] ? def[9] : 0x00001C80u;
    k[35] = modes;
    k[36] = def[56];
    fn(ctx, k);
}

static void scan_ps(const uint8_t *d, size_t size, dk_scan_fs_fn fn, void *ctx)
{
    size_t o;
    for (o = 0; o + 4 + 60 * 4 <= size; o += 4) {
        uint32_t def[60], j, textured = 0;
        unsigned sub;
        if (memcmp(d + o, "PSB0", 4))
            continue;
        for (j = 0; j < 60; j++)
            def[j] = rd32(d + o + 4 + j * 4);
        if ((def[53] & 0xFF) < 1 || (def[53] & 0xFF) > 8 || (def[53] & ~0x111FFu) || (def[54] >> 20))
            continue;                    /* not a definition */
        for (j = 0; j < 4; j++)
            if ((def[54] >> (j * 5)) & 0x1F)
                textured |= 1u << j;
        for (sub = 0; sub < 16; sub++)   /* every subset of the textured stages */
            if (!(sub & ~textured))
                emit_fs(def, sub, fn, ctx);
    }
}

void dk_scan_xbe(const uint8_t *d, size_t size, dk_scan_vs_fn vs, dk_scan_fs_fn fs, void *ctx)
{
    if (vs)
        scan_vs(d, size, vs, ctx);
    if (fs)
        scan_ps(d, size, fs, ctx);
}
