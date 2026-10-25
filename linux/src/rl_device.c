/*
 * rufus-linux - block device enumeration and safety checks
 *
 * Equivalent of Rufus' GetDevices()/GetDrivePartitionData() and the
 * mount/volume handling in drive.c, implemented against sysfs, /proc and
 * the Linux block ioctls instead of SetupAPI/DeviceIoControl.
 *
 * GPL-3.0-or-later
 */

#include "rl_device.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <sys/swap.h>
#include <unistd.h>

#define RL_ERR(...) do { if (err != NULL) snprintf(err, errlen, __VA_ARGS__); } while (0)

#define MAX_MOUNTS 512

typedef struct {
	unsigned maj;
	unsigned min;
	char     mountpoint[RL_PATH_MAX];
	char     fstype[32];
	int      critical;
} mount_entry;

static mount_entry g_mounts[MAX_MOUNTS];
static size_t      g_nmounts;

/* ------------------------------------------------------------------ */
/* /proc parsing                                                      */
/* ------------------------------------------------------------------ */

static void unescape_mountinfo(char *s)
{
	char *r = s, *w = s;

	while (*r != '\0') {
		if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' &&
		    r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
			*w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
			r += 4;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

int rl_devnode_parse(const char *devnode, unsigned *maj, unsigned *min)
{
	char *end;
	unsigned long a, b;

	if (devnode == NULL)
		return -1;
	a = strtoul(devnode, &end, 10);
	if (end == devnode || *end != ':')
		return -1;
	b = strtoul(end + 1, &end, 10);
	if (*end != '\0')
		return -1;
	*maj = (unsigned)a;
	*min = (unsigned)b;
	return 0;
}

static int is_critical_mount(const char *mp)
{
	static const char *prefixes[] = {
		"/boot", "/usr", "/var", "/etc", "/home", "/opt", "/srv", "/root"
	};
	size_t i;

	if (strcmp(mp, "/") == 0)
		return 1;
	for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i]);
		if (strncmp(mp, prefixes[i], n) == 0 && (mp[n] == '\0' || mp[n] == '/'))
			return 1;
	}
	return 0;
}

static void load_mounts(void)
{
	FILE *f;
	char line[2048];

	g_nmounts = 0;
	f = fopen("/proc/self/mountinfo", "r");
	if (f == NULL)
		return;

	while (fgets(line, sizeof(line), f) != NULL && g_nmounts < MAX_MOUNTS) {
		char *save = NULL;
		char *id, *parent, *devno, *root, *mp, *tok, *fstype;
		mount_entry *m;

		id = strtok_r(line, " ", &save);
		parent = strtok_r(NULL, " ", &save);
		devno = strtok_r(NULL, " ", &save);
		root = strtok_r(NULL, " ", &save);
		mp = strtok_r(NULL, " ", &save);
		(void)id; (void)parent; (void)root;
		if (id == NULL || mp == NULL || devno == NULL)
			continue;

		while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
			if (strcmp(tok, "-") == 0)
				break;
		}
		fstype = strtok_r(NULL, " ", &save);
		if (fstype == NULL)
			continue;

		m = &g_mounts[g_nmounts];
		if (rl_devnode_parse(devno, &m->maj, &m->min) != 0)
			continue;
		snprintf(m->mountpoint, sizeof(m->mountpoint), "%s", mp);
		unescape_mountinfo(m->mountpoint);
		snprintf(m->fstype, sizeof(m->fstype), "%s", fstype);
		m->critical = is_critical_mount(m->mountpoint);
		g_nmounts++;
	}
	fclose(f);
}

static int is_swap_path(const char *path)
{
	FILE *f;
	char line[1024];
	int found = 0;

	f = fopen("/proc/swaps", "r");
	if (f == NULL)
		return 0;
	/* skip header */
	if (fgets(line, sizeof(line), f) == NULL) {
		fclose(f);
		return 0;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char name[RL_PATH_MAX];
		if (sscanf(line, "%511s", name) != 1)
			continue;
		if (strcmp(name, path) == 0) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

static const char *find_mountpoint(unsigned maj, unsigned min, char *out, size_t outlen)
{
	size_t i;

	for (i = 0; i < g_nmounts; i++) {
		if (g_mounts[i].maj == maj && g_mounts[i].min == min) {
			snprintf(out, outlen, "%s", g_mounts[i].mountpoint);
			return out;
		}
	}
	return NULL;
}

static int mount_is_critical(unsigned maj, unsigned min)
{
	size_t i;

	for (i = 0; i < g_nmounts; i++) {
		if (g_mounts[i].maj == maj && g_mounts[i].min == min && g_mounts[i].critical)
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs helpers                                                      */
/* ------------------------------------------------------------------ */

static int read_attr(const char *dir, const char *name, char *buf, size_t buflen)
{
	char path[RL_PATH_MAX];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, buflen - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	rl_trim(buf);
	return 0;
}

static uint64_t read_attr_u64(const char *dir, const char *name, uint64_t fallback)
{
	char buf[64];
	char *end;
	unsigned long long v;

	if (read_attr(dir, name, buf, sizeof(buf)) != 0)
		return fallback;
	errno = 0;
	v = strtoull(buf, &end, 10);
	if (errno != 0 || end == buf)
		return fallback;
	return (uint64_t)v;
}

static int skip_device(const char *name)
{
	static const char *prefixes[] = {
		"loop", "ram", "zram", "dm-", "md", "nbd", "sr", "fd", "mtdblock"
	};
	size_t i;

	for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
			return 1;
	}
	/* eMMC boot/rpmb hardware partitions are not usable targets */
	if (strstr(name, "boot0") != NULL || strstr(name, "boot1") != NULL ||
	    strstr(name, "rpmb") != NULL)
		return 1;
	return 0;
}

static const char *detect_transport(const char *name, const char *realpath)
{
	if (strstr(realpath, "/usb") != NULL)
		return "usb";
	if (strncmp(name, "nvme", 4) == 0)
		return "nvme";
	if (strstr(realpath, "/virtio") != NULL || strncmp(name, "vd", 2) == 0)
		return "virtio";
	if (strstr(realpath, "/mmc") != NULL || strncmp(name, "mmcblk", 6) == 0)
		return "mmc";
	return "ata";
}

static void scan_partitions(rl_device *dev, const char *sysdir)
{
	DIR *d;
	struct dirent *de;

	d = opendir(sysdir);
	if (d == NULL)
		return;

	while ((de = readdir(d)) != NULL && dev->npartitions < RL_MAX_PARTITIONS) {
		char pdir[RL_PATH_MAX];
		char buf[64];
		unsigned maj, min;
		rl_partition *p;

		if (de->d_name[0] == '.')
			continue;
		snprintf(pdir, sizeof(pdir), "%s/%s", sysdir, de->d_name);
		/* Only real partitions carry a "partition" attribute. */
		if (read_attr(pdir, "partition", buf, sizeof(buf)) != 0)
			continue;

		p = &dev->part[dev->npartitions];
		memset(p, 0, sizeof(*p));
		snprintf(p->name, sizeof(p->name), "%s", de->d_name);
		snprintf(p->path, sizeof(p->path), "/dev/%s", de->d_name);
		p->size_bytes = read_attr_u64(pdir, "size", 0) * 512ULL;

		if (read_attr(pdir, "dev", p->devnode, sizeof(p->devnode)) == 0 &&
		    rl_devnode_parse(p->devnode, &maj, &min) == 0) {
			if (find_mountpoint(maj, min, p->mountpoint, sizeof(p->mountpoint)) != NULL) {
				dev->mounted = 1;
				if (mount_is_critical(maj, min))
					dev->is_system = 1;
			}
			p->is_swap = is_swap_path(p->path);
		}
		dev->npartitions++;
	}
	closedir(d);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int rl_devices_scan(rl_device_list *list)
{
	DIR *d;
	struct dirent *de;
	dev_t rootdev;

	if (list == NULL)
		return -1;
	memset(list, 0, sizeof(*list));

	load_mounts();
	{
		struct stat st;
		rootdev = (stat("/", &st) == 0) ? st.st_dev : 0;
	}

	d = opendir("/sys/block");
	if (d == NULL)
		return -1;

	while ((de = readdir(d)) != NULL) {
		char sysdir[RL_PATH_MAX];
		char real[PATH_MAX];
		char buf[128];
		unsigned maj, min;
		uint64_t sectors;
		rl_device dv;
		rl_device *slot;

		if (de->d_name[0] == '.' || skip_device(de->d_name))
			continue;

		snprintf(sysdir, sizeof(sysdir), "/sys/block/%s", de->d_name);

		sectors = read_attr_u64(sysdir, "size", 0);
		if (sectors == 0)
			continue;	/* empty card reader / no medium */

		memset(&dv, 0, sizeof(dv));
		snprintf(dv.name, sizeof(dv.name), "%s", de->d_name);
		snprintf(dv.path, sizeof(dv.path), "/dev/%s", de->d_name);
		dv.size_bytes = sectors * 512ULL;
		dv.sector_size = (uint32_t)read_attr_u64(sysdir, "queue/logical_block_size", 512);
		dv.removable = (int)read_attr_u64(sysdir, "removable", 0);
		dv.read_only = (int)read_attr_u64(sysdir, "ro", 0);
		read_attr(sysdir, "device/model", dv.model, sizeof(dv.model));
		read_attr(sysdir, "device/vendor", dv.vendor, sizeof(dv.vendor));
		read_attr(sysdir, "device/serial", dv.serial, sizeof(dv.serial));

		if (read_attr(sysdir, "dev", dv.devnode, sizeof(dv.devnode)) == 0 &&
		    rl_devnode_parse(dv.devnode, &maj, &min) == 0) {
			if (find_mountpoint(maj, min, buf, sizeof(buf)) != NULL) {
				dv.mounted = 1;
				if (mount_is_critical(maj, min))
					dv.is_system = 1;
			}
			if (rootdev != 0 && makedev(maj, min) == rootdev)
				dv.is_system = 1;
		}

		if (realpath(sysdir, real) != NULL) {
			dv.transport = detect_transport(dv.name, real);
			dv.is_usb = (strcmp(dv.transport, "usb") == 0);
		} else {
			dv.transport = "unknown";
		}

		scan_partitions(&dv, sysdir);

		slot = realloc(list->items, (list->count + 1) * sizeof(*list->items));
		if (slot == NULL) {
			closedir(d);
			rl_devices_free(list);
			return -1;
		}
		list->items = slot;
		list->items[list->count++] = dv;
	}
	closedir(d);

	/* Stable, predictable order for the UI. */
	if (list->count > 1) {
		size_t i, j;
		for (i = 1; i < list->count; i++) {
			rl_device tmp = list->items[i];
			j = i;
			while (j > 0 && strcmp(list->items[j - 1].path, tmp.path) > 0) {
				list->items[j] = list->items[j - 1];
				j--;
			}
			list->items[j] = tmp;
		}
	}
	return 0;
}

void rl_devices_free(rl_device_list *list)
{
	if (list == NULL)
		return;
	free(list->items);
	list->items = NULL;
	list->count = 0;
}

const rl_device *rl_device_find(const rl_device_list *list, const char *path)
{
	size_t i;
	const char *base;

	if (list == NULL || path == NULL)
		return NULL;
	base = rl_path_basename(path);

	for (i = 0; i < list->count; i++) {
		if (strcmp(list->items[i].path, path) == 0 ||
		    strcmp(list->items[i].name, base) == 0)
			return &list->items[i];
	}
	return NULL;
}

int rl_device_unmount(const rl_device *dev, char *err, size_t errlen)
{
	size_t i;
	int failed = 0;

	if (dev == NULL) {
		RL_ERR("invalid device");
		return -1;
	}

	load_mounts();

	/* Volumes that live on a partition. */
	for (i = 0; i < (size_t)dev->npartitions; i++) {
		unsigned maj, min;
		size_t m;

		if (rl_devnode_parse(dev->part[i].devnode, &maj, &min) != 0)
			continue;

		for (m = 0; m < g_nmounts; m++) {
			if (g_mounts[m].maj != maj || g_mounts[m].min != min)
				continue;
			if (umount2(g_mounts[m].mountpoint, MNT_DETACH) != 0) {
				RL_ERR("cannot unmount %s: %s", g_mounts[m].mountpoint,
				       strerror(errno));
				failed = 1;
			}
		}

		if (dev->part[i].is_swap) {
			if (swapoff(dev->part[i].path) != 0 && errno != EINVAL) {
				RL_ERR("cannot disable swap on %s: %s", dev->part[i].path,
				       strerror(errno));
				failed = 1;
			}
		}
	}

	/* Superfloppy layouts: the filesystem sits on the whole-disk node. */
	{
		unsigned maj, min;
		size_t m;

		if (rl_devnode_parse(dev->devnode, &maj, &min) == 0) {
			for (m = 0; m < g_nmounts; m++) {
				if (g_mounts[m].maj != maj || g_mounts[m].min != min)
					continue;
				if (umount2(g_mounts[m].mountpoint, MNT_DETACH) != 0) {
					RL_ERR("cannot unmount %s: %s", g_mounts[m].mountpoint,
					       strerror(errno));
					failed = 1;
				}
			}
		}
		if (is_swap_path(dev->path)) {
			if (swapoff(dev->path) != 0 && errno != EINVAL) {
				RL_ERR("cannot disable swap on %s: %s", dev->path,
				       strerror(errno));
				failed = 1;
			}
		}
	}

	if (failed)
		return -1;
	return 0;
}
