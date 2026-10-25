/*
 * rufus-linux - ISO9660 / Joliet / Rock Ridge tree walker
 *
 * Rufus uses the OS to mount the ISO and copy files. On Linux we parse the
 * image ourselves so the operation works without loop devices or extra
 * privileges. Supports Joliet names, Rock Ridge NM/SL/CE, multi-extent files
 * (needed for install.wim > 4 GiB) and directory recursion.
 *
 * GPL-3.0-or-later
 */

#ifndef RL_ISOFS_H
#define RL_ISOFS_H

#include "rl_common.h"
#include "rl_iso.h"

#define RL_ISO_MAX_EXTENTS 32

typedef struct {
	char     path[RL_PATH_MAX];      /* relative, '/'-separated */
	int      is_dir;
	int      is_symlink;
	uint64_t size;
	int      n_extents;
	struct {
		uint32_t lba;
		uint32_t bytes;
	} ext[RL_ISO_MAX_EXTENTS];
	char     link_target[RL_PATH_MAX];
} rl_iso_entry;

/* Return 0 to continue, non-zero to abort the walk. */
typedef int (*rl_iso_visit_fn)(void *user, const rl_iso_entry *e,
			       char *err, size_t errlen);

typedef struct rl_iso_vol rl_iso_vol;

int  rl_iso_vol_open(rl_iso_vol **vol, const char *path, char *err, size_t errlen);
void rl_iso_vol_close(rl_iso_vol *vol);

const rl_iso_info *rl_iso_vol_info(const rl_iso_vol *vol);

int  rl_iso_vol_walk(rl_iso_vol *vol, rl_iso_visit_fn cb, void *user,
		     char *err, size_t errlen);

/* Raw read from the image (used to stream file extents). */
int  rl_iso_vol_pread(rl_iso_vol *vol, uint64_t off, void *buf, size_t len,
		      char *err, size_t errlen);

#endif /* RL_ISOFS_H */
