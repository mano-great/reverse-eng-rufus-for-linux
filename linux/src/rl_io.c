/*
 * rufus-linux - raw block/file I/O
 * GPL-3.0-or-later
 */

#include "rl_io.h"

#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/ioctl.h>
#  include <linux/fs.h>
#endif

static int io_pread(int fd, void *buf, size_t len, uint64_t off)
{
#if defined(_WIN32)
	int n;

	if (_lseeki64(fd, (__int64)off, SEEK_SET) < 0)
		return -1;
	n = _read(fd, buf, (unsigned)len);
	if (n < 0)
		return -1;
	if ((size_t)n != len) {
		errno = EIO;
		return -1;
	}
	return 0;
#else
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, (char *)buf + done, len - done,
				  (off_t)(off + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
#endif
}

static int io_pwrite(int fd, const void *buf, size_t len, uint64_t off)
{
#if defined(_WIN32)
	int n;

	if (_lseeki64(fd, (__int64)off, SEEK_SET) < 0)
		return -1;
	n = _write(fd, buf, (unsigned)len);
	if (n < 0)
		return -1;
	if ((size_t)n != len) {
		errno = EIO;
		return -1;
	}
	return 0;
#else
	size_t done = 0;

	while (done < len) {
		ssize_t n = pwrite(fd, (const char *)buf + done, len - done,
				   (off_t)(off + done));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
#endif
}

int rl_io_open(rl_io *io, const char *path, int writable, char *err,
	       size_t errlen)
{
	if (io == NULL || path == NULL) {
		if (err) snprintf(err, errlen, "invalid argument");
		return -1;
	}
	memset(io, 0, sizeof(*io));
	io->fd = -1;
	io->sector_size = 512;

#if defined(_WIN32)
	io->fd = _open(path, writable ? (_O_RDWR | _O_BINARY) : (_O_RDONLY | _O_BINARY));
	if (io->fd < 0) {
		if (err) snprintf(err, errlen, "cannot open '%s': %s", path, strerror(errno));
		return -1;
	}
	{
		struct _stat64 st;
		if (_fstat64(io->fd, &st) != 0) {
			if (err) snprintf(err, errlen, "cannot stat '%s': %s", path, strerror(errno));
			_close(io->fd);
			io->fd = -1;
			return -1;
		}
		io->size = (uint64_t)st.st_size;
		io->is_block = 0;
	}
#else
	io->fd = open(path, writable ? (O_RDWR | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC));
	if (io->fd < 0) {
		if (err) snprintf(err, errlen, "cannot open '%s': %s", path, strerror(errno));
		return -1;
	}
	{
		struct stat st;
		if (fstat(io->fd, &st) != 0) {
			if (err) snprintf(err, errlen, "cannot stat '%s': %s", path, strerror(errno));
			close(io->fd);
			io->fd = -1;
			return -1;
		}
		if (S_ISBLK(st.st_mode)) {
			uint64_t sz = 0;
			int ssz = 0;

			io->is_block = 1;
			if (ioctl(io->fd, BLKGETSIZE64, &sz) != 0 || sz == 0) {
				if (err) snprintf(err, errlen, "cannot get size of '%s': %s",
						  path, strerror(errno));
				close(io->fd);
				io->fd = -1;
				return -1;
			}
			if (ioctl(io->fd, BLKSSZGET, &ssz) == 0 && ssz >= 512)
				io->sector_size = (uint32_t)ssz;
			io->size = sz;
		} else if (S_ISREG(st.st_mode)) {
			io->size = (uint64_t)st.st_size;
			io->is_block = 0;
		} else {
			if (err) snprintf(err, errlen, "'%s' is not a regular file or block device", path);
			close(io->fd);
			io->fd = -1;
			return -1;
		}
	}
#endif
	return 0;
}

void rl_io_close(rl_io *io)
{
	if (io == NULL || io->fd < 0)
		return;
#if defined(_WIN32)
	_close(io->fd);
#else
	close(io->fd);
#endif
	io->fd = -1;
}

int rl_io_read(rl_io *io, void *buf, size_t len, uint64_t off,
	       char *err, size_t errlen)
{
	if (io == NULL || io->fd < 0) {
		if (err) snprintf(err, errlen, "device is not open");
		return -1;
	}
	if (off > io->size || len > io->size - off) {
		if (err) snprintf(err, errlen, "read past end of device");
		return -1;
	}
	if (io_pread(io->fd, buf, len, off) != 0) {
		if (err) snprintf(err, errlen, "read at offset %llu failed: %s",
				  (unsigned long long)off, strerror(errno));
		return -1;
	}
	return 0;
}

int rl_io_write(rl_io *io, const void *buf, size_t len, uint64_t off,
		char *err, size_t errlen)
{
	if (io == NULL || io->fd < 0) {
		if (err) snprintf(err, errlen, "device is not open");
		return -1;
	}
	if (off > io->size || len > io->size - off) {
		if (err) snprintf(err, errlen, "write past end of device");
		return -1;
	}
	if (io_pwrite(io->fd, buf, len, off) != 0) {
		if (err) snprintf(err, errlen, "write at offset %llu failed: %s",
				  (unsigned long long)off, strerror(errno));
		return -1;
	}
	return 0;
}

int rl_io_zero(rl_io *io, uint64_t off, uint64_t len,
	       char *err, size_t errlen)
{
	uint8_t buf[65536];

	memset(buf, 0, sizeof(buf));
	while (len > 0) {
		size_t n = (len > sizeof(buf)) ? sizeof(buf) : (size_t)len;
		if (rl_io_write(io, buf, n, off, err, errlen) != 0)
			return -1;
		off += n;
		len -= n;
	}
	return 0;
}
