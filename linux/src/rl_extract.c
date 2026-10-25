/*
 * rufus-linux - ISO image extraction
 *
 * Copies the files of an ISO9660/Joliet/Rock Ridge image into a directory.
 * The directory is either a mounted FAT32/NTFS volume (the normal Linux
 * path, using the kernel's file system drivers) or a plain test directory.
 *
 * GPL-3.0-or-later
 */

#include "rl_extract.h"
#include "rl_isofs.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <errno.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COPY_CHUNK  (1024 * 1024)

/* Recursive mkdir; returns 0 if the directory exists afterwards. */
static int rl_mkdir_p(const char *path, char *err, size_t errlen)
{
#if defined(_WIN32)
	char tmp[RL_PATH_MAX];
	size_t i, n = strlen(path);

	if (n + 1 > RL_PATH_MAX) {
		if (err) snprintf(err, errlen, "path is too long");
		return -1;
	}
	memcpy(tmp, path, n + 1);
	if (n > 0 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\'))
		tmp[n - 1] = '\0';
	n = strlen(tmp);

	for (i = 3; i < n; i++) {
		if (tmp[i] == '/' || tmp[i] == '\\') {
			tmp[i] = '\0';
			if (GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES) {
				if (!CreateDirectoryA(tmp, NULL)) {
					DWORD le = GetLastError();
					if (le != ERROR_ALREADY_EXISTS &&
					    le != ERROR_ACCESS_DENIED &&
					    le != ERROR_INVALID_NAME &&
					    le != ERROR_PATH_NOT_FOUND) {
						if (err)
							snprintf(err, errlen,
								 "cannot create '%s': %lu",
								 tmp, le);
						return -1;
					}
				}
			}
			tmp[i] = '/';
		}
	}
	if (GetFileAttributesA(tmp) == INVALID_FILE_ATTRIBUTES) {
		if (!CreateDirectoryA(tmp, NULL)) {
			DWORD le = GetLastError();
			if (le != ERROR_ALREADY_EXISTS &&
			    le != ERROR_ACCESS_DENIED &&
			    le != ERROR_INVALID_NAME &&
			    le != ERROR_PATH_NOT_FOUND) {
				if (err)
					snprintf(err, errlen,
						 "cannot create '%s': %lu",
						 tmp, le);
				return -1;
			}
		}
	}
#else
	char tmp[RL_PATH_MAX];
	size_t i, n = strlen(path);

	if (n + 1 > RL_PATH_MAX) {
		if (err) snprintf(err, errlen, "path is too long");
		return -1;
	}
	memcpy(tmp, path, n + 1);
	if (n > 0 && tmp[n - 1] == '/')
		tmp[n - 1] = '\0';
	n = strlen(tmp);

	for (i = 1; i < n; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				if (err)
					snprintf(err, errlen,
						 "cannot create '%s': %s",
						 tmp, strerror(errno));
				return -1;
			}
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		if (err)
			snprintf(err, errlen, "cannot create '%s': %s",
				 tmp, strerror(errno));
		return -1;
	}
#endif
	return 0;
}

/* Copy all extents of one ISO entry to a regular file. */
static int copy_file(rl_iso_vol *vol, const rl_iso_entry *e,
		     const char *dest_path, char *err, size_t errlen)
{
	FILE *out = fopen(dest_path, "wb");
	uint8_t *buf = NULL;
	int rc = -1;

	if (out == NULL) {
		if (err) snprintf(err, errlen, "cannot write '%s': %s",
				  dest_path, strerror(errno));
		return -1;
	}

	buf = (uint8_t *)malloc(COPY_CHUNK);
	if (buf == NULL) {
		if (err) snprintf(err, errlen, "out of memory");
		fclose(out);
		return -1;
	}

	for (int i = 0; i < e->n_extents; i++) {
		uint64_t off = (uint64_t)e->ext[i].lba * RL_ISO_SECTOR;
		uint64_t left = e->ext[i].bytes;

		while (left > 0) {
			size_t n = left > COPY_CHUNK ? COPY_CHUNK : (size_t)left;

			if (rl_iso_vol_pread(vol, off, buf, n, err, errlen) != 0) {
				if (err) {
					char tmp[512];
					snprintf(tmp, sizeof(tmp),
						 "cannot read '%s' from image: %s",
						 e->path, err);
					strncpy(err, tmp, errlen - 1);
					err[errlen - 1] = '\0';
				}
				goto out;
			}
			if (fwrite(buf, 1, n, out) != n) {
				if (err)
					snprintf(err, errlen,
						 "cannot write '%s': %s",
						 dest_path, strerror(errno));
				goto out;
			}
			off += n;
			left -= n;
		}
	}

	if (fflush(out) != 0 || fclose(out) != 0) {
		if (err) snprintf(err, errlen, "cannot write '%s': %s",
				  dest_path, strerror(errno));
		out = NULL;
		rc = -1;
		goto out;
	}
	rc = 0;
out:
	free(buf);
	if (out != NULL)
		fclose(out);
	return rc;
}

typedef struct {
	rl_iso_vol    *vol;
	const char    *dest_dir;
	rl_extract_opts opts;
	rl_extract_stats stats;
	int            aborted;
	char           err[512];
} walk_ctx;

static int split_parent(const char *path, char *parent, size_t plen);

static int walk_cb(void *user, const rl_iso_entry *e, char *err, size_t errlen)
{
	walk_ctx *ctx = (walk_ctx *)user;
	char dest[RL_PATH_MAX];
	char parent[RL_PATH_MAX];
	size_t need;

	(void)err;
	(void)errlen;

	if (ctx->aborted)
		return -1;

	if (strstr(e->path, "..") != NULL || e->path[0] == '/') {
		snprintf(ctx->err, sizeof(ctx->err), "unsafe path '%s'", e->path);
		return -1;
	}

	need = strlen(ctx->dest_dir) + 1 + strlen(e->path) + 1;
	if (need > sizeof(dest)) {
		snprintf(ctx->err, sizeof(ctx->err),
			 "destination path too long: %s/%s",
			 ctx->dest_dir, e->path);
		return -1;
	}
	snprintf(dest, sizeof(dest), "%s/%s", ctx->dest_dir, e->path);

	if (e->is_dir) {
		if (rl_mkdir_p(dest, ctx->err, sizeof(ctx->err)) != 0)
			return -1;
		ctx->stats.dirs++;
		return 0;
	}

	if (e->is_symlink) {
		ctx->stats.symlinks_skipped++;
		return 0;
	}

	if (split_parent(dest, parent, sizeof(parent)) &&
	    strlen(parent) > 0 &&
	    rl_mkdir_p(parent, ctx->err, sizeof(ctx->err)) != 0)
		return -1;

	if (copy_file(ctx->vol, e, dest, ctx->err, sizeof(ctx->err)) != 0)
		return -1;

	ctx->stats.files++;
	ctx->stats.bytes += e->size;

	if (ctx->opts.progress != NULL &&
	    ctx->opts.progress(ctx->opts.user, dest,
			       ctx->stats.bytes, 0) != 0) {
		ctx->aborted = 1;
		return -1;
	}
	return 0;
}

/* Split a POSIX path at the last '/'. Returns 1 if a non-empty parent exists. */
static int split_parent(const char *path, char *parent, size_t plen)
{
	const char *slash = strrchr(path, '/');
	size_t n;

	if (slash == NULL || slash == path) {
		if (plen > 0)
			parent[0] = '\0';
		return 0;
	}
	n = (size_t)(slash - path);
	if (n + 1 > plen)
		n = plen - 1;
	memcpy(parent, path, n);
	parent[n] = '\0';
	return 1;
}

int rl_extract_iso(const char *image_path, const char *dest_dir,
		   const rl_extract_opts *opts, rl_extract_stats *stats,
		   char *err, size_t errlen)
{
	rl_iso_vol *vol = NULL;
	walk_ctx ctx;
	rl_extract_opts o;
	char err2[512];
	int rc;

	memset(&ctx, 0, sizeof(ctx));
	memset(&o, 0, sizeof(o));
	if (opts != NULL)
		o = *opts;

	if (rl_iso_vol_open(&vol, image_path, err2, sizeof(err2)) != 0) {
		if (err) snprintf(err, errlen, "%s", err2);
		return -1;
	}

	ctx.vol = vol;
	ctx.dest_dir = dest_dir;
	ctx.opts = o;

	rc = rl_iso_vol_walk(vol, walk_cb, &ctx, ctx.err, sizeof(ctx.err));

	if (stats != NULL)
		*stats = ctx.stats;
	rl_iso_vol_close(vol);

	if (rc != 0) {
		if (err) snprintf(err, errlen, "%s", ctx.err);
		return -1;
	}
	return 0;
}
