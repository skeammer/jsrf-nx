#version 460
/*
 * nv2a_vs.glsl -- the NV2A transform stage as one deko3d vertex shader.
 *
 * Compiled once, at build time (uam), for every title program: instead of
 * translating each NV2A vertex program into a shader of its own (gl_vsh.c,
 * compiled on the console by Mesa), this shader interprets the program. The
 * CPU decodes each 128-bit instruction once per program version into the
 * fields below (nv2a_dk.c, vp_decode) and pushes them into a uniform buffer;
 * every vertex runs the slots [u_mode.z, u_mode.w).
 *
 * The semantics are gl_vsh.c's and the executor's (vp_run), instruction for
 * instruction: MAC and ILU read all sources before either writes; a paired
 * ILU writes R1; R12 is oPos; NV2A's multiplier makes 0 * anything 0.
 *
 * Decoded instruction (uvec4):
 *   x: mac[3:0] ilu[6:4] final[7] amux[9:8] bmux[11:10] cmux[13:12]
 *      aneg[14] bneg[15] cneg[16] out_ilu[17] out_reg[18] -[19]
 *      rel[20] mac_dst[24:21] ilu_dst[28:25]
 *   y: areg[3:0] breg[7:4] creg[11:8] vin[15:12] cidx[23:16] out_addr[31:24]
 *   z: aswz[7:0] bswz[15:8] cswz[23:16] mac_mask[27:24] ilu_mask[31:28]
 *   w: out_mask[3:0]
 *
 * Fixed-function (u_mode.x 1) and pre-transformed (0) batches take the same
 * paths as gl_vsh.c's nv2a_gl_vsh_fixed.
 */

layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 a1;
layout(location = 2) in vec4 a2;
layout(location = 3) in vec4 a3;
layout(location = 4) in vec4 a4;
layout(location = 5) in vec4 a5;
layout(location = 6) in vec4 a6;
layout(location = 7) in vec4 a7;
layout(location = 8) in vec4 a8;
layout(location = 9) in vec4 a9;
layout(location = 10) in vec4 a10;
layout(location = 11) in vec4 a11;
layout(location = 12) in vec4 a12;
layout(location = 13) in vec4 a13;
layout(location = 14) in vec4 a14;
layout(location = 15) in vec4 a15;

layout(std140, binding = 0) uniform VsConsts { vec4 c[192]; };
layout(std140, binding = 1) uniform VsProgram { uvec4 prog[136]; };
layout(std140, binding = 2) uniform VsDraw {
    vec4  u_surf;       /* 2/W, 2/H (real pixels), 1/zmax, NDC y sign (-1: y up) */
    vec4  u_m[4];       /* fixed-function composite rows */
    vec4  u_vpoff;      /* fixed-function viewport offset */
    vec4  u_aa;         /* xy: logical -> real pixels, zw: pixel-centre shift (real) */
    uvec4 u_mode;       /* x transform, y attributes with data, z first slot, w end slot */
    uvec4 u_flags;      /* x: bit 0 NV2A multiply, bit 1 the program writes constants */
    vec4  u_attr[16];   /* what an attribute without data reads */
    vec4  u_fogp;       /* SET_FOG_PARAMS 0 and 1 */
    uvec4 u_fogm;       /* x SET_FOG_ENABLE, y SET_FOG_MODE, z SET_FOG_GEN_MODE */
    vec4  u_fogpl;      /* SET_FOG_PLANE */
};

/* deko3d switches on all eight clip distances for every draw (its queue
 * set-up writes ClipDistanceEnable 0xFF); a shader that writes none leaves
 * the GPU reading whatever an earlier shader left in those outputs, and
 * shapes were cut away at random, frame to frame -- shadows and pieces of
 * scenery flickered out along straight edges. All eight are written, 1.0:
 * nothing is clipped. */
out float gl_ClipDistance[8];
/* The position is computed exactly as written, never fused (mul + add
 * into one rounding): JSRF draws the same ground several times -- the
 * surface, then shadows over it with an equal-depth test -- through
 * different programs, and where the compiler fused one and not the other
 * the depths differed in the last bit, so the shadow pass failed here and
 * passed there, flickering as the camera moved. */
invariant gl_Position;

layout(location = 0) out vec4 vD0;
layout(location = 1) out vec4 vD1;
layout(location = 2) out vec4 vT0;
layout(location = 3) out vec4 vT1;
layout(location = 4) out vec4 vT2;
layout(location = 5) out vec4 vT3;
layout(location = 6) out float vFog;

vec4 V[16];
vec4 R[13];             /* R0..R11; R12 is oPos */
vec4 O[13];             /* output registers by address; O[0] unused (oPos is R12) */
int  A0;
/* Constants the program writes: a few, held here instead of a copy of all 192. */
uint nw;
int  wi[4];
vec4 wv[4];
bool mz;

float nv2a_rcp(float x) { return x != 0.0 ? 1.0 / x : 1.884467e+19; }
float nv2a_rcc(float x)
{
    float y = nv2a_rcp(x);
    float a = clamp(abs(y), 5.42101e-20, 1.884467e+19);
    return y < 0.0 ? -a : a;
}
vec4 nv2a_exp(float x) { float f = floor(x); return vec4(exp2(f), x - f, exp2(x), 1.0); }
vec4 nv2a_log(float x)
{
    float a = abs(x);
    if (a == 0.0)
        return vec4(-1.884467e+19, 1.0, -1.884467e+19, 1.0);
    float e = floor(log2(a));
    return vec4(e, a / exp2(e), log2(a), 1.0);
}
vec4 nv2a_lit(vec4 s)
{
    float lx = max(s.x, 0.0), ly = max(s.y, 0.0), w = clamp(s.w, -127.996, 127.996);
    return vec4(1.0, lx, (lx > 0.0 && ly > 0.0) ? exp2(w * log2(ly)) : 0.0, 1.0);
}

/* NV2A's multiplier: 0 times anything (infinity, NaN) is 0. */
vec4 nmul(vec4 a, vec4 b)
{
    vec4 p = a * b;
    if (!mz)
        return p;
    bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
    return mix(p, vec4(0.0), z);
}
float ndp3(vec4 a, vec4 b) { vec4 p = nmul(a, b); return p.x + p.y + p.z; }
float ndp4(vec4 a, vec4 b) { vec4 p = nmul(a, b); return p.x + p.y + p.z + p.w; }

vec2 nv2a_snap(vec2 sw, float w)
{
    return w > 0.0 ? trunc(sw / w * 16.0) / 16.0 * w : sw;
}

/* screen*w (real pixels) and clip w to deko3d clip space (depth 0..1, rows
 * top-first as in guest memory: deko3d's NDC y points up, so y turns over
 * by u_surf.w). Depth is left as the title computed it: the rasterizer's
 * depth clamp (nv2a_dk.c, depthClampEnable) clamps it per pixel and clips
 * nothing at the near and far planes, as the NV2A does. It used to be
 * clamped here, per vertex, to [0, w] -- a corner nearer than the near
 * plane then pulled the depth of its whole triangle, and next to the
 * camera the ground won over a rail and a ramp went black along straight
 * lines that moved with the camera. u_flags.x bit 2 (RECOMP_DK_ZCLAMP=
 * vertex) brings that back, to compare.
 *
 * At an internal resolution above the title's, u_aa.zw moves every position
 * by 0.5/scale - 0.5 real pixels, so that the title's pixel centres land on
 * the first image pixel's centre of each (image = (real - 0.5) * scale + 0.5).
 * A 2D quad whose edge lies on a pixel centre (x = 0.5) covers that pixel at
 * 640x480 and still does; without the shift it fell a quarter pixel past the
 * first column's centre at x1.5, and JSRF's cutscenes showed column 0 and
 * row 0 left over from earlier frames as lines along the left and top. 0 at
 * x1 and with RECOMP_DK_PIXEL_ALIGN=0. */
vec4 nv2a_clip(vec3 sw, float w)
{
    float z = sw.z * u_surf.z;
    sw.xy = nv2a_snap(sw.xy, w) + u_aa.zw * w;
    return vec4(sw.x * u_surf.x - w, (sw.y * u_surf.y - w) * u_surf.w,
                (u_flags.x & 4u) != 0u && w > 0.0 ? clamp(z, 0.0, w) : z, w);
}

/* THE FOG UNIT. The fog value a vertex carries -- a vertex program's oFog.x,
 * or for draws without one the distance SET_FOG_GEN_MODE names -- is a
 * distance, which the NV2A turns into the fog factor the combiners read as
 * FOG.a (1: none, 0: all fog colour) by SET_FOG_MODE with SET_FOG_PARAMS,
 * per vertex. The formulas are xemu's (tested against the hardware): XDK
 * D3D writes the parameters so that linear is (end - d) / (end - start) and
 * the exponential modes 1 / e^(d * density) and 1 / e^((d * density)^2).
 * The value used to be taken as the factor itself, unconverted: no fog at
 * all where distances are large, and everything fog colour where they come
 * out 0 or below -- Rokkaku-dai Heights drew as flat pink. Fog off: 1. */
float nv2a_fog(float d)
{
    uint mode = u_fogm.y;
    float f;
    if (u_fogm.x == 0u)
        return 1.0;
    if (mode == 0x800u || mode == 0x802u) {             /* EXP, EXP_ABS */
        if (mode == 0x800u && isinf(d))
            d = 0.0;
        f = u_fogp.x + exp2(d * u_fogp.y * 16.0) - 1.5;
    } else if (mode == 0x801u || mode == 0x803u) {      /* EXP2, EXP2_ABS */
        f = u_fogp.x + exp2(-d * d * u_fogp.y * u_fogp.y * 32.0) - 1.5;
    } else {                                            /* LINEAR, LINEAR_ABS */
        if (isinf(d))
            d = 0.0;
        f = u_fogp.x + d * u_fogp.y - 1.0;
    }
    if (mode >= 0x802u && mode <= 0x804u)
        f = abs(f);
    return f;
}

bvec4 mk(uint m)
{
    return bvec4((m & 8u) != 0u, (m & 4u) != 0u, (m & 2u) != 0u, (m & 1u) != 0u);
}

vec4 swz(vec4 v, uint s)
{
    return vec4(v[(s >> 6) & 3u], v[(s >> 4) & 3u], v[(s >> 2) & 3u], v[s & 3u]);
}

vec4 cread(int i)
{
    vec4 v = c[i];
    if ((u_flags.x & 2u) != 0u)
        for (uint k = 0u; k < nw; k++)
            if (wi[k] == i)
                v = wv[k];
    return v;
}

void cwrite(int i, uint m, vec4 val)
{
    uint k;
    for (k = 0u; k < nw; k++)
        if (wi[k] == i)
            break;
    if (k == nw) {
        if (nw == 4u)
            return;
        wi[k] = i;
        wv[k] = c[i];
        nw++;
    }
    wv[k] = mix(wv[k], val, mk(m));
}

vec4 src(uint mux, uint reg, uint sw, uint neg, vec4 vin, vec4 cin)
{
    vec4 v = vec4(0.0);
    if (mux == 1u)
        v = reg < 13u ? R[reg] : vec4(0.0);
    else if (mux == 2u)
        v = vin;
    else if (mux == 3u)
        v = cin;
    v = swz(v, sw);
    return neg != 0u ? -v : v;
}

void run_program()
{
    for (uint pc = u_mode.z; pc < u_mode.w; pc++) {
        uvec4 t = prog[pc];
        uint mac = t.x & 15u, ilu = (t.x >> 4) & 7u;
        bool rel = (t.x & (1u << 20)) != 0u;
        int ci = int((t.y >> 16) & 255u);
        ci = rel ? clamp(ci + A0, 0, 191) : min(ci, 191);
        vec4 vin = V[(t.y >> 12) & 15u];
        vec4 cin = cread(ci);
        vec4 A = src((t.x >> 8) & 3u, t.y & 15u, t.z & 255u, (t.x >> 14) & 1u, vin, cin);
        vec4 B = src((t.x >> 10) & 3u, (t.y >> 4) & 15u, (t.z >> 8) & 255u, (t.x >> 15) & 1u, vin, cin);
        vec4 C = src((t.x >> 12) & 3u, (t.y >> 8) & 15u, (t.z >> 16) & 255u, (t.x >> 16) & 1u, vin, cin);
        vec4 m = vec4(0.0), l = vec4(0.0);

        switch (mac) {
        case 1u:  m = A; break;                                     /* MOV */
        case 2u:  m = nmul(A, B); break;                            /* MUL */
        case 3u:  m = A + C; break;                                 /* ADD */
        case 4u:  m = nmul(A, B) + C; break;                        /* MAD */
        case 5u:  m = vec4(ndp3(A, B)); break;                      /* DP3 */
        case 6u:  m = vec4(ndp3(A, B) + B.w); break;                /* DPH */
        case 7u:  m = vec4(ndp4(A, B)); break;                      /* DP4 */
        case 8u:  m = vec4(1.0, nmul(A, B).y, A.z, B.w); break;     /* DST */
        case 9u:  m = min(A, B); break;                             /* MIN */
        case 10u: m = max(A, B); break;                             /* MAX */
        case 11u: m = vec4(lessThan(A, B)); break;                  /* SLT */
        case 12u: m = vec4(greaterThanEqual(A, B)); break;          /* SGE */
        default:  break;
        }
        switch (ilu) {
        case 1u: l = C; break;                                      /* MOV */
        case 2u: l = vec4(nv2a_rcp(C.x)); break;                    /* RCP */
        case 3u: l = vec4(nv2a_rcc(C.x)); break;                    /* RCC */
        case 4u: l = vec4(nv2a_rcp(sqrt(abs(C.x)))); break;         /* RSQ */
        case 5u: l = nv2a_exp(C.x); break;                          /* EXP */
        case 6u: l = nv2a_log(C.x); break;                          /* LOG */
        case 7u: l = nv2a_lit(C); break;                            /* LIT */
        default: break;
        }

        uint rd = (t.x >> 21) & 15u;
        if (mac == 13u)                                             /* ARL */
            A0 = int(floor(A.x + 0.001));
        else if (mac != 0u && rd < 13u)
            R[rd] = mix(R[rd], m, mk((t.z >> 24) & 15u));
        if (ilu != 0u) {
            uint ld = (t.x >> 25) & 15u;
            if (ld < 13u)
                R[ld] = mix(R[ld], l, mk(t.z >> 28));
        }
        uint om = t.w & 15u;
        if (om != 0u) {
            vec4 val = ((t.x >> 17) & 1u) != 0u ? l : m;
            uint addr = t.y >> 24;
            if (((t.x >> 18) & 1u) != 0u) {
                if (addr == 0u)
                    R[12] = mix(R[12], val, mk(om));
                else if (addr < 13u)
                    O[addr] = mix(O[addr], val, mk(om));
            } else {
                int wa = int(addr);
                wa = rel ? clamp(wa + A0, 0, 191) : min(wa, 191);
                cwrite(wa, om, val);
            }
        }
    }
}

void main()
{
    for (int i = 0; i < 8; i++)
        gl_ClipDistance[i] = 1.0;   /* see gl_ClipDistance above */
    uint present = u_mode.y;
    V[0]  = (present & 0x0001u) != 0u ? a0  : u_attr[0];
    V[1]  = (present & 0x0002u) != 0u ? a1  : u_attr[1];
    V[2]  = (present & 0x0004u) != 0u ? a2  : u_attr[2];
    V[3]  = (present & 0x0008u) != 0u ? a3  : u_attr[3];
    V[4]  = (present & 0x0010u) != 0u ? a4  : u_attr[4];
    V[5]  = (present & 0x0020u) != 0u ? a5  : u_attr[5];
    V[6]  = (present & 0x0040u) != 0u ? a6  : u_attr[6];
    V[7]  = (present & 0x0080u) != 0u ? a7  : u_attr[7];
    V[8]  = (present & 0x0100u) != 0u ? a8  : u_attr[8];
    V[9]  = (present & 0x0200u) != 0u ? a9  : u_attr[9];
    V[10] = (present & 0x0400u) != 0u ? a10 : u_attr[10];
    V[11] = (present & 0x0800u) != 0u ? a11 : u_attr[11];
    V[12] = (present & 0x1000u) != 0u ? a12 : u_attr[12];
    V[13] = (present & 0x2000u) != 0u ? a13 : u_attr[13];
    V[14] = (present & 0x4000u) != 0u ? a14 : u_attr[14];
    V[15] = (present & 0x8000u) != 0u ? a15 : u_attr[15];

    if (u_mode.x == 2u) {
        for (int i = 0; i < 13; i++)
            R[i] = vec4(0.0);
        for (int i = 0; i < 13; i++)
            O[i] = vec4(0.0);
        O[3] = O[4] = O[7] = O[8] = vec4(0.0, 0.0, 0.0, 1.0);      /* oD0 oD1 oB0 oB1 */
        O[9] = O[10] = O[11] = O[12] = vec4(0.0, 0.0, 0.0, 1.0);   /* oT0..oT3 */
        A0 = 0;
        nw = 0u;
        mz = (u_flags.x & 1u) != 0u;
        run_program();
        vec4 pos = R[12];
        float w = abs(pos.w) < 1e-6 ? 1e-6 : pos.w;
        vec3 s = vec3(pos.xy * u_aa.xy, pos.z);
        gl_Position = nv2a_clip(s * w, w);
        vD0 = clamp(O[3], 0.0, 1.0);
        vD1 = clamp(O[4], 0.0, 1.0);
        vT0 = O[9]; vT1 = O[10]; vT2 = O[11]; vT3 = O[12];
        vFog = nv2a_fog(O[5].x);
    } else {
        if (u_mode.x == 1u) {
            vec4 p = V[0];
            vec4 clip = vec4(dot(u_m[0], p), dot(u_m[1], p), dot(u_m[2], p), dot(u_m[3], p));
            float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;
            vec3 sw = clip.xyz + u_vpoff.xyz * w;
            sw.xy *= u_aa.xy;
            gl_Position = nv2a_clip(sw, w);
        } else {
            float w = (V[0].w > 0.0) ? 1.0 / V[0].w : 1.0;
            gl_Position = nv2a_clip(V[0].xyz * w, w);
        }
        vD0 = clamp(V[3], 0.0, 1.0);
        vD1 = clamp(V[4], 0.0, 1.0);
        vT0 = V[9]; vT1 = V[10]; vT2 = V[11]; vT3 = V[12];
        vFog = 1.0;                 /* nv2a_vs_fixed.glsl draws these */
    }
}
