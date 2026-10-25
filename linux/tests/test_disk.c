/*
 * rufus-linux - partition table and FAT32 formatter tests
 *
 * Runs on Windows and Linux against a plain image file, so the destructive
 * code paths can be validated without a real disk.
 *
 * GPL-3.0-or-later
 */

#include "../src/rl_io.h"
#include "../src/rl_part.h"
#include "../src/rl_fat32.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (cond) { \
		printf("  ok   "); \
	} else { \
		printf("  FAIL "); \
		failures++; \
	} \
	printf(__VA_ARGS__); \
	printf("\n"); \
} while (0)

static int make_file(const char *path, uint64_t size)
{
	FILE *f = fopen(path, "wb");
	if (f == NULL)
		return -1;
#if defined(_WIN32)
	if (_fseeki64(f, (__int64)(size - 1), SEEK_SET) != 0) {
#else
	if (fseeko(f, (off_t)(size - 1), SEEK_SET) != 0) {
#endif
		fclose(f);
		return -1;
	}
	if (fputc(0, f) == EOF) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
	return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static uint32_t crc32_calc(const uint8_t *p, size_t len)
{
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

static void test_layout(void)
{
	uint64_t start = 0, sectors = 0;

	printf("partition layout\n");
	CHECK(rl_part_layout(160ull * 1024 * 1024, 512, RL_PT_MBR, &start, &sectors) == 0,
	      "MBR layout succeeds");
	CHECK(start == 2048, "MBR partition starts at 1 MiB (%llu)", (unsigned long long)start);
	CHECK(sectors == (160ull * 1024 * 1024 / 512) - 2048,
	      "MBR partition fills the disk (%llu sectors)", (unsigned long long)sectors);

	start = sectors = 0;
	CHECK(rl_part_layout(160ull * 1024 * 1024, 512, RL_PT_GPT, &start, &sectors) == 0,
	      "GPT layout succeeds");
	CHECK(start == 2048, "GPT partition starts at 1 MiB");
	CHECK(start + sectors - 1 <= (160ull * 1024 * 1024 / 512) - 34,
	      "GPT partition leaves room for the backup table");

	CHECK(rl_part_layout(4ull * 1024 * 1024, 512, RL_PT_MBR, &start, &sectors) != 0,
	      "tiny disk is rejected");
}

static void test_mbr(const char *path)
{
	rl_io io;
	rl_part_spec spec;
	uint8_t mbr[512];
	char err[256];
	uint64_t start, sectors;

	printf("MBR writer\n");
	if (make_file(path, 160ull * 1024 * 1024) != 0) {
		printf("  FAIL cannot create image\n");
		failures++;
		return;
	}
	if (rl_io_open(&io, path, 1, err, sizeof(err)) != 0) {
		printf("  FAIL open: %s\n", err);
		failures++;
		return;
	}
	rl_part_layout(io.size, io.sector_size, RL_PT_MBR, &start, &sectors);
	memset(&spec, 0, sizeof(spec));
	spec.kind = RL_PT_MBR;
	spec.type = RL_PART_FAT32;
	spec.disk_size = io.size;
	spec.sector_size = io.sector_size;
	spec.start_lba = start;
	spec.sectors = sectors;
	spec.label = "TEST";
	CHECK(rl_part_write(&io, &spec, err, sizeof(err)) == 0, "write MBR (%s)", err);
	CHECK(rl_io_read(&io, mbr, sizeof(mbr), 0, err, sizeof(err)) == 0, "read MBR back");
	CHECK(mbr[510] == 0x55 && mbr[511] == 0xAA, "MBR signature");
	CHECK(mbr[446] == 0x80, "partition is active");
	CHECK(mbr[446 + 4] == 0x0C, "partition type is FAT32 LBA");
	CHECK(rd32(mbr + 446 + 8) == 2048, "partition start LBA");
	CHECK(rd32(mbr + 446 + 12) == sectors, "partition sector count");
	rl_io_close(&io);
	remove(path);
}

static void test_gpt(const char *path)
{
	rl_io io;
	rl_part_spec spec;
	uint8_t hdr[512], ent[128], mbr[512], bak[512];
	char err[256];
	uint64_t start, sectors, total;
	uint32_t crc, saved;

	printf("GPT writer\n");
	if (make_file(path, 160ull * 1024 * 1024) != 0) {
		printf("  FAIL cannot create image\n");
		failures++;
		return;
	}
	if (rl_io_open(&io, path, 1, err, sizeof(err)) != 0) {
		printf("  FAIL open: %s\n", err);
		failures++;
		return;
	}
	rl_part_layout(io.size, io.sector_size, RL_PT_GPT, &start, &sectors);
	memset(&spec, 0, sizeof(spec));
	spec.kind = RL_PT_GPT;
	spec.type = RL_PART_FAT32;
	spec.disk_size = io.size;
	spec.sector_size = io.sector_size;
	spec.start_lba = start;
	spec.sectors = sectors;
	spec.label = "EFI TEST";
	CHECK(rl_part_write(&io, &spec, err, sizeof(err)) == 0, "write GPT (%s)", err);

	CHECK(rl_io_read(&io, mbr, sizeof(mbr), 0, err, sizeof(err)) == 0, "read PMBR");
	CHECK(mbr[446 + 4] == 0xEE, "protective MBR type 0xEE");

	CHECK(rl_io_read(&io, hdr, sizeof(hdr), 512, err, sizeof(err)) == 0, "read GPT header");
	CHECK(memcmp(hdr, "EFI PART", 8) == 0, "GPT signature");
	saved = rd32(hdr + 16);
	hdr[16] = hdr[17] = hdr[18] = hdr[19] = 0;
	crc = crc32_calc(hdr, 92);
	CHECK(crc == saved, "GPT header CRC");
	total = io.size / io.sector_size;
	CHECK(rd64(hdr + 24) == 1, "primary header LBA");
	CHECK(rd64(hdr + 32) == total - 1, "backup header LBA");
	CHECK(rd32(hdr + 80) == 128, "128 partition entries");
	CHECK(rd32(hdr + 84) == 128, "128-byte entries");

	CHECK(rl_io_read(&io, ent, sizeof(ent), 2 * 512, err, sizeof(err)) == 0, "read entry 0");
	CHECK(memcmp(ent, "\x28\x73\x2a\xc1", 4) == 0, "entry is an EFI System Partition");
	CHECK(rd64(ent + 32) == start, "entry start LBA");
	CHECK(rd64(ent + 40) == start + sectors - 1, "entry end LBA");
	CHECK(ent[56] == 'E' && ent[57] == 0 && ent[58] == 'F', "entry name is UTF-16LE");

	CHECK(rl_io_read(&io, bak, sizeof(bak), (total - 1) * 512, err, sizeof(err)) == 0,
	      "read backup header");
	CHECK(memcmp(bak, "EFI PART", 8) == 0, "backup header signature");
	CHECK(rd64(bak + 24) == total - 1, "backup header points to itself");
	CHECK(rd64(bak + 32) == 1, "backup header points to primary");
	rl_io_close(&io);
	remove(path);
}

static void test_fat32(const char *path)
{
	rl_io io;
	rl_fat32_geom g;
	uint8_t boot[512], fat[512], label[32];
	char err[256];
	uint64_t part_off = 1024 * 1024;
	uint64_t part_size = 128ull * 1024 * 1024;
	uint64_t total;

	printf("FAT32 formatter\n");
	if (make_file(path, part_off + part_size) != 0) {
		printf("  FAIL cannot create image\n");
		failures++;
		return;
	}
	if (rl_io_open(&io, path, 1, err, sizeof(err)) != 0) {
		printf("  FAIL open: %s\n", err);
		failures++;
		return;
	}
	CHECK(rl_fat32_format(&io, part_off, part_size, 0, "RUFS TEST",
			      &g, err, sizeof(err)) == 0, "format 128 MiB (%s)", err);
	CHECK(g.cluster_size == 1024,
	      "auto cluster size steps down to 1 KiB at this boundary (%u)",
	      g.cluster_size);
	CHECK(g.reserved_sectors >= 32, "reserved sectors >= 32 (%u)", g.reserved_sectors);
	CHECK(g.data_start_sector % (1024 * 1024 / 512) == 0,
	      "data area is 1 MiB aligned (%llu)",
	      (unsigned long long)g.data_start_sector);

	CHECK(rl_io_read(&io, boot, sizeof(boot), part_off, err, sizeof(err)) == 0,
	      "read boot sector");
	{
		rl_fat32_geom p;
		CHECK(rl_fat32_parse_boot(boot, sizeof(boot), &p, err, sizeof(err)) == 0,
		      "boot sector parses (%s)", err);
		CHECK(p.cluster_size == g.cluster_size, "parsed cluster size matches");
		CHECK(p.total_sectors == g.total_sectors, "parsed total sectors matches");
		CHECK(memcmp(p.label, "RUFS TEST  ", 11) == 0, "label is in the BPB");
	}
	CHECK(boot[510] == 0x55 && boot[511] == 0xAA, "boot signature");

	CHECK(rl_io_read(&io, fat, sizeof(fat), part_off + (uint64_t)g.reserved_sectors * 512,
			 err, sizeof(err)) == 0, "read FAT");
	CHECK(rd32(fat + 0) == 0x0FFFFFF8, "FAT[0] media entry");
	CHECK(rd32(fat + 4) == 0x0FFFFFFF, "FAT[1] EOC");
	CHECK(rd32(fat + 8) == 0x0FFFFFFF, "FAT[2] root EOC");

	CHECK(rl_io_read(&io, label, sizeof(label),
			 part_off + g.data_start_sector * 512,
			 err, sizeof(err)) == 0, "read root directory");
	CHECK(memcmp(label, "RUFS TEST  ", 11) == 0, "volume label entry");
	CHECK(label[11] == 0x08, "volume label attribute");

	CHECK(rl_io_read(&io, boot, sizeof(boot),
			 part_off + 6 * 512, err, sizeof(err)) == 0, "read backup boot sector");
	CHECK(memcmp(boot + 36, "\x00\x00\x00\x00", 4) != 0, "backup boot sector has a FAT size");

	/* Reject a volume that is too small. */
	total = 32ull * 1024 * 1024;
	CHECK(rl_fat32_format(&io, part_off, total, 0, "SMALL", &g, err, sizeof(err)) != 0,
	      "32 MiB volume is rejected");
	CHECK(strstr(err, "too small") != NULL, "rejection explains the minimum size");

	rl_io_close(&io);
	remove(path);
}

static void test_fat32_sizes(void)
{
	printf("FAT32 geometry\n");
	CHECK(rl_fat32_default_cluster_size(64ull * 1024 * 1024) == 1024,
	      "64 MiB -> 1 KiB clusters");
	CHECK(rl_fat32_default_cluster_size(4ull * 1024 * 1024 * 1024) == 4096,
	      "4 GiB -> 4 KiB clusters");
	CHECK(rl_fat32_default_cluster_size(64ull * 1024 * 1024 * 1024) == 32768,
	      "64 GiB -> 32 KiB clusters");
}

int main(void)
{
	printf("=== rufus-linux disk tests ===\n");
	test_layout();
	test_mbr("test_mbr.img");
	test_gpt("test_gpt.img");
	test_fat32("test_fat32.img");
	test_fat32_sizes();
	printf("\n%s (%d checks, %d failures)\n",
	       failures == 0 ? "PASS" : "FAIL", checks, failures);
	return failures == 0 ? 0 : 1;
}
