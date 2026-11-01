/* RECOMP_SPIN_HINT: the back edge of a hardware poll loop (marked after
 * lifting by NFSU2's spin_hint pass). A pause on every turn, the busy-bit
 * acknowledgements answered on this thread every 16, and a host yield every
 * 64 -- a Switch core does not time-slice a spinning thread against the one
 * it waits for. */
#if defined(_MSC_VER)
#define RECOMP_SPIN_HINT() _mm_pause()
#else
void recomp_spin_wake(void);
void recomp_spin_yield(void);
static inline void recomp_spin_hint(void)
{
    static RECOMP_TLS unsigned turns;
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
    if ((++turns & 15u) == 0)
        recomp_spin_wake();
    if ((turns & 63u) == 0)
        recomp_spin_yield();
}
#define RECOMP_SPIN_HINT() recomp_spin_hint()
#endif

