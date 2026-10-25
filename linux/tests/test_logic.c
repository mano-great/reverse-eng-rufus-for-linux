/*
 * rufus-linux - host tests for the portable parts (SHA-256 + ISO probing)
 *
 * These build and run on any OS, including Windows, so the image analysis
 * and checksum logic can be validated without a Linux box.
 *
 * build (Windows, from linux/):
 *   zig cc -target x86_64-windows-gnu -O2 -Wall -Wextra -std=c11 \
 *       -o test_logic.exe tests/test_logic.c src/rl_iso.c src/rl_sha256.c src/rl_util.c
 *
 * build (Linux, from linux/):
 *   cc -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -o test_logic \
 *       tests/test_logic.c src/rl_iso.c src/rl_sha256.c src/rl_util.c
 */

#include "../src/rl_iso.h"
#include "../src/rl_sha256.h"

#include <stdio.h>

static int g_failures;

static void check(int cond, const char *what)
{
	if (cond) {
		printf("  ok   %s\n", what);
	} else {
		printf("  FAIL %s\n", what);
		g_failures++;
	}
}

static FILE *make_image(const char *path, size_t size)
{
	FILE *f = fopen(path, "wb");
	uint8_t zero[4096];
	size_t left = size;

	if (f == NULL)
		return NULL;
	memset(zero, 0, sizeof(zero));
	while (left > 0) {
		size_t n = (left < sizeof(zero)) ? left : sizeof(zero);
		fwrite(zero, 1, n, f);
		left -= n;
	}
	return f;
}

static void put(FILE *f, uint64_t off, const void *buf, size_t len)
{
	RL_FSEEK(f, off);
	fwrite(buf, 1, len, f);
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

/* Minimal ISO9660 descriptor helper. */
static void fill_pvd(uint8_t *pvd, const char *label)
{
	memset(pvd, 0, RL_ISO_SECTOR);
	pvd[0] = 0x01;
	memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 0x01;
	memcpy(pvd + 8, "LINUX", 5);
	memcpy(pvd + 40, label, strlen(label));
	put_le32(pvd + 80, 32);   /* volume space size, in 2048-byte blocks */
}

static void fill_el_torito(uint8_t *rec, uint32_t catalog_lba)
{
	memset(rec, 0, RL_ISO_SECTOR);
	rec[0] = 0x00;
	memcpy(rec + 1, "CD001", 5);
	memcpy(rec + 7, "EL TORITO SPECIFICATION", 23);
	put_le32(rec + 71, catalog_lba);
}

static void fill_boot_catalog(uint8_t *cat, uint8_t platform)
{
	memset(cat, 0, RL_ISO_SECTOR);
	cat[0] = 0x01;             /* validation entry */
	cat[1] = platform;         /* 0x00 = x86, 0xEF = EFI */
	cat[30] = 0x55;            /* 0xAA55 signature */
	cat[31] = 0xAA;
	cat[32] = 0x88;            /* default boot entry, bootable */
}

static void test_sha256(void)
{
	static const char *abc = "abc";
	static const char *empty = "";
	uint8_t d[32];
	char hex[65];

	printf("SHA-256\n");

	rl_sha256(abc, 3, d);
	rl_sha256_hex(d, hex);
	check(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
	      "\"abc\" matches FIPS 180-4 vector");

	rl_sha256(empty, 0, d);
	rl_sha256_hex(d, hex);
	check(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
	      "empty string matches FIPS 180-4 vector");

	/* Streaming in odd-sized pieces must agree with the one-shot digest. */
	{
		rl_sha256_ctx ctx;
		const char *msg = "The quick brown fox jumps over the lazy dog";
		size_t i, len = strlen(msg);

		rl_sha256_init(&ctx);
		for (i = 0; i < len; i += 7) {
			size_t n = (len - i < 7) ? len - i : 7;
			rl_sha256_update(&ctx, msg + i, n);
		}
		rl_sha256_final(&ctx, d);
		rl_sha256_hex(d, hex);
		check(strcmp(hex, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592") == 0,
		      "chunked update matches known vector");
	}
}

static void test_hybrid_iso(void)
{
	rl_iso_info info;
	char err[256];
	FILE *f;
	uint8_t mbr[512], pvd[RL_ISO_SECTOR], rec[RL_ISO_SECTOR], cat[RL_ISO_SECTOR];

	printf("Isohybrid image with EFI El Torito\n");
	f = make_image("test_hybrid.iso", 64 * 1024);
	if (f == NULL) {
		check(0, "create test_hybrid.iso");
		return;
	}

	memset(mbr, 0, sizeof(mbr));
	mbr[446 + 0] = 0x80;             /* bootable */
	mbr[446 + 4] = 0x0C;             /* FAT32 LBA */
	put_le32(mbr + 446 + 8, 0);      /* start LBA */
	put_le32(mbr + 446 + 12, 32);    /* sectors */
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	put(f, 0, mbr, sizeof(mbr));

	fill_pvd(pvd, "TESTISO");
	put(f, RL_ISO_PVD_LBA * RL_ISO_SECTOR, pvd, sizeof(pvd));

	fill_el_torito(rec, 20);
	put(f, RL_ISO_BOOT_LBA * RL_ISO_SECTOR, rec, sizeof(rec));

	fill_boot_catalog(cat, 0xEF);
	put(f, 20 * RL_ISO_SECTOR, cat, sizeof(cat));
	fclose(f);

	check(rl_iso_probe("test_hybrid.iso", &info, err, sizeof(err)) == 0, "probe succeeds");
	check(info.has_mbr == 1, "MBR signature detected");
	check(info.is_hybrid == 1, "classified isohybrid");
	check(info.fs == RL_FS_ISO9660, "ISO9660 detected");
	check(strcmp(info.label, "TESTISO") == 0, "volume label parsed");
	check(info.has_el_torito == 1, "El Torito detected");
	check(info.efi_bootable == 1, "EFI boot image detected");
	check(info.size_bytes == 64 * 1024, "size reported");
	remove("test_hybrid.iso");
}

static void test_syslinux_isohybrid(void)
{
	rl_iso_info info;
	char err[256];
	FILE *f;
	uint8_t mbr[512], pvd[RL_ISO_SECTOR];

	printf("syslinux-style isohybrid (type 0 + boot flag)\n");
	f = make_image("test_isohybrid.iso", 64 * 1024);
	if (f == NULL) {
		check(0, "create test_isohybrid.iso");
		return;
	}

	memset(mbr, 0, sizeof(mbr));
	mbr[446 + 0] = 0x80;             /* bootable */
	mbr[446 + 4] = 0x00;             /* no type byte, as isohybrid does */
	put_le32(mbr + 446 + 8, 0);
	put_le32(mbr + 446 + 12, 32);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	put(f, 0, mbr, sizeof(mbr));

	fill_pvd(pvd, "ISOBOOT");
	put(f, RL_ISO_PVD_LBA * RL_ISO_SECTOR, pvd, sizeof(pvd));
	fclose(f);

	check(rl_iso_probe("test_isohybrid.iso", &info, err, sizeof(err)) == 0, "probe succeeds");
	check(info.is_hybrid == 1, "type 0x00 entry still counts as isohybrid");
	check(info.has_gpt == 0, "not mistaken for GPT");
	remove("test_isohybrid.iso");
}

static void test_plain_iso(void)
{
	rl_iso_info info;
	char err[256];
	FILE *f;
	uint8_t pvd[RL_ISO_SECTOR];

	printf("Plain (non-isohybrid) image\n");
	f = make_image("test_plain.iso", 32 * 1024);
	if (f == NULL) {
		check(0, "create test_plain.iso");
		return;
	}
	fill_pvd(pvd, "PLAIN");
	put(f, RL_ISO_PVD_LBA * RL_ISO_SECTOR, pvd, sizeof(pvd));
	fclose(f);

	check(rl_iso_probe("test_plain.iso", &info, err, sizeof(err)) == 0, "probe succeeds");
	check(info.has_mbr == 0, "no MBR signature");
	check(info.is_hybrid == 0, "classified as non-isohybrid");
	check(info.has_el_torito == 0, "no El Torito");
	check(strcmp(info.label, "PLAIN") == 0, "volume label parsed");
	check(strstr(info.advice, "not boot") != NULL || strstr(info.advice, "will not boot") != NULL,
	      "advises against raw copy");
	remove("test_plain.iso");
}

static void test_gpt_iso(void)
{
	rl_iso_info info;
	char err[256];
	FILE *f;
	uint8_t mbr[512], hdr[512], ent[128], pvd[RL_ISO_SECTOR];

	printf("Protective MBR + GPT image\n");
	f = make_image("test_gpt.iso", 64 * 1024);
	if (f == NULL) {
		check(0, "create test_gpt.iso");
		return;
	}

	memset(mbr, 0, sizeof(mbr));
	mbr[446 + 4] = 0xEE;             /* protective */
	put_le32(mbr + 446 + 8, 1);
	put_le32(mbr + 446 + 12, 32);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	put(f, 0, mbr, sizeof(mbr));

	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, "EFI PART", 8);
	put_le64(hdr + 72, 2);           /* partition entry LBA */
	put_le32(hdr + 80, 4);           /* entry count */
	put_le32(hdr + 84, 128);         /* entry size */
	put(f, 512, hdr, sizeof(hdr));

	memset(ent, 0, sizeof(ent));
	memcpy(ent, "\x0f\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff", 16);
	put(f, 1024, ent, sizeof(ent));

	fill_pvd(pvd, "GPTISO");
	put(f, RL_ISO_PVD_LBA * RL_ISO_SECTOR, pvd, sizeof(pvd));
	fclose(f);

	check(rl_iso_probe("test_gpt.iso", &info, err, sizeof(err)) == 0, "probe succeeds");
	check(info.has_gpt == 1, "GPT header + entries detected");
	check(info.is_hybrid == 1, "GPT image classified isohybrid");
	remove("test_gpt.iso");
}

static void test_udf(void)
{
	rl_iso_info info;
	char err[256];
	FILE *f;
	uint8_t vsd[RL_ISO_SECTOR];

	printf("UDF volume recognition sequence\n");
	f = make_image("test_udf.img", 40 * 1024);
	if (f == NULL) {
		check(0, "create test_udf.img");
		return;
	}
	memset(vsd, 0, sizeof(vsd));
	vsd[0] = 0x00;
	memcpy(vsd + 1, "BEA01", 5);
	put(f, 16 * RL_ISO_SECTOR, vsd, sizeof(vsd));
	fclose(f);

	check(rl_iso_probe("test_udf.img", &info, err, sizeof(err)) == 0, "probe succeeds");
	check(info.fs == RL_FS_UDF, "UDF detected");
	remove("test_udf.img");
}

static void test_missing_file(void)
{
	rl_iso_info info;
	char err[256] = "";

	printf("Error handling\n");
	check(rl_iso_probe("does-not-exist.iso", &info, err, sizeof(err)) == -1,
	      "missing file returns -1");
	check(err[0] != '\0', "error message is set");
}

int main(void)
{
	printf("rufus-linux backend tests\n\n");
	test_sha256();
	test_hybrid_iso();
	test_syslinux_isohybrid();
	test_plain_iso();
	test_gpt_iso();
	test_udf();
	test_missing_file();

	printf("\n%s (%d failure%s)\n",
	       g_failures == 0 ? "PASS" : "FAIL",
	       g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
