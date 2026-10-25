/*
 * rufus-linux - dump the analysis of a real image file.
 * Portable: builds and runs on Windows or Linux.
 *
 *   cc -o iso_dump tests/iso_dump.c src/rl_iso.c src/rl_util.c
 *   ./iso_dump /path/to/image.iso
 */

#include "../src/rl_iso.h"

int main(int argc, char **argv)
{
	rl_iso_info info;
	char err[256];
	char size[32];

	if (argc < 2) {
		fprintf(stderr, "usage: %s <image>\n", argv[0]);
		return 1;
	}
	if (rl_iso_probe(argv[1], &info, err, sizeof(err)) != 0) {
		fprintf(stderr, "error: %s\n", err);
		return 1;
	}
	rl_format_bytes(info.size_bytes, size, sizeof(size));
	printf("file       : %s\n", info.path);
	printf("size       : %s (%llu bytes)\n", size, (unsigned long long)info.size_bytes);
	printf("fs         : %s\n", rl_iso_fs_name(info.fs));
	printf("label      : %s\n", info.label[0] ? info.label : "-");
	printf("system_id  : %s\n", info.system_id[0] ? info.system_id : "-");
	printf("mbr        : %d\n", info.has_mbr);
	printf("gpt        : %d\n", info.has_gpt);
	printf("isohybrid  : %d\n", info.is_hybrid);
	printf("el_torito  : %d\n", info.has_el_torito);
	printf("efi_boot   : %d\n", info.efi_bootable);
	printf("boot_cat   : %u\n", info.boot_catalog_lba);
	printf("advice     : %s\n", info.advice);
	return 0;
}
