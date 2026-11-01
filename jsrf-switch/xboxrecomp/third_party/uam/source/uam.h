/* uam.h -- uam (devkitPro's GLSL -> deko3d compiler) as a library, for
 * compiling shaders on the console (jsrf-nx, nv2a_dk/dk_compile.c).
 *
 * One compile at a time: Mesa's GLSL front end is global state. The front
 * end is set up on the first compile and kept (its built-in function library
 * is the slow part); uam_shutdown() releases it. Errors are printed to
 * stderr, as the uam tool does. */
#ifndef UAM_H
#define UAM_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { UAM_STAGE_VERTEX = 0, UAM_STAGE_FRAGMENT = 1 };

/* A DKSH module (as `uam -o`) for `glsl`, malloc'd: free() it. NULL on a
 * compile error. */
void *uam_compile_dksh(int stage, const char *glsl, uint32_t *size);

void uam_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
