/* dk_gen.c -- the GLSL of the deko3d shader pack's dedicated shaders, shared
 * by tools/dk_pack (on the PC, compiled with the uam tool) and the console's
 * own compiler (dk_compile.c, uam as a library), so both make the same
 * shader from the same key.
 *
 * A vertex program becomes dk_shadergen.c's program body inside deko3d's
 * vertex interface (nv2a_vs.glsl's inputs, uniform blocks and varyings); a
 * combiner setup is dk_shadergen.c's fragment shader, in two builds: with
 * the title's alpha test, and with it compiled out (NV2A_ALPHA_TEST 0),
 * which keeps the GPU's early depth test for draws with the test off.
 *
 * The generator is compiled into this file, so tools/dk_pack builds from
 * dk_gen.c alone. */
#include "dk_shadergen.c"

#include <stdio.h>
#include <string.h>

#include "dk_gen.h"

/* The deko3d vertex interface: nv2a_vs.glsl's inputs, uniform blocks and
 * varyings, with the helpers and register file the program body uses. */
static const char s_vs_prelude[] =
    "#version 460\n"
    "layout(location = 0) in vec4 in_v0;\n"
    "layout(location = 1) in vec4 in_v1;\n"
    "layout(location = 2) in vec4 in_v2;\n"
    "layout(location = 3) in vec4 in_v3;\n"
    "layout(location = 4) in vec4 in_v4;\n"
    "layout(location = 5) in vec4 in_v5;\n"
    "layout(location = 6) in vec4 in_v6;\n"
    "layout(location = 7) in vec4 in_v7;\n"
    "layout(location = 8) in vec4 in_v8;\n"
    "layout(location = 9) in vec4 in_v9;\n"
    "layout(location = 10) in vec4 in_v10;\n"
    "layout(location = 11) in vec4 in_v11;\n"
    "layout(location = 12) in vec4 in_v12;\n"
    "layout(location = 13) in vec4 in_v13;\n"
    "layout(location = 14) in vec4 in_v14;\n"
    "layout(location = 15) in vec4 in_v15;\n"
    "layout(std140, binding = 0) uniform VsConsts { vec4 c[192]; };\n"
    "layout(std140, binding = 2) uniform VsDraw {\n"
    "    vec4 u_surf; vec4 u_m[4]; vec4 u_vpoff; vec4 u_aa4;\n"
    "    uvec4 u_mode; uvec4 u_flags; vec4 u_attr[16];\n"
    "    vec4 u_fogp; uvec4 u_fogm; vec4 u_fogpl;\n"
    "};\n"
    /* All eight clip distances, as nv2a_vs.glsl writes them: deko3d enables
     * them all, and unwritten ones clipped shapes at random. */
    "out float gl_ClipDistance[8];\n"
    /* Positions computed as written, never fused: the same ground drawn
     * through different programs must land on the same depth (nv2a_vs.glsl). */
    "invariant gl_Position;\n"
    "layout(location = 0) out vec4 vD0; layout(location = 1) out vec4 vD1;\n"
    "layout(location = 2) out vec4 vT0; layout(location = 3) out vec4 vT1;\n"
    "layout(location = 4) out vec4 vT2; layout(location = 5) out vec4 vT3;\n"
    "layout(location = 6) out float vFog;\n"
    "vec4 v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15;\n"
    "vec4 R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11;\n"
    "vec4 oPos, o1, o2, oD0, oD1, oFog, oPts, oB0, oB1, oT0, oT1, oT2, oT3;\n"
    "float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }\n"
    "float nv2a_rcc(float x) {\n"
    "    float y = nv2a_rcp(x); float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);\n"
    "    return y < 0.0 ? -a : a;\n"
    "}\n"
    "vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "vec4 nv2a_log(float x) {\n"
    "    float a = abs(x);\n"
    "    if (a == 0.0) return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);\n"
    "    float e = floor(log2(a));\n"
    "    return vec4(e, a / exp2(e), log2(a), 1.0);\n"
    "}\n"
    "vec4 nv2a_lit(vec4 s) {\n"
    "    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);\n"
    "    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);\n"
    "}\n"
    DK_SHADERGEN_MUL_GLSL
    DK_SHADERGEN_SNAP_GLSL
    /* The fog unit: oFog.x is a distance, turned into the factor by
     * SET_FOG_MODE and SET_FOG_PARAMS (nv2a_vs.glsl, nv2a_fog). */
    "float nv2a_fog(float d) {\n"
    "    uint mode = u_fogm.y; float f;\n"
    "    if (u_fogm.x == 0u) return 1.0;\n"
    "    if (mode == 0x800u || mode == 0x802u) {\n"
    "        if (mode == 0x800u && isinf(d)) d = 0.0;\n"
    "        f = u_fogp.x + exp2(d * u_fogp.y * 16.0) - 1.5;\n"
    "    } else if (mode == 0x801u || mode == 0x803u) {\n"
    "        f = u_fogp.x + exp2(-d * d * u_fogp.y * u_fogp.y * 32.0) - 1.5;\n"
    "    } else {\n"
    "        if (isinf(d)) d = 0.0;\n"
    "        f = u_fogp.x + d * u_fogp.y - 1.0;\n"
    "    }\n"
    "    if (mode >= 0x802u && mode <= 0x804u) f = abs(f);\n"
    "    return f;\n"
    "}\n"
    "vec4 nv2a_clip(vec3 sw, float w) {\n"
    "    float z = sw.z * u_surf.z;\n"
    /* u_aa4.zw: the pixel-centre shift at an internal resolution
     * (nv2a_vs.glsl, nv2a_clip). */
    "    sw.xy = nv2a_snap(sw.xy, w) + u_aa4.zw * w;\n"
    "    return vec4(sw.x * u_surf.x - w, (sw.y * u_surf.y - w) * u_surf.w,\n"
    /* Depth as the title computed it; the rasterizer clamps it per pixel
     * (nv2a_vs.glsl, nv2a_clip). Bit 2: the old per-vertex clamp. */
    "                (u_flags.x & 4u) != 0u && w > 0.0 ? clamp(z, 0.0, w) : z, w);\n"
    "}\n";

static const char s_vs_main[] =
    "void main() {\n"
    "    for (int i = 0; i < 8; i++) gl_ClipDistance[i] = 1.0;\n"
    "    uint p = u_mode.y;\n"
    "    v0 = (p & 0x0001u) != 0u ? in_v0 : u_attr[0];\n"
    "    v1 = (p & 0x0002u) != 0u ? in_v1 : u_attr[1];\n"
    "    v2 = (p & 0x0004u) != 0u ? in_v2 : u_attr[2];\n"
    "    v3 = (p & 0x0008u) != 0u ? in_v3 : u_attr[3];\n"
    "    v4 = (p & 0x0010u) != 0u ? in_v4 : u_attr[4];\n"
    "    v5 = (p & 0x0020u) != 0u ? in_v5 : u_attr[5];\n"
    "    v6 = (p & 0x0040u) != 0u ? in_v6 : u_attr[6];\n"
    "    v7 = (p & 0x0080u) != 0u ? in_v7 : u_attr[7];\n"
    "    v8 = (p & 0x0100u) != 0u ? in_v8 : u_attr[8];\n"
    "    v9 = (p & 0x0200u) != 0u ? in_v9 : u_attr[9];\n"
    "    v10 = (p & 0x0400u) != 0u ? in_v10 : u_attr[10];\n"
    "    v11 = (p & 0x0800u) != 0u ? in_v11 : u_attr[11];\n"
    "    v12 = (p & 0x1000u) != 0u ? in_v12 : u_attr[12];\n"
    "    v13 = (p & 0x2000u) != 0u ? in_v13 : u_attr[13];\n"
    "    v14 = (p & 0x4000u) != 0u ? in_v14 : u_attr[14];\n"
    "    v15 = (p & 0x8000u) != 0u ? in_v15 : u_attr[15];\n"
    "    R0 = R1 = R2 = R3 = R4 = R5 = R6 = R7 = R8 = R9 = R10 = R11 = vec4(0.0);\n"
    "    oPos = o1 = o2 = oFog = oPts = vec4(0.0);\n"
    "    oD0 = oD1 = oB0 = oB1 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    oT0 = oT1 = oT2 = oT3 = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    nv2a_program();\n"
    "    float w = abs(oPos.w) < 1e-6 ? 1e-6 : oPos.w;\n"
    "    vec3 s = vec3(oPos.xy * u_aa4.xy, oPos.z);\n"
    "    gl_Position = nv2a_clip(s * w, w);\n"
    "    vD0 = clamp(oD0, 0.0, 1.0); vD1 = clamp(oD1, 0.0, 1.0);\n"
    "    vT0 = oT0; vT1 = oT1; vT2 = oT2; vT3 = oT3; vFog = nv2a_fog(oFog.x);\n"
    "}\n";

int dk_gen_vs(uint32_t start, uint32_t n, const uint32_t *words, char *src, size_t cap)
{
    static uint32_t prog[136][4];
    static char body[256 * 1024];
    int len;

    if (start >= 136 || !n || start + n > 136)
        return -1;
    memset(prog, 0, sizeof prog);
    memcpy(prog[start], words, (size_t)n * 16);
    if (dk_shadergen_vertex((const uint32_t (*)[4])prog, start + n, start, body, sizeof body) < 0)
        return -1;
    len = snprintf(src, cap, "%s%s%s", s_vs_prelude, body, s_vs_main);
    return len < 0 || (size_t)len >= cap ? -1 : len;
}

int dk_gen_fs(const uint32_t *words, int alpha_test, char *src, size_t cap)
{
    static char gen[64 * 1024];
    DkCombinerKey k;
    const char *nl;
    int len;

    memcpy(&k, words, sizeof k);
    if (dk_shadergen_fragment(&k, gen, sizeof gen) < 0)
        return -1;
    if (alpha_test) {
        len = snprintf(src, cap, "%s", gen);
    } else {
        nl = strchr(gen, '\n');         /* after "#version" */
        if (!nl)
            return -1;
        len = snprintf(src, cap, "%.*s#define NV2A_ALPHA_TEST 0\n%s",
                       (int)(nl + 1 - gen), gen, nl + 1);
    }
    return len < 0 || (size_t)len >= cap ? -1 : len;
}
