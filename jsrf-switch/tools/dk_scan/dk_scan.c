/*
 * dk_scan -- every shader a title ships, as dk_keys.bin records, so the
 * deko3d shader pack can cover the game before it is played.
 *
 *   dk_scan [-o keys.bin] default.xbe...
 *
 * The console records each vertex program and combiner setup it draws with
 * (nv2a_dk.c, dk_keys.bin) and tools/dk_pack builds those; until a pack has
 * a scene's shaders, the scene draws with the ubershaders, slowly (a JSRF
 * cutscene: 17 fps, 58 ms of GPU a frame). The title's own shaders are in
 * its XBE; xboxrecomp/src/nv2a_dk/dk_xbe_scan.c finds them (the console's
 * own compiler uses the same code) and this writes them as key records.
 *
 * Combiner setups D3D builds from texture-stage states (no pixel shader)
 * are made at run time and are in no file; the console still records those.
 * Runs on the PC (switch/dk_pack.sh takes .xbe arguments through this).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../xboxrecomp/src/nv2a_dk/dk_xbe_scan.c"

#define KEY_VS 0x314B5356u               /* "VSK1", as nv2a_dk_pack.h */
#define KEY_FS 0x314B5346u               /* "FSK1" */

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return d;
}

static void put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

/* Records already written, so a shader stored twice is one record. */
static uint64_t *s_seen;
static size_t s_nseen, s_capseen;

static int seen(const uint32_t *w, size_t n, uint32_t kind)
{
    uint64_t h = 0xCBF29CE484222325ull ^ kind;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= w[i];
        h *= 0x100000001B3ull;
    }
    for (i = 0; i < s_nseen; i++)
        if (s_seen[i] == h)
            return 1;
    if (s_nseen == s_capseen) {
        s_capseen = s_capseen ? s_capseen * 2 : 256;
        s_seen = (uint64_t *)realloc(s_seen, s_capseen * sizeof *s_seen);
        if (!s_seen) { perror("realloc"); exit(1); }
    }
    s_seen[s_nseen++] = h;
    return 0;
}

static unsigned s_vs, s_fs;

static void on_vs(void *ctx, uint32_t start, uint32_t n, const uint32_t *w)
{
    FILE *out = (FILE *)ctx;
    uint32_t j;
    if (seen(w, (size_t)n * 4, KEY_VS))
        return;
    put32(out, KEY_VS);
    put32(out, start);
    put32(out, n);
    for (j = 0; j < n * 4; j++)
        put32(out, w[j]);
    s_vs++;
}

static void on_fs(void *ctx, const uint32_t *k)
{
    FILE *out = (FILE *)ctx;
    uint32_t j;
    if (seen(k, 37, KEY_FS))
        return;
    put32(out, KEY_FS);
    put32(out, 37);
    for (j = 0; j < 37; j++)
        put32(out, k[j]);
    s_fs++;
}

int main(int argc, char **argv)
{
    const char *out_path = "dk_scan_keys.bin";
    FILE *out;
    int i, files = 0;

    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out_path = argv[++i];
    out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }
    for (i = 1; i < argc; i++) {
        size_t size;
        uint8_t *d;
        if (!strcmp(argv[i], "-o")) { i++; continue; }
        d = slurp(argv[i], &size);
        if (!d) { fprintf(stderr, "dk_scan: cannot read %s\n", argv[i]); return 1; }
        dk_scan_xbe(d, size, on_vs, on_fs, out);
        free(d);
        files++;
    }
    fclose(out);
    if (!files) {
        fprintf(stderr, "usage: dk_scan [-o keys.bin] default.xbe...\n");
        return 2;
    }
    printf("dk_scan: %u vertex programs, %u combiner setups -> %s\n", s_vs, s_fs, out_path);
    return 0;
}
