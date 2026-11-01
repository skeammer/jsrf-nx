/*
 * Guest memory on Horizon (nx_memory.h).
 */
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/virtmem.h>
#include <switch/runtime/env.h>

#include "nx_memory.h"

/* win32_compat.c: the lengths VirtualQuery and the frees look up. */
void   view_register(void *addr, size_t len);
size_t view_take(const void *addr);

#define PAGE           0x1000u
#define RESERVE_ALIGN  0x10000u
#define GUEST_SPACE    0x100010000ull  /* 4 GB + 64 KB */

/* A committed range. code == 0: a plain heap block (committed anywhere). */
typedef struct {
    uintptr_t base;
    size_t    bytes;
    Handle    code;                    /* the code-memory object */
    void     *backing;                 /* the heap block behind the view */
} Committed;

#define MAX_COMMITTED 256

static Committed       g_committed[MAX_COMMITTED];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_reserved_once;
static int             g_checked;

size_t nx_mem_pages(size_t bytes)
{
    return (bytes + PAGE - 1) & ~(size_t)(PAGE - 1);
}

/* Once, before the first allocation: what the loader allows. An SVC outside
 * the process's permitted set does not fail on the console -- the kernel
 * ends the process -- so this is only ever reported, never tried. */
static void check_loader(void)
{
    int create, control;

    if (g_checked)
        return;
    g_checked = 1;
    create = envIsSyscallHinted(0x4B);
    control = envIsSyscallHinted(0x4C);
    fprintf(stderr, "  [NX] SVCs: CreateCodeMemory %d, ControlCodeMemory %d\n", create, control);
    if (!create || !control)
        fprintf(stderr, "  [NX] WARNING: the loader does not allow code memory, so there"
                        " is nowhere to put guest memory\n");
    else
        fprintf(stderr, "  [NX] guest memory: code-memory views of heap blocks"
                        " (no second views)\n");
}

static Committed *find_containing(uintptr_t address)
{
    int i;

    for (i = 0; i < MAX_COMMITTED; i++) {
        Committed *c = &g_committed[i];
        if (c->bytes && address >= c->base && address - c->base < c->bytes)
            return c;
    }
    return NULL;
}

static Committed *remember(uintptr_t base, size_t bytes, Handle code, void *backing)
{
    int i;

    for (i = 0; i < MAX_COMMITTED; i++) {
        Committed *c = &g_committed[i];
        if (c->bytes == 0) {
            c->base = base;
            c->bytes = bytes;
            c->code = code;
            c->backing = backing;
            return c;
        }
    }
    fprintf(stderr, "  [NX] more than %d committed ranges: %zu KB at %p will never"
                    " be released\n", MAX_COMMITTED, bytes / 1024, (void *)base);
    return NULL;
}

static uintptr_t free_space(size_t bytes)
{
    void *at;

    virtmemLock();
    at = virtmemFindAslr(bytes, RESERVE_ALIGN);
    virtmemUnlock();
    return (uintptr_t)at;
}

/* A heap block shown at `at` through a code-memory object. g_lock held. */
static int commit_at(uintptr_t at, size_t bytes)
{
    void *backing = memalign(PAGE, bytes);
    Handle code = 0;
    Result rc;

    if (backing == NULL) {
        fprintf(stderr, "  [NX] no heap left for %zu KB at %p\n", bytes / 1024, (void *)at);
        return 0;
    }
    rc = svcCreateCodeMemory(&code, backing, bytes);
    if (R_SUCCEEDED(rc))
        rc = svcControlCodeMemory(code, CodeMapOperation_MapOwner, (void *)at, bytes, Perm_Rw);
    if (R_FAILED(rc)) {
        fprintf(stderr, "  [NX] mapping %zu KB at %p failed: 0x%X\n", bytes / 1024,
                (void *)at, (unsigned)rc);
        if (code)
            svcCloseHandle(code);
        free(backing);
        return 0;
    }
    memset((void *)at, 0, bytes);
    remember(at, bytes, code, backing);
    return 1;
}

void *nx_mem_reserve(size_t bytes)
{
    size_t span = nx_mem_pages(bytes);
    void *at;

    if (span == 0)
        return NULL;
    pthread_mutex_lock(&g_lock);
    check_loader();
    if (!g_reserved_once) {
        g_reserved_once = 1;
        if (span < GUEST_SPACE)
            span = GUEST_SPACE;
    }
    virtmemLock();
    at = virtmemFindAslr(span, RESERVE_ALIGN);
    if (at)
        virtmemAddReservation(at, span);
    virtmemUnlock();
    pthread_mutex_unlock(&g_lock);
    if (at)
        fprintf(stderr, "  [NX] reserved %zu MB at %p\n", span >> 20, at);
    return at;
}

void *nx_mem_commit(uintptr_t at, size_t bytes)
{
    size_t span = nx_mem_pages(bytes);
    void *result = NULL;

    if (span == 0)
        return NULL;
    pthread_mutex_lock(&g_lock);
    check_loader();
    if (at == 0) {
        result = memalign(PAGE, span);
        if (result) {
            memset(result, 0, span);
            remember((uintptr_t)result, span, 0, NULL);
            view_register(result, span);
        }
    } else {
        uintptr_t base = at & ~(uintptr_t)(PAGE - 1);
        Committed *c = find_containing(base);

        if (c && base + span <= c->base + c->bytes) {
            result = (void *)at;               /* already there */
        } else if (commit_at(base, span)) {
            view_register((void *)base, span);
            result = (void *)base;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

void nx_mem_release(void *base)
{
    Committed *c;

    pthread_mutex_lock(&g_lock);
    c = find_containing((uintptr_t)base);
    if (c && c->base == (uintptr_t)base) {
        view_take(base);
        if (c->code) {
            svcControlCodeMemory(c->code, CodeMapOperation_UnmapOwner, base, c->bytes, 0);
            svcCloseHandle(c->code);
            free(c->backing);
        } else {
            free(base);
        }
        memset(c, 0, sizeof *c);
    }
    pthread_mutex_unlock(&g_lock);
}

void *nx_mem_map_view(uintptr_t at, size_t bytes, uintptr_t first_view)
{
    static int told;
    size_t span = nx_mem_pages(bytes);
    void *result = NULL;

    if (first_view) {
        /* A title writes a surface through the tiled aperture and the GPU
         * reads it through the contiguous window, so a second view must be
         * the same memory -- which code memory cannot give. */
        if (!told++)
            fprintf(stderr, "  [NX] second view of a mapping: code memory cannot be mapped"
                            " twice, so it is refused\n");
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    check_loader();
    if (at == 0)
        at = free_space(span);
    if (at && commit_at(at, span)) {
        view_register((void *)at, span);
        result = (void *)at;
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}
