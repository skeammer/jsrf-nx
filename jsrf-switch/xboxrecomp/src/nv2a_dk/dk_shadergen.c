/*
 * dk_shadergen.c -- NV2A shaders as GLSL, for the deko3d renderer's
 * dedicated shaders (dk_gen.c puts them inside deko3d's interfaces; the
 * console compiles them with uam, dk_compile.c, and tools/dk_pack builds
 * the same ones on the PC).
 *
 * VERTEX PROGRAMS. An NV2A vertex program is up to 136 slots of 128-bit
 * instructions, each a MAC operation and an ILU operation issued together
 * over three shared source operands. The field layout is the one xemu
 * documents (vsh.c). The translation keeps the executor's interpreter
 * (nv2a_pb_exec.c, vp_run) and the ubershader (nv2a_vs.glsl) as its
 * references: both units read their sources before either writes, an ILU
 * paired with a MAC writes R1, R12 is oPos, and a0 indexes the constants.
 *
 * REGISTER COMBINERS. The fragment shader is the texture stages (each by
 * its SET_SHADER_STAGE_PROGRAM mode), up to eight general combiner stages
 * (RGB and alpha halves, each A*B and C*D then their sum or a mux), and the
 * final combiner (A*B + (1-A)*C + D for colour, G for alpha), then the alpha
 * test. Inputs come through the eight NV2A input mappings; outputs through
 * scale/bias and a clamp to [-1, 1].
 */
#include "dk_shadergen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Text output ─────────────────────────────────────────────────────── */

typedef struct {
    char  *cursor;
    size_t room;
    int    overflowed;
} Text;

static void put(Text *t, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (t->overflowed)
        return;
    va_start(ap, fmt);
    n = vsnprintf(t->cursor, t->room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= t->room) {
        t->overflowed = 1;
        return;
    }
    t->cursor += n;
    t->room -= (size_t)n;
}

static int finish(const Text *t, size_t cap)
{
    return t->overflowed ? -1 : (int)(cap - t->room);
}

/* ═══ Vertex programs ════════════════════════════════════════════════════ */

/* `width` bits of instruction word `word`, from bit `lsb`. */
static uint32_t field(const uint32_t *ins, int word, int lsb, int width)
{
    return (ins[word] >> lsb) & ((1u << width) - 1u);
}

/* Instruction fields (word, lsb, width). */
#define F_ILU          1, 25, 3
#define F_MAC          1, 21, 4
#define F_CONST_INDEX  1, 13, 8
#define F_INPUT_INDEX  1,  9, 4
#define F_A_NEGATE     1,  8, 1
#define F_A_SWIZZLE    1,  0, 8
#define F_A_REG        2, 28, 4
#define F_A_MUX        2, 26, 2
#define F_B_NEGATE     2, 25, 1
#define F_B_SWIZZLE    2, 17, 8
#define F_B_REG        2, 13, 4
#define F_B_MUX        2, 11, 2
#define F_C_NEGATE     2, 10, 1
#define F_C_SWIZZLE    2,  2, 8
#define F_C_REG_HIGH   2,  0, 2
#define F_C_REG_LOW    3, 30, 2
#define F_C_MUX        3, 28, 2
#define F_MAC_MASK     3, 24, 4
#define F_TEMP_DEST    3, 20, 4
#define F_ILU_MASK     3, 16, 4
#define F_OUT_MASK     3, 12, 4
#define F_OUT_IS_REG   3, 11, 1       /* 1: output register, 0: constant */
#define F_OUT_ADDRESS  3,  3, 8
#define F_OUT_FROM_ILU 3,  2, 1
#define F_RELATIVE     3,  1, 1       /* constant index + a0 */
#define F_FINAL        3,  0, 1

enum { MUX_TEMP = 1, MUX_INPUT = 2, MUX_CONST = 3 };

static const char k_xyzw[] = "xyzw";

static const char *const k_out_regs[13] = {
    "oPos", "o1", "o2", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3",
};

static int multiply_like_nv2a(void)
{
    static int setting = -1;
    if (setting < 0) {
        const char *v = getenv("RECOMP_VSH_MUL_ZERO");
        setting = !(v && v[0] == '0' && v[1] == '\0');
    }
    return setting;
}

/* Does the program write the constant file? It then works on a copy. */
static int writes_constants(const uint32_t (*prog)[4], uint32_t slots, uint32_t start)
{
    uint32_t pc;
    for (pc = start; pc < slots; pc++) {
        if (field(prog[pc], F_OUT_MASK) && !field(prog[pc], F_OUT_IS_REG))
            return 1;
        if (field(prog[pc], F_FINAL))
            break;
    }
    return 0;
}

/* Operand 0 (A), 1 (B) or 2 (C) as a vec4 expression. */
static void operand(Text *t, const uint32_t *ins, int which, int local_consts)
{
    uint32_t mux, reg, neg, swz;
    char sel[5];
    int k;

    if (which == 0) {
        mux = field(ins, F_A_MUX); reg = field(ins, F_A_REG);
        neg = field(ins, F_A_NEGATE); swz = field(ins, F_A_SWIZZLE);
    } else if (which == 1) {
        mux = field(ins, F_B_MUX); reg = field(ins, F_B_REG);
        neg = field(ins, F_B_NEGATE); swz = field(ins, F_B_SWIZZLE);
    } else {
        mux = field(ins, F_C_MUX);
        reg = field(ins, F_C_REG_HIGH) << 2 | field(ins, F_C_REG_LOW);
        neg = field(ins, F_C_NEGATE); swz = field(ins, F_C_SWIZZLE);
    }
    for (k = 0; k < 4; k++)
        sel[k] = k_xyzw[(swz >> (6 - 2 * k)) & 3];
    sel[4] = '\0';

    if (neg)
        put(t, "-");
    switch (mux) {
    case MUX_TEMP:
        if (reg < 12)       put(t, "R%u.%s", reg, sel);
        else if (reg == 12) put(t, "oPos.%s", sel);
        else                put(t, "vec4(0.0).%s", sel);
        break;
    case MUX_INPUT:
        put(t, "v%u.%s", field(ins, F_INPUT_INDEX), sel);
        break;
    case MUX_CONST: {
        uint32_t idx = field(ins, F_CONST_INDEX);
        const char *file = local_consts ? "cl" : "c";
        if (field(ins, F_RELATIVE))
            put(t, "%s[clamp(%u + a0, 0, 191)].%s", file, idx, sel);
        else
            put(t, "%s[%u].%s", file, idx > 191 ? 191u : idx, sel);
        break;
    }
    default:
        put(t, "vec4(0.0).%s", sel);
        break;
    }
}

/* `dest.mask = (value).mask;` for the components in a write mask (bit 3 x
 * .. bit 0 w); nothing for an empty mask. */
static void store(Text *t, const char *dest, uint32_t mask, const char *value)
{
    char m[6];
    int n = 0, k;

    if (!mask)
        return;
    m[n++] = '.';
    for (k = 0; k < 4; k++)
        if (mask & (8u >> k))
            m[n++] = k_xyzw[k];
    m[n] = '\0';
    put(t, "        %s%s = (%s)%s;\n", dest, m, value, m);
}

/* R0..R11, or oPos for 12; NULL above. */
static const char *temp_name(uint32_t reg, char buf[8])
{
    if (reg > 12)
        return NULL;
    if (reg == 12)
        return "oPos";
    snprintf(buf, 8, "R%u", reg);
    return buf;
}

static void mac_op(Text *t, uint32_t op, int nv2a_mul)
{
    static const char *const ieee[] = {
        NULL, "m = A;", "m = A * B;", "m = A + C;", "m = A * B + C;",
        "m = vec4(dot(A.xyz, B.xyz));", "m = vec4(dot(A.xyz, B.xyz) + B.w);",
        "m = vec4(dot(A, B));", "m = vec4(1.0, A.y * B.y, A.z, B.w);",
        "m = min(A, B);", "m = max(A, B);",
        "m = vec4(lessThan(A, B));", "m = vec4(greaterThanEqual(A, B));",
    };
    static const char *const nv2a[] = {
        NULL, "m = A;", "m = nv2a_mul(A, B);", "m = A + C;", "m = nv2a_mul(A, B) + C;",
        "m = vec4(nv2a_dp3(A, B));", "m = vec4(nv2a_dp3(A, B) + B.w);",
        "m = vec4(nv2a_dp4(A, B));", "m = vec4(1.0, nv2a_mul(A, B).y, A.z, B.w);",
        "m = min(A, B);", "m = max(A, B);",
        "m = vec4(lessThan(A, B));", "m = vec4(greaterThanEqual(A, B));",
    };
    if (op >= 1 && op <= 12)
        put(t, "        %s\n", nv2a_mul ? nv2a[op] : ieee[op]);
}

static void ilu_op(Text *t, uint32_t op)
{
    static const char *const ops[] = {
        NULL, "l = C;", "l = vec4(nv2a_rcp(C.x));", "l = vec4(nv2a_rcc(C.x));",
        "l = vec4(nv2a_rcp(sqrt(abs(C.x))));", "l = nv2a_exp(C.x);",
        "l = nv2a_log(C.x);", "l = nv2a_lit(C);",
    };
    if (op >= 1 && op <= 7)
        put(t, "        %s\n", ops[op]);
}

int dk_shadergen_vertex(const uint32_t (*prog)[4], uint32_t slots, uint32_t start,
                        char *out, size_t cap)
{
    Text t = { out, cap, 0 };
    int local = writes_constants(prog, slots, start);
    int nv2a_mul = multiply_like_nv2a();
    uint32_t pc;

    put(&t, "void nv2a_program(void)\n{\n    int a0 = 0;\n");
    if (local)
        put(&t, "    vec4 cl[192];\n    for (int i = 0; i < 192; i++) cl[i] = c[i];\n");

    for (pc = start; pc < slots; pc++) {
        const uint32_t *ins = prog[pc];
        uint32_t mac = field(ins, F_MAC), ilu = field(ins, F_ILU);
        uint32_t outmask = field(ins, F_OUT_MASK);
        char name[8];
        const char *dest;

        put(&t, "    { /* slot %u */\n        vec4 A = ", pc);
        operand(&t, ins, 0, local);
        put(&t, ";\n        vec4 B = ");
        operand(&t, ins, 1, local);
        put(&t, ";\n        vec4 C = ");
        operand(&t, ins, 2, local);
        put(&t, ";\n        vec4 m = vec4(0.0), l = vec4(0.0);\n");
        mac_op(&t, mac, nv2a_mul);
        ilu_op(&t, ilu);

        /* ARL loads the address register instead of writing a temporary. */
        if (mac == 13)
            put(&t, "        a0 = int(floor(A.x + 0.001));\n");
        else if (mac && (dest = temp_name(field(ins, F_TEMP_DEST), name)) != NULL)
            store(&t, dest, field(ins, F_MAC_MASK), "m");
        /* An ILU issued with a MAC writes R1. */
        if (ilu && (dest = temp_name(mac ? 1u : field(ins, F_TEMP_DEST), name)) != NULL)
            store(&t, dest, field(ins, F_ILU_MASK), "l");

        if (outmask) {
            const char *value = field(ins, F_OUT_FROM_ILU) ? "l" : "m";
            uint32_t addr = field(ins, F_OUT_ADDRESS);
            if (field(ins, F_OUT_IS_REG)) {
                if (addr < 13)
                    store(&t, k_out_regs[addr], outmask, value);
            } else {
                char slot[48];
                if (field(ins, F_RELATIVE))
                    snprintf(slot, sizeof slot, "cl[clamp(%u + a0, 0, 191)]", addr);
                else
                    snprintf(slot, sizeof slot, "cl[%u]", addr > 191 ? 191u : addr);
                store(&t, slot, outmask, value);
            }
        }
        put(&t, "    }\n");
        if (field(ins, F_FINAL))
            break;
    }
    put(&t, "}\n");
    return finish(&t, cap);
}

/* ═══ Register combiners ═════════════════════════════════════════════════ */

/* Register codes of a combiner input or output. */
enum {
    REG_ZERO = 0, REG_C0 = 1, REG_C1 = 2, REG_FOG = 3, REG_V0 = 4, REG_V1 = 5,
    REG_T0 = 8, REG_R0 = 12, REG_R1 = 13, REG_V1R0 = 14, REG_EF = 15,
};

/* A general stage's register as GLSL: its own C0/C1 when the control
 * register says the factors are per stage, stage 0's otherwise. */
static const char *reg_glsl(uint32_t reg, int stage, const DkCombinerKey *k, char buf[16])
{
    if (reg == REG_C0) {
        snprintf(buf, 16, "u_c0[%d]", (k->control & (1u << 12)) ? stage : 0);
        return buf;
    }
    if (reg == REG_C1) {
        snprintf(buf, 16, "u_c1[%d]", (k->control & (1u << 16)) ? stage : 0);
        return buf;
    }
    if (reg >= REG_T0 && reg < REG_T0 + 4) {
        snprintf(buf, 16, "T%u", reg - REG_T0);
        return buf;
    }
    switch (reg) {
    case REG_FOG:  return "FOG";
    case REG_V0:   return "V0";
    case REG_V1:   return "V1";
    case REG_R0:   return "R0";
    case REG_R1:   return "R1";
    case REG_V1R0: return "V1R0";
    case REG_EF:   return "EF";
    default:       return "ZERO";
    }
}

/* Writable outputs: V0, V1, T0..T3, R0, R1. */
static const char *dest_glsl(uint32_t reg)
{
    static const char *const names[16] = {
        NULL, NULL, NULL, NULL, "V0", "V1", NULL, NULL,
        "T0", "T1", "T2", "T3", "R0", "R1", NULL, NULL,
    };
    return names[reg & 15];
}

/* One general-combiner input (an 8-bit field): register, which part, and
 * mapping. The alpha half reads a single float (the RGB bit picks blue). */
static void stage_input(Text *t, uint32_t b, int stage, int alpha_half, const DkCombinerKey *k)
{
    char buf[16], v[40];
    uint32_t reg = b & 15;
    const char *r = reg >= REG_V1R0 ? "ZERO" : reg_glsl(reg, stage, k, buf);
    int take_alpha = (b >> 4) & 1;

    if (alpha_half)
        snprintf(v, sizeof v, "%s.%s", r, take_alpha ? "a" : "b");
    else if (take_alpha)
        snprintf(v, sizeof v, "vec3(%s.a)", r);
    else
        snprintf(v, sizeof v, "%s.rgb", r);

    switch ((b >> 5) & 7) {
    case 0: put(t, "max(%s, 0.0)", v); break;                      /* unsigned identity */
    case 1: put(t, "(1.0 - clamp(%s, 0.0, 1.0))", v); break;       /* unsigned invert */
    case 2: put(t, "(2.0 * max(%s, 0.0) - 1.0)", v); break;        /* expand normal */
    case 3: put(t, "(1.0 - 2.0 * max(%s, 0.0))", v); break;        /* expand negate */
    case 4: put(t, "(max(%s, 0.0) - 0.5)", v); break;              /* half-bias normal */
    case 5: put(t, "(0.5 - max(%s, 0.0))", v); break;              /* half-bias negate */
    case 6: put(t, "(%s)", v); break;                              /* signed identity */
    default: put(t, "(-(%s))", v); break;                          /* signed negate */
    }
}

/* Scale/bias of a stage's outputs, around `x`. */
static void scaled(char *out, size_t n, uint32_t op, const char *x)
{
    switch (op) {
    case 1:  snprintf(out, n, "(%s - 0.5)", x); break;             /* bias */
    case 2:  snprintf(out, n, "(%s * 2.0)", x); break;             /* scale by 2 */
    case 3:  snprintf(out, n, "((%s - 0.5) * 2.0)", x); break;     /* bias, scale by 2 */
    case 4:  snprintf(out, n, "(%s * 4.0)", x); break;             /* scale by 4 */
    case 6:  snprintf(out, n, "(%s * 0.5)", x); break;             /* scale by 1/2 */
    default: snprintf(out, n, "(%s)", x); break;
    }
}

/* The mux's condition: R0.a's top bit, or its bottom bit as a byte. */
static const char *mux_test(const DkCombinerKey *k)
{
    return (k->control & (1u << 8)) ? "(R0.a >= 0.5)" : "((int(R0.a * 255.0 + 0.5) & 1) != 0)";
}

static void general_stage(Text *t, int i, const DkCombinerKey *k)
{
    uint32_t cin = k->color_icw[i], cout = k->color_ocw[i];
    uint32_t ain = k->alpha_icw[i], aout = k->alpha_ocw[i];
    static const char *const cvar[] = { "cA", "cB", "cC", "cD" };
    static const char *const avar[] = { "aA", "aB", "aC", "aD" };
    static const char *const cres[] = { "cAB", "cCD", "cS" };
    static const char *const ares[] = { "aAB", "aCD", "aS" };
    char e[64];
    const char *d;
    int j;

    put(t, "    { /* general combiner %d */\n", i);
    for (j = 0; j < 4; j++) {
        put(t, "        vec3 %s = ", cvar[j]);
        stage_input(t, cin >> (24 - 8 * j), i, 0, k);
        put(t, ";\n");
    }
    for (j = 0; j < 4; j++) {
        put(t, "        float %s = ", avar[j]);
        stage_input(t, ain >> (24 - 8 * j), i, 1, k);
        put(t, ";\n");
    }

    /* RGB: products (or dot products), then their sum or the mux. */
    put(t, "        vec3 cAB = %s;\n", (cout & (1u << 13)) ? "vec3(dot(cA, cB))" : "cA * cB");
    put(t, "        vec3 cCD = %s;\n", (cout & (1u << 12)) ? "vec3(dot(cC, cD))" : "cC * cD");
    if (cout & (1u << 14))
        put(t, "        vec3 cS = %s ? cCD : cAB;\n", mux_test(k));
    else
        put(t, "        vec3 cS = cAB + cCD;\n");
    /* Alpha: the same without dot products. */
    put(t, "        float aAB = aA * aB, aCD = aC * aD;\n");
    if (aout & (1u << 14))
        put(t, "        float aS = %s ? aCD : aAB;\n", mux_test(k));
    else
        put(t, "        float aS = aAB + aCD;\n");

    for (j = 0; j < 3; j++) {
        scaled(e, sizeof e, (cout >> 15) & 7, cres[j]);
        put(t, "        %s = clamp(%s, -1.0, 1.0);\n", cres[j], e);
    }
    for (j = 0; j < 3; j++) {
        scaled(e, sizeof e, (aout >> 15) & 7, ares[j]);
        put(t, "        %s = clamp(%s, -1.0, 1.0);\n", ares[j], e);
    }

    /* Destinations: AB in bits 7:4, CD in 3:0, the sum in 11:8. */
    if ((d = dest_glsl(cout >> 4)) != NULL) put(t, "        %s.rgb = cAB;\n", d);
    if ((d = dest_glsl(cout)) != NULL)      put(t, "        %s.rgb = cCD;\n", d);
    if ((d = dest_glsl(cout >> 8)) != NULL) put(t, "        %s.rgb = cS;\n", d);
    if ((d = dest_glsl(aout >> 4)) != NULL) put(t, "        %s.a = aAB;\n", d);
    if ((d = dest_glsl(aout)) != NULL)      put(t, "        %s.a = aCD;\n", d);
    if ((d = dest_glsl(aout >> 8)) != NULL) put(t, "        %s.a = aS;\n", d);
    /* Blue-to-alpha: the AB / CD destination's alpha becomes its blue. */
    if ((cout & (1u << 19)) && (d = dest_glsl(cout >> 4)) != NULL)
        put(t, "        %s.a = cAB.b;\n", d);
    if ((cout & (1u << 18)) && (d = dest_glsl(cout)) != NULL)
        put(t, "        %s.a = cCD.b;\n", d);
    put(t, "    }\n");
}

/* A final-combiner input: clamped to [0, 1], perhaps inverted. Its C0/C1 are
 * the final combiner's own factors (SPECULAR_FOG_FACTOR0/1). */
static void final_input(Text *t, uint32_t b, int alpha_only, const DkCombinerKey *k)
{
    char buf[16], v[40];
    uint32_t reg = b & 15;
    const char *r = reg == REG_C0 ? "u_fc0" : reg == REG_C1 ? "u_fc1" : reg_glsl(reg, 0, k, buf);
    int take_alpha = (b >> 4) & 1;

    if (alpha_only)
        snprintf(v, sizeof v, "%s.%s", r, take_alpha ? "a" : "b");
    else if (take_alpha)
        snprintf(v, sizeof v, "vec3(%s.a)", r);
    else
        snprintf(v, sizeof v, "%s.rgb", r);
    if ((b >> 5) & 1)
        put(t, "(1.0 - clamp(%s, 0.0, 1.0))", v);
    else
        put(t, "clamp(%s, 0.0, 1.0)", v);
}

/* Which earlier stage a dependent lookup reads (SHADER_OTHER_STAGE_INPUT):
 * stage 1 always T0; stage 2 by bits 19:16, stage 3 by bits 23:20. */
static int dependent_source(const DkCombinerKey *k, int stage)
{
    int s;
    if (stage <= 1)
        return 0;
    s = (int)((k->other_input >> (stage == 2 ? 16 : 20)) & 15);
    return s < stage ? s : stage - 1;
}

/* Texture stage `i` in SET_SHADER_STAGE_PROGRAM mode `mode`. */
static void texture_stage(Text *t, int i, uint32_t mode, const DkCombinerKey *k)
{
    switch (mode) {
    case 15:                    /* DEPENDENT_AR: indexed by an earlier stage's A, R */
    case 16:                    /* DEPENDENT_GB: by its G, B */
        if (i == 0)
            put(t, "    vec4 T%d = vec4(0.0);\n", i);
        else
            put(t, "    vec4 T%d = texture(t%d, T%d.%s);\n", i, i, dependent_source(k, i),
                mode == 15 ? "ar" : "gb");
        break;
    case 4:                     /* PASS_THROUGH: the coordinates themselves */
        put(t, "    vec4 T%d = clamp(vT%d, 0.0, 1.0);\n", i, i);
        break;
    case 5:                     /* CLIP_PLANE: drop the pixel where any is negative */
        put(t, "    if (any(lessThan(vT%d, vec4(0.0)))) discard;\n"
               "    vec4 T%d = vec4(0.0);\n", i, i);
        break;
    case 0:                     /* NONE */
    case 3:                     /* CUBE_MAP: not supported, reads 0 */
        put(t, "    vec4 T%d = vec4(0.0);\n", i);
        break;
    default:                    /* 2D (1: projective), and the rest sampled as 2D */
        put(t, "    vec2 tc%d = (abs(vT%d.w) > 1e-8 && %d == 1) ? vT%d.xy / vT%d.w : vT%d.xy;\n",
            i, i, mode == 1 ? 1 : 0, i, i, i);
        put(t, "    vec4 T%d = texture(t%d, tc%d * u_tscale[%d]);\n", i, i, i, i);
        break;
    }
}

/* nv2a_fs.glsl's interface: FsDraw (std140, binding 0; a vec2 array element
 * takes 16 bytes there too) and samplers at texture bindings 0..3. */
static const char k_fragment_head[] =
    "#version 460\n"
    "layout(location = 0) in vec4 vD0; layout(location = 1) in vec4 vD1;\n"
    "layout(location = 2) in vec4 vT0; layout(location = 3) in vec4 vT1;\n"
    "layout(location = 4) in vec4 vT2; layout(location = 5) in vec4 vT3;\n"
    "layout(location = 6) in float vFog;\n"
    "layout(binding = 0) uniform sampler2D t0;\n"
    "layout(binding = 1) uniform sampler2D t1;\n"
    "layout(binding = 2) uniform sampler2D t2;\n"
    "layout(binding = 3) uniform sampler2D t3;\n"
    "layout(std140, binding = 0) uniform FsDraw {\n"
    "    vec2 u_tscale[4];\n"
    "    vec4 u_c0[8]; vec4 u_c1[8];\n"
    "    vec4 u_fc0; vec4 u_fc1;\n"
    "    vec4 u_fogcolor;\n"
    "    uvec4 u_cicw[2]; uvec4 u_cocw[2]; uvec4 u_aicw[2]; uvec4 u_aocw[2];\n"
    "    uvec4 u_ctl; uvec4 u_ctl2;\n"
    "};\n"
    "#define u_alpha_func int(u_ctl2.y)\n"
    "#define u_alpha_ref float(u_ctl2.z)\n"
    /* Built a second time with this 0 (DK_KEY_FS_NOAT): no discard, so the
     * GPU keeps its early depth test. */
    "#ifndef NV2A_ALPHA_TEST\n"
    "#define NV2A_ALPHA_TEST 1\n"
    "#endif\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main() {\n";

/* SET_ALPHA_FUNC (GL's codes, 0x200 NEVER .. 0x207 ALWAYS) against the
 * reference, on the alpha as a 0..255 value. */
static const char k_alpha_test[] =
    "#if NV2A_ALPHA_TEST\n"
    "    float ar = fragColor.a * 255.0;\n"
    "    bool pass = true;\n"
    "    if (u_alpha_func == 0x200) pass = false;\n"
    "    else if (u_alpha_func == 0x201) pass = ar <  u_alpha_ref;\n"
    "    else if (u_alpha_func == 0x202) pass = ar == u_alpha_ref;\n"
    "    else if (u_alpha_func == 0x203) pass = ar <= u_alpha_ref;\n"
    "    else if (u_alpha_func == 0x204) pass = ar >  u_alpha_ref;\n"
    "    else if (u_alpha_func == 0x205) pass = ar != u_alpha_ref;\n"
    "    else if (u_alpha_func == 0x206) pass = ar >= u_alpha_ref;\n"
    "    if (!pass) discard;\n"
    "#endif\n";

int dk_shadergen_fragment(const DkCombinerKey *k, char *out, size_t cap)
{
    Text t = { out, cap, 0 };
    uint32_t stages = k->control & 0xFF, cw0 = k->final0, cw1 = k->final1;
    int i;

    if (stages > 8)
        stages = 8;
    put(&t, "%s", k_fragment_head);
    for (i = 0; i < 4; i++)
        texture_stage(&t, i, (k->shader_program >> (5 * i)) & 0x1F, k);
    put(&t, "    vec4 ZERO = vec4(0.0);\n"
            "    vec4 V0 = vD0, V1 = vD1;\n"
            "    vec4 R0 = vec4(0.0, 0.0, 0.0, T0.a), R1 = vec4(0.0);\n"
            "    vec4 FOG = vec4(u_fogcolor.rgb, clamp(vFog, 0.0, 1.0));\n");
    for (i = 0; i < (int)stages; i++)
        general_stage(&t, i, k);

    /* Final combiner. V1R0 is the specular colour plus R0 (each perhaps
     * inverted, the sum perhaps clamped); EF the product of E and F. EF is
     * declared before it is assigned, so an E or F naming EF reads 0. */
    put(&t, "    vec4 V1R0 = vec4(%s + %s, 0.0);\n",
        (cw1 & 0x40) ? "(1.0 - clamp(V1.rgb, 0.0, 1.0))" : "clamp(V1.rgb, 0.0, 1.0)",
        (cw1 & 0x20) ? "(1.0 - clamp(R0.rgb, 0.0, 1.0))" : "clamp(R0.rgb, 0.0, 1.0)");
    if (cw1 & 0x80)
        put(&t, "    V1R0 = clamp(V1R0, 0.0, 1.0);\n");
    put(&t, "    vec4 EF = vec4(0.0);\n    EF = vec4(");
    final_input(&t, cw1 >> 24, 0, k);
    put(&t, " * ");
    final_input(&t, cw1 >> 16, 0, k);
    put(&t, ", 0.0);\n");
    put(&t, "    vec3 fA = ");  final_input(&t, cw0 >> 24, 0, k); put(&t, ";\n");
    put(&t, "    vec3 fB = ");  final_input(&t, cw0 >> 16, 0, k); put(&t, ";\n");
    put(&t, "    vec3 fC = ");  final_input(&t, cw0 >> 8, 0, k);  put(&t, ";\n");
    put(&t, "    vec3 fD = ");  final_input(&t, cw0, 0, k);       put(&t, ";\n");
    put(&t, "    float fG = "); final_input(&t, cw1 >> 8, 1, k);  put(&t, ";\n");
    put(&t, "    fragColor = vec4(fA * fB + (1.0 - fA) * fC + fD, fG);\n"
            "    fragColor = clamp(fragColor, 0.0, 1.0);\n");
    put(&t, "%s", k_alpha_test);
    put(&t, "}\n");
    return finish(&t, cap);
}
