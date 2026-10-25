/*
 * rufus-linux - raw image writer
 *
 * Linux counterpart of Rufus' WriteFileWithProgress()/verify path in
 * drive.c: open the block device exclusively, stream the image with
 * pwrite(), flush the device cache, optionally re-read and compare a
 * SHA-256 digest, then force a partition table re-read.
 *
 * GPL-3.0-or-later
 */

#include "rl_write.h"
#include "rl_sha256.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RL_ERR(...) do { if (err != NULL) snprintf(err, errlen, __VA_ARGS__); } while (0)

/* Block layer ioctls (from <linux/fs.h>, redefined to stay dependency-free). */
#ifndef BLKGETSIZE64
#  define BLKGETSIZE64 _IOR(0x12, 114, size_t)
#endif
#ifndef BLKSSZGET
#  define BLKSSZGET    _IO(0x12, 104)
#endif
#ifndef BLKROGET
#  define BLKROGET     _IO(0x12, 94)
#endif
#ifndef BLKRRPART
#  define BLKRRPART    _IO(0x12, 95)
#endif
#ifndef BLKFLSBUF
#  define BLKFLSBUF    _IO(0x12, 97)
#endif

#define CHUNK_SIZE  (4u * 1024u * 1024u)
#define PROGRESS_MS 200

static volatile sig_atomic_t g_cancel;

static void on_signal(int sig)
{
	(void)sig;
	g_cancel = 1;
}

static double now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int write_full(int fd, const uint8_t *buf, size_t len, uint64_t off)
{
	while (len > 0) {
		ssize_t n = pwrite(fd, buf, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		buf += n;
		len -= (size_t)n;
		off += (uint64_t)n;
	}
	return 0;
}

static int read_full(int fd, uint8_t *buf, size_t len, uint64_t off)
{
	while (len > 0) {
		ssize_t n = pread(fd, buf, len, (off_t)off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		buf += n;
		len -= (size_t)n;
		off += (uint64_t)n;
	}
	return 0;
}

int rl_image_write(const char *image, const rl_device *dev,
                   const rl_write_opts *opts, rl_write_result *res,
                   char *err, size_t errlen)
{
	FILE *img = NULL;
	int devfd = -1;
	uint8_t *buf = NULL;
	uint64_t size = 0, off = 0;
	uint64_t devsize = 0;
	uint32_t sector_size = 512;
	int ro = 0;
	rl_sha256_ctx ctx;
	uint8_t img_digest[32];
	struct sigaction sa, old_int, old_term;
	double t_start, last_report;
	int rc = -1;

	if (image == NULL || dev == NULL || opts == NULL || res == NULL) {
		RL_ERR("invalid arguments");
		return -1;
	}
	memset(res, 0, sizeof(*res));
	snprintf(res->image_sha256, sizeof(res->image_sha256), "-");
	snprintf(res->device_sha256, sizeof(res->device_sha256), "-");

	img = fopen(image, "rb");
	if (img == NULL) {
		RL_ERR("cannot open '%s': %s", image, strerror(errno));
		return -1;
	}
	if (fseeko(img, 0, SEEK_END) != 0 || (size = (uint64_t)ftello(img)) == 0) {
		RL_ERR("cannot determine size of '%s'", image);
		goto out;
	}
	if (fseeko(img, 0, SEEK_SET) != 0) {
		RL_ERR("cannot rewind '%s'", image);
		goto out;
	}

	if (size > dev->size_bytes) {
		char a[32], b[32];
		rl_format_bytes(size, a, sizeof(a));
		rl_format_bytes(dev->size_bytes, b, sizeof(b));
		RL_ERR("image (%s) does not fit on %s (%s)", a, dev->path, b);
		goto out;
	}

	devfd = open(dev->path, O_RDWR | O_EXCL | O_CLOEXEC);
	if (devfd < 0) {
		if (errno == EACCES)
			RL_ERR("cannot open %s: permission denied (run as root)", dev->path);
		else if (errno == EBUSY)
			RL_ERR("%s is busy or still mounted", dev->path);
		else
			RL_ERR("cannot open %s: %s", dev->path, strerror(errno));
		goto out;
	}

	if (ioctl(devfd, BLKGETSIZE64, &devsize) == 0 && devsize > 0) {
		if (size > devsize) {
			char a[32], b[32];
			rl_format_bytes(size, a, sizeof(a));
			rl_format_bytes(devsize, b, sizeof(b));
			RL_ERR("image (%s) does not fit on %s (%s)", a, dev->path, b);
			goto out;
		}
	}
	if (ioctl(devfd, BLKSSZGET, &sector_size) != 0)
		sector_size = dev->sector_size ? dev->sector_size : 512;
	if (ioctl(devfd, BLKROGET, &ro) == 0 && ro) {
		RL_ERR("%s is write-protected", dev->path);
		goto out;
	}

	if (posix_memalign((void **)&buf, 4096, CHUNK_SIZE) != 0) {
		RL_ERR("out of memory");
		buf = NULL;
		goto out;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, &old_int);
	sigaction(SIGTERM, &sa, &old_term);
	g_cancel = 0;

	rl_sha256_init(&ctx);
	t_start = now_seconds();
	last_report = 0.0;

	while (off < size) {
		size_t want = (size - off > CHUNK_SIZE) ? CHUNK_SIZE : (size_t)(size - off);
		size_t got;

		if (g_cancel) {
			RL_ERR("cancelled by user");
			goto restore;
		}

		got = fread(buf, 1, want, img);
		if (got == 0) {
			RL_ERR("short read from '%s' at offset %llu", image,
			       (unsigned long long)off);
			goto restore;
		}

		rl_sha256_update(&ctx, buf, got);
		if (write_full(devfd, buf, got, off) != 0) {
			RL_ERR("write failed at offset %llu: %s",
			       (unsigned long long)off, strerror(errno));
			goto restore;
		}
		off += got;

		if (opts->progress != NULL) {
			double t = now_seconds() - t_start;
			if (t - last_report >= (double)PROGRESS_MS / 1000.0 || off == size) {
				rl_progress p;
				p.done = off;
				p.total = size;
				p.bytes_per_sec = (t > 0.0) ? (double)off / t : 0.0;
				p.phase = 0;
				opts->progress(&p, opts->user);
				last_report = t;
			}
		}
	}

	rl_sha256_final(&ctx, img_digest);
	rl_sha256_hex(img_digest, res->image_sha256);

	if (fsync(devfd) != 0) {
		RL_ERR("fsync failed on %s: %s", dev->path, strerror(errno));
		goto restore;
	}
	/* Drop the kernel's cached view of the old contents. */
	(void)ioctl(devfd, BLKFLSBUF);

	res->bytes_written = off;

	if (opts->verify) {
		rl_sha256_ctx vctx;
		uint8_t dev_digest[32];
		uint64_t v = 0;
		double t_verify = now_seconds();

		rl_sha256_init(&vctx);
		while (v < size) {
			size_t want = (size - v > CHUNK_SIZE) ? CHUNK_SIZE : (size_t)(size - v);

			if (g_cancel) {
				RL_ERR("verification cancelled by user");
				goto restore;
			}
			if (read_full(devfd, buf, want, v) != 0) {
				RL_ERR("read-back failed at offset %llu: %s",
				       (unsigned long long)v, strerror(errno));
				goto restore;
			}
			rl_sha256_update(&vctx, buf, want);
			v += want;

			if (opts->progress != NULL) {
				rl_progress p;
				double t = now_seconds() - t_verify;
				p.done = v;
				p.total = size;
				p.bytes_per_sec = (t > 0.0) ? (double)v / t : 0.0;
				p.phase = 1;
				opts->progress(&p, opts->user);
			}
		}
		rl_sha256_final(&vctx, dev_digest);
		rl_sha256_hex(dev_digest, res->device_sha256);
		res->verified = (memcmp(img_digest, dev_digest, sizeof(img_digest)) == 0) ? 1 : -1;
	}

	res->seconds = now_seconds() - t_start;
	(void)ioctl(devfd, BLKRRPART);
	sync();
	rc = 0;

restore:
	sigaction(SIGINT, &old_int, NULL);
	sigaction(SIGTERM, &old_term, NULL);

out:
	if (buf != NULL)
		free(buf);
	if (devfd >= 0)
		close(devfd);
	if (img != NULL)
		fclose(img);
	(void)sector_size;
	return rc;
}
