/* The vector SSE helpers (recomp_types.h, RECOMP_XMM_VECTOR) against the
 * lane-wise ones, bit for bit, on random lanes with NaNs, infinities,
 * denormals and signed zeros among them.
 *
 *   regen/xmm_vector/run.sh       (needs a C compiler; any host) */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef union { float f[4]; double d[2]; uint32_t u[4]; int32_t i[4]; uint64_t q[2]; } X;
ptrdiff_t g_xbox_mem_offset;

#define DECL(P)                                                         \
    X P##_zero(void); X P##_scalar(float); X P##_mem(uint32_t);           \
    void P##_store(uint32_t, X); X P##_add(X, X); X P##_sub(X, X);        \
    X P##_mul(X, X); X P##_div(X, X); X P##_and(X, X); X P##_or(X, X);    \
    X P##_xor(X, X); X P##_andn(X, X); X P##_unpack_low(X, X);            \
    X P##_unpack_high(X, X); X P##_shuffle(X, X, uint32_t);               \
    X P##_shuffle_rt(X, X, uint32_t); void P##_seq(uint32_t, uint32_t, uint32_t);
DECL(lane)
DECL(vec)

static uint64_t s_rng = 88172645463325252ull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)s_rng; }

static uint32_t lane_bits(void)
{
    static const uint32_t special[] = {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u,
                                       0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FA00000u,
                                       0x00000001u, 0x807FFFFFu, 0x3F800000u, 0xBF800000u,
                                       0x7F7FFFFFu, 0x00800000u};
    uint32_t r = rnd();
    if ((r & 7) == 0)
        return special[rnd() % (sizeof special / sizeof special[0])];
    if ((r & 7) == 1)
        return rnd();                                  /* any bits */
    float f = ((float)(int32_t)rnd() / 65536.0f) * ldexpf(1.0f, (int)(rnd() % 40) - 20);
    uint32_t b; memcpy(&b, &f, 4); return b;
}

static X rx(void) { X x; for (int i = 0; i < 4; i++) x.u[i] = lane_bits(); return x; }

static long s_fail, s_checks, s_nan_payload;

/* Which NaN comes out when both operands of an add or multiply are NaNs is
 * not something C fixes: the compiler may swap the operands of a
 * commutative operation, in either version (SSE and ARM both return the
 * first operand's). Both NaN counts as the same; the count is reported. */
static int is_nan(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu); }
static void same(const char *what, X a, X b, int iter)
{
    s_checks++;
    if (memcmp(&a, &b, 16) == 0)
        return;
    if (s_fail++ < 20)
        printf("FAIL %s (run %d): %08X %08X %08X %08X vs %08X %08X %08X %08X\n", what, iter,
               a.u[0], a.u[1], a.u[2], a.u[3], b.u[0], b.u[1], b.u[2], b.u[3]);
}

int main(void)
{
    uint8_t *mem = mmap(NULL, 1u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 2; }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;      /* guest 0 .. 1 MB */
    same("zero", lane_zero(), vec_zero(), 0);
    for (int it = 0; it < 200000; it++) {
        X a = rx(), b = rx();
        float s; uint32_t sb = lane_bits(); memcpy(&s, &sb, 4);
        same("scalar", lane_scalar(s), vec_scalar(s), it);
        same("add", lane_add(a, b), vec_add(a, b), it);
        same("sub", lane_sub(a, b), vec_sub(a, b), it);
        same("mul", lane_mul(a, b), vec_mul(a, b), it);
        same("div", lane_div(a, b), vec_div(a, b), it);
        same("and", lane_and(a, b), vec_and(a, b), it);
        same("or", lane_or(a, b), vec_or(a, b), it);
        same("xor", lane_xor(a, b), vec_xor(a, b), it);
        same("andn", lane_andn(a, b), vec_andn(a, b), it);
        same("unpack_low", lane_unpack_low(a, b), vec_unpack_low(a, b), it);
        same("unpack_high", lane_unpack_high(a, b), vec_unpack_high(a, b), it);
        uint32_t imm = (uint32_t)it & 0xFF;
        same("shuffle", lane_shuffle(a, b, imm), vec_shuffle(a, b, imm), it);
        same("shuffle (runtime imm)", lane_shuffle_rt(a, b, imm), vec_shuffle_rt(a, b, imm), it);
        /* memory: unaligned addresses, the two versions on separate copies */
        uint32_t at = 0x1000u + (rnd() & 0xFFFF);
        for (int k = 0; k < 64; k++) mem[at + k] = (uint8_t)rnd();
        same("mem", lane_mem(at), vec_mem(at), it);
        uint32_t dst = 0x40000u + (rnd() & 0xFFFF);
        lane_store(dst, a); X l; memcpy(&l, mem + dst, 16);
        memset(mem + dst, 0, 16);
        vec_store(dst, a); X v; memcpy(&v, mem + dst, 16);
        same("store", l, v, it);
        if (it % 16 == 0) {
            uint32_t src = 0x2000u + (rnd() & 0x7FFC), mat = 0x20000u + (rnd() & 0x7FFF);
            for (int k = 0; k < 12; k++) { uint32_t w = lane_bits(); memcpy(mem + src + k, &w, 1); }
            for (int k = 0; k < 64; k += 4) { uint32_t w = lane_bits(); memcpy(mem + mat + k, &w, 4); }
            uint8_t o1[36], o2[36];
            uint32_t out = 0x60000u + (rnd() & 0xFFFF);
            memset(mem + out, 0, 36); lane_seq(src, mat, out); memcpy(o1, mem + out, 36);
            memset(mem + out, 0, 36); vec_seq(src, mat, out); memcpy(o2, mem + out, 36);
            s_checks++;
            int differ = 0;
            for (int k = 0; k < 36; k += 4) {
                uint32_t p1, p2; memcpy(&p1, o1 + k, 4); memcpy(&p2, o2 + k, 4);
                if (p1 != p2 && !(is_nan(p1) && is_nan(p2)))
                    differ = 1;
                else if (p1 != p2)
                    s_nan_payload++;
            }
            if (differ && s_fail++ < 20) {
                printf("FAIL sequence (run %d):", it);
                for (int k = 0; k < 36; k += 4) {
                    uint32_t p1, p2; memcpy(&p1, o1 + k, 4); memcpy(&p2, o2 + k, 4);
                    if (p1 != p2) printf(" +%d %08X/%08X", k, p1, p2);
                }
                printf("\n");
            }
        }
    }
    printf("%ld checks, %ld failed (%ld lanes NaN in both with another payload)\n",
           s_checks, s_fail, s_nan_payload);
    return s_fail != 0;
}
