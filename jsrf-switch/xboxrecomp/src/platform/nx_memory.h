/*
 * Guest memory on Horizon, for win32_compat.c's VirtualAlloc family.
 *
 * The Xbox HLE (xbox_memory_layout.c) works in Win32 terms: reserve 4 GB of
 * address space, commit pieces of it at fixed addresses, map a second view
 * of the contiguous window as the tiled aperture. Horizon homebrew has no
 * mmap; what it has is the code-memory SVCs JITs use. A committed range is a
 * page-aligned heap block wrapped in a code-memory object, with its owner
 * view mapped read-write at the address wanted.
 *
 * That memory cannot be mapped a second time (svcMapProcessMemory refuses
 * code memory, 0xD401), so a mapping's second view fails, and the memory
 * layout carries on with a tiled aperture of its own.
 *
 * Protection is not modelled: everything is read-write.
 */
#ifndef NX_MEMORY_H
#define NX_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* Address space only, kept from libnx's other users. The first call takes
 * at least 4 GB + 64 KB, so every guest aperture lands at base + VA. */
void *nx_mem_reserve(size_t bytes);

/* Zeroed read-write memory: at `at` (page-aligned down), or anywhere when
 * `at` is 0. A range already committed whole is returned as it is. NULL if
 * it cannot be had. */
void *nx_mem_commit(uintptr_t at, size_t bytes);

/* Give back what nx_mem_commit returned at `base`. Anything else (a slice
 * of a reservation, an unknown address) is ignored. */
void nx_mem_release(void *base);

/* A view of a file mapping. The first (first_view == 0) is committed memory;
 * a later one would have to alias it, which this memory cannot do: NULL. */
void *nx_mem_map_view(uintptr_t at, size_t bytes, uintptr_t first_view);

/* Bytes rounded up to whole pages. */
size_t nx_mem_pages(size_t bytes);

#endif
