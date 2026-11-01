/*
 * Hand-written replacements for guest functions this runtime cannot execute.
 *
 * Fed to the recompiler as --exclude-manual, so it does not generate a body for
 * anything defined here and the direct calls in the generated code link to
 * these instead. That is the project's sanctioned way to replace a guest
 * function that is reached by a DIRECT call; recomp_lookup_manual() only ever
 * sees indirect ones.
 *
 * Register conventions: RECOMP_GENERATED_CODE is not defined in this file, so
 * the eax/esp aliases are inactive and the guest register file is addressed as
 * g_eax / g_esp. A generated `ret N` epilogue is `esp += 4 + N; return;` -- the
 * 4 pops the return address the caller pushed.
 */

#include <stdint.h>
#include "recomp_types.h"

/*
 * sub_001A1769 - DSOUND: submit a command block to the MCPX APU and wait.
 *
 *     0x001A18C1  push 3 / pop eax
 *     0x001A18CE  mov  [obj+0x810], eax     ; submit
 *   L:0x001A18D0  cmp  [obj+0x810], 0
 *     0x001A18D3  jne  L                    ; wait for the APU to consume it
 *
 * Nothing in this runtime consumes that ring. src/apu is initialised and its
 * registers are routed, but src/apu/apu_dsp.c is an explicit stub -- the GP
 * processor that would drain the block is ~3000 lines of DSP56300 emulation
 * that has never been written, upstream included. So the guest waits forever;
 * this was the hang in every run.
 *
 * Replaced rather than emulated. The block carries DSP effects work (reverb,
 * EQ) which is bypassed either way, so skipping the submission loses nothing
 * that was going to happen. It is a substitution of the same kind the project
 * already makes for D3D and the kernel: the guest's hardware driver is not the
 * thing we want to run.
 *
 * THIS MAKES AUDIO SILENT AND IS NOT A FIX FOR AUDIO. It exists so that audio
 * stops gating startup and the rest of the title can be reached. The real fix
 * is to override DirectSound at its interface with src/audio/dsound_device.c;
 * see section 1 of CODEX_HANDOVER.txt.
 *
 * __thiscall, one stack argument, returns an HRESULT in eax. The caller at
 * 0x001A1A50 tests it and bails on a negative value, so report success.
 */
void sub_001A1769(void)
{
    g_eax = 0;          /* S_OK */
    g_esp += 8;         /* ret 4 */
}

/* sub_001A308E (DSOUND's wait for a stopped voice to leave the voice lists)
 * and sub_001A1ED7 (the DPC's voice-list walk) are at the end of this file,
 * in the DirectSound voice-list section. */

/* ── CRI ADX lock/unlock: restore the uniprocessor guarantee ──────────────
 *
 * sub_0013B0A0 and sub_0013B0E0 are CRI's ADX lock and unlock. They protect
 * two GLOBAL guest words with no atomics and no per-thread storage:
 *
 *     0x0025EFA0   the lock refcount
 *     0x0027D0F8   ONE slot holding the saved priority of whoever locked
 *
 * That is safe on an Xbox and only on an Xbox. The lock's first act is to
 * raise the CALLER to base priority 16 (XAPI maps 15 -> 16), and on a single
 * CPU a thread at 16 cannot be preempted by another guest thread at all. The
 * elevation IS the mutual exclusion; the refcount never needs to be atomic
 * because only one thread can ever reach it.
 *
 * This runtime records priority and does not enact it -- a frozen session on
 * 21 Sep 2026 reported `priority applied=0 failed=0` -- and guest threads run
 * concurrently on real cores. Two of them (tid 1006 and 1008) interleave:
 *
 *     A locks    count 0->1, A raised to 16, A's real priority saved
 *     B locks    count 1->2, the `jne` skips, B untouched
 *     A unlocks  count 2->1, the `jne` skips -- A IS STILL AT 16
 *     B unlocks  count 1->0, restores A's saved value onto B
 *     A locks    A is already at 16, so it saves 15. POISONED.
 *
 * From there CRI's own guard on file I/O (sub_001437B0) spins while
 * GetThreadPriority(self) reads 15, calling this unlock every pass. The one
 * pass where the count reaches zero restores 15 -> base 16 -> no change, the
 * count runs negative, and both routines become permanent no-ops because both
 * open with a `jne` on it. The thread can never leave 16. Measured at the
 * freeze: lock_count=-32680066 against queries=32680727 -- the same loop
 * driving both.
 *
 * SERIALISING THE TWO BODIES AGAINST EACH OTHER DOES NOT FIX THIS, and the
 * first version of this override did exactly that. Read the interleave above
 * again: every one of its five steps is a complete call, and they are already
 * in a serial order. A mutex taken and dropped inside each body permits that
 * order unchanged, so the poison still forms. What the elevation prevented was
 * B ENTERING THE REGION WHILE A IS INSIDE IT, which is a statement about the
 * span between A's lock and A's unlock -- not about either body.
 *
 * So the guard is taken by the lock and released by the MATCHING UNLOCK, with
 * the guest's own work in between. Under it B's lock blocks until A's unlock
 * and the interleave cannot be written down. adx_guard.c holds the mechanism
 * and the reasons for its three properties -- recursive, owner-tracked,
 * bounded -- because the unmatched unlocks in the spin at sub_001437B0 and a
 * lock that is never released both need an answer that is not a deadlock.
 *
 * NOT a general fix for priority being unenacted. This restores exclusion for
 * exactly the routine pair that was measured to need it; anything else in the
 * title relying on TIME_CRITICAL for exclusion is still exposed.
 *
 * Both are cdecl with no arguments, so the epilogue is `g_esp += 4`. The
 * bodies below are transcribed instruction-for-instruction from the generated
 * ones; the additions are the guard calls and, since G66, the per-thread
 * ledger's three decisions (adx_guard.h, WHOSE PRIORITY THE ONE SLOT HOLDS):
 *
 *   entry           a thread OWED its own pre-raise priority collects it
 *                   (RECOMP_ADX_PER_THREAD_RESTORE)
 *   unlock at <= 0  not applied; a caller that holds nothing and reads 15 is
 *                   given its own last pre-raise priority back
 *                   (RECOMP_ADX_UNMATCHED_SAFE)
 *   unlock to 0     the slot is restored onto the unlocking thread only if it
 *                   is the thread that raised (RECOMP_ADX_PER_THREAD_RESTORE)
 *
 * The lock's write of 0x0027D0F8 is untouched: the guest reads that slot and
 * must go on finding what it wrote there. RECOMP_ADX_TRACE records each call.
 */
#include "adx_guard.h"

/* The XAPI entry points this pair calls. recomp_funcs.h is generated and is
 * not included here, so name them directly; the generated bodies are what
 * these resolve to at link time. */
void sub_00147CC0(void);     /* XAPILIB::SetThreadPriority */
void sub_00147D12(void);     /* XAPILIB::GetThreadPriority */
void sub_00147DAC(void);     /* XAPILIB::SuspendThread     */
void sub_00147DD2(void);     /* XAPILIB::ResumeThread      */

static int adx_read_count(void) { return (int)(int32_t)MEM32(0x25EFA0); }

/* The trace's guest context (adx_guard.h, WHO, BEYOND THE HOST THREAD). Every
 * lock and unlock arrives through CRI's wrappers sub_00141B60/sub_00141B80,
 * which push one argument before the call, so MEM32(esp) is the wrapper and
 * MEM32(esp+8) the wrapper's caller; beyond that there is no frame chain in
 * this code, so the rest of the chain is the next code-looking dwords on the
 * guest stack -- a scan, good enough to name a caller, not a backtrace. */
__attribute__((weak)) unsigned xbox_IrqContextBits(void) { return 0; }
static void adx_fill_ctx(struct adx_guest_ctx *c)
{
    uint32_t esp = g_esp, a, v;
    int n = 0;
    c->esp = esp;
    c->tib = g_fs_base;
    c->irq = xbox_IrqContextBits();
    c->outer = (int)(int32_t)MEM32(0x25EFEC);
    if (!esp) return;
    a = esp + 8;                     /* skip the wrapper and its argument */
    for (; n < ADX_CTX_STACK && a < esp + 0x400u; a += 4) {
        v = MEM32(a);
        if (g_xbox_code_hi && v >= g_xbox_code_lo && v < g_xbox_code_hi)
            c->stack[n++] = v;
    }
}

/* SetThreadPriority(NtCurrentThread, prio) / GetThreadPriority(NtCurrentThread)
 * through the guest's own XAPI, exactly as the bodies call them. `site` is
 * pushed as the return address: the routine's own entry, so a KeSetBasePriority
 * trace names the call as ours rather than inventing a guest call site. Both
 * are stdcall and pop their arguments; eax/ecx/edx are the cdecl caller's to
 * lose, as they are to the bodies. */
static void adx_set_own_priority(uint32_t site, int prio)
{
    PUSH32(g_esp, (uint32_t)prio);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, site); RECOMP_ABI_CALL(0x00147CC0u, sub_00147CC0);
}

static int adx_get_own_priority(uint32_t site)
{
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, site); RECOMP_ABI_CALL(0x00147D12u, sub_00147D12);
    return (int)(int32_t)g_eax;
}

/* A raiser whose count reached zero on another thread collects its own
 * priority here, on its own thread, before the body can read or save it. */
static void adx_pay_owed(uint32_t site, uint32_t ra)
{
    int p;
    static int count_source_set;
    if (!count_source_set) {
        adx_trace_set_ctx_source(adx_fill_ctx);
        adx_trace_set_count_source(adx_read_count);
        count_source_set = 1;
    }
    if (adx_prio_take_owed(&p)) {
        adx_set_own_priority(site, p);
        adx_trace_note(ADX_EV_OWED, ra, adx_read_count(), adx_read_count(),
                       0, p);
    }
}

void sub_0013B0A0(void)
{
    uint32_t ra = MEM32(g_esp);     /* the guest caller, for the trace */
    int before, pre_raise = 0, raised = 0, raise_to = ADX_PRIO_NONE;

    adx_pay_owed(0x0013B0A0u, ra);
    adx_guard_lock_enter();     /* held past this body, to the matching unlock */

    before = adx_read_count();
    g_eax = MEM32(0x25EFA0);
    if (g_eax != 0) goto loc_0013B0D3;      /* jne */

    PUSH32(g_esp, g_esi);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, 0x0013B0B1u); RECOMP_ABI_CALL(0x00147D12u, sub_00147D12);

    g_esi = g_eax;
    pre_raise = (int)(int32_t)g_eax;
    raised = 1;
    g_eax = MEM32(0x25EF8C);
    raise_to = (int)(int32_t)g_eax;
    PUSH32(g_esp, g_eax);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, 0x0013B0C0u); RECOMP_ABI_CALL(0x00147CC0u, sub_00147CC0);

    g_ecx = MEM32(0x27D0E0);
    PUSH32(g_esp, g_ecx);
    RECOMP_MEM_WRITE32(0x0013B0C7u, 0x0013B0A0u, 0x27D0F8, g_esi);
    PUSH32(g_esp, 0x0013B0D2u); RECOMP_ABI_CALL(0x00147DD2u, sub_00147DD2);

    POP32(g_esp, g_esi);

loc_0013B0D3:
    RECOMP_MEM_WRITE32(0x0013B0D3u, 0x0013B0A0u, 0x25EFA0, MEM32(0x25EFA0) + 1);
    adx_prio_note_lock(raised, pre_raise);
    adx_trace_note(ADX_EV_LOCK, ra, before, adx_read_count(), 0, raise_to);

    g_esp += 4;         /* ret -- the guard stays held on purpose */
}

/* CRI'S OUTER NESTING WORD (G66's lock leak). Transcribed from the generated
 * bodies:
 *
 *   sub_0013C460   mov eax,[0x25EFEC] / test / jne +5 / call 0x141B60 /
 *                  inc [0x25EFEC] / ret
 *   sub_0013C480   dec [0x25EFEC] / jne +5 / jmp 0x141B80 / ret
 *
 * The only addition is the guard around each body (adx_guard.h, CRI'S OUTER
 * NESTING WORD IS PART OF THE REGION): the word was read outside the lock, so
 * the main thread could see the ADX thread's 1, skip its own lock, and later
 * take the word to zero and unlock the ADX thread's level. Traced in
 * ~/jsrf-build/runs/g66b/runs/leak1 (#58858-#58868) and leak3 (#79632-#79646).
 * The `dec` sets ZF from its own result, which the body now keeps in a local
 * rather than re-reading shared memory. */
void sub_00141B60(void);     /* CRI lock wrapper:   call [0x2615E8] */
void sub_00141B80(void);     /* CRI unlock wrapper: call [0x2615F0] */

void sub_0013C460(void)
{
    adx_guard_region_enter();
    g_eax = MEM32(0x25EFEC);
    if (g_eax == 0) {
        PUSH32(g_esp, 0x0013C46Eu); RECOMP_ABI_CALL(0x00141B60u, sub_00141B60);
    }
    RECOMP_MEM_WRITE32(0x0013C46Eu, 0x0013C460u, 0x25EFEC, MEM32(0x25EFEC) + 1);
    adx_guard_region_leave();
    g_esp += 4;         /* ret */
}

void sub_0013C480(void)
{
    uint32_t word;
    adx_guard_region_enter();
    word = MEM32(0x25EFEC) - 1;
    RECOMP_MEM_WRITE32(0x0013C480u, 0x0013C480u, 0x25EFEC, word);
    if (word == 0)
        sub_00141B80();  /* tail jmp: its ret pops our return address */
    else
        g_esp += 4;      /* ret */
    adx_guard_region_leave();
}

void sub_0013B0E0(void)
{
    uint32_t ra = MEM32(g_esp);
    int before, admitted, matched, prio_set = ADX_PRIO_NONE;

    adx_pay_owed(0x0013B0E0u, ra);
    before = adx_read_count();

    /* An unlock from a thread that never locked, arriving while another
     * thread is inside the region, is a spin pass that an elevated holder
     * would have prevented from running at all. Drop it: the body would
     * decrement the refcount and rewrite the one saved-priority slot
     * underneath the holder, which is how 0x0027D0F8 came to hold 15 on
     * 21 Sep 2026. CRI's guard at sub_001437B0 calls this again next pass. */
    admitted = adx_guard_unlock_enter();
    if (admitted < 0) {
        adx_trace_note(ADX_EV_UNLOCK, ra, before, before, admitted,
                       ADX_PRIO_NONE);
        g_esp += 4;     /* ret, with the guest's words untouched */
        return;
    }
    matched = adx_prio_note_unlock();

    /* UNMATCHED-SAFE. At count <= 0 there is nothing to unlock: on hardware
     * no unlock runs here, and the decrement is what drove 0x0025EFA0 to
     * -32,680,066 on 21 Sep and negative again in every poisoned G66 run. The
     * caller is sub_001437B0's `while (GetThreadPriority(self) == 15) unlock`;
     * if it holds nothing and still reads 15, nothing the unlock can do will
     * ever lower it, so hand it back its own pre-raise priority and let the
     * loop exit. */
    if ((int32_t)MEM32(0x25EFA0) <= 0 && adx_unmatched_safe_on()) {
        adx_prio_note_clamp();
        if (!matched && adx_get_own_priority(0x0013B0E0u) == 15) {
            int p;
            adx_prio_rescue(&p);
            adx_set_own_priority(0x0013B0E0u, p);
            prio_set = p;
        }
        adx_trace_note(ADX_EV_UNLOCK, ra, before, before, admitted, prio_set);
        adx_trace_note(ADX_EV_CLAMP, ra, before, before, admitted, prio_set);
        adx_guard_unlock_leave();
        g_esp += 4;
        return;
    }

    RECOMP_MEM_WRITE32(0x0013B0E0u, 0x0013B0E0u, 0x25EFA0, MEM32(0x25EFA0) - 1);
    if (MEM32(0x25EFA0) != 0) goto loc_0013B101;    /* jne */

    g_eax = MEM32(0x27D0E0);
    PUSH32(g_esp, g_eax);
    PUSH32(g_esp, 0x0013B0F3u); RECOMP_ABI_CALL(0x00147DACu, sub_00147DAC);

    /* PER-THREAD RESTORE: the slot belongs to the thread that raised. When
     * that is not this thread, this thread was never raised by this region
     * and keeps its priority; the raiser is owed its own (adx_pay_owed). */
    if (!adx_prio_restore_here()) goto loc_0013B101;

    g_ecx = MEM32(0x27D0F8);
    prio_set = (int)(int32_t)g_ecx;
    PUSH32(g_esp, g_ecx);
    PUSH32(g_esp, 0xFFFFFFFEu);
    PUSH32(g_esp, 0x0013B101u); RECOMP_ABI_CALL(0x00147CC0u, sub_00147CC0);

loc_0013B101:
    adx_trace_note(ADX_EV_UNLOCK, ra, before, adx_read_count(), admitted,
                   prio_set);
    adx_guard_unlock_leave();   /* releases what the matching lock took */
    g_esp += 4;         /* ret */
}

/*
 * sub_00194210: XDK D3D's PGRAPH interrupt handler, with the trap handshake
 * around it. The generated body is sub_00194210_gen.
 *
 * thiscall, ecx = a block whose +0 holds the NV2A base. The body reads a
 * software-method trap (PGRAPH_INTR, NSOURCE, TRAPPED_ADDR/DATA), writes
 * PGRAPH_INTR back to acknowledge it -- write-1-to-clear on hardware, nothing
 * on plain memory -- and acts on it: for NOP(5), sub_00193F70 sets the KEVENT
 * at device+0x2440 that BlockOnTime (sub_00191440) sleeps on.
 *
 * Who calls it: the DPC (sub_00194480), which loops while PMC_INTR names
 * PGRAPH, and D3D's busy-waits, which call it whenever PGRAPH_INTR is set.
 * Left to guess "taken" from the body's PGRAPH_FIFO writes, the runtime
 * cleared the status some time later, and every call until then took the
 * same NOP(5) and set the event again. BlockOnTime clears that event before
 * patching the next fence and does not look at the fence again once its wait
 * returns, so a repeat after the clear ended the wait early, D3D reused ring
 * the executor had not read, and the next trap was garbage (run 8's crash).
 *
 * So the executor posts a trap under the trap lock (nv2a_pb_exec.c), this
 * holds the same lock around the body, and the call that finds the trap
 * reports it taken, which clears it: one trap, one run of the handler.
 *
 * It runs at DISPATCH_LEVEL: the DPC (on the kernel's timer thread) and the
 * busy-waits (on the game's thread) never interleave on the Xbox, and the
 * dispatch lock keeps them apart here too.
 *
 * Nested calls (the context-switch path, sub_00197AAC, calls it again) run
 * the body under the outer call's lock: it is not recursive, and no trap can
 * be posted while it is held. The runtime never switches context that way;
 * this is a guard, not a known path.
 */
extern void sub_00194210_gen(void);
extern void nv2a_pb_trap_lock(void);
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);

#define IRQL_DISPATCH 2
#define NV2A_PGRAPH_INTR_WORD    (0x400100u / 4)
#define NV2A_PGRAPH_NSOURCE_WORD (0x400108u / 4)

/* To DISPATCH_LEVEL for an interrupt-side routine, unless already there
 * (inside the DPC). The level to go back to. */
static uint8_t raise_to_dispatch(void)
{
    uint8_t before = xbox_CurrentIrql();

    if (before >= IRQL_DISPATCH)
        return before;
    return xbox_KfRaiseIrql(IRQL_DISPATCH);
}

static void lower_from_dispatch(uint8_t before)
{
    if (before < IRQL_DISPATCH)
        xbox_KfLowerIrql(before);
}

void sub_00194210(void)
{
    static _Thread_local int inside;
    volatile uint32_t *nv2a = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint8_t before;
    int posted;

    if (inside) {
        sub_00194210_gen();
        return;
    }
    before = raise_to_dispatch();
    inside = 1;
    nv2a_pb_trap_lock();
    /* Only the executor sets NSOURCE (posting a trap) and only the taken
     * call below clears it, so under the lock it means "posted, not taken".
     * The body goes by PGRAPH_INTR (bit 0 or 20, read first at 0x0019421E),
     * which another thread's write may have cleared since: the body then did
     * nothing, the trap still counted as taken, the fence event was never set
     * and the game slept in BlockOnTime for good (1 Nov log: 0x0019151B,
     * device+0x2440, GET == PUT). So it is set again for a posted trap. */
    posted = nv2a[NV2A_PGRAPH_NSOURCE_WORD] != 0;
    if (posted)
        __atomic_fetch_or(&nv2a[NV2A_PGRAPH_INTR_WORD], 1u, __ATOMIC_SEQ_CST);
    sub_00194210_gen();
    if (posted)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    inside = 0;
    lower_from_dispatch(before);
}


/*
 * The XAPI input boundary, traced (RECOMP_INPUT_DIAG).
 *
 * The OHCI model's [INPUT] line proves a pad report leaves the host; it
 * cannot say whether the title ever sees it. These wrap seven of the title's
 * own XAPI input entry points -- the set the Mac port lifted in G49
 * (jsrf-recomp diagnostics/jsrf_first_fault/xinput_lift.c) -- and log what
 * they return: device arrivals, the handle the title opens, and every change
 * in what XInputGetState hands the game, plus a census every five seconds.
 * A press that shows up here and does nothing is the game's business; one
 * that never shows up was lost between the pad and XPP. The bodies are the
 * generated ones (sub_*_gen), unchanged; with the variable unset each
 * wrapper is a straight call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XAPI_STATE_LINES 600

static int xapi_diag(void)
{
    static int on = -1;

    if (on < 0)
        on = getenv("RECOMP_INPUT_DIAG") != NULL;
    return on;
}

static double xapi_now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/*
 * A lost wake for BlockOnTime (sub_00191440), as a moment instead of the game.
 *
 * BlockOnTime sleeps, with no timeout, on the KEVENT at device+0x2440 that
 * the NOP(5) trap's handler sets. If the GPU has consumed every command
 * (DMA_GET == DMA_PUT, unchanged for 50 ms) and no trap is pending
 * (NSOURCE 0), every fence before PUT has passed. BlockOnTime kicked before
 * it slept, so the fence it wants is one of them, and the wake was lost: the
 * event is set here. BlockOnTime does not re-check the fence after its wait,
 * which is right in this case. Upstream (JSRF Unleashed 0.5.10) has the same
 * safety net, RECOMP_SWM_RESCUE. The log line says it happened, so a cause
 * still left can be found from it. RECOMP_D3D_RESCUE=0 turns it off.
 * Installed from main.c through xbox_SetWaitRescueHook.
 */
int jsrf_d3d_wait_rescue(uint32_t va, uint32_t waited_ms)
{
    static int on = -1;
    static uint32_t s_put, s_since;
    static unsigned rescued;
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t dev, put, get, now;

    if (on < 0) {
        const char *e = getenv("RECOMP_D3D_RESCUE");
        on = !(e && e[0] == '0');
    }
    if (!on)
        return 0;
    dev = MEM32(0x0019DCE0u);
    if (!dev || va != dev + 0x2440u)
        return 0;
    if (waited_ms < 50) {
        s_since = 0;
        return 0;
    }
    put = nv[0x800040 / 4];
    get = nv[0x800044 / 4];
    now = (uint32_t)(uint64_t)(xapi_now() * 1000.0);
    if (get != put || nv[0x400108 / 4] || put != s_put || !s_since) {
        s_put = put;
        s_since = now ? now : 1;
        return 0;
    }
    if (now - s_since < 50)
        return 0;
    s_since = 0;
    if (++rescued <= 16 || (rescued & (rescued - 1)) == 0)
        fprintf(stderr, "[D3D] BlockOnTime woken: GPU idle, no NOP(5) seen "
                "(#%u, waited %u ms; GET=PUT=%08X PMC_INTR=%08X PGRAPH_INTR=%08X "
                "SUBR=%08X fence event %08X:%08X)\n",
                rescued, waited_ms, put, nv[0x000100 / 4], nv[0x400100 / 4],
                nv[0x00324C / 4], MEM32(va), MEM32(va + 4));
    return 1;
}

#define XAPI_ARG(i) MEM32(g_esp + 4u + 4u * (uint32_t)(i))
#define XAPI_U8(a)  (*(volatile uint8_t *)XBOX_PTR(a))
#define XAPI_U16(a) (*(volatile uint16_t *)XBOX_PTR(a))

extern void sub_001BCBCC_gen(void);
extern void sub_001BD5FF_gen(void);
extern void sub_001BD621_gen(void);
extern void sub_001C3BA1_gen(void);
extern void sub_001C3C16_gen(void);
extern void sub_001C3C22_gen(void);
extern void sub_001C3E14_gen(void);

/* XInitDevices(count, preallocTypes), ret 8 */
void sub_001BCBCC(void)
{
    uint32_t n;

    if (!xapi_diag()) {
        sub_001BCBCC_gen();
        return;
    }
    n = XAPI_ARG(0);
    sub_001BCBCC_gen();
    fprintf(stderr, "  [XAPI] XInitDevices(%u types)\n", n);
    fflush(stderr);
}

/* XGetDevices(type) -> port mask, ret 4. Logged when the answer changes. */
void sub_001BD5FF(void)
{
    static uint32_t types[4], masks[4];
    static unsigned ntypes;
    uint32_t type;
    unsigned i;

    if (!xapi_diag()) {
        sub_001BD5FF_gen();
        return;
    }
    type = XAPI_ARG(0);
    sub_001BD5FF_gen();
    for (i = 0; i < ntypes && types[i] != type; i++)
        ;
    if (i == ntypes) {
        if (ntypes == 4)
            return;
        types[ntypes++] = type;
        masks[i] = ~g_eax;
    }
    if (masks[i] != g_eax) {
        masks[i] = g_eax;
        fprintf(stderr, "  [XAPI] XGetDevices(type=%08X) = %08X\n", type, g_eax);
        fflush(stderr);
    }
}

/* XGetDeviceChanges(type, *insertions, *removals) -> BOOL, ret 12 */
void sub_001BD621(void)
{
    static unsigned long calls;
    uint32_t type, pins, prem;

    if (!xapi_diag()) {
        sub_001BD621_gen();
        return;
    }
    type = XAPI_ARG(0);
    pins = XAPI_ARG(1);
    prem = XAPI_ARG(2);
    sub_001BD621_gen();
    if (calls++ == 0 || g_eax) {
        fprintf(stderr, "  [XAPI] XGetDeviceChanges(type=%08X) = %u "
                "insert=%08X remove=%08X (call %lu)\n", type, g_eax,
                pins ? MEM32(pins) : 0u, prem ? MEM32(prem) : 0u, calls);
        fflush(stderr);
    }
}

/* XInputOpen(type, port, slot, pollParams) -> handle, ret 16. The polling
 * parameters' first byte holds fAutoPoll (bit 0): without it the title has
 * to call XInputPoll for anything new to arrive. */
void sub_001C3BA1(void)
{
    uint32_t type, port, slot, pp;

    if (!xapi_diag()) {
        sub_001C3BA1_gen();
        return;
    }
    type = XAPI_ARG(0);
    port = XAPI_ARG(1);
    slot = XAPI_ARG(2);
    pp = XAPI_ARG(3);
    sub_001C3BA1_gen();
    if (pp)
        fprintf(stderr, "  [XAPI] XInputOpen(type=%08X port=%u slot=%u "
                "poll=%02X/%u/%u) = %08X\n", type, port, slot, XAPI_U8(pp),
                XAPI_U8(pp + 1), XAPI_U8(pp + 2), g_eax);
    else
        fprintf(stderr, "  [XAPI] XInputOpen(type=%08X port=%u slot=%u "
                "poll=default) = %08X\n", type, port, slot, g_eax);
    fflush(stderr);
}

/* XInputClose(handle), ret 4 */
void sub_001C3C16(void)
{
    uint32_t h;

    if (!xapi_diag()) {
        sub_001C3C16_gen();
        return;
    }
    h = XAPI_ARG(0);
    sub_001C3C16_gen();
    fprintf(stderr, "  [XAPI] XInputClose(%08X)\n", h);
    fflush(stderr);
}

/* XInputGetCapabilities(handle, *caps) -> error, ret 8. Logged per change. */
void sub_001C3C22(void)
{
    static uint32_t last_h, last_rc = ~0u;
    uint32_t h;

    if (!xapi_diag()) {
        sub_001C3C22_gen();
        return;
    }
    h = XAPI_ARG(0);
    sub_001C3C22_gen();
    if (h != last_h || g_eax != last_rc) {
        last_h = h;
        last_rc = g_eax;
        fprintf(stderr, "  [XAPI] XInputGetCapabilities(%08X) = %u\n", h, g_eax);
        fflush(stderr);
    }
}

/* XInputGetState(handle, *state) -> error, ret 8. XINPUT_STATE is the packet
 * number (+0) then XINPUT_GAMEPAD: wButtons (+4), bAnalogButtons[8] (+6),
 * four sticks (+14). A line per change in (handle, result, digital buttons,
 * analog buttons past half), and a census of every call every five seconds
 * -- so a press is visible here even after the change lines run out. */
void sub_001C3E14(void)
{
    static uint32_t last_h, last_rc = ~0u, last_btn = ~0u, last_ana = ~0u;
    static unsigned lines;
    static unsigned long calls, pressed, errors;
    static uint32_t handles_seen[4];
    static double next;
    uint32_t h, ps, btn = 0, ana = 0, pkt = 0;
    unsigned i;
    double now;

    if (!xapi_diag()) {
        sub_001C3E14_gen();
        return;
    }
    h = XAPI_ARG(0);
    ps = XAPI_ARG(1);
    sub_001C3E14_gen();

    calls++;
    if (g_eax == 0 && ps) {
        pkt = MEM32(ps);
        btn = XAPI_U16(ps + 4);
        for (i = 0; i < 8; i++)
            if (XAPI_U8(ps + 6 + i) > 0x80)
                ana |= 1u << i;
        if (btn || ana)
            pressed++;
    } else {
        errors++;
    }
    for (i = 0; i < 4 && handles_seen[i] && handles_seen[i] != h; i++)
        ;
    if (i < 4 && !handles_seen[i])
        handles_seen[i] = h;

    if (h != last_h || g_eax != last_rc || btn != last_btn || ana != last_ana) {
        last_h = h;
        last_rc = g_eax;
        last_btn = btn;
        last_ana = ana;
        if (lines < XAPI_STATE_LINES) {
            lines++;
            fprintf(stderr, "  [XAPI] XInputGetState(%08X) = %u packet=%u "
                    "buttons=%04X analog=%02X%s\n", h, g_eax, pkt, btn, ana,
                    lines == XAPI_STATE_LINES ? " (change lines end here)" : "");
            fflush(stderr);
        }
    }

    now = xapi_now();
    if (next == 0.0)
        next = now + 5.0;
    if (now >= next) {
        next = now + 5.0;
        fprintf(stderr, "  [XAPI] census: %lu XInputGetState calls, %lu with "
                "a button held, %lu errors; last packet %u; handles %08X "
                "%08X %08X %08X\n", calls, pressed, errors, pkt,
                handles_seen[0], handles_seen[1], handles_seen[2],
                handles_seen[3]);
        fflush(stderr);
    }
}

/* XID gamepad report callback (type table +0x10, 0x1BDE21), ecx = the device
 * record. It copies the interrupt TD's payload (record+0x34, length from the
 * URB at +0x66 less the 2-byte header) to the state XInputGetState reads
 * (+0x14), then tail-jumps to XAPI's auto-power-down timer reset (KeSetTimer,
 * 6 h) when anything is held. Traced so a lost press can be placed: the raw
 * TD bytes before the copy say whether the report carried it, the state
 * after says whether XPP kept it. */
extern void sub_001BDE21_gen(void);

void sub_001BDE21(void)
{
    static unsigned long calls, shorts, raw_held, kept_held;
    static uint32_t last_len = ~0u;
    static uint8_t last_raw[3] = { 0xFF, 0xFF, 0xFF }, last_kept = 0xFF;
    static unsigned lines;
    static double next;
    uint32_t rec = g_ecx, len;
    uint8_t raw[3], kept, ana = 0;
    unsigned i;
    double now;

    if (!xapi_diag() || !rec) {
        sub_001BDE21_gen();
        return;
    }
    len = MEM32(rec + 0x66);
    raw[0] = XAPI_U8(rec + 0x32);
    raw[1] = XAPI_U8(rec + 0x33);
    raw[2] = XAPI_U8(rec + 0x34);
    for (i = 0; i < 8; i++)
        if (XAPI_U8(rec + 0x36 + i) > 0x80)
            ana |= (uint8_t)(1u << i);
    sub_001BDE21_gen();
    kept = XAPI_U8(rec + 0x14);

    calls++;
    if (len < 2)
        shorts++;
    if (raw[2] || ana)
        raw_held++;
    if (kept)
        kept_held++;
    if ((len != last_len || memcmp(raw, last_raw, 3) || kept != last_kept)
        && lines < XAPI_STATE_LINES) {
        lines++;
        last_len = len;
        memcpy(last_raw, raw, 3);
        last_kept = kept;
        fprintf(stderr, "  [XAPI] report(rec=%08X) len=%u hdr=%02X %02X "
                "buttons=%02X analog=%02X kept=%02X packet=%u\n", rec, len,
                raw[0], raw[1], raw[2], ana, kept, MEM32(rec + 8));
        fflush(stderr);
    }
    now = xapi_now();
    if (next == 0.0)
        next = now + 5.0;
    if (now >= next) {
        next = now + 5.0;
        fprintf(stderr, "  [XAPI] report census: %lu callbacks, %lu short, "
                "%lu carried a press, %lu kept one\n", calls, shorts,
                raw_held, kept_held);
        fflush(stderr);
    }
}

/* ── DirectSound's voice lists ─────────────────────────────────────────────
 *
 * The objects, as far as this section needs them (CMcpxVoiceClient, and the
 * MCPX APU object DirectSound keeps them on):
 *
 *   voice +0x12  u16 flags: bit 0 "the off-wait applies", bit 1 playing,
 *                bit 15 VOICE_OFF sent and the voice still listed
 *   voice +0x4C  LIST_ENTRY on the APU's active list for its kind
 *   voice +0x54  LIST_ENTRY on the APU's notify list (+0x6DC)
 *   voice +0x64  u8 hardware voice count, handles at +0x0C (u16 each)
 *   voice +0x65  u8 list index 0..2, 0xFF when on no list
 *   APU   +0x6C4/+0x6CC/+0x6D4  the three active lists (2D, 3D, MP)
 *   APU   +0x6DC  voices whose end is to be notified
 *
 * Every active-list edit runs at DISPATCH (AutoIrql, 0x001A1B7C, raises
 * through KfRaiseIrql, which takes the dispatch lock in kernel_hal.c) or in
 * the APU's ISR or DPC (the timer thread, which holds that lock), so they
 * cannot race each other here any more than on the console. What broke the lists was the
 * override of sub_001A308E below. */

#include "recomp_switch.h"

extern void sub_001A2E2E(void);         /* RemoveIdleVoice, generated */
extern void sub_001A1ED7_gen(void);     /* the DPC's list walk, generated */
extern int  xbox_thread_holds_dispatch(void);
extern void recomp_spin_yield(void);

static double dsound_now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6;
}

/*
 * sub_001A308E - DSOUND: wait for a stopped voice to leave the voice lists.
 *
 * On the Xbox this fifteen-byte routine is
 *
 *       test byte [ecx+0x12], 1        ; je  ret
 *    L: test word [ecx+0x12], 0x8000   ; jne L
 *
 * Stop (sub_001A39EF) sends VOICE_OFF and sets bit 15; the voice stays on
 * DirectSound's active list (+0x4C) and on the hardware list (TVL and the
 * link at voice+0x7C) until the voice processor finds it idle and traps
 * (SE2FE_IDLE_VOICE). The APU's ISR (sub_001A2638, on the timer thread under
 * the dispatch lock) then runs RemoveIdleVoice -- sub_001A25AA ->
 * sub_001A24BE -> sub_001A241F -> sub_001A200D -> sub_001A2E2E -- which
 * unlinks the voice from the hardware list (0x001A2EC2..0x001A2F36) and from
 * the active list (0x001A2FA8), sets +0x65 to 0xFF, and only then clears
 * bit 15 (0x001A2FBE). The callers depend on all of that: ActivateVoice
 * (sub_001A3E58) inserts the voice into both lists right after this returns,
 * and the teardown (sub_001A47AD) releases the voice (sub_001A2AC7) next.
 *
 * THE OVERRIDE THIS REPLACES CLEARED BIT 15 AND RETURNED: the last store of
 * the completion without the unlinking before it. So a Stop followed by a
 * Play inserted a voice that was still linked -- InsertHeadList of an entry
 * already first on the list makes it point at itself (0x001A3FC4), and
 * VOICE_ON's prepend of the handle TVL already names stores link(h) = h --
 * and a Stop + Release released a voice that was still on both lists. That
 * is the freeze: the DPC's walk (sub_001A1ED7) never gets back to the head,
 * so it calls sub_001A44B3 for ever holding the dispatch lock, and the APU
 * mixes the self-linked voice 256 times a frame (the screech). Stage
 * teardown and Start during the intro movie both stop and release voices
 * in bulk, which is why they are where it froze. The override came from
 * jsrf-recomp (9 Sep 2026), where this loop never ended because the
 * completion never came; here the idle trap reaches the guest ("front-end
 * traps" on the [perf] APU pacing line).
 *
 * Now it waits for that interrupt, as the console does, letting go of the
 * guest lock while it waits. It is a few milliseconds away (one APU frame to
 * trap, then the timer thread). If it has not come after
 * RECOMP_DSOUND_VOICE_OFF_WAIT_MS (default 50), or this thread holds the
 * dispatch lock so the ISR cannot run, the completion is made here instead,
 * by calling the guest's own RemoveIdleVoice at DISPATCH -- unlinked, not
 * just unflagged. RECOMP_DSOUND_VOICE_OFF_WAIT=0 restores the old override.
 *
 * Logs: "[DSOUND] voice-off wait on|OFF" once; "[DSOUND] voice-off wait gave
 * up ..." for the first fallbacks; a census line every 10 s while waits
 * happen. Fallbacks that keep coming mean the idle trap is not reaching the
 * guest, which is worth knowing on its own.
 *
 * __thiscall, no stack arguments, returns void; eax/ecx/edx are kept, as the
 * original loop keeps them.
 */
static int dsound_voice_off_wait_on(void)
{
    static int on = -1;

    if (on < 0) {
        on = recomp_switch_on_default("RECOMP_DSOUND_VOICE_OFF_WAIT", 1);
        fprintf(stderr, "  [DSOUND] voice-off wait %s "
                "(RECOMP_DSOUND_VOICE_OFF_WAIT)\n",
                on ? "on" : "OFF: bit 15 cleared, voice left linked");
        fflush(stderr);
    }
    return on;
}

static double dsound_voice_off_wait_limit_ms(void)
{
    static int ms = -1;

    if (ms < 0) {
        const char *e = getenv("RECOMP_DSOUND_VOICE_OFF_WAIT_MS");
        ms = (e && *e) ? atoi(e) : 50;
        if (ms < 0)
            ms = 0;
    }
    return (double)ms;
}

/* Bit 15 is still set and the ISR is not coming: do what it would have done,
 * under the dispatch lock so the ISR cannot do it at the same time. */
static int dsound_voice_off_complete(uint32_t obj)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    uint32_t ebp = g_ebp, seh = g_seh_ebp;
    uint8_t before = raise_to_dispatch();
    int unlinked = 0;

    if (MEM16(obj + 0x12) & 0x8000u) {          /* the ISR may have won */
        if (MEM8(obj + 0x65) < 3 && MEM32(obj + 0x4C) != obj + 0x4C) {
            g_ecx = obj;
            PUSH32(g_esp, 0x001A308Eu);
            RECOMP_ABI_CALL(0x001A2E2Eu, sub_001A2E2E);
            unlinked = 1;
        }
        MEM16(obj + 0x12) &= 0x7FFFu;           /* no-op after the call */
    }
    lower_from_dispatch(before);
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    g_ebp = ebp; g_seh_ebp = seh;
    return unlinked;
}

void sub_001A308E(void)
{
    static unsigned long waits, by_isr, unlisted, fallbacks, fallback_unlinked;
    static double worst, next_census;
    uint32_t obj = g_ecx;
    double t0, waited;
    unsigned spins = 0;
    int raised, listed;

    if (!(MEM8(obj + 0x12) & 1) || !(MEM16(obj + 0x12) & 0x8000u)) {
        g_esp += 4;     /* ret */
        return;
    }
    if (!dsound_voice_off_wait_on()) {
        MEM16(obj + 0x12) &= 0x7FFFu;
        g_esp += 4;
        return;
    }

    /* Raised, the ISR cannot run until we lower; on no list, there is nothing
     * for the voice processor to find idle. Neither can end by waiting. */
    raised = xbox_thread_holds_dispatch();
    listed = MEM8(obj + 0x65) < 3 && MEM32(obj + 0x4C) != obj + 0x4C;
    t0 = dsound_now_ms();
    if (!raised && listed) {
        while ((MEM16(obj + 0x12) & 0x8000u) &&
               dsound_now_ms() - t0 < dsound_voice_off_wait_limit_ms()) {
            if (++spins < 64) {
                recomp_spin_yield();
            } else {
                struct timespec ts = { 0, 200000 };
                nanosleep(&ts, NULL);
            }
        }
    }
    waited = dsound_now_ms() - t0;

    waits++;
    if (waited > worst)
        worst = waited;
    if (!(MEM16(obj + 0x12) & 0x8000u)) {
        by_isr++;
    } else if (!raised && !listed) {
        /* Cleared under the dispatch lock, so an ISR half way through
         * RemoveIdleVoice for this voice finishes first. */
        dsound_voice_off_complete(obj);
        unlisted++;
    } else {
        int unlinked = dsound_voice_off_complete(obj);
        fallbacks++;
        fallback_unlinked += (unsigned long)unlinked;
        if (fallbacks <= 16) {
            fprintf(stderr, "  [DSOUND] voice-off wait gave up on voice %08X "
                    "(handle %u, list %u) after %.1f ms%s; %s\n", obj,
                    (unsigned)MEM16(obj + 0x0C), (unsigned)MEM8(obj + 0x65),
                    waited, raised ? " (caller at DISPATCH)" : "",
                    unlinked ? "unlinked it here (RemoveIdleVoice)"
                             : "the ISR got there first");
            fflush(stderr);
        }
    }

    if (next_census == 0.0)
        next_census = t0 + 10000.0;
    if (t0 >= next_census) {
        next_census = t0 + 10000.0;
        fprintf(stderr, "  [DSOUND] voice-off waits: %lu, %lu ended by the "
                "ISR, %lu on no list, %lu by the fallback (%lu unlinked); "
                "worst %.1f ms\n", waits, by_isr, unlisted, fallbacks,
                fallback_unlinked, worst);
        fflush(stderr);
    }
    g_esp += 4;         /* ret */
}

/*
 * sub_001A1ED7 - DSOUND's DPC voice walk (CMcpxAPU, thiscall), wrapped.
 *
 * The generated body (sub_001A1ED7_gen) walks the three active lists calling
 * each voice's vtbl+0x14 (sub_001A44B3), then drains the notify list. It
 * stops only on reaching a list head, so one ring that does not pass through
 * the head -- a voice linked to itself is the usual one -- keeps the timer
 * thread in here for ever, holding the dispatch lock, and the game stops.
 *
 * This checks the four lists first and, if one does not come back to its head
 * within DSOUND_LIST_MAX steps or meets a bad pointer, closes it back to the
 * head after the last node not yet seen and rewrites the back links of what is
 * kept; a list that comes back but has a wrong back link gets those rewritten.
 * Voices past the break are lost to the list, which is what they already
 * were. Containment, not a fix: the cause was the old sub_001A308E above, and
 * a repair logged after that fix means there is another one.
 *
 * Runs at DISPATCH on the timer thread, as every other edit of these lists
 * does, so nothing changes them under the check. Logs "[DSOUND] cyclic voice
 * list ..." with the node addresses (first 16, then every 256th).
 * RECOMP_DSOUND_LIST_GUARD=0 calls the generated body alone.
 */
#define DSOUND_LIST_MAX 1024

static int dsound_list_guard_on(void)
{
    static int on = -1;

    if (on < 0) {
        on = recomp_switch_on_default("RECOMP_DSOUND_LIST_GUARD", 1);
        fprintf(stderr, "  [DSOUND] voice-list guard %s "
                "(RECOMP_DSOUND_LIST_GUARD)\n", on ? "on" : "OFF");
        fflush(stderr);
    }
    return on;
}

/* Walk from the head, keeping each node not yet seen and fixing its back link,
 * and close the list after the last one kept. `entry_off` is where the
 * LIST_ENTRY sits in the voice, for the log. */
/* A link the walk may follow: aligned, and in the guest RAM the voices live in
 * (the 128 MB of RAM, or its 64 MB contiguous window). A wild pointer stops
 * the walk instead of faulting the host on unmapped address space. */
static int dsound_link_ok(uint32_t p)
{
    return p && !(p & 3u) && (p < 0x08000000u || (p >= 0x80000000u && p < 0x84000000u));
}

static void dsound_list_repair(uint32_t apu, uint32_t head, uint32_t entry_off,
                               const char *reason)
{
    static unsigned long repairs;
    uint32_t seen[DSOUND_LIST_MAX];
    uint32_t prev = head, node = MEM32(head), stop_at;
    unsigned n = 0, i;
    const char *why = "back links";

    for (;;) {
        if (node == head)
            break;
        if (!dsound_link_ok(node)) {
            why = "bad pointer";
            break;
        }
        for (i = 0; i < n && seen[i] != node; i++)
            ;
        if (i < n) {
            why = "ring";
            break;
        }
        if (n == DSOUND_LIST_MAX) {
            why = "too long";
            break;
        }
        seen[n++] = node;
        MEM32(node + 4) = prev;         /* Blink */
        prev = node;
        node = MEM32(node);
    }
    stop_at = node;
    MEM32(prev) = head;                 /* Flink of the last kept */
    MEM32(head + 4) = prev;

    repairs++;
    if (repairs <= 16 || (repairs & 255u) == 0) {
        fprintf(stderr, "  [DSOUND] cyclic voice list APU+0x%03X (APU %08X, "
                "head %08X): %s (%s); kept %u nodes, the last %08X (voice "
                "%08X flags %04X list %02X) pointed at %08X and now at the "
                "head. Repair #%lu\n",
                head - apu, apu, head, why, reason, n, prev,
                prev == head ? 0u : prev - entry_off,
                prev == head ? 0u : (unsigned)MEM16(prev - entry_off + 0x12),
                prev == head ? 0u : (unsigned)MEM8(prev - entry_off + 0x65),
                stop_at, repairs);
        if (n) {
            fprintf(stderr, "  [DSOUND]   nodes:");
            for (i = 0; i < n && i < 16; i++)
                fprintf(stderr, " %08X", seen[i]);
            fprintf(stderr, "%s\n", n > 16 ? " ..." : "");
        }
        fflush(stderr);
    }
}

static void dsound_list_check(uint32_t apu, uint32_t head, uint32_t entry_off)
{
    uint32_t prev = head, node = MEM32(head);
    unsigned n = 0;
    int back_bad = 0;

    while (node != head) {
        if (!dsound_link_ok(node) || ++n > DSOUND_LIST_MAX) {
            dsound_list_repair(apu, head, entry_off, "walk did not return");
            return;
        }
        if (MEM32(node + 4) != prev)
            back_bad = 1;
        prev = node;
        node = MEM32(node);
    }
    if (back_bad || MEM32(head + 4) != prev)
        dsound_list_repair(apu, head, entry_off, "wrong Blink");
}

void sub_001A1ED7(void)
{
    uint32_t apu = g_ecx;

    if (dsound_list_guard_on()) {
        dsound_list_check(apu, apu + 0x6C4, 0x4C);
        dsound_list_check(apu, apu + 0x6CC, 0x4C);
        dsound_list_check(apu, apu + 0x6D4, 0x4C);
        dsound_list_check(apu, apu + 0x6DC, 0x54);
    }
    sub_001A1ED7_gen();     /* its ret pops our caller's return address */
}

/* ── CRI's idle spinner (thread entry 0x0013B180) ─────────────────────────
 *
 * One of the four threads CRI's ADX setup (sub_0013B330) creates suspended,
 * at priority 2. Its whole body is
 *
 *     while (![0x25EFC0]) [0x25EFA8]++;      then [0x25EFC4] = 1, ExitThread
 *
 * The ADX lock resumes it and the unlock suspends it again (sub_0013B0A0 /
 * sub_0013B0E0 above). On the Xbox's one CPU that is part of the lock: while
 * the holder waits on the disc, the spinner at priority 2 keeps every normal
 * thread off the CPU, so none of them can get into CRI's state. Nothing reads
 * the counter.
 *
 * Here it never stops. This runtime cannot suspend another running thread
 * (win32_compat.c: only the start gate and a thread suspending itself), so
 * from the first ADX lock on the spinner spun on a core of its own: the one
 * guest thread at 99-111% in every [perf] "% of a core" line on the console.
 * That was a third of the CPU the title has, and the intro, the one scene
 * that needs all of it, dropped to 34-52 fps with 21-29% of the ADX music
 * ring played stale. jsrf-recomp found the same thread at 100% on the Mac and
 * lifted it the same way (process CPU 312% -> 221%).
 *
 * The host body keeps the contract: the counter still moves while it is
 * resumed, the exit flag still ends it through the same ExitThread call. But
 * it sleeps between increments, and parks at w32_thread_suspend_point while
 * the unlock has it suspended, so the lock pair really does start and stop
 * it. The exclusion it gave on the Xbox is adx_guard.c's job here.
 * RECOMP_CRI_SPINNER_LIFT=0 runs the original. */
void sub_0013B180(void);            /* the generated spinner */
void sub_00147E4E(void);            /* XAPILIB::ExitThread */
int w32_thread_suspend_point(void);

/* ── CRI's two ADX pumps (thread entries 0x0013B1C0, 0x0013B230) ──────────
 *
 * Each loops: BlockUntilVerticalBlank, one server pass (sub_00141E50 /
 * sub_00141E70: decode into the DirectSound ring), resume the file worker.
 * BlockUntilVerticalBlank clears the event before it waits, so a pass that
 * has not finished by the next vblank is a pass lost, and the ring plays its
 * old contents (the [perf] "APU ring voices ... stale" line).
 *
 * On the Switch every guest thread runs at 59, Horizon's one time-sliced
 * priority. In the title intro -- the game thread and the renderer busy on
 * two cores -- a pump woken by the vblank waited out 10 ms slices, and was
 * cut off mid-pass for another: 7-8% of the music ring stale with no vblank
 * late or lost (20 Oct 2026). They block on every vblank, so a priority above the
 * guest's (0x2D: under the timer and APU threads' 0x2C) costs the rest only
 * the passes themselves. CRI raising them to TIME_CRITICAL inside its lock
 * changes nothing on the host (win32_compat.c applies guest priorities only
 * with RECOMP_NX_GUEST_RT). RECOMP_CRI_PUMP_PRIO=0 leaves them at 59. */
void sub_0013B1C0(void);
void sub_0013B230(void);
#include "platform/nx_threads.h"

static void cri_pump_raise(uint32_t entry)
{
    static int on = -1;
    if (on < 0)
        on = recomp_switch_on_default("RECOMP_CRI_PUMP_PRIO", 1);
    if (on && nx_thread_set_priority(0x2D))
        fprintf(stderr, "  [CRI] ADX pump 0x%08X at host priority 0x2D\n", entry);
}

void jsrf_cri_pump_a(void)
{
    cri_pump_raise(0x0013B1C0u);
    sub_0013B1C0();
}

void jsrf_cri_pump_b(void)
{
    cri_pump_raise(0x0013B230u);
    sub_0013B230();
}

void jsrf_cri_spinner(void)
{
    static int on = -1;
    unsigned parks = 0;
    if (on < 0) {
        on = recomp_switch_on_default("RECOMP_CRI_SPINNER_LIFT", 1);
        fprintf(stderr, "  [CRI] idle spinner 0x0013B180 %s\n",
                on ? "lifted: sleeps, and parks while the ADX unlock has it suspended"
                   : "runs as written (RECOMP_CRI_SPINNER_LIFT=0)");
    }
    if (!on) {
        sub_0013B180();
        return;
    }
    while (!MEM32(0x25EFC0)) {
        struct timespec ts = { 0, 1000000 };    /* 1 ms */
        RECOMP_MEM_WRITE32(0x0013B196u, 0x0013B180u, 0x25EFA8, MEM32(0x25EFA8) + 1);
        if (w32_thread_suspend_point() && ++parks <= 2)
            fprintf(stderr, "  [CRI] idle spinner parked by the ADX unlock, resumed by"
                            " the lock (%u)\n", parks);
        nanosleep(&ts, NULL);
    }
    /* The original's exit: push 0xF0000001 / [0x25EFC4] = 1 / call ExitThread,
     * which does not return. */
    PUSH32(g_esp, 0xF0000001u);
    RECOMP_MEM_WRITE32(0x0013B1A9u, 0x0013B180u, 0x25EFC4, 1);
    PUSH32(g_esp, 0x0013B1B8u); RECOMP_ABI_CALL(0x00147E4Eu, sub_00147E4E);
}
