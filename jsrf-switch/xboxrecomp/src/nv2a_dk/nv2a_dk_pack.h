/* nv2a_dk_pack.h -- the deko3d renderer's shader pack and key files, shared
 * by the console (nv2a_dk.c, dk_compile.c) and the PC tool that builds packs
 * (tools/dk_pack/dk_pack.c).
 *
 * The ubershaders draw everything, but interpreting the NV2A program per
 * vertex and the combiners per pixel is slow. So each vertex program and
 * combiner setup gets a dedicated shader -- the GL renderer's generators
 * (dk_shadergen.c) with deko3d's interface (dk_gen.c), compiled by uam
 * -- kept in dk_shaders.pak, which the console loads at start and prefers
 * over the ubershaders. The console builds the pack itself (dk_compile.c,
 * uam linked in): every shader in the game's XBE on the first start, and
 * whatever a draw meets that the pack lacks, appended as it goes; until a
 * setup's shader is built, the ubershader draws it. dk_pack builds the same
 * pack on a PC. The console also records what it meets (dk_keys.bin, next
 * to the NRO), which both read.
 *
 * Key file: a sequence of records, little-endian u32 words.
 *   vertex program:  'VSK1', start slot, n slots, n x 4 program words
 *                    (the slots from start through the FINAL one)
 *   combiners:       'FSK1', 37, the 37 words of an Nv2aPshKey
 *
 * Pack: 'DKPK', version 1, entry count; then per entry:
 *   u32 kind (0 vertex, 1 fragment), u32 dksh size (a multiple of 256),
 *   u64 key, the DKSH module.
 * or the generator's stamp (DK_PACK_STAMP, first): u32 2, u32 0, u64 the
 *   stamp. A reader skips kinds it does not know.
 *
 * The stamp is DK_GEN_STAMP, a hash of the generators' sources (CMake and
 * switch/dk_pack.sh work it out the same way: the SHA-1s of the files in
 * DK_GEN_FILES, in that order, as hex, and the first 16 hex digits of the
 * SHA-1 of that string). A pack with another stamp, or none, was made by
 * another version of the generators -- its shaders may hold a bug since
 * fixed -- so the console builds it anew (dk_compile.c) and dk_pack starts
 * over. 0 (a build without the stamp) takes any pack.
 */
#ifndef NV2A_DK_PACK_H
#define NV2A_DK_PACK_H

#include <stdint.h>

#define DK_KEY_VS     0x314B5356u       /* "VSK1" */
#define DK_KEY_FS     0x314B5346u       /* "FSK1" */
/* The pack's second build of a combiner setup, under its own key (the same
 * words hashed with this kind; never a key-file record): the alpha test
 * compiled out (NV2A_ALPHA_TEST 0). A shader that can discard loses the
 * GPU's early depth test, so every hidden pixel ran the whole combiner;
 * the renderer takes this one whenever the title has the test off. */
#define DK_KEY_FS_NOAT 0x304B5346u      /* "FSK0" */
#define DK_PACK_MAGIC 0x4B504B44u       /* "DKPK" */
#define DK_PACK_VERSION 1u
#define DK_PACK_STAMP 2u                /* entry kind: the generator's stamp */
#ifndef DK_GEN_STAMP
#define DK_GEN_STAMP 0ull
#endif
#define DK_FS_KEY_WORDS 37u             /* sizeof(DkCombinerKey) / 4 (dk_shadergen.h) */

/* FNV-1a, 64-bit, over little-endian words. Kind is folded in so a vertex
 * and a fragment key never collide. */
static inline uint64_t dk_key_hash(uint32_t kind, const uint32_t *w, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull ^ kind;
    uint32_t i, b;
    for (i = 0; i < n; i++)
        for (b = 0; b < 32; b += 8) {
            h ^= (w[i] >> b) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    return h ? h : 1;
}

/* A vertex program's key: start, slot count, then the slots' words. */
static inline uint64_t dk_vs_key(uint32_t start, uint32_t n, const uint32_t (*prog)[4])
{
    uint32_t hdr[2] = { start, n };
    uint64_t h = dk_key_hash(DK_KEY_VS, hdr, 2);
    uint32_t i, j, b;
    for (i = 0; i < n; i++)
        for (j = 0; j < 4; j++)
            for (b = 0; b < 32; b += 8) {
                h ^= (prog[start + i][j] >> b) & 0xFFu;
                h *= 0x100000001B3ull;
            }
    return h ? h : 1;
}

#endif
