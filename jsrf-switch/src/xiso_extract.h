/* xiso_extract.h -- unpack an Xbox disc image (XDVDFS) into a directory.
 *
 * Lets the port boot from the disc image itself: when game/default.xbe is
 * missing and an .iso sits next to the game folder, main.c unpacks it once
 * (see xiso_extract.c for the format and the safety checks). */
#ifndef JSRF_XISO_EXTRACT_H
#define JSRF_XISO_EXTRACT_H

#include <stddef.h>
#include <stdint.h>

/* Called as bytes are written: done and total in bytes, over all files. */
typedef void (*xiso_progress_fn)(uint64_t done, uint64_t total, void *ctx);

/* Unpack every file and directory of iso_path into out_dir (created if
 * missing; existing files of the same name are overwritten). Accepts a
 * trimmed image (XDVDFS at the start) or a full-disc dump (XDVDFS at the
 * XGD1, XGD2 or XGD3 game-partition offset). Returns 0 on success; on
 * failure -1, with a one-line reason in err. */
int xiso_extract(const char *iso_path, const char *out_dir,
                 xiso_progress_fn progress, void *ctx,
                 char *err, size_t errlen);

#endif
