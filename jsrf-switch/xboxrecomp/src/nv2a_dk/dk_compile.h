/* dk_compile.h -- the deko3d shader pack, built on the console
 * (dk_compile.c). */
#ifndef DK_COMPILE_H
#define DK_COMPILE_H
#include <stdint.h>

/* Start the compiler thread (RECOMP_DK_COMPILE=0: never). `have` are the
 * keys the pack already holds; `pack_path` is the pack to append to,
 * `keys_path` the console's dk_keys.bin, `xbe_path` the game's XBE (waited
 * for if it is still being unpacked). Returns 0 when off or failed. */
int dk_compile_start(const uint64_t *have, uint32_t nhave, const char *pack_path,
                     const char *keys_path, const char *xbe_path);

/* 0 when RECOMP_DK_COMPILE=0 (then any pack is used as it is). */
int dk_compile_enabled(void);

/* A draw met a shader the pack lacks: build it next. Renderer thread. */
void dk_compile_request_vs(uint32_t start, uint32_t n, const uint32_t *words);
void dk_compile_request_fs(const uint32_t *words);

/* One finished shader, or 0: kind (0 vertex, 1 fragment), key, and the DKSH
 * module (malloc'd: the caller frees it). Renderer thread. */
int dk_compile_take(uint32_t *kind, uint64_t *key, uint8_t **dksh, uint32_t *size);

/* The first start: wait until every shader in the XBE at `xbe_path` (the
 * game's own, read again here) is built and in the pack, calling `progress`
 * every 0.1 s. Returns 1 when done, 0 if the compiler is off or
 * RECOMP_DK_COMPILE_WAIT=0 (then it builds them beside the game), or after
 * 90 s without progress. Quick when the pack has them all. */
int dk_compile_wait(const char *xbe_path, void (*progress)(uint32_t done, uint32_t total));

/* Shaders built and queued so far, for the progress bar; 0 when idle. */
int dk_compile_progress(uint32_t *done, uint32_t *total);

#endif
