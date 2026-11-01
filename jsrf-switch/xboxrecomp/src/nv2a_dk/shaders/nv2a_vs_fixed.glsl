#version 460
/* Built twice (build.sh): as it is, and with NV2A_TEXGEN 1 for the draws
 * that generate texture coordinates or use a texture matrix, so only those
 * pay for it (the plain build is the shader as it was before texgen). */
#ifndef NV2A_TEXGEN
#define NV2A_TEXGEN 0
#endif
/*
 * nv2a_vs_fixed.glsl -- the deko3d vertex shader for batches without a
 * vertex program: fixed-function (u_mode.x 1: composite matrix, divide,
 * viewport offset) and pre-transformed (0: surface pixels, 1/rhw in w).
 * nv2a_vs.glsl's interface and its non-program paths, without the
 * program interpreter's register files (local memory) -- menus, text and
 * 2D passes run it. Built with the others by build.sh.
 *
 * Fixed-function lighting (SET_LIGHTING_ENABLE) is done here too, as the
 * NV2A does it: with lighting on, the colour the combiners get is computed
 * from the scene ambient, the lights and the material, and the vertex's
 * own colour only enters where SET_COLOR_MATERIAL routes it in. Without
 * it a lit draw got its vertex colour -- white where it had none -- and
 * JSRF's pause map came out a white silhouette (its white text on it
 * unreadable), the settings screen a white page.
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
    uvec4 u_flags;      /* x: bit 0 NV2A multiply, bit 1 the program writes constants,
                         * bit 2 depth clamped per vertex, bit 3 lit (VsLight),
                         * bit 4 texgen or a texture matrix (VsTex) */
    vec4  u_attr[16];   /* what an attribute without data reads */
    vec4  u_fogp;       /* SET_FOG_PARAMS 0 and 1 */
    uvec4 u_fogm;       /* x SET_FOG_ENABLE, y SET_FOG_MODE, z SET_FOG_GEN_MODE */
    vec4  u_fogpl;      /* SET_FOG_PLANE */
};

/* The lighting registers (nv2a_dk.c, light_push), pushed for lit draws. */
layout(std140, binding = 3) uniform VsLight {
    vec4  l_mv[4];      /* SET_MODEL_VIEW_MATRIX0, eye[i] = dot(l_mv[i], p) */
    vec4  l_scene;      /* SET_SCENE_AMBIENT_COLOR, SET_MATERIAL_ALPHA */
    uvec4 l_ctl;        /* x SET_LIGHT_ENABLE_MASK, y SET_COLOR_MATERIAL */
    vec4  l_amb[8];     /* per light: ambient rgb, w local range */
    vec4  l_dif[8];     /* diffuse rgb */
    vec4  l_dir[8];     /* infinite direction (towards the light, eye space) */
    vec4  l_pos[8];     /* local position (eye space) */
    vec4  l_att[8];     /* local attenuation: constant, linear, quadratic */
};

#if NV2A_TEXGEN
/* Texture coordinate generation and the texture matrices (nv2a_dk.c,
 * tex_push), pushed for fixed-function draws that use either. */
layout(std140, binding = 4) uniform VsTex {
    vec4  t_mat[16];    /* SET_TEXTURE_MATRIX, 4 rows a stage: t'[r] = dot(t_mat[4s+r], t) */
    uvec4 t_gen[4];     /* per stage, S T R Q: 0 the vertex's, 1 normal map,
                         * 2 reflection map, 3 sphere map */
    uvec4 t_ctl;        /* x SET_TEXTURE_MATRIX_ENABLE mask, y SET_NORMALIZATION_ENABLE,
                         * z stages with a generated component */
};
#endif

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

vec4 attr(uint i, vec4 a)
{
    return (u_mode.y & (1u << i)) != 0u ? a : u_attr[i];
}

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

/* The fog distance of a draw without a vertex program, by SET_FOG_GEN_MODE
 * (as xemu): the specular colour's alpha, the eye distance, the distance
 * from the fog plane (or its absolute value), or the fog coordinate. Eye
 * space through the model-view matrix of VsLight, which is pushed for these
 * draws. A pre-transformed vertex has no eye position: its alpha or its fog
 * coordinate. */
float fog_distance(vec4 p, vec4 d1)
{
    uint gen = u_fogm.z;
    vec4 pe = vec4(p.xyz, 1.0);
    vec3 e = vec3(dot(l_mv[0], pe), dot(l_mv[1], pe), dot(l_mv[2], pe));
    if (gen == 6u)
        return attr(5u, a5).x;
    if (gen == 0u || u_mode.x != 1u)
        return d1.a;
    if (gen == 1u)
        return length(e);
    float pl = dot(u_fogpl.xyz, e) + u_fogpl.w;
    return gen == 3u ? abs(pl) : pl;
}

/* A SET_COLOR_MATERIAL field: 0 the material (already in the registers),
 * 1 the vertex's diffuse colour, 2 its specular colour. */
vec4 mat_src(uint f, vec4 d0, vec4 d1)
{
    return f == 1u ? d0 : f == 2u ? d1 : vec4(1.0);
}

/* The lit diffuse colour. XDK D3D folds the material into the light colours
 * and emission + material ambient * global ambient into the scene ambient,
 * so with the material from the registers the sum is
 *   scene + sum_i atten_i * (ambient_i + diffuse_i * max(0, N.L_i)),
 * as the executor's CPU path (nv2a_pb_exec.c, lit_color) computes it; a
 * field taken from the vertex multiplies its terms (or, for emission, adds
 * the colour). Normals through the model-view matrix itself, the side the
 * viewer sees lit (two-sided), no specular, no spot cones. */
vec4 lit(vec4 p, vec4 d0, vec4 d1)
{
    vec4 pe = vec4(p.xyz, 1.0);
    vec3 e = vec3(dot(l_mv[0], pe), dot(l_mv[1], pe), dot(l_mv[2], pe));
    vec3 nn = attr(2u, a2).xyz;
    vec3 n = vec3(dot(l_mv[0].xyz, nn), dot(l_mv[1].xyz, nn), dot(l_mv[2].xyz, nn));
    float len = length(n);
    uint cm = l_ctl.y;
    vec3 ma = mat_src((cm >> 2) & 3u, d0, d1).rgb;
    vec3 md = mat_src((cm >> 4) & 3u, d0, d1).rgb;
    vec3 c = l_scene.rgb * ma;
    if (len > 1e-8)
        n /= len;
    if (dot(n, e) > 0.0)
        n = -n;
    if ((cm & 3u) != 0u)
        c += mat_src(cm & 3u, d0, d1).rgb;
    for (uint i = 0u; i < 8u; i++) {
        uint kind = (l_ctl.x >> (2u * i)) & 3u;
        vec3 l;
        float att = 1.0;
        if (kind == 0u)
            continue;
        if (kind == 1u) {
            l = l_dir[i].xyz;
        } else {
            vec3 v = l_pos[i].xyz - e;
            float d = length(v);
            if (l_amb[i].w > 0.0 && d > l_amb[i].w)
                continue;
            l = d > 1e-8 ? v / d : v;
            att = l_att[i].x + l_att[i].y * d + l_att[i].z * d * d;
            att = att > 1e-8 ? 1.0 / att : 1.0;
        }
        c += att * (l_amb[i].rgb * ma + l_dif[i].rgb * md * max(dot(n, l), 0.0));
    }
    return clamp(vec4(c, ((cm >> 4) & 3u) != 0u ? mat_src((cm >> 4) & 3u, d0, d1).a
                                                  : l_scene.w), 0.0, 1.0);
}

#if NV2A_TEXGEN
/* GENERATED TEXTURE COORDINATES (SET_TEXGEN_S..Q), as the T&L unit makes
 * them (xemu's vsh-ff.c, after OpenGL's texgen; JSRF Unleashed 0.5.5): per
 * component, the eye-space normal (normal map), the eye ray reflected about
 * it (reflection map), or that reflection folded onto a sphere map; then the
 * stage's texture matrix, which applies as well to coordinates the vertex
 * brings. JSRF looks textures up by the eye-space normal on reflective
 * glass and panels -- Dogenzaka Hill's footbridge roof drew dark bars and
 * striped glass without it. The normal goes to eye space through the
 * model-view itself, as lit() does. The two linear modes are not made
 * (nv2a_dk.c says so once): their components stay the vertex's. */
/* One stage's coordinate: its generated components, then its matrix. */
vec4 tg_stage(uint st, vec4 t, vec3 n, vec3 r, float m)
{
    uvec4 g = t_gen[st];
    if ((t_ctl.z & (1u << st)) != 0u) {
        t.x = g.x == 1u ? n.x : g.x == 2u ? r.x : g.x == 3u ? r.x * m + 0.5 : t.x;
        t.y = g.y == 1u ? n.y : g.y == 2u ? r.y : g.y == 3u ? r.y * m + 0.5 : t.y;
        t.z = g.z == 1u ? n.z : g.z == 2u ? r.z : t.z;
    }
    if ((t_ctl.x & (1u << st)) != 0u)
        t = vec4(dot(t_mat[4u * st], t), dot(t_mat[4u * st + 1u], t),
                 dot(t_mat[4u * st + 2u], t), dot(t_mat[4u * st + 3u], t));
    return t;
}

void texcoords(vec4 p)
{
    vec3 n = vec3(0.0, 0.0, 1.0), r = vec3(0.0, 0.0, 1.0);
    float m = 0.0;
    if (t_ctl.z != 0u) {
        vec4 pe = vec4(p.xyz, 1.0);
        vec3 e = vec3(dot(l_mv[0], pe), dot(l_mv[1], pe), dot(l_mv[2], pe));
        vec3 nn = attr(2u, a2).xyz;
        n = vec3(dot(l_mv[0].xyz, nn), dot(l_mv[1].xyz, nn), dot(l_mv[2].xyz, nn));
        if (t_ctl.y != 0u && dot(n, n) > 1e-16)
            n = normalize(n);
        vec3 u = dot(e, e) > 1e-16 ? normalize(e) : vec3(0.0, 0.0, -1.0);
        r = reflect(u, n);
        m = 0.5 / max(length(r + vec3(0.0, 0.0, 1.0)), 1e-8);
    }
    vT0 = tg_stage(0u, attr(9u, a9), n, r, m);
    vT1 = tg_stage(1u, attr(10u, a10), n, r, m);
    vT2 = tg_stage(2u, attr(11u, a11), n, r, m);
    vT3 = tg_stage(3u, attr(12u, a12), n, r, m);
}
#endif

void main()
{
    for (int i = 0; i < 8; i++)
        gl_ClipDistance[i] = 1.0;   /* see gl_ClipDistance above */
    vec4 p = attr(0u, a0);
    if (u_mode.x == 1u) {
        vec4 clip = vec4(dot(u_m[0], p), dot(u_m[1], p), dot(u_m[2], p), dot(u_m[3], p));
        float w = abs(clip.w) < 1e-6 ? 1e-6 : clip.w;
        vec3 sw = clip.xyz + u_vpoff.xyz * w;
        sw.xy *= u_aa.xy;
        gl_Position = nv2a_clip(sw, w);
    } else {
        float w = (p.w > 0.0) ? 1.0 / p.w : 1.0;
        gl_Position = nv2a_clip(p.xyz * w, w);
    }
    vD0 = clamp(attr(3u, a3), 0.0, 1.0);
    vD1 = clamp(attr(4u, a4), 0.0, 1.0);
    if ((u_flags.x & 8u) != 0u && u_mode.x == 1u) {
        /* Lit (u_flags.x bit 3): no specular lighting, so no specular. */
        vD0 = lit(p, vD0, vD1);
        vD1 = vec4(0.0, 0.0, 0.0, vD1.a);
    }
#if NV2A_TEXGEN
    texcoords(p);
#else
    vT0 = attr(9u, a9);
    vT1 = attr(10u, a10);
    vT2 = attr(11u, a11);
    vT3 = attr(12u, a12);
#endif
    vFog = nv2a_fog(fog_distance(p, clamp(attr(4u, a4), 0.0, 1.0)));
}
