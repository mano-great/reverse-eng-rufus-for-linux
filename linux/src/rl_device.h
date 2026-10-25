/*
 * rufus-linux - block device enumeration and safety checks
 * GPL-3.0-or-later
 */

#ifndef RL_DEVICE_H
#define RL_DEVICE_H

#include "rl_common.h"

#define RL_MAX_PARTITIONS 32

typedef struct {
	char     name[RL_NAME_MAX];      /* sdb2                                   */
	char     path[RL_PATH_MAX];      /* /dev/sdb2                              */
	char     devnode[32];            /* 8:18                                   */
	uint64_t size_bytes;
	char     mountpoint[RL_PATH_MAX];/* empty when not mounted                 */
	int      is_swap;
} rl_partition;

typedef struct {
	char       name[RL_NAME_MAX];
	char       path[RL_PATH_MAX];
	char       devnode[32];
	char       model[64];
	char       vendor[40];
	char       serial[64];
	const char *transport;           /* "usb", "nvme", "mmc", "ata", "optical" */
	uint64_t   size_bytes;
	uint32_t   sector_size;
	int        removable;
	int        read_only;
	int        is_usb;
	int        is_system;            /* holds /, /boot, /home, ...            */
	int        mounted;              /* at least one partition is mounted      */
	int        is_optical;
	int        npartitions;
	rl_partition part[RL_MAX_PARTITIONS];
} rl_device;

typedef struct {
	rl_device *items;
	size_t     count;
} rl_device_list;

int  rl_devices_scan(rl_device_list *list);
void rl_devices_free(rl_device_list *list);
const rl_device *rl_device_find(const rl_device_list *list, const char *path);

/*
 * Detach every mount point and swap area that lives on the device.
 * Requires root. Returns 0 on success, -1 with a message in err.
 */
int rl_device_unmount(const rl_device *dev, char *err, size_t errlen);

/* Look an entry up by "/dev/sdb" or "sdb". Returns NULL when absent. */
int rl_devnode_parse(const char *devnode, unsigned *maj, unsigned *min);

#endif /* RL_DEVICE_H */
