/* dk_gen.h -- GLSL for the deko3d shader pack (dk_gen.c). */
#ifndef DK_GEN_H
#define DK_GEN_H
#include <stddef.h>
#include <stdint.h>

/* A vertex program: `n` slots of 4 words loaded at `start`, through FINAL
 * (a 'VSK1' key). Writes NUL-terminated GLSL; returns its length or -1. */
int dk_gen_vs(uint32_t start, uint32_t n, const uint32_t *words, char *src, size_t cap);

/* A combiner setup: the 37 words of an Nv2aPshKey ('FSK1'). alpha_test 0
 * compiles the alpha test out (the DK_KEY_FS_NOAT build). */
int dk_gen_fs(const uint32_t *words, int alpha_test, char *src, size_t cap);

#endif
