/*
 * rufus-linux - MBR / GPT partition table writer
 * GPL-3.0-or-later
 */

#include "rl_part.h"

#if defined(_WIN32)
#  include <windows.h>
#  include <time.h>
#else
#  include <unistd.h>
#  include <time.h>
#endif

#define RL_GPT_ENTRIES      128
#define RL_GPT_ENTRY_SIZE   128
#define RL_MIB              (1024u * 1024u)

/* EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t guid_esp[16] = {
	0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
	0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

/* Microsoft basic data: EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
static const uint8_t guid_basic_data[16] = {
	0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};

static uint32_t crc32_calc(const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;
	int k;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc ^ 0xFFFFFFFFu;
}

static void guid_random(uint8_t g[16])
{
	static int seeded;
	uint64_t x;

	if (!seeded) {
#if defined(_WIN32)
		x = (uint64_t)time(NULL) ^ ((uint64_t)GetCurrentProcessId() << 32);
#else
		x = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32);
#endif
		seeded = 1;
	} else {
		x = (uint64_t)time(NULL) * 6364136223846793005ull + 1;
	}
	for (int i = 0; i < 16; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		g[i] = (uint8_t)(x >> 24);
	}
	/* RFC 4122 version 4 / variant bits (on-disk layout). */
	g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);
	g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)v);
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static void utf8_to_utf16le(const char *src, uint8_t *dst, size_t dst_bytes)
{
	size_t o = 0;

	memset(dst, 0, dst_bytes);
	if (src == NULL)
		return;
	while (*src != '\0' && o + 2 <= dst_bytes - 2) {
		uint32_t cp = (uint8_t)*src++;
		if ((cp & 0x80) != 0) {
			int extra = 0;
			if ((cp & 0xE0) == 0xC0) { cp &= 0x1F; extra = 1; }
			else if ((cp & 0xF0) == 0xE0) { cp &= 0x0F; extra = 2; }
			else if ((cp & 0xF8) == 0xF0) { cp &= 0x07; extra = 3; }
			while (extra-- > 0 && *src != '\0')
				cp = (cp << 6) | ((uint8_t)*src++ & 0x3F);
		}
		if (cp > 0xFFFF) {
			cp -= 0x10000;
			dst[o++] = (uint8_t)(0xD8 | (cp >> 18));
			dst[o++] = (uint8_t)(cp >> 10);
			dst[o++] = (uint8_t)(0xDC | ((cp >> 8) & 0x03));
			dst[o++] = (uint8_t)cp;
		} else {
			dst[o++] = (uint8_t)cp;
			dst[o++] = (uint8_t)(cp >> 8);
		}
	}
}

int rl_part_layout(uint64_t disk_size, uint32_t sector_size, rl_pt_kind kind,
		   uint64_t *start_lba, uint64_t *sectors)
{
	uint64_t total, start, entries_sectors, first_usable, last_usable;

	if (start_lba == NULL || sectors == NULL || sector_size < 512)
		return -1;
	if (disk_size < 8u * RL_MIB)
		return -1;

	total = disk_size / sector_size;
	start = (RL_MIB + sector_size - 1) / sector_size;

	if (kind == RL_PT_GPT) {
		entries_sectors = ((uint64_t)RL_GPT_ENTRIES * RL_GPT_ENTRY_SIZE +
				   sector_size - 1) / sector_size;
		first_usable = 2 + entries_sectors;
		last_usable = total - entries_sectors - 2;
		if (start < first_usable)
			start = first_usable;
	} else {
		last_usable = total - 1;
	}
	if (start >= last_usable)
		return -1;

	*start_lba = start;
	*sectors = last_usable - start + 1;
	return 0;
}

static int write_gpt(rl_io *io, const rl_part_spec *spec, char *err,
		     size_t errlen)
{
	uint32_t ss = spec->sector_size;
	uint64_t total = spec->disk_size / ss;
	uint64_t entries_sectors = ((uint64_t)RL_GPT_ENTRIES * RL_GPT_ENTRY_SIZE +
				    ss - 1) / ss;
	uint8_t *entries = NULL;
	uint8_t *hdr = NULL;
	uint8_t mbr[512];
	uint8_t disk_guid[16], part_guid[16];
	const uint8_t *type_guid;
	uint32_t entries_crc, header_crc;
	int rc = -1;

	entries = (uint8_t *)calloc(1, RL_GPT_ENTRIES * RL_GPT_ENTRY_SIZE);
	hdr = (uint8_t *)calloc(1, ss);
	if (entries == NULL || hdr == NULL) {
		if (err) snprintf(err, errlen, "out of memory");
		goto out;
	}

	guid_random(disk_guid);
	guid_random(part_guid);
	type_guid = (spec->type == RL_PART_FAT32) ? guid_esp : guid_basic_data;

	memcpy(entries + 0, type_guid, 16);
	memcpy(entries + 16, part_guid, 16);
	put_le64(entries + 32, spec->start_lba);
	put_le64(entries + 40, spec->start_lba + spec->sectors - 1);
	utf8_to_utf16le(spec->label ? spec->label : "RUFUSLINUX",
			entries + 56, 72);

	entries_crc = crc32_calc(entries, RL_GPT_ENTRIES * RL_GPT_ENTRY_SIZE);

	/* Protective MBR. */
	memset(mbr, 0, sizeof(mbr));
	mbr[446 + 1] = 0x00; mbr[446 + 2] = 0x02; mbr[446 + 3] = 0x00;
	mbr[446 + 4] = 0xEE;
	mbr[446 + 5] = 0xFE; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
	put_le32(mbr + 446 + 8, 1);
	put_le32(mbr + 446 + 12,
		 (uint32_t)((total - 1 > 0xFFFFFFFFull) ? 0xFFFFFFFFull : total - 1));
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	if (rl_io_write(io, mbr, sizeof(mbr), 0, err, errlen) != 0)
		goto out;

	/* Primary header. */
	memcpy(hdr, "EFI PART", 8);
	hdr[8] = 0x00; hdr[9] = 0x00; hdr[10] = 0x01; hdr[11] = 0x00;
	put_le32(hdr + 12, 92);
	put_le32(hdr + 16, 0);
	put_le64(hdr + 24, 1);
	put_le64(hdr + 32, total - 1);
	put_le64(hdr + 40, 2 + entries_sectors);
	put_le64(hdr + 48, total - entries_sectors - 2);
	memcpy(hdr + 56, disk_guid, 16);
	put_le64(hdr + 72, 2);
	put_le32(hdr + 80, RL_GPT_ENTRIES);
	put_le32(hdr + 84, RL_GPT_ENTRY_SIZE);
	put_le32(hdr + 88, entries_crc);
	header_crc = crc32_calc(hdr, 92);
	put_le32(hdr + 16, header_crc);
	if (rl_io_write(io, hdr, ss, (uint64_t)1 * ss, err, errlen) != 0)
		goto out;
	if (rl_io_write(io, entries, RL_GPT_ENTRIES * RL_GPT_ENTRY_SIZE,
			(uint64_t)2 * ss, err, errlen) != 0)
		goto out;

	/* Backup entries + header. */
	{
		uint64_t backup_entries_lba = total - entries_sectors - 1;
		uint64_t backup_header_lba = total - 1;

		if (rl_io_write(io, entries, RL_GPT_ENTRIES * RL_GPT_ENTRY_SIZE,
				backup_entries_lba * ss, err, errlen) != 0)
			goto out;

		memset(hdr, 0, ss);
		memcpy(hdr, "EFI PART", 8);
		hdr[8] = 0x00; hdr[9] = 0x00; hdr[10] = 0x01; hdr[11] = 0x00;
		put_le32(hdr + 12, 92);
		put_le32(hdr + 16, 0);
		put_le64(hdr + 24, backup_header_lba);
		put_le64(hdr + 32, 1);
		put_le64(hdr + 40, 2 + entries_sectors);
		put_le64(hdr + 48, total - entries_sectors - 2);
		memcpy(hdr + 56, disk_guid, 16);
		put_le64(hdr + 72, backup_entries_lba);
		put_le32(hdr + 80, RL_GPT_ENTRIES);
		put_le32(hdr + 84, RL_GPT_ENTRY_SIZE);
		put_le32(hdr + 88, entries_crc);
		header_crc = crc32_calc(hdr, 92);
		put_le32(hdr + 16, header_crc);
		if (rl_io_write(io, hdr, ss, backup_header_lba * ss, err, errlen) != 0)
			goto out;
	}

	rc = 0;
out:
	free(entries);
	free(hdr);
	return rc;
}

static int write_mbr(rl_io *io, const rl_part_spec *spec, char *err,
		     size_t errlen)
{
	uint8_t mbr[512];
	uint8_t *e = mbr + 446;
	uint64_t total = spec->disk_size / spec->sector_size;
	uint8_t type;

	if (spec->sectors > 0xFFFFFFFFull) {
		if (err) snprintf(err, errlen,
				  "partition is larger than 2 TiB; use GPT");
		return -1;
	}
	if (spec->start_lba > 0xFFFFFFFFull) {
		if (err) snprintf(err, errlen, "partition starts beyond the MBR limit");
		return -1;
	}

	if (spec->type == RL_PART_FAT32)
		type = 0x0C;   /* FAT32 LBA */
	else
		type = 0x07;   /* NTFS / exFAT */

	memset(mbr, 0, sizeof(mbr));
	e[0] = 0x80;            /* active */
	e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
	e[4] = type;
	e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
	put_le32(e + 8, (uint32_t)spec->start_lba);
	put_le32(e + 12, (uint32_t)spec->sectors);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	(void)total;
	return rl_io_write(io, mbr, sizeof(mbr), 0, err, errlen);
}

int rl_part_write(rl_io *io, const rl_part_spec *spec, char *err, size_t errlen)
{
	uint64_t wipe = (spec->disk_size > 2u * RL_MIB) ? RL_MIB : spec->disk_size / 2;

	if (io == NULL || spec == NULL) {
		if (err) snprintf(err, errlen, "invalid argument");
		return -1;
	}
	if (spec->sector_size < 512 ||
	    spec->disk_size < spec->start_lba * spec->sector_size +
			       spec->sectors * spec->sector_size) {
		if (err) snprintf(err, errlen, "partition does not fit on the device");
		return -1;
	}

	/* Remove any stale MBR/GPT signatures at both ends. */
	if (rl_io_zero(io, 0, wipe, err, errlen) != 0)
		return -1;
	if (rl_io_zero(io, spec->disk_size - wipe, wipe, err, errlen) != 0)
		return -1;

	if (spec->kind == RL_PT_GPT)
		return write_gpt(io, spec, err, errlen);
	return write_mbr(io, spec, err, errlen);
}
