/*
 * prof_report -- the Switch sampling profiler's prof.bin as per-thread
 * function tables (RECOMP_NX_PROFILE=1, src/switch_nx.c).
 *
 *   prof_report prof.bin symbols.txt [jsrf_log.txt]
 *
 * symbols.txt is `aarch64-none-elf-nm -n --defined-only jsrf_recomp.elf`
 * (switch/prof_report.sh makes it). Samples hold offsets into the NRO, which
 * are ELF addresses. The log's last "[prof] slots:" line names each thread
 * slot by its entry function. For every thread: "self" counts the function
 * the thread was in, "in" counts every function on the sampled stack (pc and
 * up to five frames of the x29 chain) once per sample. Runs on the PC.
 *
 * When the log has "[hitch] frame N: M ms" lines (the renderer held a frame
 * over 50 ms), the tables are given a second time for only the samples taken
 * inside those M ms before each line: what every thread was doing while the
 * screen stood still. Samples carry their time in 10 ms units since boot,
 * as the log's timestamps are.
 *
 * FROM=<s> and/or TO=<s> in the environment keep only the samples taken
 * between those log times (seconds since boot), for every table: the busy
 * stretch of a run, without the menus and loading around it.
 *
 * LINES=<function>[,<function>...]: for each, the addresses inside it the
 * samples were at, most first, as "@0x<addr> <count>" lines;
 * switch/prof_report.sh turns them into source lines with addr2line (the
 * runtime is built with line tables on the Switch).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint16_t slot, when; uint32_t pc, lr, ret[5]; } Sample;   /* 32 bytes */
typedef struct { uint32_t addr; char *name; } Sym;
typedef struct { uint32_t sym, n; } Count;
typedef struct { uint32_t from, to; } Window;    /* 10 ms units since boot */

static Window  *s_win;
static uint32_t s_nwin, s_capwin;

static void add_window(double end_s, double ms)
{
    if (s_nwin == s_capwin) {
        s_capwin = s_capwin ? s_capwin * 2 : 256;
        s_win = (Window *)realloc(s_win, s_capwin * sizeof *s_win);
        if (!s_win) { perror("realloc"); exit(1); }
    }
    s_win[s_nwin].from = (uint32_t)((end_s - ms / 1000.0) * 100.0);
    s_win[s_nwin].to = (uint32_t)(end_s * 100.0 + 1.0);
    s_nwin++;
}

/* `when` wraps every 655.36 s; a window is compared in the same lap. A table
 * of the 65,536 ticks, built once: with RECOMP_DK_HITCH_MS low there can be
 * thousands of windows, and a million samples each walking them all took
 * minutes. */
static int in_window(uint16_t when)
{
    static uint8_t *in;
    if (!in) {
        uint32_t i, k;
        in = (uint8_t *)calloc(65536, 1);
        if (!in) { perror("calloc"); exit(1); }
        for (i = 0; i < s_nwin; i++) {
            uint32_t len = s_win[i].to - s_win[i].from;
            if (len > 65535)
                len = 65535;
            for (k = 0; k <= len; k++)
                in[(s_win[i].from + k) & 0xFFFF] = 1;
        }
    }
    return in[when];
}

static Sym     *s_sym;
static uint32_t s_nsym;

static int sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return x < y ? -1 : x > y;
}

static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096];
    uint32_t cap = 0;
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        unsigned long long a;
        char type, name[3000];
        if (sscanf(line, "%llx %c %2999s", &a, &type, name) != 3)
            continue;
        if (type != 'T' && type != 't' && type != 'W' && type != 'w')
            continue;
        if (s_nsym == cap) {
            cap = cap ? cap * 2 : 65536;
            s_sym = (Sym *)realloc(s_sym, cap * sizeof *s_sym);
            if (!s_sym) { perror("realloc"); exit(1); }
        }
        s_sym[s_nsym].addr = (uint32_t)a;
        s_sym[s_nsym].name = strdup(name);
        s_nsym++;
    }
    fclose(f);
    qsort(s_sym, s_nsym, sizeof *s_sym, sym_cmp);
}

/* Index of the symbol containing `a`, or UINT32_MAX. */
static uint32_t sym_of(uint32_t a)
{
    uint32_t lo = 0, hi = s_nsym;
    if (a == 0xFFFFFFFFu || !s_nsym || a < s_sym[0].addr)
        return UINT32_MAX;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (s_sym[mid].addr <= a) lo = mid; else hi = mid;
    }
    return lo;
}

static int count_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Count *)a)->n, y = ((const Count *)b)->n;
    return x > y ? -1 : x < y;
}

/* Counts per symbol (index s_nsym: unknown), for one slot. */
static void table(const char *title, uint32_t *cnt, uint32_t total, int top)
{
    Count *c = (Count *)malloc((s_nsym + 1) * sizeof *c);
    uint32_t i, n = 0;
    for (i = 0; i <= s_nsym; i++)
        if (cnt[i]) { c[n].sym = i; c[n].n = cnt[i]; n++; }
    qsort(c, n, sizeof *c, count_cmp);
    printf("  %s\n", title);
    for (i = 0; i < n && (int)i < top; i++)
        printf("    %5.1f%%  %7u  %s\n", 100.0 * c[i].n / (total ? total : 1), c[i].n,
               c[i].sym < s_nsym ? s_sym[c[i].sym].name : "(outside the NRO)");
    free(c);
}

/* The tables for every thread slot; hitches: only samples in a window. */
static void report(const Sample *smp, size_t n, const uint32_t *slot_entry,
                   const int *slot_named, int hitches)
{
    size_t i;
    int s;
    for (s = 0; s < 64; s++) {
        uint32_t *self = (uint32_t *)calloc(s_nsym + 1, 4);
        uint32_t *in = (uint32_t *)calloc(s_nsym + 1, 4);
        uint32_t total = 0, k;
        for (i = 0; i < n; i++) {
            uint32_t seen[7], ns = 0, j;
            if (smp[i].slot != s || (hitches && !in_window(smp[i].when)))
                continue;
            total++;
            k = sym_of(smp[i].pc);
            self[k == UINT32_MAX ? s_nsym : k]++;
            /* each function once per sample, for the inclusive count */
            for (j = 0; j < 6; j++) {
                uint32_t a = j == 0 ? smp[i].pc : smp[i].ret[j - 1], q, dup = 0;
                if (a == 0xFFFFFFFFu)
                    continue;
                k = sym_of(a);
                if (k == UINT32_MAX)
                    continue;
                for (q = 0; q < ns; q++)
                    dup |= seen[q] == k;
                if (!dup) { seen[ns++] = k; in[k]++; }
            }
        }
        if (total) {
            uint32_t e = slot_named[s] ? sym_of(slot_entry[s]) : UINT32_MAX;
            printf("\nthread slot %d (%s): %u samples\n", s,
                   !slot_named[s] ? "?" : slot_entry[s] == 0 ? "the game's main thread"
                   : e < s_nsym ? s_sym[e].name : "?", total);
            table("self (where the thread was)", self, total, hitches ? 20 : 30);
            table("in (on the stack)", in, total, hitches ? 20 : 30);
        }
        free(self);
        free(in);
    }
}

static int pc_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Count *)a)->n, y = ((const Count *)b)->n;
    return x > y ? -1 : x < y;
}

/* "@0x<pc> <count>" for the 40 most sampled addresses in function `name`
 * (self samples, every thread). */
static void lines_of(const Sample *smp, size_t n, const char *name)
{
    uint32_t k, lo = 0, hi = 0, total = 0, npc = 0, i;
    Count *c;
    size_t j;
    for (k = 0; k < s_nsym; k++)
        if (!strcmp(s_sym[k].name, name))
            break;
    if (k == s_nsym) {
        printf("\n=== lines in %s: no such function ===\n", name);
        return;
    }
    lo = s_sym[k].addr;
    hi = k + 1 < s_nsym ? s_sym[k + 1].addr : 0xFFFFFFFFu;
    c = (Count *)calloc(n ? n : 1, sizeof *c);
    for (j = 0; j < n; j++) {
        uint32_t pc = smp[j].pc;
        if (pc < lo || pc >= hi)
            continue;
        total++;
        for (i = 0; i < npc && c[i].sym != pc; i++)
            ;
        if (i == npc) { c[npc].sym = pc; c[npc].n = 0; npc++; }
        c[i].n++;
    }
    qsort(c, npc, sizeof *c, pc_cmp);
    printf("\n=== lines in %s: %u samples ===\n", name, total);
    for (i = 0; i < npc && i < 40; i++)
        printf("@0x%08x %5.1f%% %u\n", c[i].sym, 100.0 * c[i].n / (total ? total : 1), c[i].n);
    free(c);
}

int main(int argc, char **argv)
{
    FILE *f;
    Sample *smp = NULL;
    size_t n = 0, cap = 0;
    uint32_t slot_entry[64] = {0};
    int slot_named[64] = {0};

    if (argc < 3) {
        fprintf(stderr, "usage: prof_report prof.bin symbols.txt [jsrf_log.txt]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            smp = (Sample *)realloc(smp, cap * sizeof *smp);
            if (!smp) { perror("realloc"); return 1; }
        }
        if (fread(&smp[n], sizeof *smp, 1, f) != 1)
            break;
        n++;
    }
    fclose(f);
    if (getenv("FROM") || getenv("TO")) {
        /* `when` wraps every 655.36 s: unwrapped in file order, which is
         * the order they were taken in. */
        double from = getenv("FROM") ? atof(getenv("FROM")) : 0.0;
        double to = getenv("TO") ? atof(getenv("TO")) : 1e12;
        size_t i, kept = 0, all = n;
        uint32_t lap = 0, prev = 0;
        for (i = 0; i < n; i++) {
            double t;
            if (i && smp[i].when + 30000u < prev)
                lap++;
            prev = smp[i].when;
            t = (lap * 65536.0 + smp[i].when) / 100.0;
            if (t >= from && t <= to)
                smp[kept++] = smp[i];
        }
        n = kept;
        printf("samples from %.1f s to %.1f s only: %zu of %zu\n", from,
               to < 1e11 ? to : 0.0, n, all);
    }
    load_syms(argv[2]);
    if (argc > 3 && (f = fopen(argv[3], "r"))) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            char *p = strstr(line, "[prof] slots:");
            char *h = strstr(line, "[hitch] frame ");
            if (h && !strstr(h, "captured")) {
                double t, ms;
                unsigned fr;
                if (sscanf(line, "[%lf]", &t) == 1 && sscanf(h, "[hitch] frame %u: %lf ms", &fr, &ms) == 2)
                    add_window(t, ms);
                continue;
            }
            if (!p)
                continue;
            p += 13;
            memset(slot_named, 0, sizeof slot_named);
            for (;;) {
                int sl, used;
                unsigned e;
                if (sscanf(p, " %d=%x%n", &sl, &e, &used) != 2)
                    break;
                if (sl >= 0 && sl < 64) { slot_entry[sl] = e; slot_named[sl] = 1; }
                p += used;
            }
        }
        fclose(f);
    }
    printf("%zu samples, %u symbols\n", n, s_nsym);
    report(smp, n, slot_entry, slot_named, 0);
    if (s_nwin) {
        double secs = 0;
        uint32_t w;
        for (w = 0; w < s_nwin; w++)
            secs += (s_win[w].to - s_win[w].from) / 100.0;
        printf("\n\n=== during the hitches: %u frames with a [hitch] line (over 50 ms, or"
               " RECOMP_DK_HITCH_MS), %.1f s in all ===\n", s_nwin, secs);
        report(smp, n, slot_entry, slot_named, 1);
    }
    if (getenv("LINES")) {
        char *list = strdup(getenv("LINES")), *tok, *save = NULL;
        for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
            lines_of(smp, n, tok);
        free(list);
    }
    free(smp);
    return 0;
}
