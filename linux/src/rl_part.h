/*
 * rufus-linux - MBR / GPT partition table writer
 * GPL-3.0-or-later
 */

#ifndef RL_PART_H
#define RL_PART_H

#include "rl_common.h"
#include "rl_io.h"

typedef enum {
	RL_PT_MBR = 0,
	RL_PT_GPT
} rl_pt_kind;

/* Partition type selectors. */
typedef enum {
	RL_PART_FAT32 = 0,   /* MBR 0x0c, GPT EFI System Partition */
	RL_PART_NTFS,        /* MBR 0x07, GPT Microsoft basic data   */
	RL_PART_EXFAT        /* MBR 0x07, GPT Microsoft basic data   */
} rl_part_type;

typedef struct {
	rl_pt_kind   kind;
	rl_part_type type;
	uint64_t     disk_size;      /* bytes */
	uint32_t     sector_size;    /* bytes */
	uint64_t     start_lba;
	uint64_t     sectors;
	const char  *label;          /* GPT partition name (UTF-16LE internally) */
} rl_part_spec;

/*
 * Choose an aligned layout for a single-partition disk. start_lba is aligned
 * to 1 MiB and the partition consumes everything up to the GPT backup area.
 */
int rl_part_layout(uint64_t disk_size, uint32_t sector_size, rl_pt_kind kind,
		   uint64_t *start_lba, uint64_t *sectors);

/* Wipe old signatures and write the partition table. */
int rl_part_write(rl_io *io, const rl_part_spec *spec, char *err, size_t errlen);

#endif /* RL_PART_H */
