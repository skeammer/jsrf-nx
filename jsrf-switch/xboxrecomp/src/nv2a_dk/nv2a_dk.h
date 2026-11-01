/* nv2a_dk.h -- deko3d renderer for the pushbuffer executor (Nintendo Switch).
 *
 * The Switch's native GPU interface in place of OpenGL over Mesa: no driver
 * between the executor and the GPU, shaders precompiled at build time
 * (ubershaders; see nv2a_dk.c). Built with XBOXRECOMP_DEKO3D=ON. */
#ifndef NV2A_DK_H
#define NV2A_DK_H

#include <stdint.h>

/* Register the renderer with the executor. */
void nv2a_dk_install(void);

/* Bring the display up early and draw a loading screen (a bar) on a thread
 * of its own until the executor's first call takes the display over. */
void nv2a_dk_loader_start(void);

/* The loading screen's bar: 0..1000 fills it (unpacking the disc image),
 * -1 animates it. */
void nv2a_dk_set_progress(int permille);

/* Before the game runs: every shader the XBE at `xbe_path` ships that the
 * shader pack lacks, built while the loading screen shows the progress (in
 * its own colour). Returns at once when the pack has them all, the renderer
 * is not up, or RECOMP_DK_COMPILE=0; RECOMP_DK_COMPILE_WAIT=0 lets the game
 * start at once and builds them beside it. */
void nv2a_dk_build_shaders(const char *xbe_path);

/* For the platform's [perf] report. */
uint32_t nv2a_dk_frame_count(void);
uint64_t nv2a_dk_texture_bytes(void);
uint32_t nv2a_dk_program_count(void);
uint32_t nv2a_dk_draws_skipped(void);
uint32_t nv2a_dk_draw_count(void);
/* Where the renderer's time went since the last call (two lines, no
 * trailing newline). */
#include <stddef.h>
void nv2a_dk_perf(char *buf, size_t n, double secs);

#endif
