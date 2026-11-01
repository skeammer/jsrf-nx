/* Built twice by run.sh: once with -DRECOMP_XMM_NO_VECTOR (the lane-wise
 * union helpers) and once without (the vector ones), each under its own
 * prefix, so test_xmm_vector.c can run both on the same inputs. */
#include "recomp_types.h"

#define F(name) PFX##_##name
#define CAT2(a, b) a##_##b
#define CAT(a, b) CAT2(a, b)
#define W(name) CAT(PFX, name)

RecompXmm W(zero)(void) { return XMM_ZERO(); }
RecompXmm W(scalar)(float v) { return XMM_SCALAR(v); }
RecompXmm W(mem)(uint32_t a) { return XMM_MEM(a); }
void W(store)(uint32_t a, RecompXmm v) { XMM_STORE(a, v); }
RecompXmm W(add)(RecompXmm a, RecompXmm b) { return XMM_ADD(a, b); }
RecompXmm W(sub)(RecompXmm a, RecompXmm b) { return XMM_SUB(a, b); }
RecompXmm W(mul)(RecompXmm a, RecompXmm b) { return XMM_MUL(a, b); }
RecompXmm W(div)(RecompXmm a, RecompXmm b) { return XMM_DIV(a, b); }
RecompXmm W(and)(RecompXmm a, RecompXmm b) { return XMM_AND(a, b); }
RecompXmm W(or)(RecompXmm a, RecompXmm b) { return XMM_OR(a, b); }
RecompXmm W(xor)(RecompXmm a, RecompXmm b) { return XMM_XOR(a, b); }
RecompXmm W(andn)(RecompXmm a, RecompXmm b) { return XMM_ANDN(a, b); }
RecompXmm W(unpack_low)(RecompXmm a, RecompXmm b) { return XMM_UNPACK_LOW(a, b); }
RecompXmm W(unpack_high)(RecompXmm a, RecompXmm b) { return XMM_UNPACK_HIGH(a, b); }
/* Every shufps immediate, as the lifter passes it: a constant. */
#define S(n) case n: return XMM_SHUFFLE(a, b, n);
#define S16(n) S(n) S(n + 1) S(n + 2) S(n + 3) S(n + 4) S(n + 5) S(n + 6) S(n + 7) \
               S(n + 8) S(n + 9) S(n + 10) S(n + 11) S(n + 12) S(n + 13) S(n + 14) S(n + 15)
RecompXmm W(shuffle)(RecompXmm a, RecompXmm b, uint32_t imm)
{
    switch (imm) {
    S16(0) S16(16) S16(32) S16(48) S16(64) S16(80) S16(96) S16(112)
    S16(128) S16(144) S16(160) S16(176) S16(192) S16(208) S16(224) S16(240)
    }
    return XMM_ZERO();
}
/* A runtime immediate too (never generated, but the helper allows it). */
RecompXmm W(shuffle_rt)(RecompXmm a, RecompXmm b, uint32_t imm) { return XMM_SHUFFLE(a, b, imm); }
/* A short lifted-style sequence: locals, lane access, a store. */
void W(seq)(uint32_t src, uint32_t mat, uint32_t dst)
{
    RecompXmm x0, x1, x2;
    x0 = XMM_SCALAR(MEMF(src)); x1 = XMM_SCALAR(MEMF(src + 4)); x2 = XMM_SCALAR(MEMF(src + 8));
    x0 = XMM_SHUFFLE(x0, x0, 0); x1 = XMM_SHUFFLE(x1, x1, 0); x2 = XMM_SHUFFLE(x2, x2, 0);
    x0 = XMM_MUL(x0, XMM_MEM(mat)); x1 = XMM_MUL(x1, XMM_MEM(mat + 0x10));
    x2 = XMM_MUL(x2, XMM_MEM(mat + 0x20));
    x0 = XMM_ADD(x0, XMM_MEM(mat + 0x30)); x1 = XMM_ADD(x1, x2); x0 = XMM_ADD(x0, x1);
    x1.f[0] = x1.f[0] * x0.f[2];
    x2 = XMM_UNPACK_LOW(x0, x1);
    XMM_STORE(dst, x0); XMM_STORE(dst + 16, x2);
    MEM32(dst + 32) = x1.u[3];
}
