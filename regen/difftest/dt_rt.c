/* Differential test runtime for regen/jsrf_reg_locals.py.
 *
 * Every lifted function is compiled twice into the same program: as it was
 * (NAME__o) and as the pass rewrote it (NAME__n). Each runs from the same
 * guest memory and registers; every call it makes lands in a stub that logs
 * the registers it sees and then changes them (and the x87 top, and memory)
 * as a function of what it saw. Stores through the memory-watch seam, MMIO,
 * the indirect-call failure logs and the spin hint are logged too, and at the
 * end the registers and a hash of all of guest memory. The two logs must be
 * identical. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>
#include "recomp_types.h"

/* ---- guest state the generated code expects --------------------------- */
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo = 0x00010000u, g_xbox_code_hi = 0x00420000u;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint32_t g_fs_base, g_seh_ebp, g_ebp;
RECOMP_TLS int g_df;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F;
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;
volatile uint64_t g_icall_guard_hits, g_icall_guard_misses;
volatile int g_gil_contended;
int g_recomp_mem_watch_enabled = 1;

#define MEM_BYTES ((size_t)1 << 32)
#define MEM_SLACK ((size_t)1 << 20)
static uint8_t *mem;

/* ---- the log ----------------------------------------------------------- */
#define EV_CAP (1u << 22)
static uint64_t *ev;
static size_t nev;
static sigjmp_buf jb;
static unsigned long ncalls, npre, nspin, nsteps;

enum { AB_NONE, AB_PREEMPT, AB_CALLS, AB_SPIN, AB_LOG, AB_SIGNAL, AB_STEPS };
static void dt_abort(int why) { siglongjmp(jb, why); }

static void E(uint64_t v)
{
    if (nev >= EV_CAP)
        dt_abort(AB_LOG);
    ev[nev++] = v;
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t fp_hash(void)
{
    uint64_t h = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t b;
        memcpy(&b, &g_fp_stack[i], 8);
        h = mix64(h ^ b ^ (uint64_t)i << 56);
    }
    return h;
}

/* The SSE and MMX registers and the x87 compare state: what
 * jsrf_simd_locals.py keeps in locals. */
static uint64_t simd_hash(void)
{
    const RecompXmm *x[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    const RecompMmx *m[8] = {&g_mm0, &g_mm1, &g_mm2, &g_mm3, &g_mm4, &g_mm5, &g_mm6, &g_mm7};
    uint64_t h = (uint64_t)(uint32_t)g_fp_cmp << 16 ^ g_fp_cc;
    for (int i = 0; i < 8; i++) {
        uint64_t b[2], c;
        memcpy(b, x[i], 16);
        memcpy(&c, m[i], 8);
        h = mix64(h ^ b[0] ^ (uint64_t)i << 56);
        h = mix64(h ^ b[1] ^ (uint64_t)i << 48);
        h = mix64(h ^ c ^ (uint64_t)i << 40);
    }
    return h;
}

static void E_regs(void)
{
    E(g_eax | (uint64_t)g_ecx << 32);
    E(g_edx | (uint64_t)g_ebx << 32);
    E(g_esi | (uint64_t)g_edi << 32);
    E(g_esp | (uint64_t)(uint32_t)g_fp_top << 32);
    E(fp_hash());
    E(simd_hash());
}

/* DT_NO_NAN=1: no word of the random state is a float NaN or infinity (nor
 * the high word of a double one). Which NaN an operation returns when both
 * operands are NaNs, and whether a float's NaN comes back quiet, are not
 * fixed by C: a pass that lets the compiler see more of the arithmetic can
 * change them without changing anything else. Run with and without. */
static int s_no_nan;
static uint32_t no_nan(uint32_t v)
{
    if (s_no_nan && (v & 0x7F800000u) == 0x7F800000u)
        v &= ~0x40000000u;
    return v;
}

/* ---- what the generated code calls ------------------------------------- */
#define MAX_CALLS 3000
#define MAX_PRE 200000
#define MAX_SPIN 2000

void dt_preempt(void)
{
    if (++npre > MAX_PRE)
        dt_abort(AB_PREEMPT);
}

int dt_preempt_room(unsigned long n) { return npre + n <= MAX_PRE; }
void dt_preempt_n(unsigned long n) { npre += n; }

#define MAX_STEPS 4000000ul      /* rep-loop turns, plus bytes / 64 copied */

int dt_step(void)
{
    if (++nsteps > MAX_STEPS)
        dt_abort(AB_STEPS);
    return 1;
}

#undef memset
#undef memcpy
void *dt_memset(void *d, int c, size_t n)
{
    nsteps += n / 64;
    if (nsteps > MAX_STEPS)
        dt_abort(AB_STEPS);
    return memset(d, c, n);
}

void *dt_memcpy(void *d, const void *s, size_t n)
{
    nsteps += n / 64;
    if (nsteps > MAX_STEPS)
        dt_abort(AB_STEPS);
    return memcpy(d, s, n);
}

void dt_spin_hint(void)
{
    E(0x5917000000000000ULL);
    E_regs();
    if (++nspin > MAX_SPIN)
        dt_abort(AB_SPIN);
}

/* A callee: log what it sees, then change it, as real code would -- pop the
 * return address (and sometimes stdcall arguments), return in eax/edx,
 * clobber ecx, now and then disturb the callee-saved registers too, push or
 * pop the x87 stack, write guest memory. */
void dt_callee(uint32_t id, uint32_t kind)
{
    if (++ncalls > MAX_CALLS)
        dt_abort(AB_CALLS);
    E(0xCA11000000000000ULL | (uint64_t)kind << 32 | id);
    E_regs();
    uint64_t h = mix64(((uint64_t)id << 32 | kind) ^ g_eax * 0x9E3779B97F4A7C15ULL
                       ^ (uint64_t)g_ecx << 7 ^ (uint64_t)g_edx << 13
                       ^ (uint64_t)g_ebx << 19 ^ (uint64_t)g_esi << 23
                       ^ (uint64_t)g_edi << 29 ^ (uint64_t)g_esp << 31
                       ^ (uint64_t)g_fp_top << 61 ^ ncalls);
    g_esp += 4;
    if ((h >> 10) & 1)
        g_esp += 4 * ((h >> 8) & 3);
    g_eax = no_nan((uint32_t)h);
    g_ecx = no_nan((uint32_t)(h >> 32));
    g_edx = no_nan((uint32_t)(h >> 16));
    if ((h & 7) == 0) {
        g_ebx = (uint32_t)(h >> 20);
        g_esi ^= 0x55u;
        g_edi += 0x100u;
    }
    switch ((h >> 40) & 3) {
    case 1: g_fp_top = (g_fp_top + 7) & 7; g_fp_stack[g_fp_top] = (double)(h & 0xFFF); break;
    case 2: g_fp_top = (g_fp_top + 1) & 7; break;
    default: break;
    }
    /* A callee changes what it returns in, and its scratch registers: st(0)
     * (as one that stores over the caller's top would), xmm0/xmm1, mm0, the
     * x87 compare result. */
    if ((h >> 44) & 1)
        g_fp_stack[g_fp_top] = (double)((h >> 45) & 0xFFF) / 8.0;
    if ((h >> 46) & 1) {
        g_xmm0.f[0] = (float)((h >> 47) & 0xFFF); g_xmm1.f[1] = (float)((h >> 50) & 0xFF);
        g_fp_cmp = (int)((h >> 52) & 3) - 1; g_fp_cc = (uint16_t)((h >> 54) & 0x4500);
        memcpy(&g_mm0, &h, 8);
    }
    *(uint32_t *)(mem + 0x00400000u + ((h >> 24) & 0x3FFFCu)) = no_nan((uint32_t)(h >> 3));
}

static uint32_t s_pending_va;
static void icall_tramp(void) { dt_callee(s_pending_va, 2); }
static void kcall_tramp(void) { dt_callee(s_pending_va, 3); }

recomp_func_t recomp_lookup(uint32_t va)
{
    E(0x100C000000000000ULL | va);
    if (mix64(va) % 5 == 0)
        return NULL;
    s_pending_va = va;
    return icall_tramp;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    if (va >= 0xFE000000u && (va & 1)) {
        s_pending_va = va;
        return kcall_tramp;
    }
    return NULL;
}
void recomp_icall_fail_log(uint32_t va) { E(0xFA11000000000000ULL | va); E_regs(); }
void recomp_icall_not_code_log(uint32_t va) { E(0xFA12000000000000ULL | va); E_regs(); }
void recomp_itail_fail_log(uint32_t va) { E(0xFA13000000000000ULL | va); E_regs(); }
void recomp_unimpl(const char *text, uint32_t va) { (void)text; E(0x0B1A000000000000ULL | va); E_regs(); }
void recomp_trace_enter(const char *n, uint32_t va) { (void)n; E(0x7E00000000000000ULL | va); E_regs(); }
void recomp_trace_exit(const char *n, uint32_t va) { (void)n; E(0x7E01000000000000ULL | va); E_regs(); }
void recomp_trace_esp(const char *n, const char *t) { (void)n; (void)t; E(0x7E02000000000000ULL); E_regs(); }
void recomp_debug_service(uint32_t s, uint32_t a)
{
    E(0xDEB0000000000000ULL);
    E(s | (uint64_t)a << 32);
    E_regs();
}
void recomp_spin_wake(void) { E(0x5918000000000000ULL); E_regs(); }
void recomp_spin_yield(void) { E(0x5919000000000000ULL); E_regs(); }
void recomp_preempt(void) {}
static uint64_t s_tsc;
uint64_t xbox_ReadTimeStampCounter(void) { E(0x75C0000000000000ULL); return s_tsc += 1000; }
uint64_t recomp_rdtsc(void) { E(0x75C1000000000000ULL); return s_tsc += 1000; }
static uint32_t s_mmio_n;
uint32_t xbox_mmio_read(uint32_t va, unsigned size)
{
    E(0x3310000000000000ULL | va);
    E(size);
    return no_nan((uint32_t)mix64(va ^ (uint64_t)++s_mmio_n << 32));
}
void xbox_mmio_write(uint32_t va, uint32_t v, unsigned size)
{
    E(0x3311000000000000ULL | va);
    E(v | (uint64_t)size << 32);
}
void recomp_mem_watch_guest_store(uint32_t pc, uint32_t fn, uint32_t va, unsigned width,
                                  volatile void *host, uint64_t value)
{
    (void)fn;
    memcpy((void *)host, &value, width);
    E(0x5703000000000000ULL | pc);
    E(va | (uint64_t)width << 32);
    E(value);
}
jmp_buf *recomp_setjmp_slot(uint32_t v) { (void)v; abort(); }
int recomp_guest_longjmp(uint32_t a, uint32_t b) { (void)a; (void)b; abort(); }

/* ---- runs --------------------------------------------------------------- */
static uint64_t s_rng;
static uint32_t rnd(void) { s_rng = mix64(s_rng + 0x9E3779B97F4A7C15ULL); return (uint32_t)s_rng; }

static uint32_t plausible_any(void);
static uint32_t plausible(void) { return no_nan(plausible_any()); }

static uint32_t plausible_any(void)
{
    uint32_t r = rnd();
    switch (r & 7) {
    case 0: case 1: case 2: return 0x00400000u + (rnd() & 0x3FFFCu);    /* data */
    case 3: return 0x00BF0000u + (rnd() & 0x1FFFCu);                   /* stack */
    case 4: return 0x00010000u + (rnd() & 0x3FFFF0u);                  /* "code" */
    case 5: return rnd() & 0xFF;
    case 6: return 0;
    default: return rnd();
    }
}

static void seed(uint64_t s)
{
    s_rng = s;
    madvise(mem, MEM_BYTES + MEM_SLACK, MADV_DONTNEED);
    for (uint32_t a = 0x00400000u; a < 0x00440000u; a += 4)
        *(uint32_t *)(mem + a) = plausible();
    for (uint32_t a = 0x00BF0000u; a < 0x00C10000u; a += 4)
        *(uint32_t *)(mem + a) = plausible();
    for (uint32_t a = 0x00001000u; a < 0x00001100u; a += 4)
        *(uint32_t *)(mem + a) = plausible();
    g_eax = plausible(); g_ecx = plausible(); g_edx = plausible();
    g_ebx = plausible(); g_esi = plausible(); g_edi = plausible();
    g_esp = 0x00C00000u - (rnd() & 0xFF0u);
    g_ebp = g_esp + 0x40; g_seh_ebp = g_ebp;
    g_fs_base = 0x00001000u;
    g_df = (rnd() & 15) == 0;
    g_fp_top = rnd() & 7;
    for (int i = 0; i < 8; i++)
        g_fp_stack[i] = (double)(int32_t)rnd() / 64.0;
    g_fp_control_word = 0x027F; g_fp_cmp = (int)(rnd() % 3) - 1; g_fp_cc = (uint16_t)(rnd() & 0x4500);
    RecompMmx *mm[8] = {&g_mm0, &g_mm1, &g_mm2, &g_mm3, &g_mm4, &g_mm5, &g_mm6, &g_mm7};
    for (int i = 0; i < 8; i++)
        for (unsigned j = 0; j < sizeof *mm[i] / 4; j++)
            ((uint32_t *)mm[i])[j] = no_nan(rnd());
    RecompXmm *x[8] = {&g_xmm0, &g_xmm1, &g_xmm2, &g_xmm3, &g_xmm4, &g_xmm5, &g_xmm6, &g_xmm7};
    for (int i = 0; i < 8; i++)
        for (unsigned j = 0; j < sizeof *x[i] / 4; j++)
            ((uint32_t *)x[i])[j] = no_nan(rnd());
    ncalls = npre = nspin = nsteps = 0;
    s_tsc = 0; s_mmio_n = 0; s_pending_va = 0;
    nev = 0;
}

static uint64_t mem_hash(void)
{
    static unsigned char vec[(MEM_BYTES + MEM_SLACK) / 4096];
    uint64_t h = 0;
    if (mincore(mem, MEM_BYTES + MEM_SLACK, vec) != 0)
        return 1;
    for (size_t p = 0; p < sizeof vec; p++) {
        if (!(vec[p] & 1))
            continue;
        const uint64_t *w = (const uint64_t *)(mem + p * 4096);
        uint64_t ph = 0;
        int nz = 0;
        for (int i = 0; i < 512; i++) {
            if (w[i]) nz = 1;
            ph = mix64(ph ^ w[i] ^ (uint64_t)i << 48);
        }
        if (nz)
            h += mix64(ph ^ p);
    }
    return h;
}

static void on_signal(int sig) { (void)sig; siglongjmp(jb, AB_SIGNAL); }

/* Run one version; the log ends with how it ended. An aborted run (too
 * long, a host trap) is compared up to the abort: its registers may still
 * be in the rewritten version's locals. */
static size_t run(void (*fn)(void), uint64_t s, uint64_t *out, int *why)
{
    ev = out;
    seed(s);
    int r = sigsetjmp(jb, 1);
    if (r == 0) {
        fn();
        E(0xE0D0000000000000ULL);
        E_regs();
        E(g_ebp | (uint64_t)g_seh_ebp << 32);
    }
    *why = r;
    ev = out;
    if (nev + 8 > EV_CAP)
        nev = EV_CAP - 8;
    E(0xAB00000000000000ULL | (uint32_t)r);
    E(npre);
    E(mem_hash());
    return nev;
}

struct dt_fn { const char *name; void (*o)(void); void (*n)(void); };
extern const struct dt_fn *const dt_tables[];
extern const int dt_table_sizes[];
extern const int dt_ntables;

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    int seeds = argc > 2 ? atoi(argv[2]) : 3;
    s_no_nan = getenv("DT_NO_NAN") && atoi(getenv("DT_NO_NAN"));
    mem = mmap(NULL, MEM_BYTES + MEM_SLACK, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }
    g_xbox_mem_offset = (ptrdiff_t)mem;
    signal(SIGFPE, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGILL, on_signal);
    uint64_t *a = malloc(EV_CAP * 8), *b = malloc(EV_CAP * 8);
    long nfn = 0, nrun = 0, nfail = 0, nab[8] = {0}, nfn_called = 0, nrun_called = 0;
    unsigned long long events = 0;
    for (int t = 0; t < dt_ntables; t++)
        for (int i = 0; i < dt_table_sizes[t]; i++) {
            const struct dt_fn *f = &dt_tables[t][i];
            if (only && strcmp(only, "all") && strcmp(only, f->name))
                continue;
            nfn++;
            int called = 0;
            for (int k = 0; k < seeds; k++) {
                uint64_t s = mix64((uint64_t)(t * 100000 + i) << 8 | k);
                int wa, wb;
                size_t na = run(f->o, s, a, &wa);
                if (ncalls) { called = 1; nrun_called++; }
                size_t nb = run(f->n, s, b, &wb);
                nrun++;
                nab[wa & 7]++;
                events += na;
                if (na != nb || memcmp(a, b, na * 8) || wa != wb) {
                    size_t j = 0;
                    while (j < na && j < nb && a[j] == b[j]) j++;
                    printf("MISMATCH %s seed %d: lengths %zu/%zu, ends %d/%d, first difference at %zu\n",
                           f->name, k, na, nb, wa, wb, j);
                    for (size_t q = j > 6 ? j - 6 : 0; q < j + 6 && (q < na || q < nb); q++)
                        printf("   %6zu  %016llx  %016llx%s\n", q,
                               q < na ? (unsigned long long)a[q] : 0ULL,
                               q < nb ? (unsigned long long)b[q] : 0ULL,
                               q == j ? "  <--" : "");
                    nfail++;
                    break;
                }
            }
            nfn_called += called;
            fflush(stdout);
        }
    printf("%ld functions, %ld runs, %ld mismatched; ended normally %ld, preempt cap %ld, "
           "call cap %ld, spin cap %ld, log cap %ld, host trap %ld, step cap %ld; "
           "%llu events; a call reached in %ld runs, in %ld functions\n",
           nfn, nrun, nfail, nab[0], nab[1], nab[2], nab[3], nab[4], nab[5], nab[6], events,
           nrun_called, nfn_called);
    return nfail != 0;
}
