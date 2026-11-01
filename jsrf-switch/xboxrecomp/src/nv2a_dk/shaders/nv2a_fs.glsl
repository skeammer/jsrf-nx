#version 460
/*
 * nv2a_fs.glsl -- the NV2A texture stages and register combiners as one
 * deko3d fragment shader.
 *
 * Compiled once at build time (uam). gl_psh.c generates a shader per
 * combiner setup; this one reads the setup -- the XDK's pixel-shader tokens,
 * which D3D writes to the combiner registers unchanged -- from a uniform
 * buffer and evaluates it. Same encoding and semantics as gl_psh.c:
 *
 *   input byte (ICW, final CW0/CW1): reg[3:0] | ALPHA 0x10 | mapping[7:5]
 *   colour/alpha OCW:  CD dst[3:0] AB dst[7:4] SUM dst[11:8]
 *                      CD_DOT 12, AB_DOT 13, MUX 14, scale/bias [17:15],
 *                      CD_BLUE_TO_ALPHA 18, AB_BLUE_TO_ALPHA 19
 *   CONTROL:           stage count [7:0], MUX_MSB 8, UNIQUE_C0 12, UNIQUE_C1 16
 *   final CW1:         E, F, G bytes; CLAMP_SUM 0x80, COMPLEMENT_V1 0x40,
 *                      COMPLEMENT_R0 0x20 in the low byte
 *   SHADER_STAGE_PROGRAM: 5 bits per texture stage
 *
 * Registers: 0 zero, 1 C0, 2 C1, 3 fog, 4 V0, 5 V1, 8-11 T0-T3, 12 R0, 13 R1;
 * the final combiner adds 14 V1R0_SUM and 15 EF_PROD. R0.a starts as T0.a.
 * Registers are read and written through switches, not an array, so they
 * stay in GPU registers.
 */

layout(location = 0) in vec4 vD0;
layout(location = 1) in vec4 vD1;
layout(location = 2) in vec4 vT0;
layout(location = 3) in vec4 vT1;
layout(location = 4) in vec4 vT2;
layout(location = 5) in vec4 vT3;
layout(location = 6) in float vFog;

layout(binding = 0) uniform sampler2D t0;
layout(binding = 1) uniform sampler2D t1;
layout(binding = 2) uniform sampler2D t2;
layout(binding = 3) uniform sampler2D t3;

layout(std140, binding = 0) uniform FsDraw {
    vec4  u_tscale[4];      /* xy: texcoord scale (linear textures, render targets) */
    vec4  u_c0[8];
    vec4  u_c1[8];
    vec4  u_fc0;            /* SPECULAR_FOG_FACTOR0/1: the final combiner's C0/C1 */
    vec4  u_fc1;
    vec4  u_fog;            /* fog colour */
    uvec4 u_cicw[2];
    uvec4 u_cocw[2];
    uvec4 u_aicw[2];
    uvec4 u_aocw[2];
    uvec4 u_ctl;            /* x control, y final CW0, z final CW1, w shader program */
    uvec4 u_ctl2;           /* x other stage input, y alpha func, z alpha ref, w flags */
};

layout(location = 0) out vec4 fragColor;

vec4 V0, V1, T0, T1, T2, T3, R0, R1, FOG, C0, C1;

vec4 reg_read(uint r)
{
    switch (r) {
    case 1u:  return C0;
    case 2u:  return C1;
    case 3u:  return FOG;
    case 4u:  return V0;
    case 5u:  return V1;
    case 8u:  return T0;
    case 9u:  return T1;
    case 10u: return T2;
    case 11u: return T3;
    case 12u: return R0;
    case 13u: return R1;
    default:  return vec4(0.0);
    }
}

void write_rgb(uint r, vec3 v)
{
    switch (r) {
    case 4u:  V0.rgb = v; break;
    case 5u:  V1.rgb = v; break;
    case 8u:  T0.rgb = v; break;
    case 9u:  T1.rgb = v; break;
    case 10u: T2.rgb = v; break;
    case 11u: T3.rgb = v; break;
    case 12u: R0.rgb = v; break;
    case 13u: R1.rgb = v; break;
    default:  break;
    }
}

void write_a(uint r, float v)
{
    switch (r) {
    case 4u:  V0.a = v; break;
    case 5u:  V1.a = v; break;
    case 8u:  T0.a = v; break;
    case 9u:  T1.a = v; break;
    case 10u: T2.a = v; break;
    case 11u: T3.a = v; break;
    case 12u: R0.a = v; break;
    case 13u: R1.a = v; break;
    default:  break;
    }
}

float map1(float x, uint m)
{
    switch (m) {
    case 0u: return max(x, 0.0);                    /* unsigned */
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);       /* unsigned invert */
    case 2u: return 2.0 * max(x, 0.0) - 1.0;        /* expand normal */
    case 3u: return 1.0 - 2.0 * max(x, 0.0);        /* expand negate */
    case 4u: return max(x, 0.0) - 0.5;              /* half-bias normal */
    case 5u: return 0.5 - max(x, 0.0);              /* half-bias negate */
    case 6u: return x;                              /* signed identity */
    default: return -x;                             /* signed negate */
    }
}

vec3 map3(vec3 x, uint m)
{
    switch (m) {
    case 0u: return max(x, 0.0);
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * max(x, 0.0) - 1.0;
    case 3u: return 1.0 - 2.0 * max(x, 0.0);
    case 4u: return max(x, 0.0) - 0.5;
    case 5u: return 0.5 - max(x, 0.0);
    case 6u: return x;
    default: return -x;
    }
}

/* A general-combiner input. V1R0_SUM and EF_PROD exist only in the final one. */
vec3 in_rgb(uint b)
{
    uint r = b & 15u;
    vec4 v = r >= 14u ? vec4(0.0) : reg_read(r);
    vec3 x = ((b >> 4) & 1u) != 0u ? vec3(v.a) : v.rgb;
    return map3(x, (b >> 5) & 7u);
}

float in_a(uint b)
{
    uint r = b & 15u;
    vec4 v = r >= 14u ? vec4(0.0) : reg_read(r);
    float x = ((b >> 4) & 1u) != 0u ? v.a : v.b;
    return map1(x, (b >> 5) & 7u);
}

vec3 sb3(vec3 v, uint op)
{
    switch (op) {
    case 1u: return v - 0.5;
    case 2u: return v * 2.0;
    case 3u: return (v - 0.5) * 2.0;
    case 4u: return v * 4.0;
    case 6u: return v * 0.5;
    default: return v;
    }
}

float sb1(float v, uint op)
{
    switch (op) {
    case 1u: return v - 0.5;
    case 2u: return v * 2.0;
    case 3u: return (v - 0.5) * 2.0;
    case 4u: return v * 4.0;
    case 6u: return v * 0.5;
    default: return v;
    }
}

vec4 V1R0, EF;

/* A final-combiner input: clamped to [0,1], optionally inverted; its C0/C1
 * are SPECULAR_FOG_FACTOR0/1. */
vec4 final_reg(uint r)
{
    switch (r) {
    case 1u:  return u_fc0;
    case 2u:  return u_fc1;
    case 14u: return V1R0;
    case 15u: return EF;
    default:  return reg_read(r);
    }
}

vec3 fin_rgb(uint b)
{
    vec4 v = final_reg(b & 15u);
    vec3 x = clamp(((b >> 4) & 1u) != 0u ? vec3(v.a) : v.rgb, 0.0, 1.0);
    return ((b >> 5) & 1u) != 0u ? 1.0 - x : x;
}

float fin_a(uint b)
{
    vec4 v = final_reg(b & 15u);
    float x = clamp(((b >> 4) & 1u) != 0u ? v.a : v.b, 0.0, 1.0);
    return ((b >> 5) & 1u) != 0u ? 1.0 - x : x;
}

/* The earlier stage a dependent mode reads (SHADER_OTHER_STAGE_INPUT). */
uint input_stage(uint t)
{
    uint s = t <= 1u ? 0u : ((u_ctl2.x >> (t == 2u ? 16u : 20u)) & 15u);
    return s < t ? s : t - 1u;
}

vec4 stage_value(uint s)
{
    return s == 0u ? T0 : s == 1u ? T1 : T2;
}

/* Texture stage I (sampler S, coordinates VT) into T. Modes: 0 none,
 * 1 projective 2D, 3 cube (not yet: black), 4 pass-through, 5 clip plane,
 * 15/16 dependent AR/GB; everything else samples 2D. */
#define FETCH(T, S, VT, I) {                                                   \
    uint mode = (u_ctl.w >> (5u * I)) & 31u;                                   \
    if (mode == 0u || mode == 3u) {                                            \
        T = vec4(0.0);                                                         \
    } else if (mode == 4u) {                                                   \
        T = clamp(VT, 0.0, 1.0);                                               \
    } else if (mode == 5u) {                                                   \
        if (any(lessThan(VT, vec4(0.0)))) discard;                             \
        T = vec4(0.0);                                                         \
    } else if (mode == 15u || mode == 16u) {                                   \
        if (I == 0u) {                                                         \
            T = vec4(0.0);                                                     \
        } else {                                                               \
            vec4 src = stage_value(input_stage(I));                            \
            T = texture(S, mode == 15u ? src.ar : src.gb);                     \
        }                                                                      \
    } else {                                                                   \
        vec2 tc = (mode == 1u && abs(VT.w) > 1e-8) ? VT.xy / VT.w : VT.xy;     \
        T = texture(S, tc * u_tscale[I].xy);                                   \
    }                                                                          \
}

void main()
{
    FETCH(T0, t0, vT0, 0u)
    FETCH(T1, t1, vT1, 1u)
    FETCH(T2, t2, vT2, 2u)
    FETCH(T3, t3, vT3, 3u)

    V0 = vD0;
    V1 = vD1;
    R0 = vec4(0.0, 0.0, 0.0, T0.a);
    R1 = vec4(0.0);
    FOG = vec4(u_fog.rgb, clamp(vFog, 0.0, 1.0));

    uint ctl = u_ctl.x;
    uint count = min(ctl & 255u, 8u);
    for (uint i = 0u; i < count; i++) {
        uint cicw = u_cicw[i >> 2][i & 3u], cocw = u_cocw[i >> 2][i & 3u];
        uint aicw = u_aicw[i >> 2][i & 3u], aocw = u_aocw[i >> 2][i & 3u];
        C0 = u_c0[(ctl & (1u << 12)) != 0u ? i : 0u];
        C1 = u_c1[(ctl & (1u << 16)) != 0u ? i : 0u];

        vec3 cA = in_rgb(cicw >> 24), cB = in_rgb((cicw >> 16) & 255u);
        vec3 cC = in_rgb((cicw >> 8) & 255u), cD = in_rgb(cicw & 255u);
        float aA = in_a(aicw >> 24), aB = in_a((aicw >> 16) & 255u);
        float aC = in_a((aicw >> 8) & 255u), aD = in_a(aicw & 255u);

        bool sel = (ctl & 256u) != 0u ? (R0.a >= 0.5)
                                      : ((int(R0.a * 255.0 + 0.5) & 1) != 0);
        vec3 cAB = (cocw & (1u << 13)) != 0u ? vec3(dot(cA, cB)) : cA * cB;
        vec3 cCD = (cocw & (1u << 12)) != 0u ? vec3(dot(cC, cD)) : cC * cD;
        vec3 cS = (cocw & (1u << 14)) != 0u ? (sel ? cCD : cAB) : cAB + cCD;
        float aAB = aA * aB, aCD = aC * aD;
        float aS = (aocw & (1u << 14)) != 0u ? (sel ? aCD : aAB) : aAB + aCD;

        uint cop = (cocw >> 15) & 7u, aop = (aocw >> 15) & 7u;
        cAB = clamp(sb3(cAB, cop), -1.0, 1.0);
        cCD = clamp(sb3(cCD, cop), -1.0, 1.0);
        cS  = clamp(sb3(cS, cop), -1.0, 1.0);
        aAB = clamp(sb1(aAB, aop), -1.0, 1.0);
        aCD = clamp(sb1(aCD, aop), -1.0, 1.0);
        aS  = clamp(sb1(aS, aop), -1.0, 1.0);

        write_rgb((cocw >> 4) & 15u, cAB);
        write_rgb(cocw & 15u, cCD);
        write_rgb((cocw >> 8) & 15u, cS);
        write_a((aocw >> 4) & 15u, aAB);
        write_a(aocw & 15u, aCD);
        write_a((aocw >> 8) & 15u, aS);
        /* Blue-to-alpha replaces the alpha written to the AB / CD destination. */
        if ((cocw & (1u << 19)) != 0u)
            write_a((cocw >> 4) & 15u, cAB.b);
        if ((cocw & (1u << 18)) != 0u)
            write_a(cocw & 15u, cCD.b);
    }

    /* The final combiner. */
    uint cw0 = u_ctl.y, cw1 = u_ctl.z;
    C0 = u_c0[0];
    C1 = u_c1[0];
    vec3 v1 = clamp(V1.rgb, 0.0, 1.0), r0 = clamp(R0.rgb, 0.0, 1.0);
    V1R0 = vec4(((cw1 & 0x40u) != 0u ? 1.0 - v1 : v1) + ((cw1 & 0x20u) != 0u ? 1.0 - r0 : r0), 0.0);
    if ((cw1 & 0x80u) != 0u)
        V1R0 = clamp(V1R0, 0.0, 1.0);
    EF = vec4(0.0);
    EF = vec4(fin_rgb(cw1 >> 24) * fin_rgb((cw1 >> 16) & 255u), 0.0);
    vec3 fA = fin_rgb(cw0 >> 24), fB = fin_rgb((cw0 >> 16) & 255u);
    vec3 fC = fin_rgb((cw0 >> 8) & 255u), fD = fin_rgb(cw0 & 255u);
    float fG = fin_a((cw1 >> 8) & 255u);
    fragColor = clamp(vec4(fA * fB + (1.0 - fA) * fC + fD, fG), 0.0, 1.0);

    /* Alpha test, on the 0..255 scale the title's reference uses. */
    float ar = fragColor.a * 255.0, aref = float(u_ctl2.z);
    uint func = u_ctl2.y;
    bool pass = true;
    if (func == 0x200u)      pass = false;
    else if (func == 0x201u) pass = ar <  aref;
    else if (func == 0x202u) pass = ar == aref;
    else if (func == 0x203u) pass = ar <= aref;
    else if (func == 0x204u) pass = ar >  aref;
    else if (func == 0x205u) pass = ar != aref;
    else if (func == 0x206u) pass = ar >= aref;
    if (!pass) {
        if ((u_ctl2.w & 1u) != 0u)          /* RECOMP_DK_HOLE_DIAG: show, not drop */
            fragColor = vec4(0.0, 1.0, 1.0, 1.0);
        else
            discard;
    }
}
