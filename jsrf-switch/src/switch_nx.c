/*
 * switch_nx.c -- the Nintendo Switch host layer of the JSRF recompilation.
 *
 * Everything the port needs from Horizon that is not the game or the GPU:
 *
 *   log        stdout/stderr into RAM, written to SD:/switch/jsrf/jsrf_log.txt
 *              twice a second by a thread of its own (or sent to nxlink when
 *              the NRO was started with `nxlink -s`); every line timestamped
 *   settings   SD:/switch/jsrf/jsrf_env.txt, KEY=VALUE lines -> environment
 *   statistics every 10 s, [perf] lines: frame rate, audio, renderer, threads
 *   profiler   RECOMP_NX_PROFILE=1: busy threads sampled at 1 kHz, prof.bin
 *   clocks     a docked boost and JSRF_*_MHZ overrides, kept across dock
 *              changes and put back on exit
 *   applet     the system's messages (dock, focus, exit) taken off the queue
 *   errors     the system error dialog, and a report on a CPU exception
 *   audio      the SDL audio thread's priority, and a count of output gaps
 *
 * Kept apart from main.c: <switch.h> and the runtime's Win32 vocabulary share
 * names (Event, Thread, MemoryRegion...). The Switch build renders with
 * deko3d (nv2a_dk), which draws its own loading screen.
 */
#ifdef __SWITCH__
#include <switch.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include "apu_output.h"
#include "nv2a_dk.h"

#if !defined(JSRF_DEKO3D)
#error "The Switch build renders with deko3d: configure with -DJSRF_DEKO3D=ON"
#endif

#ifndef JSRF_SD_HOME
#define JSRF_SD_HOME "sdmc:/switch/jsrf"     /* CMake: JSRF_HOME_NAME */
#endif
#define HOME_DIR JSRF_SD_HOME

extern ptrdiff_t g_xbox_mem_offset;
void print_guest_state(void);

/* Elsewhere in the runtime. */
#include "platform/nx_threads.h"
void  xbox_perf_sample(void);
void  xbox_vblank_report(double dt, char *buf, size_t n);
void  xbox_main_wait_report(double secs);
void  xbox_WatchdogDump(const char *why);
int   mcpx_apu_frame_rate(void);
float mcpx_apu_busy_fraction(void);
int   mcpx_apu_pacing_stats(char *buf, int cap);
void  mcpx_apu_ring_stats(uint32_t *stale, uint32_t *total);

static u64 g_boot_tick;

static u64 ns_since(u64 tick)
{
    return armTicksToNs(armGetSystemTick() - tick);
}

/* The first character of an environment value, 0 when unset or empty. */
static char env_first(const char *name)
{
    const char *v = getenv(name);
    return v ? v[0] : 0;
}

/* ═══ Log ════════════════════════════════════════════════════════════════
 *
 * A "log:" device takes stdout and stderr. Text goes into a RAM buffer under
 * a mutex; a writer thread swaps that buffer for a second one twice a second
 * and puts the full one on the card with no lock held, so threads logging
 * meanwhile never wait for the SD card. The runtime flushes stderr after
 * many lines, and with the streams on a card file every flush was a write:
 * boot took half a minute that way. A buffer that fills between two turns
 * is written on the spot. HOME closes a hung title without flushing
 * anything, so the writer is also what keeps the tail of the log.
 *
 * JSRF_LOG_SYNC=1 writes and syncs every line (a hard freeze then loses
 * nothing, at the old cost). JSRF_NO_LOG=1 or JSRF_LOG=0 drops everything
 * after the settings, the statistics and the profiler included. */
#define LOG_BUF_BYTES (512u * 1024u)

static struct {
    Mutex         lock;
    char         *fill;         /* takes new text */
    char         *spare;        /* idle, or being written by the writer */
    size_t        used;         /* bytes in fill */
    int           fd;
    int           at_line_start;
    int           every_line;   /* JSRF_LOG_SYNC */
    volatile int  dropped;      /* JSRF_NO_LOG */
    volatile int  writing;      /* spare is on its way to the card */
} g_log = { .fd = -1, .at_line_start = 1 };

static int g_nxlink = -1;

static void write_fully(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

static void wait_for_writer(void)
{
    while (__atomic_load_n(&g_log.writing, __ATOMIC_ACQUIRE))
        svcSleepThread(200000ull);
}

/* Lock held. */
static void log_append(const char *p, size_t n)
{
    if (g_log.used + n > LOG_BUF_BYTES) {
        wait_for_writer();          /* older text first */
        write_fully(g_log.fd, g_log.fill, g_log.used);
        g_log.used = 0;
    }
    if (n > LOG_BUF_BYTES) {
        write_fully(g_log.fd, p, n);
        return;
    }
    memcpy(g_log.fill + g_log.used, p, n);
    g_log.used += n;
}

static ssize_t logdev_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    size_t done = 0;
    (void)r; (void)fd;
    if (g_log.dropped)
        return (ssize_t)len;
    mutexLock(&g_log.lock);
    while (done < len) {
        const char *nl = memchr(ptr + done, '\n', len - done);
        size_t piece = nl ? (size_t)(nl - (ptr + done)) + 1 : len - done;
        if (g_log.at_line_start) {
            /* Integer arithmetic only: this runs inside the printf that is
             * writing the line, and a nested %f would share newlib's
             * per-thread conversion buffer with it. */
            char stamp[24];
            u64 ms = ns_since(g_boot_tick) / 1000000ull;
            int k = snprintf(stamp, sizeof stamp, "[%4llu.%03llu] ",
                             (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
            if (k > 0)
                log_append(stamp, (size_t)k);
        }
        log_append(ptr + done, piece);
        g_log.at_line_start = nl != NULL;
        done += piece;
    }
    if (g_log.every_line) {
        wait_for_writer();
        write_fully(g_log.fd, g_log.fill, g_log.used);
        g_log.used = 0;
        fsync(g_log.fd);
    }
    mutexUnlock(&g_log.lock);
    return (ssize_t)len;
}

static int logdev_open(struct _reent *r, void *f, const char *path, int flags, int mode)
{
    (void)r; (void)f; (void)path; (void)flags; (void)mode;
    return 0;
}

static int logdev_close(struct _reent *r, void *fd)
{
    (void)r; (void)fd;
    return 0;
}

static int logdev_fstat(struct _reent *r, void *fd, struct stat *st)
{
    (void)r; (void)fd;
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR;
    return 0;
}

static const devoptab_t g_logdev = {
    .name       = "log",
    .structSize = sizeof(int),
    .open_r     = logdev_open,
    .close_r    = logdev_close,
    .write_r    = logdev_write,
    .fstat_r    = logdev_fstat,
};

/* Everything logged so far onto the card. `crashing`: the faulting thread
 * may be the one holding the lock, so it is only tried. */
static void log_flush(int crashing)
{
    char *full;
    size_t n;

    fflush(stdout);
    fflush(stderr);
    if (g_log.fd < 0)
        return;
    if (crashing) {
        int got = mutexTryLock(&g_log.lock);
        write_fully(g_log.fd, g_log.fill, g_log.used);
        g_log.used = 0;
        if (got)
            mutexUnlock(&g_log.lock);
        return;
    }
    mutexLock(&g_log.lock);
    if (!g_log.spare) {             /* one buffer only: write it under the lock */
        write_fully(g_log.fd, g_log.fill, g_log.used);
        g_log.used = 0;
        mutexUnlock(&g_log.lock);
        return;
    }
    while (__atomic_load_n(&g_log.writing, __ATOMIC_ACQUIRE)) {
        mutexUnlock(&g_log.lock);   /* another flush is on the card */
        svcSleepThread(200000ull);
        mutexLock(&g_log.lock);
    }
    full = g_log.fill;
    n = g_log.used;
    g_log.fill = g_log.spare;
    g_log.spare = full;
    g_log.used = 0;
    __atomic_store_n(&g_log.writing, 1, __ATOMIC_RELEASE);
    mutexUnlock(&g_log.lock);
    write_fully(g_log.fd, full, n);
    __atomic_store_n(&g_log.writing, 0, __ATOMIC_RELEASE);
}

/* For steps that may end the program before the writer's next turn (the
 * renderer's bring-up calls it). */
void xbox_log_sync(void)
{
    log_flush(0);
    if (g_log.fd >= 0)
        fsync(g_log.fd);
}

static void report_statistics(void);

static void *log_writer(void *arg)
{
    unsigned turn = 0;
    (void)arg;
    while (!g_log.dropped) {
        svcSleepThread(500000000ull);          /* 0.5 s */
        if (g_log.dropped)
            break;
        if (++turn % 20 == 0)                  /* every 10 s */
            report_statistics();
        log_flush(0);
    }
    return NULL;
}

static int log_start(void)
{
    pthread_t t;

    mutexInit(&g_log.lock);
    g_log.fill = malloc(LOG_BUF_BYTES);
    g_log.spare = malloc(LOG_BUF_BYTES);       /* may be NULL: flushes under the lock */
    g_log.fd = open(HOME_DIR "/jsrf_log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!g_log.fill || g_log.fd < 0 || AddDevice(&g_logdev) < 0)
        return 0;
    if (!freopen("log:", "w", stdout))
        return 0;
    dup2(fileno(stdout), fileno(stderr));
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (pthread_create(&t, NULL, log_writer, NULL) == 0)
        pthread_detach(t);
    return 1;
}

/* ═══ Settings ═══════════════════════════════════════════════════════════
 *
 * A console has no environment: SD:/switch/jsrf/jsrf_env.txt provides one.
 * KEY=VALUE per line; blank lines and lines starting with '#' are skipped,
 * trailing spaces and line ends are trimmed. */
static void settings_load(void)
{
    char line[1024];
    FILE *f = fopen(HOME_DIR "/jsrf_env.txt", "r");

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        char *eq;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = '\0';
        if (!n || line[0] == '#')
            continue;
        eq = strchr(line, '=');
        if (!eq || eq == line)
            continue;
        *eq = '\0';
        setenv(line, eq + 1, 1);
        printf("[switch] env %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

/* ═══ SDL's audio thread ═════════════════════════════════════════════════
 *
 * switch-sdl2 plays through audren with two 1024-sample wave buffers
 * (21.3 ms each); its thread refills one while the other plays. It asks for
 * SDL_THREAD_PRIORITY_TIME_CRITICAL, which SDL's Switch port maps to the
 * time-sliced priority 59, where busy game threads kept it waiting and the
 * output crackled. Both functions below are wrapped at link time (--wrap in
 * CMakeLists.txt): the time-critical request gets priority 0x2B instead
 * (RECOMP_NX_AUDIO_PRIO=0: as SDL asks), and every wave buffer handed to the
 * driver is counted, with a gap when the one before had already run out. */
int __real_SDL_SYS_SetThreadPriority(int prio);
bool __real_audrvVoiceAddWaveBuf(AudioDriver *d, int id, AudioDriverWaveBuf *wb);

static struct {
    AudioDriverWaveBuf *previous;
    u64                 last_tick;
    volatile uint32_t   buffers, gaps, worst_us;
} g_aout;

int __wrap_SDL_SYS_SetThreadPriority(int prio)
{
    if (prio == 3 /* SDL_THREAD_PRIORITY_TIME_CRITICAL */ && env_first("RECOMP_NX_AUDIO_PRIO") != '0') {
        Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
        fprintf(stderr, "  [AUDIO] audio thread raised to 0x2B (result 0x%X)\n", rc);
        if (R_SUCCEEDED(rc))
            return 0;
    }
    return __real_SDL_SYS_SetThreadPriority(prio);
}

bool __wrap_audrvVoiceAddWaveBuf(AudioDriver *d, int id, AudioDriverWaveBuf *wb)
{
    u64 now = armGetSystemTick();

    if (g_aout.previous && g_aout.previous != wb) {
        audrvUpdate(d);
        if (g_aout.previous->state == AudioDriverWaveBufState_Done)
            g_aout.gaps++;
    }
    if (g_aout.last_tick) {
        uint32_t us = (uint32_t)(armTicksToNs(now - g_aout.last_tick) / 1000u);
        if (us > g_aout.worst_us)
            g_aout.worst_us = us;
    }
    g_aout.last_tick = now;
    g_aout.previous = wb;
    g_aout.buffers++;
    return __real_audrvVoiceAddWaveBuf(d, id, wb);
}

/* ═══ Statistics ═════════════════════════════════════════════════════════
 *
 * Every 10 s (from the log writer): presented frames per second, the sound
 * chip's pace and the audio output's health, the renderer's GPU and CPU
 * time, where the main thread waited, and each busy thread's share of a
 * core -- the console has no profiler on hand. A title that presented and
 * then shows nothing for 20 s is hung: the watchdog's snapshot of the main
 * guest thread goes into the log then, and again 20 s later. */
static void report_statistics(void)
{
    static uint32_t frames_before, frames_at_last_change;
    static u64 tick_before;
    static uint64_t underruns_before, dropped_before;
    static int presented_once, idle_reports;
    static char renderer[4096];
    uint32_t frames = nv2a_dk_frame_count();
    u64 now = armGetSystemTick();
    double secs = tick_before ? (double)armTicksToNs(now - tick_before) / 1e9 : 10.0;
    char text[256];
    AudioOutStats out;
    uint32_t stale, total;

    /* The real interval: this thread is time-sliced with the game's, and a
     * late report divided by 10 s overstated the frame rate. */
    if (secs < 1.0)
        secs = 10.0;
    tick_before = now;

    xbox_vblank_report(secs, text, sizeof text);
    fprintf(stderr, "[perf] %.1f fps over %.1f s, textures %llu MB, %u shader programs, %s\n",
            (frames - frames_before) / secs, secs,
            (unsigned long long)(nv2a_dk_texture_bytes() >> 20), nv2a_dk_program_count(), text);
    frames_before = frames;

    fprintf(stderr, "[perf] APU %d frames/s (1500 = real time), frame thread %.0f%% busy\n",
            mcpx_apu_frame_rate(), mcpx_apu_busy_fraction() * 100.0f);
    mcpx_apu_pacing_stats(text, (int)sizeof text);
    fprintf(stderr, "[perf] APU pacing: %s\n", text);

    audio_out_stats(&out);
    fprintf(stderr, "[perf] audio out: %u buffers, %u device gaps, worst %u.%u ms between"
                    " buffers, %llu queue underruns, %llu dropped, peak %d\n",
            g_aout.buffers, g_aout.gaps, g_aout.worst_us / 1000u, g_aout.worst_us / 100u % 10u,
            (unsigned long long)(out.ran_dry - underruns_before),
            (unsigned long long)(out.dropped - dropped_before), out.loudest);
    underruns_before = out.ran_dry;
    dropped_before = out.dropped;
    g_aout.buffers = g_aout.gaps = g_aout.worst_us = 0;

    /* Samples the voice processor read unchanged from a lap (50 ms) before:
     * the game's mixer running late. */
    mcpx_apu_ring_stats(&stale, &total);
    fprintf(stderr, "[perf] APU ring voices: %u of %u samples stale (%u.%02u%%)\n",
            stale, total, total ? (unsigned)(100ull * stale / total) : 0u,
            total ? (unsigned)(10000ull * stale / total % 100u) : 0u);

    nv2a_dk_perf(renderer, sizeof renderer, secs);
    fprintf(stderr, "%s\n", renderer);
    xbox_main_wait_report(secs);
    xbox_perf_sample();
    nx_thread_report(secs);

    if (frames != frames_at_last_change) {
        presented_once = 1;
        idle_reports = 0;
        frames_at_last_change = frames;
    } else if (presented_once) {
        idle_reports++;
        if (idle_reports == 2)
            xbox_WatchdogDump("no frame presented for 20 s");
        else if (idle_reports == 4)
            xbox_WatchdogDump("still no frame after 40 s");
    }
}

/* ═══ Profiler (RECOMP_NX_PROFILE=1) ═════════════════════════════════════
 *
 * Eden's costs are not the console's, and the console has no profiler. A
 * thread above all the others wakes every millisecond and, for each thread
 * that is busy, pauses it (svcSetThreadActivity), reads its registers
 * (svcGetThreadContext3) and lets it go: pc, lr and up to five return
 * addresses along the x29 frame chain, all as offsets into the NRO.
 * "Busy" is decided every 100 ms from the threads' tick counts: more than
 * 10% of a core, or 2% with RECOMP_NX_PROFILE=2 (to catch the DPC and
 * interrupt threads). Every 10 s the samples are appended to
 * SD:/switch/jsrf/prof.bin -- in 64 KB pieces with a pause between, since
 * the card does one thing at a time and one big write held the game's own
 * file opens behind it -- and a "[prof] slots:" line names each thread
 * slot by its entry point. switch/prof_report.sh (tools/prof_report) reads
 * both with the ELF. Costs a few percent of a core while on: leave it off
 * for playing.
 *
 * Record (32 bytes): u16 thread slot, u16 time in 10 ms units since boot,
 * u32 pc, u32 lr, u32 return addresses[5] (0xFFFFFFFF: none). */
typedef struct {
    u16 slot, when;
    u32 pc, lr, ret[5];
} ProfRecord;

#define PROF_CAPACITY 120000
#define PROF_THREADS  64

extern void _start(void);   /* the NRO's first instruction: ELF address 0 */

static ProfRecord g_prof[PROF_CAPACITY];
static unsigned   g_prof_count;
static int        g_prof_low_bar;   /* RECOMP_NX_PROFILE=2 */

static u32 nro_offset(u64 addr)
{
    u64 base = (u64)(uintptr_t)&_start;
    return addr >= base && addr - base < 0xFFFFFFFFull ? (u32)(addr - base) : 0xFFFFFFFFu;
}

static void prof_save(void)
{
    const unsigned per_piece = 65536u / sizeof(ProfRecord);
    unsigned at;
    FILE *f;

    if (!g_prof_count)
        return;
    f = fopen(HOME_DIR "/prof.bin", "ab");
    if (f) {
        setvbuf(f, NULL, _IONBF, 0);
        for (at = 0; at < g_prof_count; at += per_piece) {
            unsigned n = g_prof_count - at;
            if (n > per_piece)
                n = per_piece;
            fwrite(&g_prof[at], sizeof(ProfRecord), n, f);
            svcSleepThread(3000000ull);
        }
        fclose(f);
    }
    fprintf(stderr, "[prof] %u samples written\n", g_prof_count);
    g_prof_count = 0;
}

static void prof_name_slots(void)
{
    char line[1024];
    int len = snprintf(line, sizeof line, "[prof] slots:");
    Handle h;
    uintptr_t entry;
    int i;

    for (i = 0; i < PROF_THREADS && len < 1000 && nx_thread_at(i, &h, &entry); i++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %d=%x", i,
                        entry ? nro_offset(entry) : 0u);
    fprintf(stderr, "%s\n", line);
}

/* Fill one record from a paused thread. */
static void prof_take(int slot, Handle h)
{
    ThreadContext ctx;
    ProfRecord *rec;
    MemoryInfo mi;
    u32 page;
    u64 fp;
    int k;

    if (R_FAILED(svcGetThreadContext3(&ctx, h)))
        return;
    rec = &g_prof[g_prof_count++];
    rec->slot = (u16)slot;
    rec->when = (u16)((armGetSystemTick() - g_boot_tick) / 192000u);   /* 19.2 MHz -> 10 ms */
    rec->pc = nro_offset(ctx.pc.x);
    rec->lr = nro_offset(ctx.lr);
    for (k = 0; k < 5; k++)
        rec->ret[k] = 0xFFFFFFFFu;
    /* Walk the frame records within the stack's own mapping. */
    fp = ctx.fp;
    if ((fp & 7) || R_FAILED(svcQueryMemory(&mi, &page, fp)) || !(mi.perm & Perm_R))
        return;
    for (k = 0; k < 5; k++) {
        const u64 *frame;
        if (fp < mi.addr || fp + 16 > mi.addr + mi.size || (fp & 7))
            break;
        frame = (const u64 *)(uintptr_t)fp;
        rec->ret[k] = nro_offset(frame[1]);
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
}

static void prof_main(void *arg)
{
    Handle threads[PROF_THREADS];
    u64 ticks_before[PROF_THREADS] = { 0 };
    int busy[PROF_THREADS] = { 0 };
    int count = 0, i;
    u64 freq = armGetSystemTickFreq();
    u64 last_census = 0, last_save = armGetSystemTick();

    (void)arg;
    remove(HOME_DIR "/prof.bin");
    for (;;) {
        u64 now;

        svcSleepThread(1000000ull);
        now = armGetSystemTick();

        if (now - last_census > freq / 10) {
            u64 span = now - last_census;
            int any_ticks = 0;
            uintptr_t entry;
            for (count = 0; count < PROF_THREADS && nx_thread_at(count, &threads[count], &entry);
                 count++) {
                u64 t = 0;
                if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, threads[count], (u64)-1)))
                    t = 0;
                if (t)
                    any_ticks = 1;
                /* Thread ticks run at the system tick's 19.2 MHz. */
                busy[count] = last_census
                    && (t - ticks_before[count]) * (g_prof_low_bar ? 50u : 10u) > span;
                ticks_before[count] = t;
            }
            /* Eden reports no tick counts: the main thread only there (pausing
             * every thread every millisecond hung its boot). */
            if (!any_ticks)
                for (i = 0; i < count; i++)
                    busy[i] = i == 0;
            last_census = now;
        }

        for (i = 0; i < count && g_prof_count < PROF_CAPACITY; i++) {
            if (!busy[i] || R_FAILED(svcSetThreadActivity(threads[i], ThreadActivity_Paused)))
                continue;
            prof_take(i, threads[i]);
            svcSetThreadActivity(threads[i], ThreadActivity_Runnable);
        }

        if (now - last_save > freq * 10) {
            prof_name_slots();
            prof_save();
            last_save = armGetSystemTick();
        }
    }
}

static void prof_start(void)
{
    static Thread t;
    char mode = env_first("RECOMP_NX_PROFILE");

    if (mode != '1' && mode != '2')
        return;
    g_prof_low_bar = mode == '2';
    /* 0x2A: above every game and host thread, so its samples are on time. */
    if (R_FAILED(threadCreate(&t, prof_main, NULL, NULL, 0x4000, 0x2A, -2))
        || R_FAILED(threadStart(&t))) {
        fprintf(stderr, "[prof] profiler thread failed to start: no profile this run\n");
        return;
    }
    fprintf(stderr, "[prof] on: busy threads sampled every 1 ms into prof.bin (NRO loaded at %p)\n",
            (void *)&_start);
}

/* ═══ Clocks ═════════════════════════════════════════════════════════════
 *
 * Docked, the CPU goes to 1785 MHz and the GPU to 921.6 MHz (the system's
 * docked profile is 1020 / 768): the top of the Switch's own clock tables,
 * no patches, on mains power with the dock's fan. In the hand the system's
 * clocks are kept (battery and heat). JSRF_DOCK_BOOST=0 turns that off.
 *
 * JSRF_CPU_MHZ, JSRF_GPU_MHZ and JSRF_MEM_MHZ ask for a fixed clock in both
 * modes: the highest rate the hardware lists that is not above the request,
 * capped at 1785 / 921.6 / 1600 MHz.
 *
 * Set through clkrst (8.0.0 and later) or pcv. The system resets clocks on a
 * dock change and after sleep, so a thread applies them again every second
 * while the game has focus, and follows the mode (undocked, the boost goes).
 * The rates from before come back on exit. sys-clk, if installed, may
 * override all of this with its own profile. */
typedef struct {
    const char  *setting, *label;
    PcvModule    pcv_module;
    PcvModuleId  module_id;
    u32          ceiling_hz;            /* highest a setting may ask for */
    u32          boost_hz;              /* docked boost, 0 = none */
    u32          system_hand_hz;        /* the system's own handheld rate */
    u32          system_dock_hz;        /* and docked rate */
    u32          requested_hz;          /* from the setting, 0 = none */
    u32          original_hz;           /* when we started */
    u32          ours_hz;               /* what we set and still hold, 0 = none */
    bool         has_session;
    ClkrstSession session;
} ClockDomain;

static ClockDomain g_clocks[3] = {
    { "JSRF_CPU_MHZ", "CPU",    PcvModule_CpuBus, PcvModuleId_CpuBus,
      1785000000u, 1785000000u, 1020000000u, 1020000000u },
    { "JSRF_GPU_MHZ", "GPU",    PcvModule_GPU,    PcvModuleId_GPU,
       921600000u,  921600000u,  384000000u,  768000000u },
    { "JSRF_MEM_MHZ", "memory", PcvModule_EMC,    PcvModuleId_EMC,
      1600000000u, 0u, 0u, 0u },
};

static struct {
    int           service;          /* 1 clkrst, 0 pcv, -1 none */
    int           boost;            /* docked boost wanted */
    int           mode_docked;      /* the mode last applied, -1 none */
    int           started_docked;   /* the mode original_hz was read in */
    volatile int  stop;
    Thread        keeper;
    int           keeper_running;
} g_clk = { .service = -1, .mode_docked = -1, .started_docked = -1 };

static int is_docked(void)
{
    return appletGetOperationMode() == AppletOperationMode_Console;
}

static Result clock_read(ClockDomain *c, u32 *hz)
{
    return g_clk.service == 1 ? clkrstGetClockRate(&c->session, hz) : pcvGetClockRate(c->pcv_module, hz);
}

static Result clock_write(ClockDomain *c, u32 hz)
{
    return g_clk.service == 1 ? clkrstSetClockRate(&c->session, hz) : pcvSetClockRate(c->pcv_module, hz);
}

/* The highest rate the hardware lists that is not above `want`. */
static u32 clock_nearest(ClockDomain *c, u32 want)
{
    u32 list[64], pick = 0;
    s32 n = 0, k;
    PcvClockRatesListType kind;
    Result rc = g_clk.service == 1
        ? clkrstGetPossibleClockRates(&c->session, list, 64, &kind, &n)
        : pcvGetPossibleClockRates(c->pcv_module, list, 64, &kind, &n);

    if (R_FAILED(rc) || n <= 0 || kind != PcvClockRatesListType_Discrete)
        return want;
    for (k = 0; k < n; k++)
        if (list[k] <= want && list[k] > pick)
            pick = list[k];
    return pick ? pick : want;
}

static bool clock_usable(ClockDomain *c)
{
    return g_clk.service == 0 || c->has_session;
}

/* What a domain should run at in this mode: a setting in both modes, the
 * boost when docked, or 0 for the system's own. */
static u32 clock_goal(ClockDomain *c, int docked)
{
    if (c->requested_hz)
        return c->requested_hz;
    return docked && g_clk.boost ? c->boost_hz : 0u;
}

static void clocks_apply(int verbose)
{
    static unsigned lines;
    int docked = is_docked(), i;
    int mode_changed = docked != g_clk.mode_docked;

    g_clk.mode_docked = docked;
    for (i = 0; i < 3; i++) {
        ClockDomain *c = &g_clocks[i];
        u32 goal, current = 0, after = 0;
        Result rc;

        if (!clock_usable(c) || R_FAILED(clock_read(c, &current)))
            continue;
        goal = clock_goal(c, docked);
        if (!goal) {
            /* The system's rate -- unless our boost outlived an undock. */
            if (c->ours_hz && current == c->ours_hz && !docked && c->system_hand_hz) {
                clock_write(c, clock_nearest(c, c->system_hand_hz));
                clock_read(c, &after);
                printf("[clock] handheld: %s %u -> %u MHz (the system's)\n", c->label,
                       current / 1000000u, after / 1000000u);
            } else if (mode_changed && c->ours_hz) {
                printf("[clock] handheld: %s at the system's %u MHz\n", c->label,
                       current / 1000000u);
            }
            c->ours_hz = 0;
            continue;
        }
        goal = clock_nearest(c, goal);
        if (current == goal && c->ours_hz == goal)
            continue;
        rc = clock_write(c, goal);
        clock_read(c, &after);
        if (R_SUCCEEDED(rc))
            c->ours_hz = after ? after : goal;
        if (verbose || mode_changed || lines++ < 8)
            printf("[clock] %s%s %u -> %u MHz (asked %u)%s\n", docked ? "docked: " : "",
                   c->label, current / 1000000u, after / 1000000u, goal / 1000000u,
                   R_FAILED(rc) ? " -- refused" : "");
    }
}

static void clocks_keeper(void *arg)
{
    (void)arg;
    while (!g_clk.stop) {
        svcSleepThread(1000000000ull);
        if (!g_clk.stop && appletGetFocusState() == AppletFocusState_InFocus)
            clocks_apply(0);
    }
}

static void clocks_restore(void)
{
    int docked, i;

    if (g_clk.service < 0)
        return;
    g_clk.stop = 1;
    if (g_clk.keeper_running) {
        threadWaitForExit(&g_clk.keeper);
        threadClose(&g_clk.keeper);
        g_clk.keeper_running = 0;
    }
    /* The rates from before when still in the mode they were read in, else
     * the system's own for this mode: the HOME menu and the homebrew menu
     * run on in this process's clocks. */
    docked = is_docked();
    for (i = 0; i < 3; i++) {
        ClockDomain *c = &g_clocks[i];
        u32 back = docked == g_clk.started_docked ? c->original_hz
                 : docked ? c->system_dock_hz : c->system_hand_hz;
        if (c->ours_hz && back)
            clock_write(c, clock_nearest(c, back));
    }
    for (i = 0; i < 3; i++)
        if (g_clocks[i].has_session)
            clkrstCloseSession(&g_clocks[i].session);
    if (g_clk.service == 1)
        clkrstExit();
    else
        pcvExit();
    g_clk.service = -1;
}

static void clocks_start(void)
{
    int wanted = 0, i;
    Result rc;

    g_clk.boost = env_first("JSRF_DOCK_BOOST") != '0';
    for (i = 0; i < 3; i++) {
        ClockDomain *c = &g_clocks[i];
        const char *v = getenv(c->setting);
        double mhz = v ? atof(v) : 0.0;
        c->requested_hz = mhz > 0.0 ? (u32)(mhz * 1e6 + 0.5) : 0u;
        if (c->requested_hz > c->ceiling_hz)
            c->requested_hz = c->ceiling_hz;
        if (c->requested_hz || (g_clk.boost && c->boost_hz))
            wanted = 1;
    }
    if (!wanted)
        return;
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize()))
        g_clk.service = 1;
    else if (R_SUCCEEDED(pcvInitialize()))
        g_clk.service = 0;
    else {
        printf("[clock] no clock service (clkrst/pcv): running at the system's clocks\n");
        return;
    }
    for (i = 0; i < 3; i++) {
        ClockDomain *c = &g_clocks[i];
        if (!c->requested_hz && !(g_clk.boost && c->boost_hz))
            continue;
        if (g_clk.service == 1) {
            rc = clkrstOpenSession(&c->session, c->module_id, 3);
            if (R_FAILED(rc)) {
                printf("[clock] %s: no clkrst session (0x%X)\n", c->label, rc);
                continue;
            }
            c->has_session = true;
        }
        clock_read(c, &c->original_hz);
    }
    g_clk.started_docked = is_docked();
    if (g_clk.boost)
        printf("[clock] docked boost on (CPU 1785, GPU 921.6 MHz while docked; the system's"
               " clocks in the hand; JSRF_DOCK_BOOST=0 turns it off)\n");
    clocks_apply(1);
    atexit(clocks_restore);
    /* 0x3B: the lowest priority an application may give a thread. */
    rc = threadCreate(&g_clk.keeper, clocks_keeper, NULL, NULL, 0x4000, 0x3B, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_clk.keeper);
    if (R_SUCCEEDED(rc))
        g_clk.keeper_running = 1;
    else
        printf("[clock] no keeper thread (0x%X): clocks set once, not after a dock change\n", rc);
}

/* ═══ Applet messages ════════════════════════════════════════════════════
 *
 * libnx updates the operation mode (docked / handheld) and the focus state
 * only as the system's messages are taken off the applet queue. Nothing in
 * this build pumps them (SDL opens no video), so a thread does, every
 * 50 ms; otherwise a dock change or the HOME menu would go unseen and the
 * clock keeper would act on stale state. Closing the title from HOME ends
 * the process from the system's side, so an exit request is only logged. */
static void applet_main(void *arg)
{
    int docked = is_docked();

    (void)arg;
    printf("[NX] %s\n", docked ? "docked" : "handheld");
    for (;;) {
        if (!appletMainLoop())
            printf("[NX] exit requested\n");
        if (is_docked() != docked) {
            docked = !docked;
            printf("[NX] now %s\n", docked ? "docked" : "handheld");
        }
        svcSleepThread(50000000ull);
    }
}

static void applet_start(void)
{
    static Thread t;

    if (R_FAILED(threadCreate(&t, applet_main, NULL, NULL, 0x4000, 0x2C, -2))
        || R_FAILED(threadStart(&t)))
        printf("[NX] no applet message thread: dock and focus changes go unseen\n");
}

/* ═══ Boot, exit, errors ═════════════════════════════════════════════════ */

/* Quitting ends the process instead of going back to the homebrew loader:
 * the next NRO would load into this process beside threads still running,
 * an open display layer and heap locked under code memory. Mode 1 tells the
 * system first (as a title exit), so it is not reported as a crash. */
u32 __nx_applet_exit_mode = 1;

/* Loading-screen bar while the disc image is unpacked; -1 hides it. */
void switch_set_progress(int permille)
{
    nv2a_dk_set_progress(permille);
}

/* The shader pack made from the XBE on the first start, on the loading
 * screen (nv2a_dk.c); later starts find it made. */
void switch_build_shaders(const char *xbe_path)
{
    nv2a_dk_build_shaders(xbe_path);
}

static void logging_settings(void)
{
    g_log.every_line = env_first("JSRF_LOG_SYNC") == '1' && !getenv("JSRF_LOG_SYNC")[1];
    if (env_first("JSRF_NO_LOG") == '1' || env_first("JSRF_LOG") == '0') {
        printf("[switch] logging off (JSRF_NO_LOG): no log, [perf] or profiler\n");
        setenv("RECOMP_NO_LOG", "1", 1);       /* the kernel's own log file */
        setenv("RECOMP_QUIET", "1", 1);
        unsetenv("RECOMP_NX_PROFILE");
        unsetenv("JSRF_LOG_SYNC");
        g_log.every_line = 0;
        log_flush(0);
        g_log.dropped = 1;
    }
}

static void describe_process(void)
{
    u64 total = 0, used = 0;
    AppletType type = appletGetAppletType();
    int full = type == AppletType_Application || type == AppletType_SystemApplication;

    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    printf("[switch] applet type %d (%s), memory %lu MB total / %lu MB used\n", (int)type,
           full ? "running as an application, full memory"
                : "running as an applet -- start it by holding R on a game for full memory",
           (unsigned long)(total >> 20), (unsigned long)(used >> 20));
}

void switch_boot(void)
{
    g_boot_tick = armGetSystemTick();
    /* For applet types the system has no exit command for. */
    envSetExitFuncPtr((LoaderReturnFn)&svcExitProcess);
    socketInitializeDefault();
    g_nxlink = nxlinkStdio();
    if (g_nxlink < 0 && !log_start())
        fprintf(stderr, "[switch] no log file: output goes nowhere this run\n");
    settings_load();
    logging_settings();
    describe_process();
    clocks_start();
    nv2a_dk_loader_start();
    prof_start();
    applet_start();
}

void switch_error_dialog(const char *msg, const char *details)
{
    AppletType type = appletGetAppletType();
    Result rc;

    log_flush(0);
    if (type == AppletType_Application || type == AppletType_SystemApplication) {
        ErrorApplicationConfig cfg;
        rc = errorApplicationCreate(&cfg, msg, details);
        if (R_SUCCEEDED(rc))
            rc = errorApplicationShow(&cfg);
    } else {
        ErrorSystemConfig cfg;
        rc = errorSystemCreate(&cfg, msg, details);
        if (R_SUCCEEDED(rc))
            rc = errorSystemShow(&cfg);
    }
    if (R_FAILED(rc))
        fprintf(stderr, "[switch] could not show the error dialog (0x%X)\n", rc);
}

void switch_shutdown(void)
{
    clocks_restore();
    log_flush(0);
    socketExit();
}

/* libnx calls this for a CPU exception in the application: the host and
 * guest state go into the log, which is flushed, and the process ends so the
 * report is the log's last lines. */
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    u64 fault = ctx->far.x;
    fprintf(stderr, "[CRASH] CPU exception %u: pc %#lx, fault address %#lx = guest %#010x\n",
            ctx->error_desc, (unsigned long)ctx->pc.x, (unsigned long)fault,
            (unsigned)(uint32_t)(fault - (u64)(uintptr_t)g_xbox_mem_offset));
    fprintf(stderr, "  lr %#lx, sp %#lx; this build's switch_boot is at %p (subtract it, add"
                    " its ELF address, to look up a host pc)\n",
            (unsigned long)ctx->lr.x, (unsigned long)ctx->sp.x, (void *)switch_boot);
    print_guest_state();
    log_flush(1);
    svcExitProcess();
}

#endif /* __SWITCH__ */
