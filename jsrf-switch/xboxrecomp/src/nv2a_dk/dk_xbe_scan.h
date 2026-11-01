/* dk_xbe_scan.h -- shader-pack keys from a title's XBE (dk_xbe_scan.c). */
#ifndef DK_XBE_SCAN_H
#define DK_XBE_SCAN_H
#include <stddef.h>
#include <stdint.h>

/* A vertex program found: `n` slots of 4 words at slot `start` ('VSK1'). */
typedef void (*dk_scan_vs_fn)(void *ctx, uint32_t start, uint32_t n, const uint32_t *words);
/* A combiner setup: 37 words of an Nv2aPshKey ('FSK1'). */
typedef void (*dk_scan_fs_fn)(void *ctx, const uint32_t *words);

/* Every vertex shader and pixel shader in the XBE image `d` (the file as it
 * is, or any memory holding its sections), possibly more than once each. */
void dk_scan_xbe(const uint8_t *d, size_t size, dk_scan_vs_fn vs, dk_scan_fs_fn fs, void *ctx);

#endif
