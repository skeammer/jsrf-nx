#include "guest_vmem.h"
#include "xbox_memory_layout.h"
#include <string.h>
#include <stdio.h>

extern ptrdiff_t g_xbox_mem_offset;

/* Real Xbox user-mode address space ends at 0x7FFEFFFF (kernel/system
 * reserve everything from 0x7FFF0000 up). Round the ceiling down to a
 * 64 KiB allocation granularity boundary, matching how NtAllocateVirtualMemory
 * behaves on real hardware. */
#define VMEM_TOP            0x7FFE0000u
#define VMEM_PAGE           4096u

#define MEM_COMMIT_FLAG     0x1000u
#define MEM_RESERVE_FLAG    0x2000u
#define MEM_DECOMMIT_FLAG   0x4000u
#define MEM_RELEASE_FLAG    0x8000u
#define MEM_FREE_STATE      0x10000u
#define MEM_PRIVATE_TYPE    0x20000u

/* winnt.h defines STATUS_INVALID_PARAMETER/STATUS_NO_MEMORY on Windows
 * builds; only fill in what's missing. */
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER       0xC000000Du
#endif
#ifndef STATUS_NO_MEMORY
#define STATUS_NO_MEMORY               0xC0000017u
#endif
#define STATUS_CONFLICTING_ADDRESSES   0xC0000018u

typedef struct {
    uint32_t reservation; /* base VA of the reservation holding it; 0 = free */
    uint16_t state;       /* 0 (free), MEM_RESERVE_FLAG or MEM_COMMIT_FLAG */
    uint16_t protect;
    uint32_t reserve_protect; /* protection the reservation was created with */
} guest_page_t;

/* One entry per 4 KiB page from g_xbox_total_ram to VMEM_TOP. Sized for the
 * worst case (RAM starting at 0) so it works regardless of how much RAM a
 * given run maps; unused low entries just never get touched. */
static guest_page_t g_vpages[VMEM_TOP / VMEM_PAGE];

#define PAGE_OF(va) ((va) / VMEM_PAGE)

/* The top of the range xbox_memory_layout.c's RAM mirrors cover: every
 * address below it is the same physical RAM again (the console's 26-bit
 * bus wraps around), so none of it can be handed out as new memory. */
static uint64_t mirrors_end(void)
{
    return (uint64_t)g_xbox_total_ram * (XBOX_NUM_MIRRORS + 1u);
}

/* Is `va` outside what this file tracks: plain RAM (the bump allocator's)
 * or past the user-mode top? */
static int untracked(uint32_t va)
{
    return va < g_xbox_total_ram || va >= VMEM_TOP;
}

/* Host backing for the extended-VMA range [aliased_top, VMEM_TOP).
 *
 * xbox_memory_layout.c's RAM-plus-mirrors scheme only ever reserves real
 * host memory for [0, aliased_top) -- the base MapViewOfFileEx region plus
 * XBOX_NUM_MIRRORS more views of the same physical pages, placed back to
 * back. Nothing else in this project reserves host memory above that, so
 * a guest reservation up here (now that guest_vmem_query correctly steers
 * the caller's own scanner past aliased_top instead of looping on it --
 * see FINDINGS.md) was succeeding at the bookkeeping level and then
 * faulting for real the first time anything touched it: g_xbox_mem_offset
 * + guest_va pointed at host address space nothing had ever reserved.
 * Reserve it for real, once, lazily (guest_vmem_allocate is the only
 * caller, and by the time it first runs xbox_memory_layout.c has already
 * set g_xbox_mem_offset), then MEM_COMMIT the specific sub-ranges this
 * file's own commit paths touch instead of assuming the memory is already
 * live. */
/* Set by xbox_MemoryLayoutInit, right after the RAM mirrors, once it has
 * actually reserved host memory for this range -- see the big comment
 * there for why that has to happen early rather than lazily here. */
extern int g_xbox_extvma_reserved;    /* xbox_memory_layout.c */

/* Commits [start, end) of the extended-VMA host reservation (real
 * VirtualAlloc MEM_COMMIT, which also zeroes the pages -- the memset
 * callers already did stays too, harmless and cheap, in case any commit
 * path here ever runs before extvma_ensure_reserved for a range that
 * needs it, e.g. inside plain RAM where no reservation is needed at all). */
static int extvma_commit(uint32_t start, uint32_t end)
{
    void *host = (void *)((uintptr_t)g_xbox_mem_offset + start);
    SIZE_T bytes = (SIZE_T)(end - start);

    if (g_xbox_extvma_reserved == 0)
        return 0;
    if (VirtualAlloc(host, bytes, MEM_COMMIT, PAGE_READWRITE) != NULL)
        return 1;
    fprintf(stderr, "guest_vmem: no host memory for guest 0x%08X..0x%08X (error %lu)\n",
            start, end, (unsigned long)GetLastError());
    return 0;
}

/* True when every page in [start, end) is currently free. */
static int range_is_free(uint32_t start, uint32_t end)
{
    uint32_t p;
    for (p = PAGE_OF(start); p < PAGE_OF(end); ++p) {
        if (g_vpages[p].state != 0)
            return 0;
    }
    return 1;
}

/* True when every page in [start, end) belongs to the same existing
 * reservation (needed before a commit can back part of it). */
static int range_owned_by(uint32_t start, uint32_t end, uint32_t reservation)
{
    uint32_t p;
    if (!reservation)
        return 0;
    for (p = PAGE_OF(start); p < PAGE_OF(end); ++p) {
        if (g_vpages[p].reservation != reservation)
            return 0;
    }
    return 1;
}

int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                         uint32_t protect, uint32_t *status)
{
    uint32_t requested_base = *base;
    uint32_t requested_size = *size;
    uint32_t start, end, p, reservation;
    int is_reserve = (alloc_type & MEM_RESERVE_FLAG) != 0;
    int is_commit  = (alloc_type & MEM_COMMIT_FLAG) != 0;
    uint64_t end64;

    /* Only handle explicit high addresses here -- base=0 ("anywhere") and
     * anything inside RAM still goes through the existing bump allocator. */
    if (requested_base < g_xbox_total_ram)       /* RAM: the bump allocator's */
        return 0;

    /* Everything from here up to (mirror count + 1) * RAM size aliases back
     * onto the exact same physical RAM -- this toolkit's own mirror views
     * (xbox_memory_layout.c) replicate the console's 26-bit address bus
     * wraparound faithfully, so there is no real, distinct memory anywhere
     * in that range to hand out. Treating a request in it as free, new
     * memory (this function's original design) silently backs it with a
     * memset that lands on whatever real guest data already occupies the
     * aliased low address -- confirmed live: a title reserving inside this
     * range zeroed live .rdata sitting at the wrapped-around offset.
     * Refuse cleanly instead, so the caller's own address-mismatch retry
     * (confirmed to exist and work -- see FINDINGS.md Entry 5) picks a
     * real, non-aliased address instead of us corrupting one. */
    if ((uint64_t)requested_base < mirrors_end()) {
        *status = STATUS_CONFLICTING_ADDRESSES;
        return 1;
    }

    *status = STATUS_INVALID_PARAMETER;

    if (!requested_size || (!is_reserve && !is_commit))
        return 1;

    /* A commit's base has to land on a real page; a reservation's base is
     * rounded down to the 64 KiB allocation granularity real Xbox uses. */
    start = requested_base & ~(is_reserve ? 0xFFFFu : (VMEM_PAGE - 1));
    end64 = (uint64_t)requested_base + requested_size;
    end64 = (end64 + VMEM_PAGE - 1) & ~(uint64_t)(VMEM_PAGE - 1);

    if (end64 <= start || end64 > VMEM_TOP || start < g_xbox_total_ram)
        return 1;
    end = (uint32_t)end64;

    if (is_reserve) {
        if (!range_is_free(start, end)) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 1;
        }
        reservation = start;
        for (p = PAGE_OF(start); p < PAGE_OF(end); ++p) {
            g_vpages[p].reservation = reservation;
            g_vpages[p].state = (uint16_t)MEM_RESERVE_FLAG;
            g_vpages[p].reserve_protect = protect;
            g_vpages[p].protect = 0;
        }
        if (is_commit) {                          /* reserve and commit at once */
            if (!extvma_commit(start, end)) {
                *status = STATUS_NO_MEMORY;
                return 1;
            }
            for (p = PAGE_OF(start); p < PAGE_OF(end); ++p) {
                g_vpages[p].state = (uint16_t)MEM_COMMIT_FLAG;
                g_vpages[p].protect = (uint16_t)protect;
            }
        }
    } else {
        /* MEM_COMMIT alone: every page in range must already belong to one
         * existing reservation. */
        reservation = g_vpages[PAGE_OF(start)].reservation;
        if (!range_owned_by(start, end, reservation)) {
            *status = STATUS_CONFLICTING_ADDRESSES;
            return 1;
        }
        if (!extvma_commit(start, end)) {
            *status = STATUS_NO_MEMORY;
            return 1;
        }
        for (p = PAGE_OF(start); p < PAGE_OF(end); ++p) {
            g_vpages[p].state = (uint16_t)MEM_COMMIT_FLAG;
            g_vpages[p].protect = (uint16_t)protect;
        }
    }

    *base = start;
    *size = end - start;
    *status = 0; /* STATUS_SUCCESS */
    return 1;
}

int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                     uint32_t *status)
{
    uint32_t start, end, p, reservation;
    uint64_t end64;

    if (*base < g_xbox_total_ram)                /* RAM: the bump allocator's */
        return 0;
    *status = STATUS_INVALID_PARAMETER;
    if (untracked(*base))
        return 1;

    start = *base & ~(VMEM_PAGE - 1);
    reservation = g_vpages[PAGE_OF(start)].reservation;
    if (reservation == 0)
        return 1;

    if (free_type == MEM_RELEASE_FLAG) {
        /* Release has to name the exact reservation base and frees the
         * whole thing; *size must be 0 per the NT contract. */
        if (*size || *base != reservation)
            return 1;
        for (end = start; end < VMEM_TOP; end += VMEM_PAGE)
            if (g_vpages[PAGE_OF(end)].reservation != reservation)
                break;
    } else if (free_type == MEM_DECOMMIT_FLAG) {
        if (!*size)
            return 1;
        end64 = (uint64_t)*base + *size;
        end64 = (end64 + VMEM_PAGE - 1) & ~(uint64_t)(VMEM_PAGE - 1);
        if (VMEM_TOP < end64)
            return 1;
        end = (uint32_t)end64;
        if (!range_owned_by(start, end, reservation))
            return 1;
    } else {
        return 1;
    }

    for (p = PAGE_OF(start); p < PAGE_OF(end); ++p) {
        /* Whatever is committed again later starts out zeroed, as NT's does. */
        if (g_vpages[p].state == MEM_COMMIT_FLAG)
            memset((uint8_t *)((uintptr_t)g_xbox_mem_offset) + (size_t)p * VMEM_PAGE, 0,
                   VMEM_PAGE);
        if (free_type == MEM_RELEASE_FLAG) {
            g_vpages[p] = (guest_page_t){ 0 };
        } else {
            g_vpages[p].state = (uint16_t)MEM_RESERVE_FLAG;
            g_vpages[p].protect = 0;
        }
    }

    *base = start;
    *size = end - start;
    *status = 0; /* STATUS_SUCCESS */
    return 1;
}

/* MEMORY_BASIC_INFORMATION, as the seven dwords the HLE copies out. */
static void answer(uint32_t info[7], uint32_t base, uint32_t allocation_base,
                   uint32_t allocation_protect, uint32_t size, uint32_t state,
                   uint32_t protect, uint32_t type)
{
    info[0] = base;
    info[1] = allocation_base;
    info[2] = allocation_protect;
    info[3] = size;
    info[4] = state;
    info[5] = protect;
    info[6] = type;
}

int guest_vmem_query(uint32_t address, uint32_t info[7])
{
    guest_page_t first;
    uint32_t start, end;

    if (untracked(address))
        return 0;

    /* The mirrored range (see guest_vmem_allocate) is never handed out, so
     * it is never marked here, and would look like one huge free region. A
     * title's own scanner looking for free space then kept proposing an
     * address inside it, was refused, and asked again forever (X-Men
     * Legends' CRT heap at 0x04000000). Reported reserved, the scanner moves
     * past it at once. */
    if ((uint64_t)address < mirrors_end()) {
        uint32_t ram = (uint32_t)g_xbox_total_ram;
        answer(info, ram, ram, 0, (uint32_t)(mirrors_end() - ram), MEM_RESERVE_FLAG, 0,
               MEM_PRIVATE_TYPE);
        return 1;
    }

    /* The run of pages from here that share the first one's reservation,
     * state and protection. */
    start = address & ~(VMEM_PAGE - 1);
    first = g_vpages[PAGE_OF(start)];
    for (end = start + VMEM_PAGE; end < VMEM_TOP; end += VMEM_PAGE) {
        const guest_page_t *page = &g_vpages[PAGE_OF(end)];
        if (page->reservation != first.reservation || page->state != first.state ||
            page->protect != first.protect)
            break;
    }
    answer(info, start, first.reservation, first.reserve_protect, end - start,
           first.state ? first.state : MEM_FREE_STATE, first.protect,
           first.state ? MEM_PRIVATE_TYPE : 0);
    return 1;
}
