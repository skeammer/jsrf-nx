/* dk_compile.c -- the deko3d shader pack, built on the console.
 *
 * The renderer draws with a dedicated shader per vertex program and combiner
 * setup where dk_shaders.pak has one, and with the ubershaders -- slowly --
 * where it does not (a JSRF cutscene missing its vertex programs: 17 fps,
 * 58 ms of GPU a frame). The pack used to be built on a PC from the keys the
 * console recorded. Now the console builds it, with uam compiled in as a
 * library (third_party/uam, uam.h), on this thread:
 *
 *   1. shaders the renderer met and the pack lacks, at once (requests);
 *   2. the keys recorded in dk_keys.bin by earlier runs;
 *   3. every vertex and pixel shader the game's XBE ships (dk_xbe_scan.c) --
 *      on the first start this is the whole game, some 370 shaders.
 *
 * Each result goes to the renderer (dk_compile_take; it loads it into spare
 * code memory at the start of a frame and draws with it from then on) and
 * is appended to dk_shaders.pak, so later starts load it with the rest. The
 * GLSL is dk_gen.c's, the same tools/dk_pack builds on the PC, so a pack from
 * either place is the same pack.
 *
 * On the first start the game waits on the loading screen until the XBE's
 * shaders are built (dk_compile_wait, from nv2a_dk_build_shaders), so it
 * never draws with an ubershader for one of them; later starts find them in
 * the pack and go straight on.
 *
 * One compile at a time (Mesa's front end is global state), on a thread at
 * 59, the guest threads' priority (time-sliced with them) and the lowest an
 * application may use: the first version asked for 60, so as to run only on
 * an idle core, and Horizon refused the thread outright -- the console
 * built nothing and the game drew everything with the ubershaders.
 * RECOMP_DK_COMPILE=0 turns it all off (the pack is then only read). */
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dk_compile.h"
#include "dk_gen.h"
#include "dk_xbe_scan.h"
#include "nv2a_dk_pack.h"
#include "uam.h"

#define PRIO 0x3B                       /* the guest threads'; an application's lowest */

typedef struct DkJob {
    struct DkJob *next;
    uint32_t kind;                      /* 0 vertex program, 1 combiner setup */
    uint32_t start, n;                  /* vertex: slots */
    uint64_t key;                       /* vertex: its key; combiners: the alpha-test build's */
    uint32_t words[];                   /* n * 4, or 37 */
} DkJob;

typedef struct DkDone {
    struct DkDone *next;
    uint32_t kind, size;
    uint64_t key;
    uint8_t *dksh;
} DkDone;

/* Keys the pack holds, or queued or built this run: open addressing. */
#define KNOWN_N 16384
static uint64_t s_known[KNOWN_N];
static uint32_t s_nknown;

static Mutex    s_lock;
static CondVar  s_wake;
static Thread   s_thread;
static int      s_up;
static DkJob   *s_urgent, *s_urgent_tail, *s_normal, *s_normal_tail;
static DkDone  *s_done, *s_done_tail;
/* Shaders queued and built (or failed) this run; s_base: done when the
 * worker last ran dry, so the progress bar counts the current batch. */
static volatile uint32_t s_queued, s_built, s_failed, s_base;
static char     s_pack_path[256], s_keys_path[256], s_xbe_path[256];
/* s_hold: the game waits for the XBE's shaders (dk_compile_wait). s_scanned:
 * the XBE is queued (or given up on). s_idle: the worker found nothing to
 * do and the pack file is written. */
static volatile int s_hold, s_scanned, s_idle;

static uint32_t known_slot(uint64_t key)
{
    return (uint32_t)(key ^ (key >> 31)) & (KNOWN_N - 1);
}

/* Under s_lock. 1 if `key` was new (now known). */
static int known_add(uint64_t key)
{
    uint32_t slot = known_slot(key);
    while (s_known[slot]) {
        if (s_known[slot] == key)
            return 0;
        slot = (slot + 1) & (KNOWN_N - 1);
    }
    if (s_nknown >= KNOWN_N * 3 / 4)
        return 0;                       /* full: nothing more is queued */
    s_known[slot] = key;
    s_nknown++;
    return 1;
}

static int known_has(uint64_t key)
{
    uint32_t slot = known_slot(key);
    while (s_known[slot]) {
        if (s_known[slot] == key)
            return 1;
        slot = (slot + 1) & (KNOWN_N - 1);
    }
    return 0;
}

static void push(DkJob **head, DkJob **tail, DkJob *j)
{
    j->next = NULL;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
}

/* Under s_lock: the queued job for `key` moved to the urgent queue. */
static void promote(uint64_t key)
{
    DkJob *j = s_normal, *prev = NULL;
    while (j && j->key != key) {
        prev = j;
        j = j->next;
    }
    if (!j)
        return;                         /* being built, or built */
    if (prev) prev->next = j->next; else s_normal = j->next;
    if (s_normal_tail == j) s_normal_tail = prev;
    push(&s_urgent, &s_urgent_tail, j);
}

/* Under s_lock (the scratch program is shared). */
static uint64_t vs_key(uint32_t start, uint32_t n, const uint32_t *words)
{
    static uint32_t prog[136][4];
    memcpy(prog[start], words, (size_t)n * 16);
    return dk_vs_key(start, n, (const uint32_t (*)[4])prog);
}

/* Under s_lock. A vertex program, unless known; an urgent request moves a
 * queued one ahead. 1 if it was queued. */
static int queue_vs(uint32_t start, uint32_t n, const uint32_t *words, int urgent)
{
    uint64_t key;
    DkJob *j;
    if (start >= 136 || !n || n > 136 - start)
        return 0;
    key = vs_key(start, n, words);
    if (known_has(key)) {
        if (urgent)
            promote(key);
        return 0;
    }
    j = (DkJob *)malloc(sizeof *j + (size_t)n * 16);
    if (!j || !known_add(key)) {
        free(j);
        return 0;
    }
    j->kind = 0;
    j->start = start;
    j->n = n;
    j->key = key;
    memcpy(j->words, words, (size_t)n * 16);
    if (urgent) push(&s_urgent, &s_urgent_tail, j); else push(&s_normal, &s_normal_tail, j);
    s_queued++;
    return 1;
}

/* Under s_lock. A combiner setup: its two builds (with the alpha test and
 * without), unless both are known; the worker skips a half the pack has. */
static int queue_fs(const uint32_t *words, int urgent)
{
    uint64_t key = dk_key_hash(DK_KEY_FS, words, DK_FS_KEY_WORDS);
    uint64_t key0 = dk_key_hash(DK_KEY_FS_NOAT, words, DK_FS_KEY_WORDS);
    DkJob *j;
    if (known_has(key) && known_has(key0)) {
        if (urgent)
            promote(key);
        return 0;
    }
    j = (DkJob *)malloc(sizeof *j + DK_FS_KEY_WORDS * 4);
    if (!j)
        return 0;
    known_add(key);
    known_add(key0);
    j->kind = 1;
    j->start = j->n = 0;
    j->key = key;
    memcpy(j->words, words, DK_FS_KEY_WORDS * 4);
    if (urgent) push(&s_urgent, &s_urgent_tail, j); else push(&s_normal, &s_normal_tail, j);
    s_queued += 2;
    return 1;
}

void dk_compile_request_vs(uint32_t start, uint32_t n, const uint32_t *words)
{
    if (!s_up)
        return;
    mutexLock(&s_lock);
    queue_vs(start, n, words, 1);
    condvarWakeOne(&s_wake);
    mutexUnlock(&s_lock);
}

void dk_compile_request_fs(const uint32_t *words)
{
    if (!s_up)
        return;
    mutexLock(&s_lock);
    queue_fs(words, 1);
    condvarWakeOne(&s_wake);
    mutexUnlock(&s_lock);
}

int dk_compile_take(uint32_t *kind, uint64_t *key, uint8_t **dksh, uint32_t *size)
{
    DkDone *d;
    /* Unlocked peek, so a frame with nothing new takes no lock; a result
     * missed here is the next frame's. */
    if (!s_up || !__atomic_load_n(&s_done, __ATOMIC_ACQUIRE))
        return 0;
    mutexLock(&s_lock);
    d = s_done;
    if (d) {
        s_done = d->next;
        if (!s_done)
            s_done_tail = NULL;
    }
    mutexUnlock(&s_lock);
    if (!d)
        return 0;
    *kind = d->kind;
    *key = d->key;
    *dksh = d->dksh;
    *size = d->size;
    free(d);
    return 1;
}

int dk_compile_progress(uint32_t *done, uint32_t *total)
{
    uint32_t q = s_queued, b = s_built + s_failed, base = s_base;
    if (!s_up || b >= q || base > b)
        return 0;
    *done = b - base;
    *total = q - base;
    return 1;
}

/* ── The worker ─────────────────────────────────────────────────────── */

static uint8_t *read_all(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p;
    long n;
    *size = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    if (p)
        *size = (uint32_t)n;
    return p;
}

/* The XBE whole: its header and every section's bytes inside the file. */
static int xbe_complete(const uint8_t *d, uint32_t size)
{
    uint32_t base, nsec, sec, i;
    if (size < 0x178 || memcmp(d, "XBEH", 4))
        return 0;
    memcpy(&base, d + 0x104, 4);
    memcpy(&nsec, d + 0x11C, 4);
    memcpy(&sec, d + 0x120, 4);
    if (sec < base || nsec > 64 || sec - base > size || nsec * 56u > size - (sec - base))
        return 0;
    sec -= base;
    for (i = 0; i < nsec; i++) {
        uint32_t raw, rsize;
        memcpy(&raw, d + sec + i * 56 + 12, 4);
        memcpy(&rsize, d + sec + i * 56 + 16, 4);
        if (raw > size || rsize > size - raw)
            return 0;
    }
    return 1;
}

static uint32_t s_from_xbe_vs, s_from_xbe_fs;

/* The scan runs unlocked; each find takes the lock for its own queueing,
 * so a draw's request never waits for the whole scan. */
static void scan_vs_cb(void *ctx, uint32_t start, uint32_t n, const uint32_t *words)
{
    (void)ctx;
    mutexLock(&s_lock);
    s_from_xbe_vs += (uint32_t)queue_vs(start, n, words, 0);
    mutexUnlock(&s_lock);
}

static void scan_fs_cb(void *ctx, const uint32_t *words)
{
    (void)ctx;
    mutexLock(&s_lock);
    s_from_xbe_fs += (uint32_t)queue_fs(words, 0);
    mutexUnlock(&s_lock);
}

/* dk_keys.bin's records (what earlier runs met; also the combiner setups D3D
 * makes from texture-stage states, which no file holds), queued. */
static void queue_key_file(void)
{
    uint32_t size, n, at = 0, nv = 0, nf = 0;
    uint32_t *w;
    uint8_t *p = read_all(s_keys_path, &size);
    if (!p)
        return;
    w = (uint32_t *)p;
    n = size / 4;
    mutexLock(&s_lock);
    while (at + 3 <= n) {
        if (w[at] == DK_KEY_VS && w[at + 1] < 136 && w[at + 2] <= 136
            && at + 3 + w[at + 2] * 4 <= n) {
            nv += (uint32_t)queue_vs(w[at + 1], w[at + 2], &w[at + 3], 0);
            at += 3 + w[at + 2] * 4;
        } else if (w[at] == DK_KEY_FS && w[at + 1] == DK_FS_KEY_WORDS
                   && at + 2 + DK_FS_KEY_WORDS <= n) {
            nf += (uint32_t)queue_fs(&w[at + 2], 0);
            at += 2 + DK_FS_KEY_WORDS;
        } else {
            break;
        }
    }
    mutexUnlock(&s_lock);
    free(p);
    if (nv || nf)
        fprintf(stderr, "  [DK] compiler: %u vertex programs and %u combiner setups from %s"
                        " to build\n", nv, nf, s_keys_path);
}

/* Built results not yet in the pack file. */
typedef struct { uint32_t kind, size; uint64_t key; uint8_t *dksh; } Pending;
static Pending  s_pend[64];
static uint32_t s_npend, s_written;
static u64      s_flushed_at;
static int      s_pack_bad;
static long     s_pack_end = -1;        /* past the last counted entry */
static uint32_t s_pack_count;

/* The pack file open for appending at s_pack_end. The first time: made if
 * absent, or made anew if it is not a pack of these generators (the
 * renderer did not use it either: nv2a_dk.c, pack_read); else its counted
 * entries are walked to find their end, and anything after them (a write
 * cut short) is cut off. */
static FILE *pack_open(void)
{
    uint32_t hdr[5];
    uint64_t stamp = 0;
    FILE *f = fopen(s_pack_path, "r+b");
    if (f && s_pack_end >= 12)
        return f;
    if (f) {
        size_t n = fread(hdr, 4, 5, f);
        if (n >= 5 && hdr[2] && hdr[3] == DK_PACK_STAMP && fread(&stamp, 8, 1, f) != 1)
            stamp = 0;
        if (n < 3 || hdr[0] != DK_PACK_MAGIC || hdr[1] != DK_PACK_VERSION
            || (DK_GEN_STAMP && stamp != DK_GEN_STAMP)) {
            fclose(f);
            f = NULL;
            fprintf(stderr, "  [DK] compiler: %s is not a pack of these shader generators;"
                            " making it anew\n", s_pack_path);
            remove(s_pack_path);
        }
    }
    if (!f) {
        uint32_t st[2] = { DK_PACK_STAMP, 0 };
        uint64_t mine = DK_GEN_STAMP;
        hdr[0] = DK_PACK_MAGIC;
        hdr[1] = DK_PACK_VERSION;
        hdr[2] = DK_GEN_STAMP ? 1 : 0;
        f = fopen(s_pack_path, "w+b");
        if (!f || fwrite(hdr, 4, 3, f) != 3
            || (DK_GEN_STAMP && (fwrite(st, 4, 2, f) != 2 || fwrite(&mine, 8, 1, f) != 1))) {
            if (f) fclose(f);
            return NULL;
        }
    }
    {
        long at = 12, end;
        uint32_t k;
        fseek(f, 0, SEEK_END);
        end = ftell(f);
        for (k = 0; k < hdr[2]; k++) {  /* whole entries only */
            uint32_t e[2];
            if (at + 16 > end || fseek(f, at, SEEK_SET) || fread(e, 4, 2, f) != 2
                || (long)e[1] > end - at - 16)
                break;
            at += 16 + (long)e[1];
        }
        if (k < hdr[2])                 /* the count is put right by the next write */
            fprintf(stderr, "  [DK] compiler: %s ends early (%u of %u entries); kept those\n",
                    s_pack_path, k, hdr[2]);
        if (at < end && ftruncate(fileno(f), at) != 0)
            fprintf(stderr, "  [DK] compiler: %s: could not cut %ld stray bytes off\n",
                    s_pack_path, end - at);
        s_pack_end = at;
        s_pack_count = k;
    }
    return f;
}

/* Append the pending results to dk_shaders.pak and fix its count; the count
 * is written last, so a start cut short leaves a pack whose count still
 * matches whole entries. */
static void pack_flush(void)
{
    uint32_t i;
    FILE *f;
    s_flushed_at = armGetSystemTick();
    if (!s_npend)
        return;
    f = s_pack_bad ? NULL : pack_open();
    if (!f) {
        if (!s_pack_bad)
            fprintf(stderr, "  [DK] compiler: cannot write %s; this run's shaders are not kept\n",
                    s_pack_path);
        s_pack_bad = 1;
        goto drop;
    }
    fseek(f, s_pack_end, SEEK_SET);
    for (i = 0; i < s_npend; i++) {
        Pending *e = &s_pend[i];
        if (fwrite(&e->kind, 4, 1, f) != 1 || fwrite(&e->size, 4, 1, f) != 1
            || fwrite(&e->key, 8, 1, f) != 1 || fwrite(e->dksh, 1, e->size, f) != e->size)
            break;
        s_pack_end += 16 + (long)e->size;
    }
    fflush(f);
    s_pack_count += i;
    s_written += i;
    fseek(f, 8, SEEK_SET);
    fwrite(&s_pack_count, 4, 1, f);
    if (fclose(f) != 0 || i < s_npend) {
        fprintf(stderr, "  [DK] compiler: %s: a write failed (card full?); no more this run\n",
                s_pack_path);
        s_pack_bad = 1;
    }
drop:
    for (i = 0; i < s_npend; i++)
        free(s_pend[i].dksh);
    s_npend = 0;
}

/* A built shader: one copy for the renderer, one for the pack file. */
static void deliver(uint32_t kind, uint64_t key, uint8_t *dksh, uint32_t size)
{
    DkDone *d = (DkDone *)malloc(sizeof *d);
    uint8_t *copy = (uint8_t *)malloc(size);
    if (copy) {
        memcpy(copy, dksh, size);
        s_pend[s_npend].kind = kind;
        s_pend[s_npend].size = size;
        s_pend[s_npend].key = key;
        s_pend[s_npend].dksh = copy;
        if (++s_npend == sizeof s_pend / sizeof *s_pend)
            pack_flush();
    }
    if (!d) {
        free(dksh);
        return;
    }
    d->kind = kind;
    d->key = key;
    d->dksh = dksh;
    d->size = size;
    d->next = NULL;
    mutexLock(&s_lock);
    if (s_done_tail) s_done_tail->next = d; else __atomic_store_n(&s_done, d, __ATOMIC_RELEASE);
    s_done_tail = d;
    mutexUnlock(&s_lock);
}

/* The pack's own keys, sorted (dk_compile_start): the half of a combiner
 * setup an older pack already has is not built again. */
static uint64_t *s_have;
static uint32_t  s_nhave;

static int pack_has(uint64_t key)
{
    uint32_t lo = 0, hi = s_nhave;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s_have[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo < s_nhave && s_have[lo] == key;
}

/* A DKSH module of one program of `kind`, padded to 256 bytes as the pack's
 * entries are (nv2a_dk.c checks the same when it reads the pack). */
static int dksh_ok(const uint8_t *d, uint32_t size, uint32_t kind)
{
    uint32_t h[6], p[6];
    if (size < 256 || (size & 255))
        return 0;
    memcpy(h, d, sizeof h);
    if (h[0] != 0x48534B44u || h[2] > size || h[3] > size - h[2] || h[4] > h[2]
        || sizeof p > h[2] - h[4])
        return 0;
    memcpy(p, d + h[4], sizeof p);
    return p[0] == kind;
}

static void build(DkJob *j)
{
    static char src[320 * 1024];
    uint32_t size = 0;
    uint8_t *d;
    int b;

    if (j->kind == 0) {
        d = dk_gen_vs(j->start, j->n, j->words, src, sizeof src) < 0 ? NULL
          : (uint8_t *)uam_compile_dksh(UAM_STAGE_VERTEX, src, &size);
        if (!d || !dksh_ok(d, size, 0)) {
            fprintf(stderr, "  [DK] compiler: vertex program %016llx (%u slots) did not build\n",
                    (unsigned long long)j->key, j->n);
            free(d);
            s_failed++;
            return;
        }
        deliver(0, j->key, d, size);
        s_built++;
        return;
    }
    for (b = 1; b >= 0; b--) {          /* with the alpha test, then without */
        uint64_t key = dk_key_hash(b ? DK_KEY_FS : DK_KEY_FS_NOAT, j->words, DK_FS_KEY_WORDS);
        if (pack_has(key)) {
            s_built++;                  /* the pack had this half */
            continue;
        }
        d = dk_gen_fs(j->words, b, src, sizeof src) < 0 ? NULL
          : (uint8_t *)uam_compile_dksh(UAM_STAGE_FRAGMENT, src, &size);
        if (!d || !dksh_ok(d, size, 1)) {
            fprintf(stderr, "  [DK] compiler: combiner setup %016llx did not build\n",
                    (unsigned long long)key);
            free(d);
            s_failed++;
            continue;
        }
        deliver(1, key, d, size);
        s_built++;
    }
}

static double secs_since(u64 t)
{
    return (double)armTicksToNs(armGetSystemTick() - t) / 1e9;
}

/* Every shader the XBE ships, queued, once the file is there and whole. On
 * a first start from a disc image it appears when the unpacking is done (the
 * game folder is renamed into place); until then this is a stat a second.
 * 1 when done with the XBE (scanned, or given up on). */
static int xbe_poll(void)
{
    static u64 last;
    static uint32_t reads;
    static int said;
    struct stat st;
    uint32_t size;
    uint8_t *x;
    u64 now = armGetSystemTick();
    char path[256];

    if (last && armTicksToNs(now - last) < 1000000000ull && !s_hold)
        return 0;
    last = now;
    mutexLock(&s_lock);                 /* dk_compile_wait may set it */
    memcpy(path, s_xbe_path, sizeof path);
    mutexUnlock(&s_lock);
    if (stat(path, &st) != 0) {
        if (!said++)
            fprintf(stderr, "  [DK] compiler: waiting for %s\n", path);
        return 0;
    }
    x = read_all(path, &size);
    if (!x || !xbe_complete(x, size)) {
        free(x);
        if (++reads == 120) {
            fprintf(stderr, "  [DK] compiler: %s is not a whole XBE; building only what is"
                            " drawn\n", path);
            return 1;
        }
        return 0;
    }
    dk_scan_xbe(x, size, scan_vs_cb, scan_fs_cb, NULL);
    free(x);
    fprintf(stderr, "  [DK] compiler: %u vertex programs and %u combiner setups from %s to"
                    " build\n", s_from_xbe_vs, s_from_xbe_fs, path);
    return 1;
}

static void worker(void *arg)
{
    uint32_t b0 = 0, f0 = 0, next_log = 0;  /* done and failed when the batch began */
    u64 t0 = 0;
    (void)arg;

    /* Any of the application's cores (0-2): Horizon moves a thread to a core
     * that has nothing to run, which is where the background work goes. */
    {
        u64 mask = 0;
        if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))
            && (mask &= 7u) && (mask & (mask - 1)))
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, 31 - __builtin_clz((u32)mask), (u32)mask);
    }
    queue_key_file();

    for (;;) {
        DkJob *j;
        int urgent;

        if (!s_scanned && xbe_poll())
            s_scanned = 1;

        mutexLock(&s_lock);
        urgent = s_urgent != NULL;
        j = urgent ? s_urgent : s_normal;
        if (j) {
            s_idle = 0;
            if (urgent) {
                s_urgent = j->next;
                if (!s_urgent) s_urgent_tail = NULL;
            } else {
                s_normal = j->next;
                if (!s_normal) s_normal_tail = NULL;
            }
        }
        mutexUnlock(&s_lock);

        if (!j) {
            pack_flush();
            if (t0) {
                fprintf(stderr, "  [DK] compiler: %u shaders done in %.1f s (%u failed);"
                                " %u added to %s this run\n", s_built + s_failed - b0,
                        secs_since(t0), s_failed - f0, s_written, s_pack_path);
                t0 = 0;
            }
            mutexLock(&s_lock);
            if (!s_urgent && !s_normal) {
                s_idle = 1;
                s_base = s_built + s_failed;
                condvarWaitTimeout(&s_wake, &s_lock, 1000000000ull);
            }
            mutexUnlock(&s_lock);
            continue;
        }

        if (!t0) {
            t0 = armGetSystemTick();
            b0 = s_built + s_failed;
            f0 = s_failed;
            next_log = b0 + 50;
        }
        build(j);
        free(j);
        if (s_built + s_failed >= next_log) {
            next_log += 50;
            fprintf(stderr, "  [DK] compiler: %u of %u done (%.1f s)\n",
                    s_built + s_failed - s_base, s_queued - s_base, secs_since(t0));
        }
        /* To the card every 16 shaders or 5 s, so closing the game keeps
         * nearly everything. */
        if (s_npend >= 16 || (s_npend && armTicksToNs(armGetSystemTick() - s_flushed_at)
                                         > 5000000000ull))
            pack_flush();
    }
}

int dk_compile_wait(const char *xbe_path, void (*progress)(uint32_t done, uint32_t total))
{
    const char *e = getenv("RECOMP_DK_COMPILE_WAIT");
    u64 t0, moved;
    uint32_t last = ~0u, shown = 0;
    int done = 0;
    if (!s_up)
        return 0;
    mutexLock(&s_lock);
    if (xbe_path && *xbe_path)
        snprintf(s_xbe_path, sizeof s_xbe_path, "%s", xbe_path);
    if (!(e && *e == '0'))
        s_hold = 1;
    condvarWakeOne(&s_wake);
    mutexUnlock(&s_lock);
    if (!s_hold) {
        fprintf(stderr, "  [DK] compiler: RECOMP_DK_COMPILE_WAIT=0: the game starts at once\n");
        return 0;
    }
    t0 = moved = armGetSystemTick();
    for (;;) {
        uint32_t d, t;
        mutexLock(&s_lock);
        done = s_scanned && s_idle && !s_urgent && !s_normal;
        d = s_built + s_failed - s_base;
        t = s_queued - s_base;
        mutexUnlock(&s_lock);
        if (done)
            break;
        if (t >= 4 && !shown++)
            fprintf(stderr, "  [DK] compiler: the game waits on the loading screen until the"
                            " shaders are built\n");
        if (progress && t)
            progress(d < t ? d : t, t);
        if (d != last) {
            last = d;
            moved = armGetSystemTick();
        } else if (armTicksToNs(armGetSystemTick() - moved) > 90000000000ull) {
            fprintf(stderr, "  [DK] compiler: nothing built for 90 s; the game starts and the"
                            " rest is built beside it\n");
            break;
        }
        svcSleepThread(100000000ull);
    }
    s_hold = 0;
    if (shown)
        fprintf(stderr, "  [DK] compiler: the game waited %.1f s for its shaders\n",
                secs_since(t0));
    return done;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int dk_compile_enabled(void)
{
    const char *e = getenv("RECOMP_DK_COMPILE");
    return !(e && *e == '0');
}

int dk_compile_start(const uint64_t *have, uint32_t nhave, const char *pack_path,
                     const char *keys_path, const char *xbe_path)
{
    uint32_t i;
    if (s_up)
        return 1;
    if (!dk_compile_enabled()) {
        fprintf(stderr, "  [DK] compiler off (RECOMP_DK_COMPILE=0): the pack is only read\n");
        return 0;
    }
    mutexInit(&s_lock);
    condvarInit(&s_wake);
    snprintf(s_pack_path, sizeof s_pack_path, "%s", pack_path);
    snprintf(s_keys_path, sizeof s_keys_path, "%s", keys_path);
    snprintf(s_xbe_path, sizeof s_xbe_path, "%s", xbe_path);
    s_have = (uint64_t *)malloc((nhave ? nhave : 1) * sizeof *s_have);
    if (!s_have)
        return 0;
    if (nhave)
        memcpy(s_have, have, (size_t)nhave * sizeof *s_have);
    qsort(s_have, nhave, sizeof *s_have, cmp_u64);
    s_nhave = nhave;
    for (i = 0; i < nhave; i++)
        known_add(have[i]);
    s_flushed_at = armGetSystemTick();
    /* Mesa's parser and the codegen recurse: all of JSRF's shaders build
     * in a 256 KB stack (measured on Linux); 2 MB to spare. The compiler's
     * heap peaks around 25 MB (the GLSL built-ins it keeps, and a shader). */
    {
        Result rc = threadCreate(&s_thread, worker, NULL, NULL, 2u << 20, PRIO, -2);
        if (R_FAILED(rc)) {
            fprintf(stderr, "  [DK] compiler: no thread (threadCreate 0x%X); the pack stays as"
                            " it is\n", rc);
            return 0;
        }
        s_up = 1;
        if (R_FAILED(rc = threadStart(&s_thread))) {
            s_up = 0;
            threadClose(&s_thread);
            fprintf(stderr, "  [DK] compiler: no thread (threadStart 0x%X); the pack stays as"
                            " it is\n", rc);
            return 0;
        }
    }
    fprintf(stderr, "  [DK] compiler up: builds what %s lacks (from the game's XBE, earlier"
                    " runs' keys, and what is drawn)\n", pack_path);
    return 1;
}
