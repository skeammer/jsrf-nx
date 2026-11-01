/*
 * A render back end on a thread of its own (nv2a_backend.h:
 * nv2a_backend_queue).
 *
 * On the Switch the executor thread decoded the pushbuffer (~10.6 ms of a
 * busy JSRF frame) and then recorded each draw for the GPU (~8.3 ms), one
 * after the other, and that one thread was the frame-rate limit. With the
 * queue the executor decodes and copies each call into a ring; this file's
 * thread takes them out in order and makes the back end's calls. The two
 * halves run on two cores.
 *
 * What a record carries:
 *  - The method shadow's 64-byte blocks written since the last record (the
 *    executor marks them: nv2a_pb_reg_dirty), as they are -- copying one
 *    costs less than finding which of its words changed. The thread keeps
 *    its own shadow and hands the back end that, so a clear reading the
 *    registers (the clip, the depth range) sees what the executor saw then.
 *  - The transform program rows and constant rows that changed: the program
 *    when its version changes; the constants as the rows the batch says were
 *    written since the batch before (vp_const_lo/hi, as they are), or every
 *    row that differs when that batch is not the one queued last.
 *  - The batch: its fixed part by value, the direct entries in use, the
 *    attribute constants when they changed; its indices, the converted
 *    attributes (packed) and inline vertex data (attr_direct_local).
 *  - Clears (by value), flips, stores and calls.
 *
 * Vertex arrays are copied into the record too (RECOMP_QUEUE_COPY_VERTICES,
 * default 1). JSRF's D3D finds room in its pushbuffer by reading the
 * semaphore, so holding releases back behind the queue held the game back
 * whenever the thread was behind -- in a present, say: the ring-space spin
 * (sub_00191440) and its waits were ~16% of the game thread. With the
 * vertices copied, a draw reads only textures and palettes from guest
 * memory later, and stores and calls go through at once; at worst a
 * texture rewritten right after a fence shows its new contents a frame
 * early.
 *
 * With RECOMP_QUEUE_COPY_VERTICES=0 vertex arrays, textures and palettes
 * stay in guest memory and are read
 * when the thread gets to the draw. The title only reuses such memory once
 * the GPU says it is past the draws that read it -- a semaphore release, the
 * PATT_COLOR0 latch, a NOP trap, a swap -- so the executor passes those on
 * only once the thread has made every call before them: a store or call is
 * queued behind them (or made at once when nothing that touches guest memory
 * is still queued), and a trap waits for the thread (nv2a_backend_queue_sync).
 * Flips are the one record that touches no guest memory, so a store need not
 * wait for a present. The 2D copy's surface_at/written wait until the thread
 * is idle and call the back end directly (it moves guest bytes a queued draw
 * may still sample).
 *
 * Records are handed over in batches (16 KB or 16 records, at a flip, before
 * the executor waits, and when its walk ends: nv2a_backend_queue_flush), so
 * the two cores do not trade the ring's head for every draw. At most
 * RECOMP_QUEUE_FRAMES (2) flips are queued; the executor waits for the thread
 * beyond that, as it does when the ring (RECOMP_QUEUE_RING_KB, 8 MB) is
 * full. A record larger than a quarter of the ring is made on the executor
 * thread once the queue thread is idle.
 *
 * Both sides spin briefly (RECOMP_QUEUE_SPIN_US, 50) before they sleep on the
 * condition variable: in a busy frame a batch arrives every ~100 us, and
 * waking a sleeping thread for each would cost more than the spin.
 * RECOMP_QUEUE_PRIO sets the thread's host priority (Switch; default that of
 * the executor, 59).
 */
#include "nv2a_backend.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

/* No pthreads here: the back end stays on the executor's thread. */
const Nv2aBackend *nv2a_backend_queue(const Nv2aBackend *inner) { return inner; }
int nv2a_backend_queue_on(void) { return 0; }
void nv2a_backend_queue_sync(int all) { (void)all; }
void nv2a_backend_queue_flush(void) { }
void nv2a_backend_queue_store(volatile uint32_t *word, uint32_t value) { *word = value; }
void nv2a_backend_queue_call(void (*fn)(void)) { fn(); }
int nv2a_backend_queue_report(char *buf, unsigned long n, double frames)
{
    (void)buf; (void)n; (void)frames;
    return 0;
}

#else

#include <pthread.h>
#include <time.h>

#include "platform/nx_threads.h"

enum { Q_WRAP = 0, Q_DRAW, Q_CLEAR, Q_FLIP, Q_STORE, Q_CALL };

typedef struct {
    uint32_t type;
    uint32_t size;                      /* the whole record, a multiple of 16 */
    uint16_t nreg, nprog, nconst;       /* shadow blocks; program, constant runs */
    uint16_t pad;
} QHdr;                                 /* 16 bytes */
typedef struct { uint32_t idx, pad[3]; uint32_t w[16]; } QBlock;  /* 64 B of shadow */
typedef struct { uint32_t first, count, pad[2]; } QRun;          /* + count 16 B rows */
typedef struct {
    Nv2aSurface s;
    Nv2aRenderState rs;
    uint32_t has_rs, flags, argb, zs;
} QClear;
typedef struct { volatile uint32_t *word; uint32_t value, pad; } QStore;
typedef struct { void (*fn)(void); uint64_t pad; } QCall;
/* A draw: where its parts are in the record. */
typedef struct { uint32_t idx_at, attrs_at, has_const, copied; } QDraw;  /* copied: attributes in the record */

#define Q_REG_WORDS  (0x2000 / 4)
#define Q_BLOCKS     (0x2000 / 64)
#define Q_PROG_ROWS  136
#define Q_CONST_ROWS 192
#define Q_ALIGN(x)   (((size_t)(x) + 15u) & ~(size_t)15u)
#define Q_BATCH_BYTES   (16u << 10)
#define Q_BATCH_RECORDS 16u
/* The batch by value: everything before direct[], the direct entries in
 * use, the attribute constants when they changed, everything from
 * attr_const_gen on. */
#define B_HEAD  offsetof(Nv2aRawBatch, direct)
#define B_TAIL  offsetof(Nv2aRawBatch, attr_const_gen)
#define B_DSZ   sizeof(((Nv2aRawBatch *)0)->direct[0])

/* Each side's words on a cache line of its own: the other side polls them. */
#define Q_LINE __attribute__((aligned(64)))

static const Nv2aBackend *s_inner;
static Nv2aBackend s_qb;
static int s_on;
static uint8_t *s_ring;
static size_t s_ring_bytes;
static Q_LINE size_t s_head;            /* bytes handed over (producer writes) */
static Q_LINE size_t s_tail;            /* bytes made (the thread writes) */
static Q_LINE int s_flips;              /* flips queued and not yet made */
static Q_LINE int s_cons_sleeps;
static Q_LINE int s_prod_waits;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;
static int s_flip_limit = 2;
static uint64_t s_spin_ns = 50000;
static int s_prio = -1;
static int s_copy_vertices = 1;         /* RECOMP_QUEUE_COPY_VERTICES */
static int s_exec_core = -2;            /* -2: not known yet */

/* Executor thread: records written (some not yet handed over), the tail as
 * last read, p_head after the last record that touches guest memory (all
 * but flips), a wrap still to be committed with the next record. */
static Q_LINE size_t p_head;
static size_t p_tail_seen, p_need, p_wrap;
static uint32_t p_batched;
/* What the thread was last sent. */
static uint32_t p_prog[Q_PROG_ROWS][4], p_consts[Q_CONST_ROWS][4];
static float p_attr_const[NV2A_RAW_ATTRS][4];
static uint32_t p_prog_gen, p_const_gen, p_attr_gen;
static int p_prog_ok, p_const_ok, p_attr_ok;
/* Collected for the next record: shadow blocks, and program and constant
 * rows as runs (first row, count). */
typedef struct { uint16_t first[Q_CONST_ROWS], count[Q_CONST_ROWS]; uint32_t runs, rows; } QRows;
static uint16_t p_blk[Q_BLOCKS];
static uint32_t p_nblk;
static QRows p_pr, p_cr;

/* Queue thread: its copies, which the back end is handed. */
static uint32_t c_regs[Q_REG_WORDS];
static uint32_t c_prog[Q_PROG_ROWS][4];
static float    c_consts[Q_CONST_ROWS][4];
static float    c_attr_const[NV2A_RAW_ATTRS][4];
static float   *c_attrs;
static size_t   c_attrs_cap;            /* vertices */

/* [perf]: the thread's idle time; the executor's waits by reason. */
enum { W_ROOM, W_FLIPS, W_SYNC, W_ALL, W_N };
static Q_LINE uint64_t st_idle_ns;
static Q_LINE uint64_t st_wait_ns[W_N];
static uint32_t st_waits[W_N];
static uint64_t st_bytes, st_idx_bytes, st_attr_bytes, st_vtx_bytes;
static uint32_t st_stores_queued, st_stores_now, st_big, st_records, st_batches;

static uint64_t q_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static void timed_wait(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&s_cv, &s_lock, &ts);
}

static void wake(void)
{
    pthread_mutex_lock(&s_lock);
    pthread_cond_broadcast(&s_cv);
    pthread_mutex_unlock(&s_lock);
}

/* ── Executor side ─────────────────────────────────────────────────── */

/* Hand over everything written. */
static void q_flush(void)
{
    if (s_head == p_head)
        return;
    st_batches++;
    p_batched = 0;
    if (s_exec_core == -2)
        s_exec_core = nx_thread_core();   /* where the thread should not be */
    __atomic_store_n(&s_head, p_head, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_cons_sleeps, __ATOMIC_SEQ_CST))
        wake();
}

static size_t tail_now(void)
{
    p_tail_seen = __atomic_load_n(&s_tail, __ATOMIC_SEQ_CST);
    return p_tail_seen;
}

static int prod_ready(int why, size_t arg)
{
    switch (why) {
    case W_ROOM:  return s_ring_bytes - (p_head - tail_now()) >= arg;
    case W_FLIPS: return __atomic_load_n(&s_flips, __ATOMIC_SEQ_CST) < s_flip_limit;
    default:      return tail_now() >= arg;   /* arg: a p_head value */
    }
}

static void prod_wait(int why, size_t arg)
{
    uint64_t t0;
    unsigned i;

    if (prod_ready(why, arg))
        return;
    q_flush();                          /* the thread can only make what it has */
    t0 = q_ns();
    st_waits[why]++;
    for (i = 1;; i++) {
        cpu_relax();
        if (prod_ready(why, arg))
            goto done;
        if ((i & 63) == 0) {
            if (q_ns() - t0 > s_spin_ns)
                break;
            nx_thread_yield();
        }
    }
    pthread_mutex_lock(&s_lock);
    __atomic_store_n(&s_prod_waits, 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (!prod_ready(why, arg))
        timed_wait();
    __atomic_store_n(&s_prod_waits, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s_lock);
done:
    st_wait_ns[why] += q_ns() - t0;
}

/* Contiguous room for a record of at most `bound` bytes. */
static uint8_t *q_reserve(size_t bound)
{
    size_t off = p_head % s_ring_bytes, need = bound;
    int wrap = off + bound > s_ring_bytes;
    if (wrap)
        need += s_ring_bytes - off;
    if (s_ring_bytes - (p_head - p_tail_seen) < need)
        prod_wait(W_ROOM, need);
    if (wrap) {
        QHdr *w = (QHdr *)(s_ring + off);
        memset(w, 0, sizeof *w);
        w->type = Q_WRAP;
        w->size = (uint32_t)(s_ring_bytes - off);
        p_wrap = s_ring_bytes - off;    /* committed with the record */
        return s_ring;
    }
    p_wrap = 0;
    return s_ring + off;
}

static void q_commit(size_t size, int touches)
{
    p_head += p_wrap + size;
    p_wrap = 0;
    if (touches)
        p_need = p_head;
    st_bytes += size;
    st_records++;
    if (++p_batched >= Q_BATCH_RECORDS || p_head - s_head >= Q_BATCH_BYTES)
        q_flush();
}

/* The shadow blocks written since the last record, into p_blk. */
static void regs_collect(void)
{
    uint8_t *dirty = nv2a_pb_reg_dirty();
    uint32_t k, c;

    p_nblk = 0;
    for (k = 0; k < Q_BLOCKS; k += 8) {
        uint64_t m;
        memcpy(&m, dirty + k, 8);
        if (!m)
            continue;
        memset(dirty + k, 0, 8);
        for (c = k; m; c++, m >>= 8)
            if (m & 0xFF)
                p_blk[p_nblk++] = (uint16_t)c;
    }
}

/* Rows [lo, hi) of live that differ from sent, as runs. */
static void rows_diff(const uint32_t (*live)[4], const uint32_t (*sent)[4],
                      uint32_t lo, uint32_t hi, QRows *out)
{
    uint32_t r, n = 0, rows = 0;
    for (r = lo; r < hi; r++) {
        if (memcmp(live[r], sent[r], 16) == 0)
            continue;
        if (n && out->first[n - 1] + out->count[n - 1] == r) {
            out->count[n - 1]++;
        } else {
            out->first[n] = (uint16_t)r;
            out->count[n] = 1;
            n++;
        }
        rows++;
    }
    out->runs = n;
    out->rows = rows;
}

static size_t runs_write(uint8_t *out, const QRows *in, const uint32_t (*live)[4],
                         uint32_t (*sent)[4], uint16_t *nruns)
{
    size_t at = 0;
    uint32_t k;
    for (k = 0; k < in->runs; k++) {
        QRun *r = (QRun *)(out + at);
        uint32_t f = in->first[k], c = in->count[k];
        r->first = f;
        r->count = c;
        r->pad[0] = r->pad[1] = 0;
        at += sizeof(QRun);
        memcpy(out + at, live[f], (size_t)c * 16);
        memcpy(sent[f], live[f], (size_t)c * 16);
        at += (size_t)c * 16;
    }
    *nruns = (uint16_t)in->runs;
    return at;
}

static size_t state_size(void)
{
    return sizeof(QHdr) + (size_t)p_nblk * sizeof(QBlock)
         + (size_t)(p_pr.runs + p_cr.runs) * sizeof(QRun) + (size_t)(p_pr.rows + p_cr.rows) * 16;
}

/* Header and the state changes collected; returns the offset after them. */
static size_t state_write(uint8_t *rec, uint32_t type, const Nv2aRawBatch *b)
{
    QHdr *h = (QHdr *)rec;
    const uint32_t *live = nv2a_pb_reg_shadow();
    QBlock *o = (QBlock *)(rec + sizeof(QHdr));
    size_t at;
    uint32_t k;

    memset(h, 0, sizeof *h);
    h->type = type;
    h->nreg = (uint16_t)p_nblk;
    for (k = 0; k < p_nblk; k++) {
        uint32_t c = p_blk[k];
        o[k].idx = c;
        o[k].pad[0] = o[k].pad[1] = o[k].pad[2] = 0;
        memcpy(o[k].w, live + c * 16, 64);
    }
    at = sizeof(QHdr) + (size_t)p_nblk * sizeof(QBlock);
    if (b && p_pr.runs)
        at += runs_write(rec + at, &p_pr, b->vp_program, p_prog, &h->nprog);
    if (b && p_cr.runs)
        at += runs_write(rec + at, &p_cr, (const uint32_t (*)[4])b->vp_consts, p_consts,
                         &h->nconst);
    p_nblk = 0;
    p_pr.runs = p_pr.rows = p_cr.runs = p_cr.rows = 0;
    return at;
}

static void q_run(const uint8_t *rec);

static uint32_t attr_elem(uint32_t type, uint32_t size)
{
    return type == 0 ? 4 : type == 2 ? 4 * size : type == 4 ? size : 2 * size;
}

static void q_draw(const Nv2aRawBatch *b)
{
    uint16_t conv = b->attr_present & (uint16_t)~b->attr_direct;
    /* What is copied into the record: inline vertices always; with
     * s_copy_vertices, the vertex arrays in guest memory too. */
    uint16_t local = s_copy_vertices ? b->attr_direct : b->attr_direct & b->attr_direct_local;
    uint32_t nv = b->vertex_count, np = (uint32_t)__builtin_popcount(conv);
    uint32_t nd = (uint32_t)__builtin_popcount(b->attr_direct), a, v;
    /* Inline vertex data: one byte range per attribute (ranges that touch
     * are merged), each from a 4-aligned start, as deko3d reads them. The
     * immediate-mode buffer keeps each attribute 128 KB apart, so one range
     * over all of them would copy the gaps. */
    const uint8_t *run_lo[NV2A_RAW_ATTRS], *run_hi[NV2A_RAW_ATTRS];
    size_t run_at[NV2A_RAW_ATTRS], local_bytes = 0;
    int of_run[NV2A_RAW_ATTRS];
    uint32_t nrun = 0, r;
    size_t size, draw_at, idx_at, attrs_at, local_at, at;
    int send_prog = 0, send_const = 0, send_attr;
    QDraw *qd;
    uint8_t *rec, *big = NULL;

    regs_collect();
    if (b->vp_program && (!p_prog_ok || b->vp_prog_gen != p_prog_gen)) {
        uint32_t hi = b->vp_slots < Q_PROG_ROWS ? b->vp_slots : Q_PROG_ROWS;
        rows_diff(b->vp_program, (const uint32_t (*)[4])p_prog, 0, hi, &p_pr);
        send_prog = 1;
    }
    if (b->vp_consts && (!p_const_ok || b->vp_const_gen != p_const_gen)) {
        uint32_t lo = 0, hi = b->vp_const_count < Q_CONST_ROWS ? b->vp_const_count : Q_CONST_ROWS;
        if (p_const_ok && b->vp_const_gen_prev && b->vp_const_gen_prev == p_const_gen) {
            /* Only these were written since: sent as they are. */
            lo = b->vp_const_lo;
            if (b->vp_const_hi < hi)
                hi = b->vp_const_hi;
            p_cr.runs = lo < hi;
            p_cr.rows = lo < hi ? hi - lo : 0;
            p_cr.first[0] = (uint16_t)lo;
            p_cr.count[0] = (uint16_t)p_cr.rows;
        } else {
            rows_diff((const uint32_t (*)[4])b->vp_consts, (const uint32_t (*)[4])p_consts,
                      lo, hi, &p_cr);
        }
        send_const = 1;
    }
    send_attr = !p_attr_ok || b->attr_const_gen != p_attr_gen;
    for (a = 0; local && a < NV2A_RAW_ATTRS; a++) {
        const uint8_t *lo, *hi;
        if (!(local & (1u << a)))
            continue;
        lo = (const uint8_t *)((uintptr_t)b->direct[a].ptr & ~(uintptr_t)3);
        hi = b->direct[a].ptr + (size_t)(nv ? nv - 1 : 0) * b->direct[a].stride
           + attr_elem(b->direct[a].type, b->direct[a].size);
        for (r = 0; r < nrun && !(lo <= run_hi[r] && run_lo[r] <= hi); r++) { }
        if (r == nrun) {
            run_lo[nrun] = lo;
            run_hi[nrun] = hi;
            nrun++;
        } else {
            if (lo < run_lo[r]) run_lo[r] = lo;
            if (hi > run_hi[r]) run_hi[r] = hi;
        }
    }
    for (r = 0; r + 1 < nrun; ) {       /* a range that grew may now touch another */
        uint32_t q;
        for (q = r + 1; q < nrun && !(run_lo[q] <= run_hi[r] && run_lo[r] <= run_hi[q]); q++) { }
        if (q == nrun) {
            r++;
            continue;
        }
        if (run_lo[q] < run_lo[r]) run_lo[r] = run_lo[q];
        if (run_hi[q] > run_hi[r]) run_hi[r] = run_hi[q];
        run_lo[q] = run_lo[nrun - 1];
        run_hi[q] = run_hi[nrun - 1];
        nrun--;
        r = 0;
    }
    for (a = 0; local && a < NV2A_RAW_ATTRS; a++)
        if (local & (1u << a))
            for (r = 0; r < nrun; r++)
                if (b->direct[a].ptr >= run_lo[r] && b->direct[a].ptr <= run_hi[r]) {
                    of_run[a] = (int)r;
                    break;
                }
    for (r = 0; r < nrun; r++) {
        run_at[r] = local_bytes;
        local_bytes += Q_ALIGN((size_t)(run_hi[r] - run_lo[r]));
    }

    draw_at = Q_ALIGN(state_size());
    size = draw_at + sizeof(QDraw) + Q_ALIGN(B_HEAD) + Q_ALIGN((size_t)nd * B_DSZ)
         + (send_attr ? sizeof b->attr_const : 0) + Q_ALIGN(sizeof(Nv2aRawBatch) - B_TAIL);
    idx_at = size;
    attrs_at = idx_at + Q_ALIGN((size_t)b->index_count * 4);
    local_at = attrs_at + Q_ALIGN((size_t)nv * np * 16);
    size = local_at + local_bytes;

    if (size > s_ring_bytes / 4) {
        /* Too big for the ring: built aside and made here, once the thread
         * is idle (it then touches neither its copies nor the back end). */
        rec = big = (uint8_t *)malloc(size);     /* 16-aligned, as records need */
        if (!rec) {
            /* The state taken for it must still reach the thread: send it
             * all again with the next record. */
            memset(nv2a_pb_reg_dirty(), 1, Q_BLOCKS);
            p_nblk = 0;
            p_pr.runs = p_pr.rows = p_cr.runs = p_cr.rows = 0;
            p_prog_ok = p_const_ok = p_attr_ok = 0;
            return;
        }
        prod_wait(W_ALL, p_head);
        st_big++;
    } else {
        rec = q_reserve(size);
    }
    at = state_write(rec, Q_DRAW, b);
    if (send_prog) {
        p_prog_gen = b->vp_prog_gen;
        p_prog_ok = 1;
    }
    if (send_const) {
        p_const_gen = b->vp_const_gen;
        p_const_ok = 1;
    }
    if (at < draw_at)
        memset(rec + at, 0, draw_at - at);
    qd = (QDraw *)(rec + draw_at);
    qd->idx_at = (uint32_t)idx_at;
    qd->attrs_at = (uint32_t)attrs_at;
    qd->has_const = (uint32_t)send_attr;
    qd->copied = local;
    at = draw_at + sizeof(QDraw);
    memcpy(rec + at, b, B_HEAD);
    at += Q_ALIGN(B_HEAD);
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (b->attr_direct & (1u << a)) {
            memcpy(rec + at, &b->direct[a], B_DSZ);
            if (local & (1u << a)) {
                /* an offset into the record; the thread makes it a pointer */
                const uint8_t *off = (const uint8_t *)(uintptr_t)
                    (local_at + run_at[of_run[a]] + (size_t)(b->direct[a].ptr - run_lo[of_run[a]]));
                memcpy(rec + at, &off, sizeof off);
            }
            at += B_DSZ;
        }
    at = Q_ALIGN(at);
    if (send_attr) {
        memcpy(rec + at, b->attr_const, sizeof b->attr_const);
        memcpy(p_attr_const, b->attr_const, sizeof p_attr_const);
        p_attr_gen = b->attr_const_gen;
        p_attr_ok = 1;
        at += sizeof b->attr_const;
    }
    memcpy(rec + at, (const uint8_t *)b + B_TAIL, sizeof(Nv2aRawBatch) - B_TAIL);
    memcpy(rec + idx_at, b->indices, (size_t)b->index_count * 4);
    st_idx_bytes += (size_t)b->index_count * 4;
    if (np) {
        float *o = (float *)(rec + attrs_at);
        uint32_t slot[NV2A_RAW_ATTRS], k;
        for (a = 0, k = 0; a < NV2A_RAW_ATTRS; a++)
            if (conv & (1u << a))
                slot[k++] = a * 4;
        for (v = 0; v < nv; v++) {
            const float *in = b->attrs + (size_t)v * NV2A_RAW_ATTRS * 4;
            for (k = 0; k < np; k++, o += 4)
                memcpy(o, in + slot[k], 16);
        }
        st_attr_bytes += (size_t)nv * np * 16;
    }
    for (r = 0; r < nrun; r++)
        memcpy(rec + local_at + run_at[r], run_lo[r], (size_t)(run_hi[r] - run_lo[r]));
    st_vtx_bytes += local_bytes;
    ((QHdr *)rec)->size = (uint32_t)size;
    if (big) {
        q_run(big);
        free(big);
        return;
    }
    /* With its vertices copied a draw reads only textures and palettes from
     * guest memory later, and those the title does not rewrite behind a
     * fence it has just seen pass -- at worst a texture changed that moment
     * shows its new contents a frame early. So releases do not wait for it. */
    q_commit(size, !s_copy_vertices);
}

static void q_clear(const Nv2aSurface *sf, const Nv2aRenderState *rs,
                    uint32_t flags, uint32_t argb, uint32_t zs)
{
    size_t size, at;
    uint8_t *rec;
    QClear *c;

    regs_collect();
    size = Q_ALIGN(state_size()) + Q_ALIGN(sizeof(QClear));
    rec = q_reserve(size);
    at = state_write(rec, Q_CLEAR, NULL);
    if (Q_ALIGN(at) != at)
        memset(rec + at, 0, Q_ALIGN(at) - at);
    c = (QClear *)(rec + Q_ALIGN(at));
    memset(c, 0, sizeof *c);
    c->s = *sf;
    if (rs) {
        c->rs = *rs;
        c->has_rs = 1;
    }
    c->flags = flags;
    c->argb = argb;
    c->zs = zs;
    ((QHdr *)rec)->size = (uint32_t)size;
    q_commit(size, !s_copy_vertices);   /* reads no guest memory either */
}

static void q_flip(void)
{
    size_t size;
    uint8_t *rec;

    prod_wait(W_FLIPS, 0);              /* no more than s_flip_limit frames behind */
    regs_collect();
    size = Q_ALIGN(state_size());
    rec = q_reserve(size);
    state_write(rec, Q_FLIP, NULL);
    ((QHdr *)rec)->size = (uint32_t)size;
    __atomic_add_fetch(&s_flips, 1, __ATOMIC_SEQ_CST);
    q_commit(size, 0);
    q_flush();                          /* the frame is complete: present it now */
}

static int q_surface_at(uint32_t va, uint32_t bytes)
{
    prod_wait(W_ALL, p_head);
    return s_inner->surface_at ? s_inner->surface_at(va, bytes) : 0;
}

static int q_written(uint32_t va, uint32_t bytes)
{
    prod_wait(W_ALL, p_head);
    return s_inner->written(va, bytes);
}

int nv2a_backend_queue_on(void)
{
    return s_on;
}

void nv2a_backend_queue_sync(int all)
{
    if (!s_on)
        return;
    q_flush();
    prod_wait(all ? W_ALL : W_SYNC, all ? p_head : p_need);
}

void nv2a_backend_queue_flush(void)
{
    if (s_on)
        q_flush();
}

/* Nothing queued that touches guest memory: a store or call can be made now. */
static int now_ok(void)
{
    return p_tail_seen >= p_need || tail_now() >= p_need;
}

void nv2a_backend_queue_store(volatile uint32_t *word, uint32_t value)
{
    uint8_t *rec;
    QStore *st;
    size_t size = sizeof(QHdr) + sizeof(QStore);

    if (!s_on || now_ok()) {
        __atomic_store_n((uint32_t *)word, value, __ATOMIC_RELEASE);
        st_stores_now++;
        return;
    }
    rec = q_reserve(size);
    memset(rec, 0, sizeof(QHdr));
    ((QHdr *)rec)->type = Q_STORE;
    ((QHdr *)rec)->size = (uint32_t)size;
    st = (QStore *)(rec + sizeof(QHdr));
    st->word = word;
    st->value = value;
    st->pad = 0;
    st_stores_queued++;
    q_commit(size, 1);
}

void nv2a_backend_queue_call(void (*fn)(void))
{
    uint8_t *rec;
    QCall *c;
    size_t size = sizeof(QHdr) + sizeof(QCall);

    if (!s_on || now_ok()) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        fn();
        return;
    }
    rec = q_reserve(size);
    memset(rec, 0, sizeof(QHdr));
    ((QHdr *)rec)->type = Q_CALL;
    ((QHdr *)rec)->size = (uint32_t)size;
    c = (QCall *)(rec + sizeof(QHdr));
    c->fn = fn;
    c->pad = 0;
    q_commit(size, 1);
}

/* ── The queue's thread ───────────────────────────────────────────── */

static size_t runs_apply(const uint8_t *rec, size_t at, uint32_t nruns, uint8_t *rows,
                         uint32_t max_rows)
{
    uint32_t k;
    for (k = 0; k < nruns; k++) {
        const QRun *r = (const QRun *)(rec + at);
        at += sizeof(QRun);
        if (r->first + r->count <= max_rows)
            memcpy(rows + (size_t)r->first * 16, rec + at, (size_t)r->count * 16);
        at += (size_t)r->count * 16;
    }
    return at;
}

static void run_draw(const uint8_t *rec, size_t draw_at)
{
    const QDraw *qd = (const QDraw *)(rec + draw_at);
    Nv2aRawBatch bb;
    uint16_t conv;
    uint32_t nv, a, v;
    size_t at = draw_at + sizeof(QDraw);

    memcpy(&bb, rec + at, B_HEAD);
    at += Q_ALIGN(B_HEAD);
    memset(bb.direct, 0, sizeof bb.direct);
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (bb.attr_direct & (1u << a)) {
            memcpy(&bb.direct[a], rec + at, B_DSZ);
            at += B_DSZ;
        }
    at = Q_ALIGN(at);
    if (qd->has_const) {
        memcpy(c_attr_const, rec + at, sizeof c_attr_const);
        at += sizeof c_attr_const;
    }
    memcpy(bb.attr_const, c_attr_const, sizeof bb.attr_const);
    memcpy((uint8_t *)&bb + B_TAIL, rec + at, sizeof(Nv2aRawBatch) - B_TAIL);

    nv = bb.vertex_count;
    conv = bb.attr_present & (uint16_t)~bb.attr_direct;
    bb.indices = (const uint32_t *)(rec + qd->idx_at);
    for (a = 0; a < NV2A_RAW_ATTRS; a++)
        if (bb.attr_direct & qd->copied & (1u << a))
            bb.direct[a].ptr = rec + (uintptr_t)bb.direct[a].ptr;
    if ((s_inner->flags & NV2A_BACKEND_RAW_PACKED) && (s_inner->flags & NV2A_BACKEND_RAW_DIRECT)) {
        /* As packed: the back end reads them so. */
        bb.attrs = (const float *)(rec + qd->attrs_at);
        bb.attrs_packed = 1;
    } else if (conv || !(s_inner->flags & NV2A_BACKEND_RAW_DIRECT)) {
        /* Back to [vertex][attribute], where the back end reads it. */
        const float *src = (const float *)(rec + qd->attrs_at);
        uint32_t slot[NV2A_RAW_ATTRS], np = 0, k;
        for (a = 0; a < NV2A_RAW_ATTRS; a++)
            if (conv & (1u << a))
                slot[np++] = a * 4;
        if ((size_t)nv > c_attrs_cap) {
            free(c_attrs);
            c_attrs_cap = (size_t)nv + 1024;
            c_attrs = (float *)malloc(c_attrs_cap * NV2A_RAW_ATTRS * 16);
            if (!c_attrs) {
                c_attrs_cap = 0;
                return;
            }
        }
        for (v = 0; v < nv; v++) {
            float *o = c_attrs + (size_t)v * NV2A_RAW_ATTRS * 4;
            for (k = 0; k < np; k++, src += 4)
                memcpy(o + slot[k], src, 16);
        }
        if (!(s_inner->flags & NV2A_BACKEND_RAW_DIRECT))
            for (v = 0; v < nv; v++)        /* absent ones, as the executor fills them */
                for (a = 0; a < NV2A_RAW_ATTRS; a++)
                    if (!(bb.attr_present & (1u << a)))
                        memcpy(c_attrs + ((size_t)v * NV2A_RAW_ATTRS + a) * 4, bb.attr_const[a], 16);
        bb.attrs = c_attrs;
    } else {
        bb.attrs = c_attrs;             /* not read: nothing converted */
    }
    bb.regs = c_regs;
    bb.vp_program = (const uint32_t (*)[4])c_prog;
    if (bb.vp_slots > Q_PROG_ROWS)
        bb.vp_slots = Q_PROG_ROWS;
    bb.vp_consts = (const float (*)[4])c_consts;
    if (bb.vp_const_count > Q_CONST_ROWS)
        bb.vp_const_count = Q_CONST_ROWS;
    s_inner->draw_raw(&bb);
}

static void q_run(const uint8_t *rec)
{
    const QHdr *h = (const QHdr *)rec;
    const QBlock *b = (const QBlock *)(rec + sizeof(QHdr));
    size_t at;
    uint32_t i;

    for (i = 0; i < h->nreg; i++)
        memcpy(c_regs + (b[i].idx & (Q_BLOCKS - 1)) * 16, b[i].w, 64);
    at = sizeof(QHdr) + (size_t)h->nreg * sizeof(QBlock);
    at = runs_apply(rec, at, h->nprog, (uint8_t *)c_prog, Q_PROG_ROWS);
    at = runs_apply(rec, at, h->nconst, (uint8_t *)c_consts, Q_CONST_ROWS);
    switch (h->type) {
    case Q_DRAW:
        run_draw(rec, Q_ALIGN(at));
        break;
    case Q_CLEAR: {
        const QClear *c = (const QClear *)(rec + Q_ALIGN(at));
        s_inner->clear(&c->s, c->has_rs ? &c->rs : NULL, c->flags, c->argb, c->zs);
        break;
    }
    case Q_FLIP:
        s_inner->flip();
        __atomic_sub_fetch(&s_flips, 1, __ATOMIC_SEQ_CST);
        break;
    case Q_STORE: {
        const QStore *st = (const QStore *)(rec + sizeof(QHdr));
        __atomic_store_n((uint32_t *)st->word, st->value, __ATOMIC_RELEASE);
        break;
    }
    case Q_CALL:
        ((const QCall *)(rec + sizeof(QHdr)))->fn();
        break;
    default:
        break;
    }
}

static void cons_wait(size_t tail)
{
    uint64_t t0 = q_ns();
    unsigned i;

    /* The executor polls for this when it waits on the thread. */
    __atomic_store_n(&s_tail, tail, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_prod_waits, __ATOMIC_SEQ_CST))
        wake();
    for (i = 1;; i++) {
        cpu_relax();
        if (__atomic_load_n(&s_head, __ATOMIC_ACQUIRE) != tail)
            goto done;
        if ((i & 63) == 0) {
            if (q_ns() - t0 > s_spin_ns)
                break;
            nx_thread_yield();
        }
    }
    pthread_mutex_lock(&s_lock);
    __atomic_store_n(&s_cons_sleeps, 1, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_head, __ATOMIC_SEQ_CST) == tail)
        timed_wait();
    __atomic_store_n(&s_cons_sleeps, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s_lock);
done:
    __atomic_add_fetch(&st_idle_ns, q_ns() - t0, __ATOMIC_RELAXED);
}

static void *q_thread(void *arg)
{
    int placed = 0;
    size_t tail = 0, woke_at = 0;
    (void)arg;
    nx_thread_track((void *)q_thread);
    if (s_prio >= 0)
        nx_thread_set_priority(s_prio);
    for (;;) {
        size_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
        if (head == tail) {
            cons_wait(tail);
            continue;
        }
        if (!placed) {
            /* Beside the executor, now that it has said where it is. */
            int core = nx_thread_place_beside(s_exec_core);
            placed = 1;
            fprintf(stderr, "  [QUEUE] renderer thread on core %d (executor on %d)\n",
                    core, s_exec_core);
        }
        /* Everything handed over, a record at a time; the tail goes back
         * after each, for an executor waiting on a sync or for room. */
        woke_at = tail;
        while (tail != head) {
            const QHdr *h = (const QHdr *)(s_ring + tail % s_ring_bytes);
            uint32_t type = h->type;
            if (type != Q_WRAP)
                q_run((const uint8_t *)h);
            tail += h->size;
            __atomic_store_n(&s_tail, tail, __ATOMIC_SEQ_CST);
            /* An executor asleep on the frame limit wants this flip; one
             * asleep on room or a sync, the next 256 KB. Not every record:
             * a full ring would then trade a wake-up per draw. */
            if ((type == Q_FLIP || tail - woke_at >= (256u << 10))
                && __atomic_load_n(&s_prod_waits, __ATOMIC_SEQ_CST)) {
                wake();
                woke_at = tail;
            }
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s_prod_waits, __ATOMIC_SEQ_CST))
            wake();
    }
    return NULL;
}

static long env_num(const char *name, long dflt, long lo, long hi)
{
    const char *e = getenv(name);
    long v;
    if (!e || !*e)
        return dflt;
    v = strtol(e, NULL, 10);
    return v < lo ? lo : v > hi ? hi : v;
}

const Nv2aBackend *nv2a_backend_queue(const Nv2aBackend *inner)
{
    pthread_t t;
    pthread_attr_t at;

    if (s_on)
        return &s_qb;
    if (!inner || !inner->draw_raw || inner->draw) {
        fprintf(stderr, "  [QUEUE] back end not queued: it needs draw_raw and no draw\n");
        return inner;
    }
    s_ring_bytes = (size_t)env_num("RECOMP_QUEUE_RING_KB", 8 * 1024, 64, 512 * 1024) * 1024;
    s_ring_bytes &= ~(size_t)15;
    s_flip_limit = (int)env_num("RECOMP_QUEUE_FRAMES", 2, 1, 8);
    s_spin_ns = (uint64_t)env_num("RECOMP_QUEUE_SPIN_US", 50, 0, 10000) * 1000;
    s_copy_vertices = (int)env_num("RECOMP_QUEUE_COPY_VERTICES", 1, 0, 1);
    s_prio = (int)env_num("RECOMP_QUEUE_PRIO", -1, -1, 63);
    s_ring = (uint8_t *)malloc(s_ring_bytes + 16);
    if (!s_ring) {
        fprintf(stderr, "  [QUEUE] no memory for a %zu KB ring: back end on the executor's thread\n",
                s_ring_bytes / 1024);
        return inner;
    }
    s_ring = (uint8_t *)Q_ALIGN((uintptr_t)s_ring);
    s_inner = inner;
    memset(&s_qb, 0, sizeof s_qb);
    s_qb.clear = inner->clear ? q_clear : NULL;
    s_qb.flip = inner->flip ? q_flip : NULL;
    s_qb.draw_raw = q_draw;
    s_qb.flags = inner->flags;
    s_qb.surface_at = q_surface_at;     /* always: the 2D copy waits here */
    s_qb.written = inner->written ? q_written : NULL;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1u << 20);
    if (pthread_create(&t, &at, q_thread, NULL) != 0) {
        pthread_attr_destroy(&at);
        fprintf(stderr, "  [QUEUE] thread did not start: back end on the executor's thread\n");
        return inner;
    }
    pthread_attr_destroy(&at);
    pthread_detach(t);
    s_on = 1;
    fprintf(stderr, "  [QUEUE] back end on a thread of its own: %zu KB ring, up to %d frames"
                    " queued, %llu us spin, vertex arrays %s\n", s_ring_bytes / 1024, s_flip_limit,
            (unsigned long long)(s_spin_ns / 1000),
            s_copy_vertices ? "copied (GPU progress not held back for draws)"
                            : "read later (GPU progress held back; RECOMP_QUEUE_COPY_VERTICES=0)");
    return &s_qb;
}

int nv2a_backend_queue_report(char *buf, unsigned long n, double frames)
{
    static uint64_t last_ns;
    uint64_t now = q_ns(), span, idle = __atomic_exchange_n(&st_idle_ns, 0, __ATOMIC_RELAXED);
    double f = frames > 0 ? frames : 1.0, wait_ms = 0;
    int len, i;

    if (!s_on || !n)
        return 0;
    span = last_ns ? now - last_ns : 0;
    last_ns = now;
    for (i = 0; i < W_N; i++)
        wait_ms += (double)st_wait_ns[i] / 1e6;
    len = snprintf(buf, n,
                   "[perf] renderer thread: idle %.0f%%; queue %.0f KB/frame (indices %.0f,"
                   " converted attributes %.0f, vertices %.0f), %.0f records/frame in %.0f batches, %u made on"
                   " the executor; the executor waited %.2f ms/frame (traps %.1f/frame %.2f ms,"
                   " until idle %.1f/frame %.2f ms, two frames ahead %.2f ms, ring full %.2f ms);"
                   " stores %.1f/frame queued, %.1f at once",
                   span ? 100.0 * (double)idle / (double)span : 0.0,
                   (double)st_bytes / 1024.0 / f, (double)st_idx_bytes / 1024.0 / f,
                   (double)st_attr_bytes / 1024.0 / f, (double)st_vtx_bytes / 1024.0 / f, st_records / f, st_batches / f, st_big,
                   wait_ms / f, st_waits[W_SYNC] / f, (double)st_wait_ns[W_SYNC] / 1e6 / f,
                   st_waits[W_ALL] / f, (double)st_wait_ns[W_ALL] / 1e6 / f,
                   (double)st_wait_ns[W_FLIPS] / 1e6 / f, (double)st_wait_ns[W_ROOM] / 1e6 / f,
                   st_stores_queued / f, st_stores_now / f);
    memset(st_wait_ns, 0, sizeof st_wait_ns);
    memset(st_waits, 0, sizeof st_waits);
    st_bytes = st_idx_bytes = st_attr_bytes = st_vtx_bytes = 0;
    st_records = st_stores_queued = st_stores_now = st_big = st_batches = 0;
    return len < 0 ? 0 : (size_t)len < n ? len : (int)n - 1;
}

#endif /* !_WIN32 */
