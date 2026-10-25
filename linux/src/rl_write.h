/*
 * rufus-linux - raw image writer
 * GPL-3.0-or-later
 */

#ifndef RL_WRITE_H
#define RL_WRITE_H

#include "rl_common.h"
#include "rl_device.h"

typedef struct {
	uint64_t done;
	uint64_t total;
	double   bytes_per_sec;
	int      phase;    /* 0 = writing, 1 = verifying */
} rl_progress;

typedef void (*rl_progress_fn)(const rl_progress *p, void *user);

typedef struct {
	int             verify;      /* re-read and compare SHA-256 */
	rl_progress_fn  progress;    /* optional, may be NULL      */
	void           *user;
} rl_write_opts;

typedef struct {
	uint64_t bytes_written;
	int      verified;           /* 0 = not requested, 1 = OK, -1 = mismatch */
	char     image_sha256[65];
	char     device_sha256[65];
	double   seconds;
} rl_write_result;

/*
 * Write image bytes verbatim to the start of dev.
 * The caller must have unmounted the device (rl_device_unmount).
 * Returns 0 on success, -1 with a message in err.
 */
int rl_image_write(const char *image, const rl_device *dev,
                   const rl_write_opts *opts, rl_write_result *res,
                   char *err, size_t errlen);

#endif /* RL_WRITE_H */
