/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    /* The rest of the disk. Partition 0 is the whole raw device, 2 holds
     * system data, and 3-5 are the per-title caches behind X:, Y: and Z:.
     * Without these the path layer reported "Unrecognized Xbox path" and
     * NtOpenFile returned STATUS_OBJECT_PATH_NOT_FOUND; DSTEAL_JP probes
     * partition0 at startup and treats the failure as fatal.
     *
     * No trailing separator: the probe opens the device itself, with nothing
     * after it. Listed after Partition1 so that keeps its own mapping. */
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    if (match_prefix(xbox_path, "\\??\\") && xbox_path[4] && xbox_path[5] == ':')
        xbox_path += 4;
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_prefix(out, s_rules[i].prefix))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

/* ======================================================================== */
/* Shared by both backends, for the same reason as xbox_LastFileError: the
 * bridge calls it unconditionally, and a _WIN32-only definition breaks every
 * POSIX link. The Win32 backend fills this during translation; the POSIX one
 * does not yet, so it reads empty there. */
static XBOX_THREAD_LOCAL wchar_t s_last_host_path_shared[MAX_PATH];

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path_shared;
}

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

/* The retail partition layout, in 512-byte sectors. Partition devices 1-5
 * report these sizes, so a title that asks a device how big it is gets an
 * answer that agrees with the table it read from Partition0. */
typedef struct {
    int         device;                 /* \Device\Harddisk0\PartitionN */
    const char *table_name;             /* its name in the partition table */
    uint32_t    first_sector, sectors;
} XboxPartition;

static const XboxPartition k_partitions[] = {    /* in table order */
    { 3, "XBOX_PART_X", 0x00000400u, 0x00177000u },  /* X: cache        */
    { 4, "XBOX_PART_Y", 0x00177400u, 0x00177000u },  /* Y: cache        */
    { 5, "XBOX_PART_Z", 0x002EE400u, 0x00177000u },  /* Z: cache        */
    { 2, "XBOX_PART_C", 0x00465400u, 0x000FA000u },  /* C: system       */
    { 1, "XBOX_PART_E", 0x0055F400u, 0x00465400u },  /* E: game, saves  */
};
#define PARTITION_COUNT ((int)(sizeof k_partitions / sizeof k_partitions[0]))

static uint64_t partition_bytes(int device)
{
    int i;

    for (i = 0; i < PARTITION_COUNT; i++)
        if (k_partitions[i].device == device)
            return (uint64_t)k_partitions[i].sectors * 512u;
    return 0;                           /* 0 (the whole disk) and unknowns */
}

/* The size a partition device reports, for a host path naming one of the
 * images (".../PartitionN.img", N 1-5); 0 for anything else. The image
 * itself stays empty on the Switch: growing a file on a FAT or exFAT card
 * really allocates the space. */
unsigned long long xbox_PartitionImageBytes(const char *host_path)
{
    const char *name, *slash;
    int device;

    if (host_path == NULL)
        return 0;
    name = host_path;
    if ((slash = strrchr(name, '/')) != NULL)
        name = slash + 1;
    else if ((slash = strrchr(name, '\\')) != NULL)
        name = slash + 1;
    else
        return 0;
    if (sscanf(name, "Partition%d.img", &device) != 1 || device < 1 || device > 5)
        return 0;
    return partition_bytes(device);
}

/* Partition0's table sector: "****PARTINFO****", 32 bytes reserved, then a
 * 32-byte entry per partition -- its name padded with spaces to 16, then
 * four little-endian dwords: flags (in use), first sector, sectors, 0. */
static void xbox_build_partition_sector(unsigned char sector[512])
{
    int i;

    memset(sector, 0, 512);
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < PARTITION_COUNT; i++) {
        const XboxPartition *part = &k_partitions[i];
        unsigned char *entry = sector + 48 + 32 * i;
        uint32_t fields[4];
        size_t name_len = strlen(part->table_name);

        fields[0] = XBOX_PART_IN_USE;
        fields[1] = part->first_sector;
        fields[2] = part->sectors;
        fields[3] = 0;
        memset(entry, ' ', 16);
        memcpy(entry, part->table_name, name_len > 16 ? 16 : name_len);
        memcpy(entry + 16, fields, sizeof fields);
    }
}

/* The N of "\Device\Harddisk0\PartitionN" naming the device itself (an
 * image file), or -1. With a path below it -- Partition1\TDATA -- it is a
 * filesystem access for the rules table instead.
 *
 * with_root: "PartitionN\" counts too. Under NT that is the volume's root
 * directory, which XAPI opens (FILE_DIRECTORY_FILE) for the cluster size; a
 * directory open cannot be answered with an image file here, so the
 * translation passes 0 and the rules table maps the root to a directory. */
static int xbox_partition_device_digit(const char *xbox_path, int with_root)
{
    int prefix = match_prefix(xbox_path, "\\Device\\Harddisk0\\Partition");
    const char *after;
    char digit;

    if (prefix == 0)
        return -1;
    digit = xbox_path[prefix];
    if (digit < '0' || '9' < digit)
        return -1;
    after = xbox_path + prefix + 1;
    if (with_root && (after[0] == '\\' || after[0] == '/'))
        after++;
    if (after[0] != '\0')
        return -1;
    return digit - '0';
}

/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

/* mkdir_p, once per directory: the few the path rules name (save_dir's
 * TitleData, UserData, Cache ...), remembered by a 64-bit hash. A lookup and
 * a store are single aligned words, so threads translating paths at once at
 * worst make a directory twice. */
static void mkdir_p_once(const char* path)
{
    static uint64_t done[16];
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p;
    int i;

    for (p = (const unsigned char *)path; *p; p++)
        h = (h ^ *p) * 1099511628211ull;
    if (!h)
        h = 1;
    for (i = 0; i < 16; i++)
        if (__atomic_load_n(&done[i], __ATOMIC_RELAXED) == h)
            return;
    mkdir_p(path);
    for (i = 0; i < 16; i++) {
        uint64_t zero = 0;
        if (__atomic_compare_exchange_n(&done[i], &zero, h, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return;
    }
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    /* The save-side directories and the partition device images, made
     * now: a title opens them without creating them first, as on a console
     * whose hard disk was formatted at the factory. */
    {
        static const char *subs[] = { "TitleData", "UserData", "Cache",
                                      "SystemData" };
        char path[MAX_PATH];
        unsigned char sector[512];
        FILE *f;
        int i;

        for (i = 0; i < (int)(sizeof subs / sizeof subs[0]); i++) {
            snprintf(path, sizeof(path), "%s/%s", s_save_dir, subs[i]);
            mkdir_p(path);
        }
        /* Partition0, the whole disk: its table sector, written over any
         * older one. The others are created empty: their sizes come from
         * the table, and growing them would really allocate the space on a
         * FAT or exFAT card. */
        snprintf(path, sizeof(path), "%s/Partition0.img", s_save_dir);
        f = fopen(path, "r+b");
        if (f == NULL)
            f = fopen(path, "w+b");
        if (f != NULL) {
            xbox_build_partition_sector(sector);
            if (fseek(f, XBOX_PART_TABLE_OFFSET, SEEK_SET) == 0)
                fwrite(sector, sizeof sector, 1, f);
            fclose(f);
        }
        for (i = 0; i < PARTITION_COUNT; i++) {
            snprintf(path, sizeof(path), "%s/Partition%d.img", s_save_dir,
                     k_partitions[i].device);
            if ((f = fopen(path, "ab")) != NULL)
                fclose(f);
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

/* Nonzero when a translated host path lies in the game directory -- the
 * unpacked disc, which nothing writes to (kernel_file.c caches its opens). */
int xbox_path_in_game(const char *host)
{
    size_t n = strlen(s_game_dir);
    return n && host && strncmp(host, s_game_dir, n) == 0 && (host[n] == '/' || !host[n]);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    /* The request as the title made it; the [FILE] line after the open pairs
     * with it, the same as on the Win32 backend. */
    fprintf(stderr, "  [PATH] %s\n", xbox_path);

    {
        int digit = xbox_partition_device_digit(xbox_path, 0);
        if (digit >= 0) {
            snprintf(host_path_buf, buf_size, "%s/Partition%d.img",
                     s_save_dir, digit);
            XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
            return TRUE;
        }
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists -- once per directory:
             * a mkdir for every component on every translation was a
             * handful of SD-card round trips per file the title opened. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p_once(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }
        /* No case folding: the Switch's SD card (FAT32 or exFAT) finds names
         * in any case already, as the Xbox's filesystems do. */

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}
