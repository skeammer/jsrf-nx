/* Per-site indirect-call cache (RECOMP_ICALL_LOOKUP): under four threads
 * calling through shared sites whose targets change all the time, every call
 * must reach the function its VA names, and kernel thunks must be looked up
 * (they select the thunk) every time. */
#include "recomp_types.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NF 64
static __thread uint32_t t_expect;
static __thread unsigned long t_bad, t_calls;
static volatile unsigned long s_kernel_lookups;
#define F(i) static void f##i(void) { t_calls++; if (t_expect != 0x00100000u + (i) * 16u) t_bad++; }
F(0)
F(1)
F(2)
F(3)
F(4)
F(5)
F(6)
F(7)
F(8)
F(9)
F(10)
F(11)
F(12)
F(13)
F(14)
F(15)
F(16)
F(17)
F(18)
F(19)
F(20)
F(21)
F(22)
F(23)
F(24)
F(25)
F(26)
F(27)
F(28)
F(29)
F(30)
F(31)
F(32)
F(33)
F(34)
F(35)
F(36)
F(37)
F(38)
F(39)
F(40)
F(41)
F(42)
F(43)
F(44)
F(45)
F(46)
F(47)
F(48)
F(49)
F(50)
F(51)
F(52)
F(53)
F(54)
F(55)
F(56)
F(57)
F(58)
F(59)
F(60)
F(61)
F(62)
F(63)
static recomp_func_t s_f[NF] = { f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63 };
static __thread int t_slot = -1;
static void kthunk(void) { t_calls++; if (t_slot != (int)(t_expect - 0xFE000000u) / 4) t_bad++; }

/* The three lookups, as the runtime has them: */
recomp_func_t recomp_lookup_manual(uint32_t va) { return va == 0x00100000u + 5 * 16 ? s_f[5] : 0; }
recomp_func_t recomp_lookup(uint32_t va)
{
    uint32_t o = va - 0x00100000u;
    return o < NF * 16 && o % 16 == 0 && o != 5 * 16 ? s_f[o / 16] : 0;
}
recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    if (va < 0xFE000000u || va >= 0xFE000400u) return 0;
    __atomic_add_fetch(&s_kernel_lookups, 1, __ATOMIC_RELAXED);
    t_slot = (int)(va - 0xFE000000u) / 4;
    return kthunk;
}
#include "resolve.inc"

static uint32_t rnd(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

/* Two call sites, each hit with whatever VA comes along. */
static void site_a(uint32_t va) { recomp_func_t fn; t_expect = va; RECOMP_ICALL_LOOKUP(va, fn); if (fn) fn(); }
static void site_b(uint32_t va) { recomp_func_t fn; t_expect = va; RECOMP_ICALL_LOOKUP(va, fn); if (fn) fn(); }

static unsigned long s_bad, s_calls, s_kcalls;
static void *worker(void *arg)
{
    uint32_t s = (uint32_t)(uintptr_t)arg, i, k = 0;
    for (i = 0; i < 4000000; i++) {
        uint32_t r = rnd(&s), va;
        if (r % 97 == 0) { va = 0xFE000000u + (r % 256) * 4; k++; }
        else va = 0x00100000u + ((r % 3 == 0) ? (r % NF) : (r % 4)) * 16;   /* mostly a few */
        if (r & 1) site_a(va); else site_b(va);
    }
    __atomic_add_fetch(&s_bad, t_bad, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_calls, t_calls, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_kcalls, k, __ATOMIC_RELAXED);
    return NULL;
}

int main(void)
{
    pthread_t t[4];
    int i;
    for (i = 0; i < (getenv("ONE") ? 1 : 4); i++) pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i * 7919 + 1));
    for (i = 0; i < (getenv("ONE") ? 1 : 4); i++) pthread_join(t[i], NULL);
    printf("%lu calls, %lu to the wrong function, %lu kernel calls, %lu kernel lookups\n",
           s_calls, s_bad, s_kcalls, s_kernel_lookups);
    int ok = s_calls == 16000000 && !s_bad && s_kcalls == s_kernel_lookups;
    puts(ok ? "PASS" : "FAIL");
    return !ok;
}
