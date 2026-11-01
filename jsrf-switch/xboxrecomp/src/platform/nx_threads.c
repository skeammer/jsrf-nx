/*
 * Where host threads run on the Switch, and what they cost (nx_threads.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

#include "nx_threads.h"

#define GAME_CORE      0
#define GAME_PRIORITY  0x3A
#define FAST_PRIORITY  0x2C            /* above the guest, not time-sliced */
#define APP_CORES      0x7u            /* 0-2; 3 is the system's */

Handle threadGetCurHandle(void);       /* libnx */
extern char __start__[];               /* the ELF's base, for the report */

/* The cores this process may use, among 0-2; 0 if they cannot be read. */
static u32 app_cores(void)
{
    u64 mask = 0;

    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
        return 0;
    return (u32)mask & APP_CORES;
}

/* The cores every thread but the game's uses: the game's core is left out
 * when at least two remain. */
static u32 other_cores(u32 cores)
{
    u32 without_game = cores & ~(1u << GAME_CORE);

    if ((cores & (1u << GAME_CORE)) && __builtin_popcount(without_game) >= 2)
        return without_game;
    return cores;
}

/* The n-th core in `cores`, counting from core 0. */
static int nth_core(u32 cores, int n)
{
    int core;

    for (core = 0; core < 3; core++) {
        if (!(cores & (1u << core)))
            continue;
        if (n-- == 0)
            return core;
    }
    return -1;
}

void nx_thread_spread(void)
{
    static int dealt = 1;              /* the first thread spread gets the 2nd core */
    u32 cores = other_cores(app_cores());
    int turn;

    if (__builtin_popcount(cores) < 2)
        return;                        /* nothing to choose between */
    turn = __atomic_fetch_add(&dealt, 1, __ATOMIC_RELAXED);
    if (turn == 1)
        fprintf(stderr, "  [NX] threads dealt over cores 0x%X\n", (unsigned)cores);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE,
                         nth_core(cores, turn % __builtin_popcount(cores)), cores);
}

void nx_thread_place_game(void)
{
    u32 cores = app_cores();
    Result rc;

    if (cores == 0 || other_cores(cores) == cores) {
        /* No core to spare for it: treated like the rest. */
        nx_thread_spread();
        return;
    }
    rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, GAME_CORE, cores);
    if (R_SUCCEEDED(rc))
        rc = svcSetThreadPriority(CUR_THREAD_HANDLE, GAME_PRIORITY);
    fprintf(stderr, "  [NX] game thread: core %d, priority 0x%X; other threads: cores 0x%X"
                    " (rc 0x%X)\n", GAME_CORE, GAME_PRIORITY, (unsigned)other_cores(cores),
            (unsigned)rc);
}

int nx_thread_core(void)
{
    s32 core = -1;
    u64 affinity = 0;

    if (R_FAILED(svcGetThreadCoreMask(&core, &affinity, CUR_THREAD_HANDLE)))
        return -1;
    return core >= 0 ? core : -1;
}

int nx_thread_place_beside(int avoid)
{
    u32 cores = other_cores(app_cores());
    int core;

    if (cores == 0)
        return -1;
    /* From the top: core 0, the game's when it is in the set at all, last. */
    for (core = 2; core >= 0; core--)
        if ((cores & (1u << core)) && core != avoid)
            break;
    if (core < 0 || __builtin_popcount(cores) < 2) {
        nx_thread_spread();
        return -1;
    }
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, cores);
    return core;
}

void nx_thread_yield(void)
{
    /* sched_yield may move the thread to another core; a zero sleep only
     * lets this core's other threads at this priority have a turn. */
    svcSleepThread(0);
}

int nx_thread_set_priority(int priority)
{
    return R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority));
}

static int turned_off(const char *setting)
{
    const char *value = getenv(setting);
    return value && value[0] == '0';
}

void nx_thread_raise_audio(void)
{
    /* At 59 the APU frame thread waited out other threads' 10 ms slices and
     * fell behind real time when the game kept every core busy. It sleeps
     * between blocks, so running unsliced costs the others little. */
    if (!turned_off("RECOMP_NX_AUDIO_PRIO"))
        nx_thread_set_priority(FAST_PRIORITY);
}

void nx_thread_raise_timer(void)
{
    /* The vblank, ISRs, DPCs and kernel timers are the Xbox's interrupt
     * side. At 59 they waited out the guest's slices: in JSRF's intro the
     * vblank came over half a frame late often enough that 4% were dropped,
     * and CRI's ADX pumps, which decode once per vblank, starved the music.
     * The thread blocks between ticks. */
    if (turned_off("RECOMP_NX_TIMER_PRIO"))
        return;
    if (nx_thread_set_priority(FAST_PRIORITY))
        fprintf(stderr, "  [NX] timer thread (vblank, ISRs, DPCs) at priority 0x%X\n",
                FAST_PRIORITY);
}

/* ---- The per-thread CPU report ----------------------------------------- */

#define MAX_THREADS 64

typedef struct {
    Handle    handle;
    uintptr_t entry;                   /* 0: the game's main thread */
    u64       ticks_before;            /* at the previous report */
} TrackedThread;

static TrackedThread g_threads[MAX_THREADS];
static int           g_tracked;        /* slots taken (may pass MAX_THREADS) */

static int tracked_count(void)
{
    int n = __atomic_load_n(&g_tracked, __ATOMIC_ACQUIRE);
    return n < MAX_THREADS ? n : MAX_THREADS;
}

void nx_thread_track(void *entry)
{
    int slot = __atomic_fetch_add(&g_tracked, 1, __ATOMIC_ACQ_REL);

    if (slot >= MAX_THREADS)
        return;
    g_threads[slot].entry = (uintptr_t)entry;
    g_threads[slot].handle = threadGetCurHandle();
}

void nx_thread_rename(void *entry)
{
    Handle self = threadGetCurHandle();
    int i, n = tracked_count();

    for (i = 0; i < n; i++) {
        if (g_threads[i].handle == self) {
            g_threads[i].entry = (uintptr_t)entry;
            return;
        }
    }
}

int nx_thread_at(int i, uint32_t *handle, uintptr_t *entry)
{
    if (i < 0 || i >= tracked_count() || g_threads[i].handle == 0)
        return 0;
    *handle = g_threads[i].handle;
    *entry = g_threads[i].entry;
    return 1;
}

/* The thread's run time so far, in system ticks. */
static int thread_ticks(Handle thread, u64 *ticks)
{
    if (R_SUCCEEDED(svcGetInfo(ticks, InfoType_ThreadTickCount, thread, (u64)-1)))
        return 1;
    return R_SUCCEEDED(svcGetInfo(ticks, InfoType_ThreadTickCountDeprecated, thread, (u64)-1));
}

/* One line: each thread that used at least 1% of a core since the last
 * report, as name=percent@core/priority. Names are entry points relative to
 * the ELF's base (aarch64-none-elf-addr2line -f -e <elf> 0x<name>), or
 * "game". The core is the one Horizon prefers for it; priorities are hex
 * (3B is the guest's time-sliced 59). */
void nx_thread_report(double seconds)
{
    char line[1024];
    double per_core = (double)armGetSystemTickFreq() * seconds;
    int n = tracked_count(), unreadable = 0, len, i;

    len = snprintf(line, sizeof line, "[perf] threads, %% of a core each (%d tracked):", n);
    for (i = 0; i < n && len < (int)sizeof line - 48; i++) {
        TrackedThread *t = &g_threads[i];
        u64 ticks = 0, used, affinity = 0;
        s32 core = -1, priority = -1;
        double percent;

        if (t->handle == 0 || !thread_ticks(t->handle, &ticks)) {
            unreadable++;
            continue;
        }
        used = ticks - t->ticks_before;
        t->ticks_before = ticks;
        percent = 100.0 * (double)used / per_core;
        if (percent < 1.0)
            continue;
        if (R_FAILED(svcGetThreadCoreMask(&core, &affinity, t->handle)))
            core = -1;
        if (R_FAILED(svcGetThreadPriority(&priority, t->handle)))
            priority = -1;
        if (t->entry)
            len += snprintf(line + len, sizeof line - len, " %lx", (unsigned long)(t->entry -
                            (uintptr_t)__start__));
        else
            len += snprintf(line + len, sizeof line - len, " game");
        len += snprintf(line + len, sizeof line - len, "=%.0f@%d/%X", percent, (int)core,
                        (unsigned)priority);
    }
    if (n > 0 && unreadable == n)
        snprintf(line + len, sizeof line - len, " (thread run times unavailable)");
    fprintf(stderr, "%s\n", line);
}
