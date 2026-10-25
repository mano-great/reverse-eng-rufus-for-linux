/*
 * rufus-linux - bootable image analysis
 * GPL-3.0-or-later
 */

#include "rl_iso.h"

#define RL_ERR(...) do { if (err != NULL) snprintf(err, errlen, __VA_ARGS__); } while (0)

/* Read exactly len bytes at byte offset off. */
static int read_at(FILE *f, uint64_t off, void *buf, size_t len)
{
	if (RL_FSEEK(f, off) != 0)
		return -1;
	if (fread(buf, 1, len, f) != len)
		return -1;
	return 0;
}

static int file_size(FILE *f, uint64_t *out)
{
	int64_t sz;

	if (RL_FSEEK(f, 0) != 0)
		return -1;
	/* Seek to end through the portable macro by using a large sentinel. */
#if defined(_WIN32)
	if (_fseeki64(f, 0, SEEK_END) != 0)
		return -1;
	sz = _ftelli64(f);
#else
	if (fseeko(f, 0, SEEK_END) != 0)
		return -1;
	sz = (int64_t)ftello(f);
#endif
	if (sz < 0)
		return -1;
	*out = (uint64_t)sz;
	return 0;
}

static void copy_field(char *dst, size_t dstlen, const uint8_t *src, size_t srclen)
{
	size_t n = srclen;

	while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\0'))
		n--;
	if (n >= dstlen)
		n = dstlen - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

/*
 * MBR: 4 primary entries at 446, 16 bytes each.
 * Returns 1 if the 0x55AA boot signature is present, 0 otherwise.
 * *used is set when at least one entry describes a real partition, which is
 * what makes an ISO isohybrid (raw-copy bootable).
 */
static int probe_mbr(const uint8_t *mbr, int *has_gpt, int *used)
{
	int i;

	*has_gpt = 0;
	*used = 0;
	if (mbr[510] != 0x55 || mbr[511] != 0xAA)
		return 0;

	for (i = 0; i < 4; i++) {
		const uint8_t *e = mbr + 446 + i * 16;
		uint8_t status = e[0];
		uint8_t type = e[4];
		uint32_t lba = rl_le32(e + 8);
		uint32_t sectors = rl_le32(e + 12);

		/* syslinux isohybrid commonly writes type 0x00 with the boot
		 * flag set, so an entry can be meaningful without a type byte. */
		if (type == 0 && lba == 0 && sectors == 0 && status == 0)
			continue;
		if (type == 0xEE)
			*has_gpt = 1;
		if (sectors > 0 && lba < 0xFFFFFFFEu)
			*used = 1;
	}
	return 1;
}

/* LBA1 GPT header: signature, partition entry LBA, count, entry size. */
static int probe_gpt(FILE *f, int *has_gpt)
{
	uint8_t hdr[512];
	uint64_t entry_lba;
	uint32_t num, esize;
	uint64_t i;

	if (read_at(f, 512, hdr, sizeof(hdr)) != 0)
		return 0;
	if (memcmp(hdr, "EFI PART", 8) != 0)
		return 0;

	entry_lba = rl_le64(hdr + 72);
	num = rl_le32(hdr + 80);
	esize = rl_le32(hdr + 84);
	if (entry_lba == 0 || num == 0 || esize < 128 || esize > 4096 || num > 1024)
		return 0;

	for (i = 0; i < num && i < 8; i++) {
		uint8_t ent[128];
		uint64_t off = entry_lba * 512 + i * esize;
		int j, nonzero = 0;

		if (read_at(f, off, ent, sizeof(ent)) != 0)
			return 0;
		for (j = 0; j < 16; j++) {
			if (ent[j] != 0) {
				nonzero = 1;
				break;
			}
		}
		if (nonzero) {
			*has_gpt = 1;
			return 1;
		}
	}
	return 0;
}

/*
 * El Torito: boot record at LBA 17 points at a boot catalog in LBA 71..74.
 * The catalog's validation entry names the platform; section headers can
 * override it per boot entry (0xEF == EFI).
 */
static void probe_el_torito(FILE *f, rl_iso_info *info, const uint8_t *rec)
{
	uint8_t cat[RL_ISO_SECTOR];
	uint32_t lba;
	int i;

	lba = rl_le32(rec + 71);
	if (lba == 0 || lba > 0x00100000u)
		return;
	if (read_at(f, (uint64_t)lba * RL_ISO_SECTOR, cat, sizeof(cat)) != 0)
		return;
	if (cat[0] != 0x01 || cat[30] != 0x55 || cat[31] != 0xAA)
		return;

	info->has_el_torito = 1;
	info->boot_catalog_lba = lba;
	if (cat[1] == 0xEF)
		info->efi_bootable = 1;

	for (i = 0; i < 32; i++) {
		const uint8_t *e = cat + 32 + i * 32;
		if (e[0] == 0x00)
			break;
		if ((e[0] == 0x90 || e[0] == 0x91) && e[1] == 0xEF)
			info->efi_bootable = 1;
	}
}

int rl_iso_probe(const char *path, rl_iso_info *info, char *err, size_t errlen)
{
	FILE *f;
	uint8_t mbr[512];
	uint8_t sec[RL_ISO_SECTOR];
	uint8_t boot[RL_ISO_SECTOR];
	int has_gpt_from_mbr = 0;
	int mbr_used = 0;
	int i;

	if (path == NULL || info == NULL) {
		RL_ERR("invalid arguments");
		return -1;
	}
	memset(info, 0, sizeof(*info));
	snprintf(info->path, sizeof(info->path), "%s", path);
	info->fs = RL_FS_UNKNOWN;

	f = fopen(path, "rb");
	if (f == NULL) {
		RL_ERR("cannot open '%s': %s", path, strerror(errno));
		return -1;
	}
	if (file_size(f, &info->size_bytes) != 0) {
		RL_ERR("cannot read size of '%s': %s", path, strerror(errno));
		fclose(f);
		return -1;
	}

	/* System area / isohybrid MBR. */
	if (read_at(f, 0, mbr, sizeof(mbr)) != 0) {
		RL_ERR("cannot read '%s': %s", path, strerror(errno));
		fclose(f);
		return -1;
	}
	info->has_mbr = probe_mbr(mbr, &has_gpt_from_mbr, &mbr_used);
	if (info->has_mbr && has_gpt_from_mbr)
		info->has_gpt = probe_gpt(f, &info->has_gpt);
	/* A protective MBR alone is not enough: the GPT itself must validate. */
	info->is_hybrid = info->has_mbr &&
	                  (has_gpt_from_mbr ? info->has_gpt : mbr_used);

	/* ISO9660 primary volume descriptor. */
	if (read_at(f, (uint64_t)RL_ISO_PVD_LBA * RL_ISO_SECTOR, sec, sizeof(sec)) == 0 &&
	    sec[0] == 0x01 && memcmp(sec + 1, "CD001", 5) == 0) {
		info->fs = RL_FS_ISO9660;
		copy_field(info->label, sizeof(info->label), sec + 40, 32);
		copy_field(info->system_id, sizeof(info->system_id), sec + 8, 32);

		if (read_at(f, (uint64_t)RL_ISO_BOOT_LBA * RL_ISO_SECTOR, boot, sizeof(boot)) == 0 &&
		    boot[0] == 0x00 && memcmp(boot + 1, "CD001", 5) == 0 &&
		    memcmp(boot + 7, "EL TORITO SPECIFICATION", 23) == 0)
			probe_el_torito(f, info, boot);
	}

	/* UDF: volume recognition sequence starts at sector 16. */
	for (i = 16; i <= 31; i++) {
		uint8_t vsd[RL_ISO_SECTOR];

		if (read_at(f, (uint64_t)i * RL_ISO_SECTOR, vsd, sizeof(vsd)) != 0)
			break;
		if (memcmp(vsd + 1, "BEA01", 5) == 0 || memcmp(vsd + 1, "NSR02", 5) == 0 ||
		    memcmp(vsd + 1, "NSR03", 5) == 0) {
			if (info->fs == RL_FS_ISO9660)
				info->fs = RL_FS_ISO9660_UDF;
			else if (info->fs == RL_FS_UNKNOWN)
				info->fs = RL_FS_UDF;
			break;
		}
	}
	fclose(f);

	/* Human-readable verdict, mirroring Rufus' DD vs ISO image mode. */
	if (!info->is_hybrid) {
		snprintf(info->advice, sizeof(info->advice),
		         "Not isohybrid: a raw copy will not boot. This image needs "
		         "Rufus' 'ISO image mode' (extract + bootloader install), "
		         "which is not ported yet.");
	} else if (info->efi_bootable) {
		snprintf(info->advice, sizeof(info->advice),
		         "Isohybrid with an El Torito EFI image: raw copy boots UEFI%s.",
		         info->has_gpt ? " (GPT)" : "");
	} else {
		snprintf(info->advice, sizeof(info->advice),
		         "Isohybrid: raw copy is bootable.");
	}

	return 0;
}

const char *rl_iso_fs_name(rl_fs_kind fs)
{
	switch (fs) {
	case RL_FS_ISO9660:     return "ISO9660";
	case RL_FS_UDF:         return "UDF";
	case RL_FS_ISO9660_UDF: return "ISO9660+UDF";
	default:                return "unknown";
	}
}
