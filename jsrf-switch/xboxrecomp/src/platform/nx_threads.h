/*
 * Where host threads run on the Switch, and what they cost.
 *
 * The application has cores 0-2 (core 3 is the system's). Horizon
 * time-slices only priority 59 (0x3B), libnx's default, in 10 ms slices, and
 * a new pthread prefers the process's default core, so left alone every
 * thread queues on core 0 and reaches the others only by migrating.
 *
 * The layout used here:
 *   - the game's main thread prefers core 0, one step above 59 (0x3A), so
 *     a thread Horizon moved there while the game slept yields as soon as it
 *     wakes;
 *   - every other thread is dealt a preferred core from 1 and 2 in turn,
 *     still allowed on both;
 *   - threads with deadlines that sleep between them (audio, the kernel's
 *     timer thread) go above 59, where they are not sliced.
 */
#ifndef NX_THREADS_H
#define NX_THREADS_H

#include <stdint.h>

/* For the game's main thread, at its start. */
void nx_thread_place_game(void);

/* For any other thread, at its start: a preferred core from the others. */
void nx_thread_spread(void);

/* The core this thread prefers, or -1. */
int  nx_thread_core(void);

/* Prefer a core other than `avoid` (and other than the game's); for a thread
 * that works in step with the one on `avoid`. Returns the core, or -1 if it
 * was spread as usual. */
int  nx_thread_place_beside(int avoid);

/* Let another thread on this core run, without leaving the core. */
void nx_thread_yield(void);

/* This thread at host priority `priority` (lower is more urgent). 1 if set. */
int  nx_thread_set_priority(int priority);

/* The APU frame thread (RECOMP_NX_AUDIO_PRIO=0 leaves it at 59) and the
 * kernel's timer thread (RECOMP_NX_TIMER_PRIO=0), above the guest's threads. */
void nx_thread_raise_audio(void);
void nx_thread_raise_timer(void);

/* The per-thread CPU report. A thread is recorded at its start with the
 * function it runs (NULL: the game's main thread); a guest thread, which
 * starts in the bridge's trampoline, is renamed by the lifted function it
 * runs. nx_thread_at gives thread i's kernel handle and name for the
 * sampling profiler, 0 past the last. */
void nx_thread_track(void *entry);
void nx_thread_rename(void *entry);
int  nx_thread_at(int i, uint32_t *handle, uintptr_t *entry);
void nx_thread_report(double seconds);

#endif
