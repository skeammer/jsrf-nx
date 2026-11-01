/*
 * Jet Set Radio Future (Xbox, NTSC-U), statically recompiled for the
 * Nintendo Switch: the program's entry point and the game's boot.
 *
 * main() sets up the console side (switch_nx.c) and runs the game on a
 * thread with a 16 MB stack. The game thread then, in order:
 *
 *   1. finds the game -- default.xbe in the game folder, or a disc image
 *      next to it, unpacked into the folder once (xiso_extract.c);
 *   2. builds the shader pack if this build's is not on the card yet;
 *   3. lays out the Xbox's memory and loads the XBE into it;
 *   4. starts the device models: the APU, the USB controllers (the pad), and
 *      the deko3d renderer behind the pushbuffer executor;
 *   5. starts the kernel, with JSRF's hooks (ADX guard, D3D wait rescue);
 *   6. calls the XBE's entry point, which runs the game until it exits.
 *
 * On the SD card, under sdmc:/switch/jsrf/ (or the build's own folder,
 * JSRF_HOME_NAME): the NRO, game/ (shared by every build), save/,
 * jsrf_env.txt (settings) and jsrf_log.txt.
 */
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __fastcall
#undef __fastcall
#endif

#include <xbox/xboxrecomp.h>
#include "recomp_types.h"
#include "recomp_switch.h"
#include "adx_guard.h"
#include "xiso_extract.h"
#include "platform/nx_threads.h"

/* switch_nx.c (libnx's names and the Win32 vocabulary cannot share a file). */
void switch_boot(void);
void switch_shutdown(void);
void switch_error_dialog(const char *msg, const char *details);
void switch_set_progress(int permille);         /* loading bar; -1 hides it */
void switch_build_shaders(const char *xbe_path);

/* The guest's registers and memory, for the crash report. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

extern void xbe_entry_point(void);
extern void xbox_gil_mark_main(void);
extern int  recomp_dispatch_init(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);

/* JSRF's XBE. */
#define JSRF_ENTRY_POINT        0x00148023u
#define JSRF_D3D_DEVICE_SLOT    0x0019DCE0u     /* holds the D3D device pointer */
#define JSRF_D3D_FENCE_COUNTER  0x30u           /* device + : SetFence's counter */
#define JSRF_D3D_FENCE_TARGET   0x34u           /* device + : where the GPU writes it */

#ifndef JSRF_SD_HOME
#define JSRF_SD_HOME "sdmc:/switch/jsrf"
#endif
/* Every build reads the same game files; saves are each build's own. */
#define GAME_DIR_DEFAULT "sdmc:/switch/jsrf/game"
#define SAVE_DIR_DEFAULT JSRF_SD_HOME "/save"

#define GAME_STACK_BYTES (16u << 20)

/* ── Reporting ─────────────────────────────────────────────────────────── */

/* The guest's registers, the return addresses on its stack (values inside
 * the XBE's code; the innermost 24) and the last indirect-call targets. For
 * the crash handler in switch_nx.c, and at exit with JSRF_EXIT_TRACE. */
void print_guest_state(void)
{
    const uint32_t stack_end = XBOX_STACK_BASE + XBOX_STACK_SIZE;
    unsigned k;

    fprintf(stderr, "  guest: eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                    "         esi=%08X edi=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    if (g_xbox_mem_offset != 0 && g_esp >= XBOX_STACK_BASE && g_esp < stack_end) {
        uint32_t at;
        int found = 0;

        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (at = g_esp; at < stack_end && at - g_esp < 2048 && found < 24; at += 4) {
            uint32_t word = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + at);
            if (word <= g_xbox_code_lo || word >= g_xbox_code_hi)
                continue;
            fprintf(stderr, "    [esp+%-4u] 0x%08X\n", at - g_esp, word);
            found++;
        }
    }
    fprintf(stderr, "  recent ICALL targets:");
    for (k = 0; k < 16; k++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) % 16]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* A message for the player: the log, then the console's error dialog. */
static void fail(const char *message)
{
    fprintf(stderr, "[FATAL] %s\n", message);
    switch_error_dialog(message, NULL);
}

/* ── Finding the game ──────────────────────────────────────────────────── */

/* The whole of a file, or NULL. */
static void *read_whole_file(const char *path, size_t *bytes)
{
    struct stat info;
    unsigned char *data;
    size_t have = 0;
    FILE *file;

    if (stat(path, &info) != 0 || info.st_size <= 0)
        return NULL;
    file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    data = malloc((size_t)info.st_size);
    while (data != NULL && have < (size_t)info.st_size) {
        size_t got = fread(data + have, 1, (size_t)info.st_size - have, file);
        if (got == 0) {
            free(data);
            data = NULL;
        }
        have += got;
    }
    fclose(file);
    if (data != NULL)
        *bytes = have;
    return data;
}

static int exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

/* Does the name end in .iso (any case), with something before it? */
static int is_iso_name(const char *name)
{
    size_t length = strlen(name);

    return name[0] != '.' && length > 4 && strcasecmp(name + length - 4, ".iso") == 0;
}

/* The first disc image in `dir`, in directory order. */
static int first_iso_in(const char *dir, char *path, size_t size)
{
    DIR *listing = opendir(dir);
    struct dirent *entry;
    int found = 0;

    if (listing == NULL)
        return 0;
    while (!found && (entry = readdir(listing)) != NULL) {
        int written;
        if (!is_iso_name(entry->d_name))
            continue;
        written = snprintf(path, size, "%s/%s", dir, entry->d_name);
        found = written > 0 && (size_t)written < size;
    }
    closedir(listing);
    return found;
}

static void unpack_progress(uint64_t done, uint64_t total, void *unused)
{
    static int tenth_shown = -1;
    int permille = total != 0 ? (int)(done * 1000 / total) : 0;

    (void)unused;
    switch_set_progress(permille);
    if (permille / 100 == tenth_shown)
        return;
    tenth_shown = permille / 100;
    printf("[iso] %d%% (%llu of %llu MB)\n", tenth_shown * 10,
           (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
}

/* No default.xbe in the game folder: unpack a disc image into it, once --
 * JSRF_ISO=<path>, or the first *.iso beside the folder. It goes to
 * <folder>.part and is renamed only when complete, so an unpack cut short
 * (the card pulled, HOME and close) is never taken for a finished one.
 * JSRF_ISO_DELETE=1 deletes the image afterwards.
 * 1: unpacked. 0: no image. -1: failed (the player has been told). */
static int unpack_disc_image(const char *game_dir)
{
    const char *chosen = getenv("JSRF_ISO");
    const char *deleting = getenv("JSRF_ISO_DELETE");
    const char *last_slash = strrchr(game_dir, '/');
    char beside[512], image[768], partial[600], why[256], message[1024];
    size_t parent_length = last_slash != NULL ? (size_t)(last_slash - game_dir) : 0;

    if (parent_length == 0 || parent_length >= sizeof beside)
        return 0;
    memcpy(beside, game_dir, parent_length);
    beside[parent_length] = '\0';
    if (chosen != NULL && chosen[0] != '\0')
        snprintf(image, sizeof image, "%s", chosen);
    else if (!first_iso_in(beside, image, sizeof image))
        return 0;
    snprintf(partial, sizeof partial, "%s.part", game_dir);

    printf("[iso] no game files in %s; unpacking %s (once)\n", game_dir, image);
    if (xiso_extract(image, partial, unpack_progress, NULL, why, sizeof why) != 0) {
        switch_set_progress(-1);
        fprintf(stderr, "[iso] unpack failed: %s\n", why);
        snprintf(message, sizeof message, "Could not unpack the disc image: %s", why);
        fail(message);
        return -1;
    }
    /* An empty game folder made by hand would block the rename; rmdir only
     * removes an empty one. */
    rmdir(game_dir);
    switch_set_progress(-1);
    if (rename(partial, game_dir) != 0) {
        snprintf(message, sizeof message, "The disc image was unpacked to %s, but it could "
                 "not be renamed to %s (is there a %s folder already? Delete it and start "
                 "again).", partial, game_dir, game_dir);
        fprintf(stderr, "[iso] %s\n", message);
        fail(message);
        return -1;
    }
    printf("[iso] unpacked to %s\n", game_dir);
    if (deleting == NULL || strcmp(deleting, "1") != 0)
        printf("[iso] %s is no longer needed and can be deleted\n", image);
    else if (remove(image) == 0)
        printf("[iso] deleted %s (JSRF_ISO_DELETE=1)\n", image);
    else
        fprintf(stderr, "[iso] could not delete %s\n", image);
    return 1;
}

/* The first of the settings that is set and not empty, else `fallback`. */
static const char *setting_or(const char *first, const char *second, const char *fallback)
{
    const char *value = getenv(first);

    if (value == NULL || value[0] == '\0')
        value = getenv(second);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

/* ── Boot ──────────────────────────────────────────────────────────────── */

/* The defaults JSRF runs with; jsrf_env.txt, read before this, wins. */
static void default_settings(void)
{
    setenv("RECOMP_VBLANK", "1", 0);
    setenv("RECOMP_AC97_READY", "plain", 0);
    setenv("RECOMP_USB", "1", 0);
    setenv("RECOMP_PB_EXEC", "1", 0);
    /* The mix: music (the 2D bins) up 6 dB, sound effects (3D) down 6 dB --
     * on hardware the speaker-layout mixdown alone left the effects over the
     * music. */
    setenv("RECOMP_APU_2D_DB", "6", 0);
    setenv("RECOMP_APU_3D_DB", "-6", 0);
    setenv("RECOMP_QUIET", "1", 0);
    /* The lifted code carries per-call [TRACE] hooks, sub_00177FE0 about a
     * dozen times a frame: the boot's lines, not the flood. */
    setenv("RECOMP_TRACE_BUDGET", "200", 0);
}

/* What the generated code was built with, for the log; and a stop if its
 * fast thread-local register access does not reach the compiler's own
 * variables (every guest register would be written somewhere else). */
static int check_generated_code(void)
{
    extern int recomp_fast_tls_selfcheck(void) __attribute__((weak));
    extern int recomp_reg_locals_on(void) __attribute__((weak));
    extern int recomp_no_preempt_points(void) __attribute__((weak));

    if (recomp_fast_tls_selfcheck) {
        int mismatch = recomp_fast_tls_selfcheck();
        if (mismatch != 0) {
            char message[160];
            snprintf(message, sizeof message, "Fast thread-local registers do not match the"
                     " compiler's (check %d). Rebuild with FAST_TLS=0.", mismatch);
            fail(message);
            return 0;
        }
        fprintf(stderr, "[BOOT] generated code: registers reached without the TLS call"
                        " (checked; FAST_TLS=0 at build time turns it off)\n");
    }
    fprintf(stderr, "[BOOT] generated code: %s\n", recomp_reg_locals_on
            ? "x86 registers in locals (regen/jsrf_reg_locals.py)"
            : "x86 registers in memory; regen/jsrf_reg_locals.py has not been run on it");
    fprintf(stderr, "[BOOT] generated code: %s\n", recomp_no_preempt_points
            ? "no preemption points (JSRF_PREEMPT_POINTS=OFF)"
            : "preemption points at function entries and loop back edges");
    return 1;
}

/* The APU's registers go to its model through the MMIO table. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
extern void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size);

#define MCPX_APU_FIRST 0xFE800000u
#define MCPX_APU_END   0xFE880000u

static uint32_t apu_register_read(void *apu, uint32_t offset, unsigned bytes)
{
    return (uint32_t)mcpx_apu_mmio_read(apu, offset, bytes);
}

static void apu_register_write(void *apu, uint32_t offset, uint32_t value, unsigned bytes)
{
    mcpx_apu_mmio_write(apu, offset, value, bytes);
}

/* The device models: the APU (unless JSRF_APU=0), the USB controllers and
 * the renderer (unless JSRF_RENDER=0, which runs without one). */
static void start_devices(void)
{
    extern void xbox_OhciInit(void);
    extern void nv2a_dk_install(void);
    const char *apu = getenv("JSRF_APU"), *render = getenv("JSRF_RENDER");

    if (getenv("RECOMP_AC97_READY") != NULL && (apu == NULL || strcmp(apu, "0") != 0)) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n", g_apu_state ? "up" : "FAILED to initialise");
        if (g_apu_state != NULL)
            xbox_MmioRegister(MCPX_APU_FIRST, MCPX_APU_END, apu_register_read,
                              apu_register_write, g_apu_state);
    }
    xbox_OhciInit();
    if (render == NULL || strcmp(render, "0") != 0)
        nv2a_dk_install();
}

/* JSRF's hooks into the kernel's waits. */
static void install_wait_hooks(void)
{
    extern void xbox_SetBlockingWaitHooks(unsigned (*begin)(void), void (*end)(unsigned));
    extern void xbox_SetWaitRescueHook(int (*fn)(uint32_t va, uint32_t waited_ms));
    extern int jsrf_d3d_wait_rescue(uint32_t va, uint32_t waited_ms);

    if (adx_guard_on()) {
        xbox_SetBlockingWaitHooks(adx_guard_block_begin, adx_guard_block_end);
        fprintf(stderr, "  [ADX-GUARD] blocking-wait release installed: a holder that"
                        " blocks in KeWaitForSingleObject lets the region go until it wakes\n");
    }
    /* BlockOnTime's fence event, set once the GPU is idle with no trap
     * pending: a lost wake costs 50-100 ms instead of the game
     * (jsrf_manual_overrides.c; RECOMP_D3D_RESCUE=0 turns it off). */
    xbox_SetWaitRescueHook(jsrf_d3d_wait_rescue);
}

static int run_game(void)
{
    const char *game_dir, *save_dir;
    char xbe_path[512];
    void *xbe;
    size_t xbe_bytes = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== Jet Set Radio Future (Xbox) - static recompilation for Nintendo Switch ===\n");
    if (getenv("JSRF_EXIT_TRACE"))
        atexit(print_guest_state);
    default_settings();

    game_dir = setting_or("JSRF_GAME_DIR", "RECOMP_GAME_DIR", GAME_DIR_DEFAULT);
    save_dir = setting_or("JSRF_SAVE_DIR", "RECOMP_SAVE_DIR", SAVE_DIR_DEFAULT);
    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", game_dir);
    if (!exists(xbe_path) && unpack_disc_image(game_dir) < 0)
        return 1;
    xbe = read_whole_file(xbe_path, &xbe_bytes);
    if (xbe == NULL) {
        fprintf(stderr, "cannot read %s\n", xbe_path);
        fail("Failed to load default.xbe. Put the disc image (.iso) in sdmc:/switch/jsrf/,"
             " or the extracted game files in sdmc:/switch/jsrf/game/ (or set"
             " JSRF_GAME_DIR).");
        return 1;
    }
    printf("XBE %s: %zu bytes\n", xbe_path, xbe_bytes);
    /* On the loading screen, before the game runs: the first start, or after
     * an update that changes the shaders. Later starts read dk_shaders.pak. */
    switch_build_shaders(xbe_path);

    /* JSRF's heaps reserve virtual ranges of their own. */
    xbox_EnableSeparateReserveSpace(64u << 20);
    if (!xbox_MemoryLayoutInit(xbe, xbe_bytes)) {
        fail("Failed to initialise Xbox memory layout.");
        free(xbe);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped at host offset 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    if (!check_generated_code()) {
        free(xbe);
        return 1;
    }
    xbox_EnablePhysicalHeapAlias();
    /* D3D's fence: device+0x30 is the counter D3D_SetFence (0x00191390) puts
     * in a 0x1D70 semaphore release, device+0x34 the word the GPU writes it
     * to. The executor's releases write it; the mirror registers where. */
    xbox_Nv2aMirrorFence(JSRF_D3D_DEVICE_SLOT, JSRF_D3D_FENCE_COUNTER, JSRF_D3D_FENCE_TARGET);
    start_devices();

    xbox_kernel_init();
    xbox_path_init(game_dir, save_dir);
    xbox_kernel_bridge_init();
    install_wait_hooks();

    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");
    xbox_WatchdogStart();

    printf("Starting JSRF at 0x%08X (esp=0x%08X)\n", JSRF_ENTRY_POINT, g_esp);
    xbox_gil_mark_main();               /* this thread's waits are timed (kernel_bridge.c) */
    xbe_entry_point();

    printf("Guest returned. Cleaning up.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);
    return 0;
}

/* The game's thread: core 0 to itself (nx_threads.c), and a stack deep
 * enough for lifted code, which nests host calls as deeply as the guest
 * nests its own. */
static void *game_thread(void *exit_code)
{
    nx_thread_place_game();
    nx_thread_track(NULL);
    *(int *)exit_code = run_game();
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attributes;
    pthread_t thread;
    int exit_code = 1;

    (void)argc;
    (void)argv;
    switch_boot();
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, GAME_STACK_BYTES);
    if (pthread_create(&thread, &attributes, game_thread, &exit_code) == 0)
        pthread_join(thread, NULL);
    else
        fprintf(stderr, "[FATAL] cannot start the game thread\n");
    switch_shutdown();
    return exit_code;
}
