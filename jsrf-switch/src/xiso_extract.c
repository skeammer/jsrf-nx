/* xiso_extract.c -- unpack an Xbox disc image (XDVDFS) into a directory.
 *
 * The format, as extract-xiso (XboxDev) and xemu read it:
 *   - 2048-byte sectors. The volume descriptor is sector 32 of the game
 *     partition: "MICROSOFT*XBOX*MEDIA" (20 bytes), then the root directory
 *     table's sector (u32) and size in bytes (u32), all little-endian.
 *   - A trimmed image ("xiso") starts with the game partition. A full-disc
 *     dump carries a video partition first, so the descriptor is found at
 *     one of the known game-partition offsets (XGD1, XGD2, XGD3).
 *   - A directory table is a binary tree of entries. Each entry: left and
 *     right subtree offsets (u16, in 4-byte units from the table start, 0 =
 *     none), the file's first sector (u32), its size (u32), attributes (u8,
 *     0x10 = directory), the name length (u8), then the name. The root entry
 *     is at offset 0. A directory's sector and size name its own table; size
 *     0 is an empty directory. 0xFFFF in both offsets is padding.
 *
 * The image is data from the user's disc, so nothing in it is trusted: every
 * offset is checked against the table and the image, names that could leave
 * the output directory are refused, trees are walked with an explicit stack
 * and a visited map (no recursion on the tree, no cycles), and directory
 * nesting is capped. */
#define _FILE_OFFSET_BITS 64
#include "xiso_extract.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#  include <direct.h>
#  define xmkdir(p) _mkdir(p)
#  define xseek(f, o) _fseeki64((f), (__int64)(o), SEEK_SET)
#  define xseek_end(f) _fseeki64((f), 0, SEEK_END)
#  define xtell(f) ((uint64_t)_ftelli64(f))
#else
#  define xmkdir(p) mkdir((p), 0777)
#  define xseek(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#  define xseek_end(f) fseeko((f), 0, SEEK_END)
#  define xtell(f) ((uint64_t)ftello(f))
#endif

#define SECTOR        2048u
#define HEADER_SECTOR 32u
#define MAX_TABLE     (16u * 1024u * 1024u)   /* no real directory is near this */
#define MAX_DEPTH     32
#define COPY_CHUNK    (1u * 1024u * 1024u)
#define PATH_MAX_LEN  1024

static const char k_magic[20] = "MICROSOFT*XBOX*MEDIA";
/* Where the game partition starts: trimmed image, XGD1 (original Xbox
 * titles, JSRF among them), XGD2, XGD3. */
static const uint64_t k_bases[] = { 0, 0x18300000ull, 0x0FD90000ull, 0x02080000ull };

typedef struct {
    FILE    *iso;
    uint64_t base, size;        /* partition start; bytes from there to EOF */
    uint64_t done, total;
    unsigned files, dirs;
    int      extract;           /* 0: count, 1: write */
    xiso_progress_fn progress;
    void    *ctx;
    unsigned char *buf;
    char    *err;
    size_t   errlen;
} Xiso;

static int fail(Xiso *x, const char *fmt, ...)
{
    va_list ap;
    if (x->err && x->errlen) {
        va_start(ap, fmt);
        vsnprintf(x->err, x->errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* n bytes at partition offset off. */
static int rd(Xiso *x, uint64_t off, void *dst, size_t n)
{
    if (off > x->size || n > x->size - off)
        return fail(x, "the image is truncated (read past its end)");
    if (xseek(x->iso, x->base + off) != 0 || fread(dst, 1, n, x->iso) != n)
        return fail(x, "cannot read the image: %s", strerror(errno));
    return 0;
}

/* A name that stays inside the directory it is written to. */
static int name_ok(const unsigned char *n, unsigned len)
{
    unsigned i;
    if (len == 0 || (len == 1 && n[0] == '.') || (len == 2 && n[0] == '.' && n[1] == '.'))
        return 0;
    for (i = 0; i < len; i++)
        if (n[i] < 0x20 || n[i] == '/' || n[i] == '\\' || n[i] == ':')
            return 0;
    return 1;
}

static int make_dir(Xiso *x, const char *path)
{
    if (xmkdir(path) != 0 && errno != EEXIST)
        return fail(x, "cannot create %s: %s", path, strerror(errno));
    return 0;
}

static int copy_file(Xiso *x, const char *path, uint32_t sector, uint32_t size)
{
    uint64_t off = (uint64_t)sector * SECTOR, left = size;
    FILE *out;

    if (off > x->size || size > x->size - off)
        return fail(x, "the image is truncated (%s runs past its end)", path);
    out = fopen(path, "wb");
    if (!out)
        return fail(x, "cannot create %s: %s", path, strerror(errno));
    if (left && xseek(x->iso, x->base + off) != 0) {
        fclose(out);
        return fail(x, "cannot read the image: %s", strerror(errno));
    }
    while (left) {
        size_t n = left > COPY_CHUNK ? COPY_CHUNK : (size_t)left;
        if (fread(x->buf, 1, n, x->iso) != n) {
            fclose(out);
            return fail(x, "cannot read the image: %s", strerror(errno));
        }
        if (fwrite(x->buf, 1, n, out) != n) {
            int e = errno;
            fclose(out);
            return fail(x, e == ENOSPC ? "the SD card is full (writing %s)"
                                       : "cannot write %s: %s", path, strerror(e));
        }
        left -= n;
        x->done += n;
        if (x->progress)
            x->progress(x->done, x->total, x->ctx);
    }
    if (fclose(out) != 0)
        return fail(x, "cannot write %s: %s", path, strerror(errno));
    return 0;
}

static int walk_dir(Xiso *x, uint32_t sector, uint32_t size, const char *dir, int depth);

/* One entry of a table: the file or subdirectory it names. */
static int visit(Xiso *x, const unsigned char *e, unsigned nlen, const char *dir, int depth)
{
    char path[PATH_MAX_LEN];
    uint32_t sector = le32(e + 4), size = le32(e + 8);
    int is_dir = (e[12] & 0x10) != 0;
    int n;

    if (!name_ok(e + 14, nlen))
        return fail(x, "the image has an unsafe file name in %s", dir);
    n = snprintf(path, sizeof path, "%s/%.*s", dir, (int)nlen, (const char *)(e + 14));
    if (n < 0 || (size_t)n >= sizeof path)
        return fail(x, "a path in the image is too long (%s/...)", dir);
    if (is_dir) {
        x->dirs++;
        if (x->extract && make_dir(x, path) != 0)
            return -1;
        return size ? walk_dir(x, sector, size, path, depth + 1) : 0;
    }
    x->files++;
    if (!x->extract) {
        x->total += size;
        return 0;
    }
    return copy_file(x, path, sector, size);
}

/* The directory table at sector, size bytes long, into dir. */
static int walk_dir(Xiso *x, uint32_t sector, uint32_t size, const char *dir, int depth)
{
    unsigned char *tab;
    unsigned char *seen;
    uint32_t *stack;
    uint32_t slots, sp = 0;
    int rc = 0;

    if (depth > MAX_DEPTH)
        return fail(x, "the image's directories nest too deep");
    if (size > MAX_TABLE)
        return fail(x, "the image has an impossible directory size");
    slots = size / 4u + 1u;
    tab = (unsigned char *)malloc(size);
    seen = (unsigned char *)calloc(slots, 1);
    stack = (uint32_t *)malloc(slots * sizeof *stack);
    if (!tab || !seen || !stack) {
        rc = fail(x, "out of memory reading a directory");
        goto out;
    }
    if (rd(x, (uint64_t)sector * SECTOR, tab, size) != 0) {
        rc = -1;
        goto out;
    }
    stack[sp++] = 0;
    while (sp && rc == 0) {
        uint32_t slot = stack[--sp], pos = slot * 4u;
        uint16_t left, right;
        unsigned nlen;

        if (slot >= slots || seen[slot])
            continue;
        seen[slot] = 1;
        if (pos + 14u > size) {
            rc = fail(x, "the image has a damaged directory in %s", dir);
            break;
        }
        left = le16(tab + pos);
        right = le16(tab + pos + 2);
        if (left == 0xFFFF && right == 0xFFFF)
            continue;                      /* padding: an empty table */
        nlen = tab[pos + 13];
        if (pos + 14u + nlen > size) {
            rc = fail(x, "the image has a damaged directory in %s", dir);
            break;
        }
        if (left && left != 0xFFFF && sp < slots)
            stack[sp++] = left;
        if (right && right != 0xFFFF && sp < slots)
            stack[sp++] = right;
        rc = visit(x, tab + pos, nlen, dir, depth);
    }
out:
    free(stack);
    free(seen);
    free(tab);
    return rc;
}

int xiso_extract(const char *iso_path, const char *out_dir,
                 xiso_progress_fn progress, void *ctx,
                 char *err, size_t errlen)
{
    Xiso x;
    unsigned char hdr[28];
    uint64_t file_size;
    uint32_t root_sector = 0, root_size = 0;
    size_t i;
    int found = 0, rc = -1;

    memset(&x, 0, sizeof x);
    x.progress = progress;
    x.ctx = ctx;
    x.err = err;
    x.errlen = errlen;
    if (err && errlen)
        err[0] = 0;

    x.iso = fopen(iso_path, "rb");
    if (!x.iso)
        return fail(&x, "cannot open %s: %s", iso_path, strerror(errno));
    if (xseek_end(x.iso) != 0) {
        fail(&x, "cannot read %s: %s", iso_path, strerror(errno));
        goto out;
    }
    file_size = xtell(x.iso);

    for (i = 0; i < sizeof k_bases / sizeof k_bases[0] && !found; i++) {
        uint64_t at = k_bases[i] + (uint64_t)HEADER_SECTOR * SECTOR;
        if (at + sizeof hdr > file_size)
            continue;
        if (xseek(x.iso, at) != 0 || fread(hdr, 1, sizeof hdr, x.iso) != sizeof hdr)
            continue;
        if (memcmp(hdr, k_magic, sizeof k_magic) == 0) {
            x.base = k_bases[i];
            x.size = file_size - k_bases[i];
            root_sector = le32(hdr + 20);
            root_size = le32(hdr + 24);
            found = 1;
        }
    }
    if (!found) {
        fail(&x, "%s is not an Xbox disc image (no XDVDFS volume found)", iso_path);
        goto out;
    }
    x.buf = (unsigned char *)malloc(COPY_CHUNK);
    if (!x.buf) {
        fail(&x, "out of memory");
        goto out;
    }

    /* Count first, so progress has a total; then write. */
    x.extract = 0;
    if (root_size && walk_dir(&x, root_sector, root_size, out_dir, 0) != 0)
        goto out;
    if (x.files == 0) {
        fail(&x, "%s holds no files", iso_path);
        goto out;
    }
    x.extract = 1;
    x.files = x.dirs = 0;
    if (make_dir(&x, out_dir) != 0)
        goto out;
    if (walk_dir(&x, root_sector, root_size, out_dir, 0) != 0)
        goto out;
    rc = 0;
out:
    free(x.buf);
    fclose(x.iso);
    return rc;
}
