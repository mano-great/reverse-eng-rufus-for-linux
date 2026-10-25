/*
 * rufus-linux - ISO image extraction
 *
 * Copies the files of an ISO9660/Joliet/Rock Ridge image into a directory.
 * The directory is either a mounted FAT32/NTFS volume (the normal Linux
 * path, using the kernel's file system drivers) or a plain test directory.
 *
 * GPL-3.0-or-later
 */

#ifndef RL_EXTRACT_H
#define RL_EXTRACT_H

#include "rl_common.h"

typedef struct {
	uint64_t files;
	uint64_t dirs;
	uint64_t bytes;
	uint64_t symlinks_skipped;
} rl_extract_stats;

/* Called after each file; return non-zero to abort. */
typedef int (*rl_extract_progress_fn)(void *user, const char *path,
				      uint64_t done_bytes, uint64_t total_bytes);

typedef struct {
	rl_extract_progress_fn progress;
	void                   *user;
	int                     verbose;
} rl_extract_opts;

/*
 * Extract every file of image_path below dest_dir. Existing files are
 * overwritten. Returns 0 on success.
 */
int rl_extract_iso(const char *image_path, const char *dest_dir,
		   const rl_extract_opts *opts, rl_extract_stats *stats,
		   char *err, size_t errlen);

/* Recursive mkdir; returns 0 if the directory exists afterwards. */
int rl_mkdir_p(const char *path, char *err, size_t errlen);

#endif /* RL_EXTRACT_H */
