/*
 * rufus-linux - raw block/file I/O
 *
 * Thin, portable wrapper so the partitioning and formatting code can run
 * against either a real block device or a plain image file (used by tests).
 *
 * GPL-3.0-or-later
 */

#ifndef RL_IO_H
#define RL_IO_H

#include "rl_common.h"

typedef struct {
	int      fd;
	uint64_t size;
	uint32_t sector_size;
	int      is_block;
} rl_io;

int  rl_io_open(rl_io *io, const char *path, int writable, char *err, size_t errlen);
void rl_io_close(rl_io *io);

int  rl_io_read(rl_io *io, void *buf, size_t len, uint64_t off,
		char *err, size_t errlen);
int  rl_io_write(rl_io *io, const void *buf, size_t len, uint64_t off,
		 char *err, size_t errlen);
int  rl_io_zero(rl_io *io, uint64_t off, uint64_t len,
		char *err, size_t errlen);

#endif /* RL_IO_H */
