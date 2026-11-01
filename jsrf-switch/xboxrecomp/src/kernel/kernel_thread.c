/*
 * kernel_thread.c - Xbox Threading Subsystem
 *
 * Implements Xbox thread creation, termination, delays, and priority
 * management using Win32 threading APIs.
 *
 * Xbox threading model:
 *   - PsCreateSystemThreadEx creates kernel-mode threads (→ CreateThread)
 *   - Thread start routines are __stdcall with a single PVOID context
 *   - Time intervals use NT 100-nanosecond units (negative = relative)
 *   - Thread priorities use NT KPRIORITY increments
 */

#include <stdio.h>
#include "kernel.h"
#include "xbox_memory_layout.h"   /* XBOX_WORKER_STACK_* + worker-stack decls */

/* ============================================================================
 * Thread Start Wrapper
 *
 * Xbox start routines are __stdcall void(*)(PVOID), but Win32 CreateThread
 * expects DWORD WINAPI (*)(LPVOID). We wrap the Xbox routine to bridge
 * the calling convention and return type.
 * ============================================================================ */

typedef struct _XBOX_THREAD_START_INFO {
    PXBOX_SYSTEM_ROUTINE StartRoutine;
    PVOID                StartContext;
} XBOX_THREAD_START_INFO;

static DWORD WINAPI xbox_thread_wrapper(LPVOID lpParameter)
{
    XBOX_THREAD_START_INFO info = *(XBOX_THREAD_START_INFO*)lpParameter;

    /* Free the start info before calling the routine - the routine may
     * never return (calling PsTerminateSystemThread instead) */
    HeapFree(GetProcessHeap(), 0, lpParameter);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "Thread %u starting at %p",
        GetCurrentThreadId(), info.StartRoutine);

    info.StartRoutine(info.StartContext);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD, "Thread %u returned normally",
        GetCurrentThreadId());

    return 0;
}

/* ============================================================================
 * PsCreateSystemThreadEx
 *
 * Xbox signature:
 *   PsCreateSystemThreadEx(
 *     OUT PHANDLE ThreadHandle,
 *     IN ULONG ThreadExtraSize,      // extra bytes in thread object (ignored)
 *     IN ULONG KernelStackSize,      // stack size (0 = default)
 *     IN ULONG TlsDataSize,          // TLS data size (Xbox-specific, ignored)
 *     OUT PULONG ThreadId,           // optional thread ID
 *     IN PVOID StartContext1,        // context passed to StartRoutine
 *     IN PVOID StartContext2,        // alternate context (unused by game code)
 *     IN BOOLEAN CreateSuspended,
 *     IN BOOLEAN DebugStack,         // debug stack (ignored)
 *     IN PXBOX_SYSTEM_ROUTINE StartRoutine
 *   )
 *
 * Maps to: CreateThread with a wrapper for calling convention adaptation.
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsCreateSystemThreadEx(
    PHANDLE ThreadHandle,
    ULONG ThreadExtraSize,
    ULONG KernelStackSize,
    ULONG TlsDataSize,
    PULONG ThreadId,
    PVOID StartContext1,
    PVOID StartContext2,
    BOOLEAN CreateSuspended,
    BOOLEAN DebugStack,
    PXBOX_SYSTEM_ROUTINE StartRoutine)
{
    XBOX_THREAD_START_INFO* info;
    HANDLE hThread;
    DWORD dwThreadId;
    DWORD dwCreationFlags;

    (void)ThreadExtraSize;
    (void)TlsDataSize;
    (void)StartContext2;
    (void)DebugStack;

    if (!ThreadHandle || !StartRoutine)
        return STATUS_INVALID_PARAMETER;

    /* Allocate start info - freed by the wrapper thread */
    info = (XBOX_THREAD_START_INFO*)HeapAlloc(GetProcessHeap(), 0, sizeof(XBOX_THREAD_START_INFO));
    if (!info)
        return STATUS_NO_MEMORY;

    info->StartRoutine = StartRoutine;
    info->StartContext = StartContext1;

    dwCreationFlags = CreateSuspended ? CREATE_SUSPENDED : 0;

    /* Use default stack size if 0 (Xbox default is 64KB) */
    if (KernelStackSize == 0)
        KernelStackSize = 65536;

    hThread = CreateThread(NULL, KernelStackSize, xbox_thread_wrapper, info,
                           dwCreationFlags, &dwThreadId);
    if (!hThread) {
        HeapFree(GetProcessHeap(), 0, info);
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "PsCreateSystemThreadEx: CreateThread failed (error %u)", GetLastError());
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *ThreadHandle = hThread;
    if (ThreadId)
        *ThreadId = dwThreadId;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_THREAD,
        "PsCreateSystemThreadEx: created thread %u (handle=%p, routine=%p, suspended=%d)",
        dwThreadId, hThread, StartRoutine, CreateSuspended);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * PsTerminateSystemThread
 *
 * Terminates the calling thread. On Xbox this is the standard way for
 * system threads to exit. Maps directly to ExitThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "PsTerminateSystemThread: thread %u exiting with status 0x%08X",
        GetCurrentThreadId(), ExitStatus);

    ExitThread((DWORD)ExitStatus);

    /* ExitThread never returns, but the compiler needs this */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * KeDelayExecutionThread
 *
 * Delays the current thread. The interval uses NT 100-nanosecond units:
 *   - Negative values = relative delay (most common)
 *   - Positive values = absolute time (rare)
 *   - Zero = yield
 *
 * Maps to: SleepEx (for alertable waits) or Sleep
 * ============================================================================ */

NTSTATUS __stdcall xbox_KeDelayExecutionThread(
    KPROCESSOR_MODE WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Interval)
{
    DWORD ms;

    (void)WaitMode;

    if (!Interval)
        return STATUS_INVALID_PARAMETER;

    if (Interval->QuadPart == 0) {
        /* Zero interval = yield the thread's time slice */
        SwitchToThread();
        return STATUS_SUCCESS;
    }

    if (Interval->QuadPart < 0) {
        /* Negative = relative time in 100ns units. Convert to milliseconds. */
        LONGLONG relative_100ns = -Interval->QuadPart;
        ms = (DWORD)(relative_100ns / 10000);
        /* Ensure at least 1ms for very short intervals */
        if (ms == 0 && relative_100ns > 0)
            ms = 1;
    } else {
        /* Positive = absolute time. Calculate relative delay from now. */
        LARGE_INTEGER now;
        GetSystemTimeAsFileTime((LPFILETIME)&now);
        LONGLONG diff = Interval->QuadPart - now.QuadPart;
        if (diff <= 0)
            return STATUS_SUCCESS; /* Already past */
        ms = (DWORD)(diff / 10000);
    }

    /* As every guest wait (kernel_sync.c): the executor gets what was
     * queued, and a sleeping thread holds nothing, so it is a safe point. */
    {
        DWORD slept;

        nv2a_kick();
        xbox_kernel_busy(-1);
        slept = Alertable ? SleepEx(ms, TRUE) : (Sleep(ms), 0u);
        xbox_kernel_busy(+1);
        return slept == WAIT_IO_COMPLETION ? STATUS_ALERTED : STATUS_SUCCESS;
    }
}

/* ============================================================================
 * Thread Priority
 *
 * Xbox uses NT KPRIORITY base priority increments relative to the process.
 * We map these to Win32 thread priority levels.
 * ============================================================================ */

/*
 * Map Xbox priority increment to Win32 priority level.
 * Xbox base priorities typically range from -2 to +2 for game threads.
 */
static int xbox_priority_to_win32(LONG increment)
{
    if (increment <= -15)       return THREAD_PRIORITY_IDLE;
    else if (increment <= -2)   return THREAD_PRIORITY_LOWEST;
    else if (increment == -1)   return THREAD_PRIORITY_BELOW_NORMAL;
    else if (increment == 0)    return THREAD_PRIORITY_NORMAL;
    else if (increment == 1)    return THREAD_PRIORITY_ABOVE_NORMAL;
    else if (increment <= 2)    return THREAD_PRIORITY_HIGHEST;
    else                        return THREAD_PRIORITY_TIME_CRITICAL;
}

static LONG win32_priority_to_xbox(int priority)
{
    switch (priority) {
        case THREAD_PRIORITY_IDLE:          return -15;
        case THREAD_PRIORITY_LOWEST:        return -2;
        case THREAD_PRIORITY_BELOW_NORMAL:  return -1;
        case THREAD_PRIORITY_NORMAL:        return 0;
        case THREAD_PRIORITY_ABOVE_NORMAL:  return 1;
        case THREAD_PRIORITY_HIGHEST:       return 2;
        case THREAD_PRIORITY_TIME_CRITICAL: return 15;
        default:                            return 0;
    }
}

LONG __stdcall xbox_KeSetBasePriorityThread(PVOID Thread, LONG Increment)
{
    HANDLE hThread = (HANDLE)Thread;
    LONG previous;

    /* Get previous priority before setting new one */
    int prev_win32 = GetThreadPriority(hThread);
    previous = win32_priority_to_xbox(prev_win32);

    int new_win32 = xbox_priority_to_win32(Increment);
    SetThreadPriority(hThread, new_win32);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "KeSetBasePriorityThread: thread=%p, increment=%d (win32=%d), prev=%d",
        Thread, Increment, new_win32, previous);

    return previous;
}

LONG __stdcall xbox_KeQueryBasePriorityThread(PVOID Thread)
{
    HANDLE hThread = (HANDLE)Thread;
    int win32_priority = GetThreadPriority(hThread);
    return win32_priority_to_xbox(win32_priority);
}

/* ============================================================================
 * KeAlertThread
 *
 * Sends an alert to a thread, which can wake it from an alertable wait.
 * On Xbox, this sets the alerted flag on the thread object.
 * We approximate this with QueueUserAPC using a no-op APC routine.
 * ============================================================================ */

static VOID CALLBACK xbox_alert_apc(ULONG_PTR dwParam)
{
    (void)dwParam;
    /* No-op - the purpose is just to wake the thread from alertable wait */
}

NTSTATUS __stdcall xbox_KeAlertThread(PVOID Thread, KPROCESSOR_MODE AlertMode)
{
    HANDLE hThread = (HANDLE)Thread;

    (void)AlertMode;

    if (!QueueUserAPC(xbox_alert_apc, hThread, 0)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_THREAD,
            "KeAlertThread: QueueUserAPC failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtYieldExecution
 *
 * Yields the current thread's remaining time slice.
 * Maps directly to SwitchToThread.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtYieldExecution(void)
{
    SwitchToThread();
    return STATUS_SUCCESS;
}

/* ============================================================================
 * NtDuplicateObject
 *
 * Duplicates a kernel handle. On Xbox this is simpler than Win32 since
 * there's only one process. Maps to DuplicateHandle within the same process.
 * ============================================================================ */

NTSTATUS __stdcall xbox_NtDuplicateObject(
    HANDLE SourceHandle,
    PHANDLE TargetHandle,
    ULONG Options)
{
    HANDLE hProcess = GetCurrentProcess();
    DWORD dwOptions = 0;

    if (!TargetHandle)
        return STATUS_INVALID_PARAMETER;

    /* Xbox DUPLICATE_CLOSE_SOURCE = 0x1, same as Win32 */
    if (Options & 0x1)
        dwOptions |= DUPLICATE_CLOSE_SOURCE;
    /* Xbox DUPLICATE_SAME_ACCESS = 0x2, same as Win32 */
    if (Options & 0x2)
        dwOptions |= DUPLICATE_SAME_ACCESS;

    if (!DuplicateHandle(hProcess, SourceHandle, hProcess, TargetHandle,
                         0, FALSE, dwOptions)) {
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtDuplicateObject: DuplicateHandle failed (error %u)", GetLastError());
        return STATUS_UNSUCCESSFUL;
    }

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtDuplicateObject: source=%p → target=%p (options=0x%X)",
        SourceHandle, *TargetHandle, Options);

    return STATUS_SUCCESS;
}

/* Safe points for NtSuspendThread.
 *
 * The Xbox kernel never leaves a suspended thread holding one of its locks:
 * the thread stops in title code or in a kernel wait. A host SuspendThread
 * stops it wherever it is -- inside a bridge call that holds a bridge lock,
 * the CRT's stderr lock, the heap's -- and then every thread wanting that
 * lock stops as well. CRI's stream scheduler suspends and resumes its
 * threads all the time; this froze JSRF's predecessors at boot and mid-level.
 *
 * So each thread keeps a count of how deep it is in the kernel
 * (kernel_thunk_dispatch adds to it, raised IRQL too), stepping out of it
 * while blocked in a host wait, which holds nothing. A suspend that catches
 * its target with a count above zero lets it go, yields, and tries again,
 * for up to 100 ms, so the thread ends up stopped at a safe point.
 */
#define MAX_KERNEL_THREADS 256

typedef struct {
    volatile LONG thread_id;            /* 0 until the slot is filled in */
    volatile LONG depth;                /* kernel calls + raised IRQL */
} KernelDepth;

static KernelDepth          g_depths[MAX_KERNEL_THREADS];
static volatile LONG        g_depths_used;
static XBOX_THREAD_LOCAL KernelDepth *t_depth;

/* The slot of thread `id`, or NULL if it has never entered the kernel. */
static KernelDepth *depth_of(DWORD id)
{
    LONG used = g_depths_used, i;

    if (used > MAX_KERNEL_THREADS)
        used = MAX_KERNEL_THREADS;
    for (i = 0; i < used; i++)
        if ((DWORD)g_depths[i].thread_id == id)
            return &g_depths[i];
    return NULL;
}

void xbox_kernel_busy(int delta)
{
    if (t_depth == NULL) {
        LONG slot = InterlockedIncrement(&g_depths_used) - 1;
        if (slot >= MAX_KERNEL_THREADS)
            return;                     /* untracked: suspends it anywhere */
        g_depths[slot].thread_id = (LONG)GetCurrentThreadId();
        t_depth = &g_depths[slot];
    }
    t_depth->depth += delta;
}

NTSTATUS __stdcall xbox_NtSuspendThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD target = GetThreadId(ThreadHandle);
    KernelDepth *in_kernel = target == GetCurrentThreadId() ? NULL : depth_of(target);
    ULONGLONG deadline = GetTickCount64() + 100;
    DWORD prev;

    for (;;) {
        /* (DWORD)-1 is failure, which the out-parameter cannot say: an
         * error status, not a suspend count of 0xFFFFFFFF. */
        prev = SuspendThread(ThreadHandle);
        if (prev == (DWORD)-1) {
            xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
                "NtSuspendThread: SuspendThread failed (error %u)", GetLastError());
            return STATUS_UNSUCCESSFUL;
        }
        /* Already suspended before this call, never in the kernel, at a
         * safe point, or out of patience: it stays stopped where it is. */
        if (prev != 0 || in_kernel == NULL || in_kernel->depth <= 0
                || GetTickCount64() > deadline)
            break;
        ResumeThread(ThreadHandle);
        SwitchToThread();
    }

    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtSuspendThread: thread=%p previous_count=%u", ThreadHandle, prev);
    return STATUS_SUCCESS;
}

/* Diagnostics for NtResumeThread: the first failures, and a thread that
 * keeps resuming another that was never suspended (a scheduler spinning). */
static void note_resume_failed(HANDLE thread)
{
    static int reported;

    if (reported < 10) {
        reported++;
        fprintf(stderr, "  [KERNEL] NtResumeThread(%p) failed: error %lu (thread %lu)\n",
                thread, (unsigned long)GetLastError(), (unsigned long)GetThreadId(thread));
    }
}

static void note_resume_of_running(HANDLE thread, DWORD previous_count)
{
    static XBOX_THREAD_LOCAL HANDLE same;
    static XBOX_THREAD_LOCAL unsigned in_a_row;

    if (previous_count != 0 || thread != same) {
        same = thread;
        in_a_row = 0;
        return;
    }
    if (++in_a_row == 20000)
        fprintf(stderr, "  [KERNEL] NtResumeThread: thread %lu has resumed thread %lu"
                        " (handle %p) 20000 times in a row, and it was never suspended\n",
                (unsigned long)GetCurrentThreadId(), (unsigned long)GetThreadId(thread), thread);
}

NTSTATUS __stdcall xbox_NtResumeThread(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount)
{
    DWORD prev = ResumeThread(ThreadHandle);

    if (prev == (DWORD)-1) {             /* as in NtSuspendThread */
        xbox_log(XBOX_LOG_ERROR, XBOX_LOG_THREAD,
            "NtResumeThread: ResumeThread failed (error %u)", GetLastError());
        note_resume_failed(ThreadHandle);
        return STATUS_UNSUCCESSFUL;
    }
    note_resume_of_running(ThreadHandle, prev);
    if (PreviousSuspendCount)
        *PreviousSuspendCount = (ULONG)prev;
    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_THREAD,
        "NtResumeThread: thread=%p previous_count=%u", ThreadHandle, prev);
    return STATUS_SUCCESS;
}


/* ================================================================
 * Worker stack slices + game-thread tracking (host-tick-driven titles)
 * ================================================================
 *
 * Ported from the Burnout 3 fork as the runtimes reunite. A host-driven title
 * returns from its entry after spawning an init thread and expects the host's
 * own thread to drive the per-frame tick; to call recompiled code from there it
 * needs a guest stack, which a worker slice provides. Additive: a default-model
 * title never calls any of this, so it is inert for Halo, Crimson Skies, etc.
 * See docs/technical/burnout3-reunification.md and XBOX_WORKER_STACK_* in
 * xbox_memory_layout.h.
 */

/* The game's own thread, kept so a wedged boot can be inspected from the host
 * watchdog. Set when a title's game thread is spawned; NULL under the default
 * inline model, where there is no separate thread to sample. */
static HANDLE g_game_thread = NULL;

void  xbox_set_game_thread(void *h) { g_game_thread = (HANDLE)h; }
void *xbox_thread_debug_handle(void) { return (void *)g_game_thread; }

/* One bit per worker stack slice. Interlocked because a host-driven title can
 * allocate a slice from the host thread while recompiled code allocates one for
 * a spawned worker, so the allocator itself must be thread-safe. */
static volatile LONG g_worker_stack_used[XBOX_WORKER_STACK_COUNT];

int xbox_worker_stack_alloc(void)
{
    int i;
    for (i = 0; i < XBOX_WORKER_STACK_COUNT; i++) {
        if (InterlockedCompareExchange(&g_worker_stack_used[i], 1, 0) == 0)
            return i;
    }
    return -1;  /* all slices in use */
}

void xbox_worker_stack_free(int slot)
{
    if (slot >= 0 && slot < XBOX_WORKER_STACK_COUNT)
        InterlockedExchange(&g_worker_stack_used[slot], 0);
}
