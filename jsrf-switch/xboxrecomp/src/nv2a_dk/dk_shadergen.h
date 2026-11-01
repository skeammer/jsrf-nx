/* dk_shadergen.h -- the NV2A's vertex programs and register combiners as GLSL
 * for the deko3d renderer (see dk_shadergen.c). */
#ifndef DK_SHADERGEN_H
#define DK_SHADERGEN_H

#include <stddef.h>
#include <stdint.h>

/* A register-combiner setup: every register that changes the generated
 * code, and nothing that does not (factors, the fog colour and the alpha
 * reference are uniforms). 37 words, compared and hashed as they are
 * (nv2a_dk_pack.h: DK_FS_KEY_WORDS), so fill every field. */
typedef struct {
    uint32_t color_icw[8], color_ocw[8];   /* SET_COMBINER_COLOR_ICW / _OCW */
    uint32_t alpha_icw[8], alpha_ocw[8];   /* SET_COMBINER_ALPHA_ICW / _OCW */
    uint32_t control;                      /* SET_COMBINER_CONTROL */
    uint32_t final0, final1;               /* SET_COMBINER_SPECULAR_FOG_CW0 / CW1 */
    uint32_t shader_program;               /* SET_SHADER_STAGE_PROGRAM */
    uint32_t other_input;                  /* SET_SHADER_OTHER_STAGE_INPUT */
} DkCombinerKey;

/* `void nv2a_program(void)`: the vertex program from slot `start` to its
 * FINAL instruction (or `slots`), on the register file the vertex shader
 * declares (v0..v15, R0..R11, oPos..oT3, c[192]). Returns the length
 * written, -1 when `cap` is too small. */
int dk_shadergen_vertex(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                        char *out, size_t cap);

/* The whole fragment shader for a combiner setup, against nv2a_fs.glsl's
 * interface (FsDraw at binding 0, samplers t0..t3). The alpha test sits
 * behind NV2A_ALPHA_TEST (defined 1 unless the source defines it first).
 * Returns the length written, -1 when `cap` is too small. */
int dk_shadergen_fragment(const DkCombinerKey *k, char *out, size_t cap);

/* GLSL helpers the vertex shader needs before the program body, as string
 * literals so a prelude can be put together at compile time.
 *
 * DK_SHADERGEN_MUL_GLSL: the NV2A's multiplier, where 0 times anything is 0
 * -- infinity and NaN included -- unlike IEEE, where 0 * inf is a NaN and a
 * single NaN in oPos loses the triangle. The product is replaced, not
 * computed differently (mix with a bvec selects without arithmetic), so no
 * NaN gets through. Used by MUL, MAD, DP3, DP4 and DST unless
 * RECOMP_VSH_MUL_ZERO=0.
 *
 * DK_SHADERGEN_SNAP_GLSL: the rasterizer takes screen positions as fixed
 * point with 4 fraction bits, truncated; Xbox D3D's +0.53125 viewport bias
 * relies on that to cover row and column 0 with a full-screen pass. */
#define DK_SHADERGEN_MUL_GLSL \
    "vec4 nv2a_mul(vec4 a, vec4 b) {\n" \
    "    bvec4 either_zero = bvec4(vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0))));\n" \
    "    return mix(a * b, vec4(0.0), either_zero);\n" \
    "}\n" \
    "float nv2a_dp3(vec4 a, vec4 b) { vec4 p = nv2a_mul(a, b); return p.x + p.y + p.z; }\n" \
    "float nv2a_dp4(vec4 a, vec4 b) { vec4 p = nv2a_mul(a, b); return p.x + p.y + p.z + p.w; }\n"

#define DK_SHADERGEN_SNAP_GLSL \
    "vec2 nv2a_snap(vec2 pos, float w) {\n" \
    "    return w > 0.0 ? trunc(pos / w * 16.0) / 16.0 * w : pos;\n" \
    "}\n"

#endif
