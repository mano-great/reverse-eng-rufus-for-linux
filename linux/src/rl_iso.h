/*
 * rufus-linux - bootable image analysis
 *
 * Port of the read-only half of Rufus' iso.c: identify the file system,
 * the volume label, whether the image carries a bootable MBR/GPT
 * ("isohybrid") and whether El Torito provides an EFI boot image.
 *
 * GPL-3.0-or-later
 */

#ifndef RL_ISO_H
#define RL_ISO_H

#include "rl_common.h"

#define RL_ISO_SECTOR        2048
#define RL_ISO_PVD_LBA       16
#define RL_ISO_BOOT_LBA      17

typedef enum {
	RL_FS_UNKNOWN = 0,
	RL_FS_ISO9660,
	RL_FS_UDF,
	RL_FS_ISO9660_UDF
} rl_fs_kind;

typedef struct {
	char       path[RL_PATH_MAX];
	uint64_t   size_bytes;
	rl_fs_kind fs;
	char       label[33];
	char       system_id[33];

	int        has_mbr;          /* 0x55AA boot signature present        */
	int        has_gpt;          /* protective MBR + valid GPT header    */
	int        is_hybrid;        /* raw write is bootable on its own     */
	int        has_el_torito;    /* El Torito boot catalog found         */
	int        efi_bootable;     /* El Torito exposes an EFI image       */
	uint32_t   boot_catalog_lba;

	char       advice[192];
} rl_iso_info;

/*
 * Inspect an image file. Returns 0 on success (even if the image is not
 * bootable, which is reported through the fields), -1 on I/O error with a
 * message in err.
 */
int rl_iso_probe(const char *path, rl_iso_info *info, char *err, size_t errlen);

const char *rl_iso_fs_name(rl_fs_kind fs);

#endif /* RL_ISO_H */
