/*
 * dk_pack -- build the deko3d renderer's shader pack from the keys the
 * console recorded (see xboxrecomp/src/nv2a_dk/nv2a_dk_pack.h).
 *
 *   dk_pack [-u uam] [-o dk_shaders.pak] [-w workdir] dk_keys.bin...
 *
 * Every vertex program and combiner setup in the key files becomes a
 * dedicated shader: the shader generator (dk_shadergen.c's program body
 * and fragment shader) around deko3d's interface -- the same
 * uniform blocks, bindings and varyings as the ubershaders, so the console
 * swaps one for the other per draw. Each is compiled with uam. An existing
 * pack named by -o is kept and only new keys are added -- if the same
 * generators made it (its stamp is DK_GEN_STAMP, which switch/dk_pack.sh
 * passes in; nv2a_dk_pack.h), else it is started over. Runs on the PC
 * (switch/dk_pack.sh runs it in the devkitPro image); the console builds the
 * same pack itself (xboxrecomp/src/nv2a_dk/dk_compile.c).
 */
/* The GLSL, as the console's own compiler makes it (dk_gen.c: dk_shadergen.c's
 * program body in deko3d's vertex interface, and its fragment shader). */
#include "../../xboxrecomp/src/nv2a_dk/dk_gen.c"
#include "../../xboxrecomp/src/nv2a_dk/nv2a_dk_pack.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    uint32_t kind;                      /* 0 vertex, 1 fragment */
    uint64_t key;
    uint8_t *dksh;
    uint32_t size;
} Entry;

static Entry   *s_ent;
static uint32_t s_nent, s_cap;
static const char *s_uam = "uam";
static const char *s_work = "dk_pack_work";
static uint32_t s_new_vs, s_new_fs, s_failed;

static int have(uint64_t key)
{
    uint32_t i;
    for (i = 0; i < s_nent; i++)
        if (s_ent[i].key == key)
            return 1;
    return 0;
}

static void add(uint32_t kind, uint64_t key, uint8_t *dksh, uint32_t size)
{
    if (s_nent == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 256;
        s_ent = (Entry *)realloc(s_ent, s_cap * sizeof *s_ent);
        if (!s_ent) { perror("realloc"); exit(1); }
    }
    s_ent[s_nent].kind = kind;
    s_ent[s_nent].key = key;
    s_ent[s_nent].dksh = dksh;
    s_ent[s_nent].size = size;
    s_nent++;
}

static uint8_t *slurp(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *p;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return p;
}

/* Compile `src` for `stage` ("vert"/"frag"); the DKSH module, padded to a
 * multiple of 256 bytes, or NULL (the source is kept for a look). */
static uint8_t *compile(const char *src, const char *stage, uint64_t key, uint32_t *size)
{
    char in[512], out[512], cmd[2048];
    FILE *f;
    uint8_t *d;
    uint32_t n;

    snprintf(in, sizeof in, "%s/%016llx.%s", s_work, (unsigned long long)key, stage);
    snprintf(out, sizeof out, "%s/%016llx.%s.dksh", s_work, (unsigned long long)key, stage);
    f = fopen(in, "w");
    if (!f) { perror(in); return NULL; }
    fputs(src, f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "\"%s\" -s %s -o \"%s\" \"%s\" > \"%s.log\" 2>&1",
             s_uam, stage, out, in, in);
    if (system(cmd) != 0) {
        fprintf(stderr, "dk_pack: uam failed on %s (see %s.log)\n", in, in);
        s_failed++;
        return NULL;
    }
    d = slurp(out, &n);
    if (!d || n < 24 || (n & 255)) {
        fprintf(stderr, "dk_pack: %s is not a DKSH module\n", out);
        free(d);
        s_failed++;
        return NULL;
    }
    unlink(in);
    unlink(out);
    snprintf(cmd, sizeof cmd, "%s.log", in);
    unlink(cmd);
    *size = n;
    return d;
}

static void do_vs(uint32_t start, uint32_t n, const uint32_t *words)
{
    static uint32_t prog[136][4];
    static char src[300 * 1024];
    uint64_t key;
    uint8_t *d;
    uint32_t size;

    if (start >= 136 || !n || start + n > 136)
        return;
    memset(prog, 0, sizeof prog);
    memcpy(prog[start], words, (size_t)n * 16);
    key = dk_vs_key(start, n, (const uint32_t (*)[4])prog);
    if (have(key))
        return;
    if (dk_gen_vs(start, n, words, src, sizeof src) < 0) {
        fprintf(stderr, "dk_pack: vertex program %016llx too long\n", (unsigned long long)key);
        s_failed++;
        return;
    }
    d = compile(src, "vert", key, &size);
    if (!d)
        return;
    add(0, key, d, size);
    s_new_vs++;
}

/* Two builds per combiner setup: as the title may draw it (alpha test from
 * the draw's uniforms, so it can discard), and with the test compiled out,
 * which keeps the GPU's early depth test (DK_KEY_FS_NOAT). dk_shadergen.c's
 * deko3d mode wraps the test in #if NV2A_ALPHA_TEST, default 1. */
static void do_fs(const uint32_t *words)
{
    static char src[64 * 1024 + 64];
    uint64_t key = dk_key_hash(DK_KEY_FS, words, DK_FS_KEY_WORDS);
    uint64_t key0 = dk_key_hash(DK_KEY_FS_NOAT, words, DK_FS_KEY_WORDS);
    uint8_t *d;
    uint32_t size;

    if (!have(key)) {
        if (dk_gen_fs(words, 1, src, sizeof src) < 0) {
            s_failed++;
        } else if ((d = compile(src, "frag", key, &size)) != NULL) {
            add(1, key, d, size);
            s_new_fs++;
        }
    }
    if (!have(key0)) {
        if (dk_gen_fs(words, 0, src, sizeof src) < 0) {
            s_failed++;
        } else if ((d = compile(src, "frag", key0, &size)) != NULL) {
            add(1, key0, d, size);
            s_new_fs++;
        }
    }
}

static void read_keys(const char *path)
{
    uint32_t size, at = 0, *w;
    uint8_t *p = slurp(path, &size);
    if (!p) {
        fprintf(stderr, "dk_pack: cannot read %s\n", path);
        exit(1);
    }
    w = (uint32_t *)p;
    size /= 4;
    while (at + 3 <= size) {
        if (w[at] == DK_KEY_VS) {
            uint32_t start = w[at + 1], n = w[at + 2];
            if (n > 136 || at + 3 + n * 4 > size)
                break;
            do_vs(start, n, &w[at + 3]);
            at += 3 + n * 4;
        } else if (w[at] == DK_KEY_FS) {
            if (w[at + 1] != DK_FS_KEY_WORDS || at + 2 + DK_FS_KEY_WORDS > size)
                break;
            do_fs(&w[at + 2]);
            at += 2 + DK_FS_KEY_WORDS;
        } else {
            fprintf(stderr, "dk_pack: %s: unknown record at word %u, stopping there\n", path, at);
            break;
        }
    }
    free(p);
}

static void read_pack(const char *path)
{
    uint32_t size, at = 12, i, count;
    uint8_t *p = slurp(path, &size);
    if (!p)
        return;                         /* a new pack */
    if (size < 12 || ((uint32_t *)p)[0] != DK_PACK_MAGIC || ((uint32_t *)p)[1] != DK_PACK_VERSION) {
        fprintf(stderr, "dk_pack: %s is not a version %u pack; starting a new one\n",
                path, DK_PACK_VERSION);
        free(p);
        return;
    }
    count = ((uint32_t *)p)[2];
    {
        uint64_t stamp = 0;
        if (count && size >= 28 && ((uint32_t *)p)[3] == DK_PACK_STAMP)
            memcpy(&stamp, p + 20, 8);
        if (DK_GEN_STAMP && stamp != DK_GEN_STAMP) {
            fprintf(stderr, "dk_pack: %s was made by other shader generators (stamp %016llx,"
                            " these %016llx); starting a new one\n", path,
                    (unsigned long long)stamp, (unsigned long long)DK_GEN_STAMP);
            free(p);
            return;
        }
    }
    for (i = 0; i < count && at + 16 <= size; i++) {
        uint32_t kind, n;
        uint64_t key;
        uint8_t *d;
        memcpy(&kind, p + at, 4);
        memcpy(&n, p + at + 4, 4);
        memcpy(&key, p + at + 8, 8);
        if (at + 16 + n > size)
            break;
        if (kind <= 1) {
            d = (uint8_t *)malloc(n);
            memcpy(d, p + at + 16, n);
            add(kind, key, d, n);
        }
        at += 16 + n;
    }
    free(p);
    fprintf(stderr, "dk_pack: %s: %u shaders kept\n", path, s_nent);
}

static int write_pack(const char *path)
{
    char tmp[1024];
    uint32_t hdr[3] = { DK_PACK_MAGIC, DK_PACK_VERSION, s_nent + (DK_GEN_STAMP != 0) }, i;
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) { perror(tmp); return 1; }
    fwrite(hdr, 4, 3, f);
    if (DK_GEN_STAMP) {                 /* first: the generators' stamp */
        uint32_t st[2] = { DK_PACK_STAMP, 0 };
        uint64_t stamp = DK_GEN_STAMP;
        fwrite(st, 4, 2, f);
        fwrite(&stamp, 8, 1, f);
    }
    for (i = 0; i < s_nent; i++) {
        fwrite(&s_ent[i].kind, 4, 1, f);
        fwrite(&s_ent[i].size, 4, 1, f);
        fwrite(&s_ent[i].key, 8, 1, f);
        fwrite(s_ent[i].dksh, 1, s_ent[i].size, f);
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        perror(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = "dk_shaders.pak";
    int i, nkeys = 0;
    uint32_t vs = 0, fs = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u") && i + 1 < argc) s_uam = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) s_work = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: dk_pack [-u uam] [-o dk_shaders.pak] [-w workdir] dk_keys.bin...\n");
            return 2;
        }
    }
    if (mkdir(s_work, 0755) != 0 && errno != EEXIST) {
        perror(s_work);
        return 1;
    }
    read_pack(out);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "-o") || !strcmp(argv[i], "-w")) { i++; continue; }
        read_keys(argv[i]);
        nkeys++;
    }
    if (!nkeys) {
        fprintf(stderr, "dk_pack: no key files given\n");
        return 2;
    }
    for (i = 0; i < (int)s_nent; i++) {
        if (s_ent[i].kind) fs++; else vs++;
    }
    if (write_pack(out))
        return 1;
    printf("dk_pack: %s: %u vertex + %u fragment shaders (%u + %u new), %u failed\n",
           out, vs, fs, s_new_vs, s_new_fs, s_failed);
    return s_failed ? 3 : 0;
}
