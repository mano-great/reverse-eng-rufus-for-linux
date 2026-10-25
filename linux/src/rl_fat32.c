/*
 * rufus-linux - FAT32 formatter
 * GPL-3.0-or-later
 */

#include "rl_fat32.h"

#include <time.h>
#if !defined(_WIN32)
#  include <unistd.h>
#endif

#define RL_FAT32_MIN_CLUSTERS  65525ull
#define RL_FAT32_MAX_CLUSTERS  0x0FFFFFF5ull
#define RL_FAT32_RESERVED      32u
#define RL_FAT32_NUM_FATS      2u
#define RL_FAT32_BACKUP_BOOT   6u

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

uint32_t rl_fat32_default_cluster_size(uint64_t size)
{
	if (size < 64ull * 1024 * 1024)
		return 512;
	if (size < 128ull * 1024 * 1024)
		return 1024;
	if (size < 256ull * 1024 * 1024)
		return 2048;
	if (size < 8ull * 1024 * 1024 * 1024)
		return 4096;
	if (size < 16ull * 1024 * 1024 * 1024)
		return 8192;
	if (size < 32ull * 1024 * 1024 * 1024)
		return 16384;
	if (size < 2ull * 1024 * 1024 * 1024 * 1024)
		return 32768;
	return 65536;
}

static void make_label(const char *src, char out[12])
{
	static const char *bad = "\"*+,./:;<=>?[\\]|";
	size_t i;

	memset(out, ' ', 11);
	out[11] = '\0';
	if (src == NULL)
		return;
	for (i = 0; i < 11 && src[i] != '\0'; i++) {
		char c = src[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if ((unsigned char)c < 0x20 || strchr(bad, c) != NULL)
			c = '_';
		out[i] = c;
	}
}

static uint32_t volume_id(void)
{
	time_t now = time(NULL);
	uint32_t x = (uint32_t)now;
#if defined(_WIN32)
	x ^= (uint32_t)(uintptr_t)&now << 16;
#else
	x ^= (uint32_t)getpid() << 16;
#endif
	x ^= x >> 13;
	x *= 0x9E3779B1u;
	return x ? x : 0x12345678u;
}

static int write_boot_sector(rl_io *io, uint64_t part_off, uint32_t bps,
			     const rl_fat32_geom *g, char *err, size_t errlen)
{
	uint8_t *bs = (uint8_t *)calloc(1, bps);
	uint8_t *fi = (uint8_t *)calloc(1, bps);
	int rc = -1;

	if (bs == NULL || fi == NULL) {
		if (err) snprintf(err, errlen, "out of memory");
		goto out;
	}

	/* BPB */
	bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
	memcpy(bs + 3, "MSWIN4.1", 8);
	put_le16(bs + 11, (uint16_t)bps);
	bs[13] = (uint8_t)g->sectors_per_cluster;
	put_le16(bs + 14, (uint16_t)g->reserved_sectors);
	bs[16] = (uint8_t)g->num_fats;
	put_le16(bs + 17, 0);
	put_le16(bs + 19, 0);
	bs[21] = 0xF8;
	put_le16(bs + 22, 0);
	put_le16(bs + 24, 63);
	put_le16(bs + 26, 255);
	put_le32(bs + 28, (uint32_t)(part_off / bps));
	put_le32(bs + 32, (uint32_t)g->total_sectors);
	put_le32(bs + 36, g->fat_sectors);
	put_le16(bs + 40, 0);
	put_le16(bs + 42, 0);
	put_le32(bs + 44, 2);
	put_le16(bs + 48, 1);
	put_le16(bs + 50, RL_FAT32_BACKUP_BOOT);
	bs[64] = 0x80;
	bs[66] = 0x29;
	put_le32(bs + 67, g->volume_id);
	memcpy(bs + 71, g->label, 11);
	memcpy(bs + 82, "FAT32   ", 8);
	bs[510] = 0x55;
	bs[511] = 0xAA;
	if (bps > 512) {
		bs[bps - 2] = 0x55;
		bs[bps - 1] = 0xAA;
	}

	/* FSInfo */
	put_le32(fi + 0, 0x41615252u);
	put_le32(fi + 484, 0x61417272u);
	put_le32(fi + 488, (uint32_t)(g->cluster_count - 1));
	put_le32(fi + 492, 3);
	put_le32(fi + 508, 0xAA550000u);

	if (rl_io_write(io, bs, bps, part_off, err, errlen) != 0)
		goto out;
	if (rl_io_write(io, fi, bps, part_off + bps, err, errlen) != 0)
		goto out;
	if (rl_io_write(io, bs, bps, part_off + (uint64_t)RL_FAT32_BACKUP_BOOT * bps,
			err, errlen) != 0)
		goto out;
	if (rl_io_write(io, fi, bps, part_off + (uint64_t)(RL_FAT32_BACKUP_BOOT + 1) * bps,
			err, errlen) != 0)
		goto out;
	rc = 0;
out:
	free(bs);
	free(fi);
	return rc;
}

int rl_fat32_format(rl_io *io, uint64_t part_off, uint64_t part_size,
		    uint32_t cluster_size, const char *label,
		    rl_fat32_geom *geom, char *err, size_t errlen)
{
	rl_fat32_geom g;
	uint8_t *first_fat = NULL;
	uint32_t bps, spc, reserved, fat_sectors, num_fats = RL_FAT32_NUM_FATS;
	uint64_t total, cluster_count, user_sectors, fat_needed;
	uint8_t label_entry[32];
	int rc = -1;

	memset(&g, 0, sizeof(g));
	if (io == NULL || geom == NULL) {
		if (err) snprintf(err, errlen, "invalid argument");
		return -1;
	}
	bps = io->sector_size ? io->sector_size : 512;
	if (bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0) {
		if (err) snprintf(err, errlen, "unsupported sector size %u", bps);
		return -1;
	}
	if (part_size < 64ull * 1024 * 1024) {
		if (err) snprintf(err, errlen,
				  "volume is too small for FAT32 (minimum 64 MiB)");
		return -1;
	}
	if (part_off % bps != 0 || part_size % bps != 0) {
		if (err) snprintf(err, errlen, "partition is not sector aligned");
		return -1;
	}

	total = part_size / bps;
	if (total >= 0xFFFFFFFFull) {
		if (err) snprintf(err, errlen, "volume is too large for FAT32 (maximum 2 TiB)");
		return -1;
	}

	int auto_cluster = (cluster_size == 0);

	if (cluster_size == 0)
		cluster_size = rl_fat32_default_cluster_size(part_size);
	if (cluster_size < bps)
		cluster_size = bps;
	if (cluster_size > 65536 || (cluster_size & (cluster_size - 1)) != 0 ||
	    cluster_size % bps != 0) {
		if (err) snprintf(err, errlen, "invalid cluster size %u", cluster_size);
		return -1;
	}
	spc = cluster_size / bps;

	/*
	 * Compute the FAT geometry. With an automatic cluster size, step down
	 * if the volume would end up with fewer than the FAT32 minimum number
	 * of clusters (this happens near the size boundaries).
	 */
	for (;;) {
		spc = cluster_size / bps;
		reserved = RL_FAT32_RESERVED;
		fat_sectors = (uint32_t)((total - reserved + 2ull * spc) /
					 ((uint64_t)spc * bps / 4ull + num_fats) + 1);
		{
			uint64_t system = reserved + (uint64_t)num_fats * fat_sectors;
			uint64_t align = (1024ull * 1024ull) / bps;
			system = (system + align - 1) / align * align;
			if (system < (uint64_t)num_fats * fat_sectors) {
				if (err) snprintf(err, errlen, "FAT geometry overflow");
				return -1;
			}
			reserved = (uint32_t)(system - (uint64_t)num_fats * fat_sectors);
		}
		user_sectors = total - reserved - (uint64_t)num_fats * fat_sectors;
		cluster_count = user_sectors / spc;
		if (cluster_count >= RL_FAT32_MIN_CLUSTERS || !auto_cluster ||
		    cluster_size <= bps)
			break;
		cluster_size /= 2;
	}
	if (reserved > 0xFFFF) {
		if (err) snprintf(err, errlen, "FAT reserved area is too large");
		return -1;
	}

	if (cluster_count < RL_FAT32_MIN_CLUSTERS) {
		if (err) snprintf(err, errlen,
				  "volume would have only %llu clusters; FAT32 needs %llu",
				  (unsigned long long)cluster_count,
				  (unsigned long long)RL_FAT32_MIN_CLUSTERS);
		return -1;
	}
	if (cluster_count > RL_FAT32_MAX_CLUSTERS) {
		if (err) snprintf(err, errlen,
				  "volume has too many clusters for FAT32; use a larger cluster size");
		return -1;
	}
	fat_needed = (cluster_count * 4ull + bps - 1) / bps;
	if (fat_needed > fat_sectors) {
		if (err) snprintf(err, errlen, "FAT is too small for the cluster count");
		return -1;
	}

	g.bytes_per_sector = bps;
	g.sectors_per_cluster = spc;
	g.cluster_size = cluster_size;
	g.reserved_sectors = reserved;
	g.fat_sectors = fat_sectors;
	g.num_fats = num_fats;
	g.total_sectors = total;
	g.cluster_count = cluster_count;
	g.data_start_sector = reserved + (uint64_t)num_fats * fat_sectors;
	g.volume_id = volume_id();
	make_label(label, g.label);

	/* Clear reserved sectors, both FATs and the root directory cluster. */
	if (rl_io_zero(io, part_off,
		       (g.data_start_sector + spc) * bps, err, errlen) != 0)
		return -1;

	first_fat = (uint8_t *)calloc(1, bps);
	if (first_fat == NULL) {
		if (err) snprintf(err, errlen, "out of memory");
		return -1;
	}
	put_le32(first_fat + 0, 0x0FFFFFF8u);
	put_le32(first_fat + 4, 0x0FFFFFFFu);
	put_le32(first_fat + 8, 0x0FFFFFFFu);   /* root directory, cluster 2 */
	for (uint32_t i = 0; i < num_fats; i++) {
		uint64_t off = part_off +
			((uint64_t)reserved + (uint64_t)i * fat_sectors) * bps;
		if (rl_io_write(io, first_fat, bps, off, err, errlen) != 0) {
			free(first_fat);
			return -1;
		}
	}

	if (write_boot_sector(io, part_off, bps, &g, err, errlen) != 0) {
		free(first_fat);
		return -1;
	}

	/* Volume label entry in the root directory (cluster 2). */
	memset(label_entry, 0, sizeof(label_entry));
	memcpy(label_entry, g.label, 11);
	label_entry[11] = 0x08;
	if (rl_io_write(io, label_entry, sizeof(label_entry),
			part_off + g.data_start_sector * bps, err, errlen) != 0) {
		free(first_fat);
		return -1;
	}

	free(first_fat);
	*geom = g;
	rc = 0;
	return rc;
}

int rl_fat32_parse_boot(const uint8_t *boot, size_t len, rl_fat32_geom *geom,
			char *err, size_t errlen)
{
	uint16_t bps;
	uint8_t spc;
	uint32_t total, fat_sectors;
	uint64_t data_start, cluster_count;

	if (boot == NULL || geom == NULL || len < 512) {
		if (err) snprintf(err, errlen, "boot sector is too short");
		return -1;
	}
	if (boot[510] != 0x55 || boot[511] != 0xAA) {
		if (err) snprintf(err, errlen, "missing boot signature");
		return -1;
	}
	bps = (uint16_t)(boot[11] | (boot[12] << 8));
	spc = boot[13];
	if (bps < 512 || (bps & (bps - 1)) != 0 || spc == 0 ||
	    (spc & (spc - 1)) != 0) {
		if (err) snprintf(err, errlen, "invalid BPB geometry");
		return -1;
	}
	if (memcmp(boot + 82, "FAT32   ", 8) != 0) {
		if (err) snprintf(err, errlen, "not a FAT32 boot sector");
		return -1;
	}
	total = (uint32_t)(boot[32] | (boot[33] << 8) |
			   ((uint32_t)boot[34] << 16) | ((uint32_t)boot[35] << 24));
	fat_sectors = (uint32_t)(boot[36] | (boot[37] << 8) |
				 ((uint32_t)boot[38] << 16) | ((uint32_t)boot[39] << 24));
	if (total == 0 || fat_sectors == 0) {
		if (err) snprintf(err, errlen, "invalid FAT32 sizes");
		return -1;
	}
	data_start = (uint64_t)(boot[14] | (boot[15] << 8)) +
		     (uint64_t)boot[16] * fat_sectors;
	cluster_count = (total - data_start) / spc;

	memset(geom, 0, sizeof(*geom));
	geom->bytes_per_sector = bps;
	geom->sectors_per_cluster = spc;
	geom->cluster_size = (uint32_t)spc * bps;
	geom->reserved_sectors = (uint32_t)(boot[14] | (boot[15] << 8));
	geom->fat_sectors = fat_sectors;
	geom->num_fats = boot[16];
	geom->total_sectors = total;
	geom->cluster_count = cluster_count;
	geom->data_start_sector = data_start;
	geom->volume_id = (uint32_t)(boot[67] | (boot[68] << 8) |
				     ((uint32_t)boot[69] << 16) |
				     ((uint32_t)boot[70] << 24));
	memcpy(geom->label, boot + 71, 11);
	geom->label[11] = '\0';
	return 0;
}
