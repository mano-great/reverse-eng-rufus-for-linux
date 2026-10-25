/*
 * rufus-linux - ISO9660/Joliet/Rock Ridge walker test
 * Usage: test_isofs <image.iso>
 * GPL-3.0-or-later
 */

#include "../src/rl_isofs.h"
#include "../src/rl_sha256.h"

static int failures;
static int checks;
static int files;
static int dirs;
static int max_depth;
static int found_vmlinuz, found_initrd, found_isolinux, found_efi, found_ldlinux;
static uint64_t vmlinuz_size, initrd_size;
static int initrd_lba, initrd_bytes;

#define CHECK(cond, ...) do { \
	checks++; \
	if (cond) printf("  ok   "); else { printf("  FAIL "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); \
} while (0)

static int visit(void *user, const rl_iso_entry *e, char *err, size_t errlen)
{
	int depth = 0;
	const char *p;

	(void)user; (void)err; (void)errlen;
	for (p = e->path; *p; p++)
		if (*p == '/')
			depth++;
	if (depth > max_depth)
		max_depth = depth;
	if (e->is_dir)
		dirs++;
	else
		files++;

	if (strcasecmp(e->path, "boot/vmlinuz-virt") == 0) {
		found_vmlinuz = 1;
		vmlinuz_size = e->size;
	}
	if (strcasecmp(e->path, "boot/initramfs-virt") == 0) {
		found_initrd = 1;
		initrd_size = e->size;
		if (e->n_extents > 0) {
			initrd_lba = (int)e->ext[0].lba;
			initrd_bytes = (int)e->ext[0].bytes;
		}
	}
	if (strcasecmp(e->path, "boot/syslinux/isolinux.bin") == 0)
		found_isolinux = 1;
	if (strcasecmp(e->path, "efi/boot/bootx64.efi") == 0)
		found_efi = 1;
	if (strcasecmp(e->path, "boot/syslinux/ldlinux.c32") == 0)
		found_ldlinux = 1;
	return 0;
}

int main(int argc, char **argv)
{
	rl_iso_vol *vol = NULL;
	const rl_iso_info *info;
	char err[512];
	int rc;

	if (argc < 2) {
		printf("usage: %s image.iso\n", argv[0]);
		return 2;
	}
	printf("=== ISO filesystem walker ===\n");
	rc = rl_iso_vol_open(&vol, argv[1], err, sizeof(err));
	CHECK(rc == 0, "open volume (%s)", err);
	if (rc != 0)
		return 1;

	info = rl_iso_vol_info(vol);
	printf("  label: %s, fs: %s, hybrid: %d\n", info->label,
	       rl_iso_fs_name(info->fs), info->is_hybrid);

	rc = rl_iso_vol_walk(vol, visit, NULL, err, sizeof(err));
	CHECK(rc == 0, "walk completes (%s)", err);
	CHECK(files > 20, "found %d files", files);
	CHECK(dirs > 5, "found %d directories", dirs);
	CHECK(found_vmlinuz, "found boot/vmlinuz-virt");
	CHECK(found_initrd, "found boot/initramfs-virt");
	CHECK(found_isolinux, "found boot/syslinux/isolinux.bin");
	CHECK(found_efi, "found efi/boot/bootx64.efi");
	CHECK(found_ldlinux, "found boot/syslinux/ldlinux.c32");
	CHECK(vmlinuz_size == 12641280, "vmlinuz size is 12641280 (%llu)",
	      (unsigned long long)vmlinuz_size);
	CHECK(initrd_size == 9843765, "initramfs size is 9843765 (%llu)",
	      (unsigned long long)initrd_size);

	if (initrd_bytes > 0) {
		rl_sha256_ctx ctx;
		uint8_t digest[32];
		char hex[65];
		uint8_t buf[65536];
		int left = initrd_bytes;
		uint64_t off = (uint64_t)initrd_lba * 2048;

		rl_sha256_init(&ctx);
		while (left > 0) {
			size_t n = left > (int)sizeof(buf) ? sizeof(buf) : (size_t)left;
			if (rl_iso_vol_pread(vol, off, buf, n, err, sizeof(err)) != 0) {
				CHECK(0, "read initramfs extent (%s)", err);
				break;
			}
			rl_sha256_update(&ctx, buf, n);
			off += n;
			left -= (int)n;
		}
		rl_sha256_final(&ctx, digest);
		rl_sha256_hex(digest, hex);
		printf("  initramfs sha256: %s\n", hex);
	}

	rl_iso_vol_close(vol);
	printf("\n%s (%d checks, %d failures)\n",
	       failures == 0 ? "PASS" : "FAIL", checks, failures);
	return failures == 0 ? 0 : 1;
}
