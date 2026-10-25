/*
 * rufus-linux - FAT32 formatter
 *
 * Port of Rufus' FormatLargeFAT32() (originally from fat32format by
 * Tom Thornhill) to the rufus-linux block I/O layer.
 *
 * GPL-3.0-or-later
 */

#ifndef RL_FAT32_H
#define RL_FAT32_H

#include "rl_common.h"
#include "rl_io.h"

typedef struct {
	uint32_t bytes_per_sector;
	uint32_t sectors_per_cluster;
	uint32_t cluster_size;
	uint32_t reserved_sectors;
	uint32_t fat_sectors;
	uint32_t num_fats;
	uint64_t total_sectors;
	uint64_t cluster_count;
	uint64_t data_start_sector;
	uint32_t volume_id;
	char     label[12];
} rl_fat32_geom;

uint32_t rl_fat32_default_cluster_size(uint64_t volume_size);

/*
 * Quick-format a FAT32 volume at part_off. cluster_size 0 selects the
 * default for the volume size. Returns 0 and fills geom on success.
 */
int rl_fat32_format(rl_io *io, uint64_t part_off, uint64_t part_size,
		    uint32_t cluster_size, const char *label,
		    rl_fat32_geom *geom, char *err, size_t errlen);

/* Parse a FAT32 boot sector (used by tests and by ISO-mode verification). */
int rl_fat32_parse_boot(const uint8_t *boot, size_t len, rl_fat32_geom *geom,
			char *err, size_t errlen);

#endif /* RL_FAT32_H */
