/*
 * rufus-linux - command line frontend
 *
 * This is the interim UI. It exercises the same backend the GTK frontend
 * will use (rl_device / rl_iso / rl_write), so the porting work is shared.
 *
 * GPL-3.0-or-later
 */

#include "rl_common.h"
#include "rl_device.h"
#include "rl_iso.h"
#include "rl_write.h"

#include <getopt.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static void usage(FILE *out)
{
	fprintf(out,
	        "rufus-linux %s - native Linux port of Rufus\n"
	        "\n"
	        "Usage:\n"
	        "  rufus-linux list\n"
	        "  rufus-linux info <image>\n"
	        "  rufus-linux write <image> <device> [options]\n"
	        "\n"
	        "Options for 'write':\n"
	        "  -f, --force      unmount volumes and write even if in use\n"
	        "  -y, --yes        skip the confirmation prompt\n"
	        "  -v, --verify     re-read the device and compare SHA-256\n"
	        "  -r, --raw        write non-isohybrid images anyway (DD mode)\n"
	        "  -n, --dry-run    analyse and show the plan, write nothing\n"
	        "  -h, --help       show this help\n"
	        "\n"
	        "Examples:\n"
	        "  sudo rufus-linux list\n"
	        "  sudo rufus-linux info fedora.iso\n"
	        "  sudo rufus-linux write fedora.iso /dev/sdb --verify\n",
	        RL_VERSION);
}

static void print_devices(const rl_device_list *list)
{
	size_t i, j;

	if (list->count == 0) {
		printf("No usable block devices found.\n");
		return;
	}

	printf("%-12s %-11s %-8s %-32s %s\n",
	       "DEVICE", "SIZE", "BUS", "MODEL", "NOTES");
	printf("%-12s %-11s %-8s %-32s %s\n",
	       "------------", "-----------", "--------",
	       "--------------------------------", "-----");

	for (i = 0; i < list->count; i++) {
		const rl_device *d = &list->items[i];
		char size[32];
		char notes[160] = "";
		size_t n = 0;

		rl_format_bytes(d->size_bytes, size, sizeof(size));
		if (d->is_system)
			n += (size_t)snprintf(notes + n, sizeof(notes) - n, "SYSTEM DISK ");
		if (d->mounted)
			n += (size_t)snprintf(notes + n, sizeof(notes) - n, "mounted ");
		if (d->read_only)
			n += (size_t)snprintf(notes + n, sizeof(notes) - n, "read-only ");
		if (d->removable)
			n += (size_t)snprintf(notes + n, sizeof(notes) - n, "removable ");
		if (n == 0)
			snprintf(notes, sizeof(notes), "-");

		printf("%-12s %-11s %-8s %-32s %s\n",
		       d->path, size, d->transport, d->model[0] ? d->model : "-", notes);

		for (j = 0; j < (size_t)d->npartitions; j++) {
			const rl_partition *p = &d->part[j];
			char psize[32];

			rl_format_bytes(p->size_bytes, psize, sizeof(psize));
			printf("  %-10s %-11s %-8s %-32s %s\n",
			       p->name, psize, "-", "-",
			       p->is_swap ? "[swap]" :
			       (p->mountpoint[0] ? p->mountpoint : ""));
		}
	}
}

static int cmd_list(void)
{
	rl_device_list list;
	int rc;

	rc = rl_devices_scan(&list);
	if (rc != 0) {
		fprintf(stderr, "rufus-linux: cannot enumerate block devices\n");
		return 1;
	}
	print_devices(&list);
	rl_devices_free(&list);
	return 0;
}

static int cmd_info(const char *image)
{
	rl_iso_info info;
	char err[256];
	char size[32];

	if (rl_iso_probe(image, &info, err, sizeof(err)) != 0) {
		fprintf(stderr, "rufus-linux: %s\n", err);
		return 1;
	}
	rl_format_bytes(info.size_bytes, size, sizeof(size));

	printf("Image      : %s\n", info.path);
	printf("Size       : %s (%llu bytes)\n", size, (unsigned long long)info.size_bytes);
	printf("File system: %s\n", rl_iso_fs_name(info.fs));
	printf("Label      : %s\n", info.label[0] ? info.label : "-");
	printf("System ID  : %s\n", info.system_id[0] ? info.system_id : "-");
	printf("MBR        : %s\n", info.has_mbr ? "yes" : "no");
	printf("GPT        : %s\n", info.has_gpt ? "yes" : "no");
	printf("Isohybrid  : %s\n", info.is_hybrid ? "yes (raw copy is bootable)" : "no");
	printf("El Torito  : %s\n", info.has_el_torito ? "yes" : "no");
	printf("EFI boot   : %s\n", info.efi_bootable ? "yes" : "no");
	printf("Verdict    : %s\n", info.advice);
	return 0;
}

static int device_holds_dev(const rl_device *d, dev_t id)
{
	unsigned maj = major(id), min = minor(id);
	size_t i;

	if (d->devnode[0] != '\0') {
		unsigned dmaj, dmin;
		if (rl_devnode_parse(d->devnode, &dmaj, &dmin) == 0 && dmaj == maj && dmin == min)
			return 1;
	}
	for (i = 0; i < (size_t)d->npartitions; i++) {
		unsigned dmaj, dmin;
		if (rl_devnode_parse(d->part[i].devnode, &dmaj, &dmin) == 0 &&
		    dmaj == maj && dmin == min)
			return 1;
	}
	return 0;
}

static void progress_cb(const rl_progress *p, void *user)
{
	int tty = *(int *)user;
	double pct = (p->total > 0) ? (double)p->done * 100.0 / (double)p->total : 0.0;
	char done[32], total[32], speed[32], eta[16];
	char bar[41];
	int filled = (int)(pct / 2.5);
	int i;

	for (i = 0; i < 40; i++)
		bar[i] = (i < filled) ? '#' : '-';
	bar[40] = '\0';

	rl_format_bytes(p->done, done, sizeof(done));
	rl_format_bytes(p->total, total, sizeof(total));
	rl_format_speed(p->bytes_per_sec, speed, sizeof(speed));
	if (p->bytes_per_sec > 0.0)
		rl_format_duration((double)(p->total - p->done) / p->bytes_per_sec,
		                   eta, sizeof(eta));
	else
		snprintf(eta, sizeof(eta), "--:--");

	if (tty) {
		fprintf(stderr, "\r%s [%s] %5.1f%%  %s / %s  %s  ETA %s   ",
		        p->phase ? "Verify " : "Writing", bar, pct, done, total, speed, eta);
	} else {
		fprintf(stderr, "%s %.1f%% (%s / %s, %s)\n",
		        p->phase ? "Verify" : "Writing", pct, done, total, speed);
	}
	fflush(stderr);
}

static int cmd_write(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "force",   no_argument, NULL, 'f' },
		{ "yes",     no_argument, NULL, 'y' },
		{ "verify",  no_argument, NULL, 'v' },
		{ "raw",     no_argument, NULL, 'r' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "help",    no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	rl_device_list list;
	rl_device *dev = NULL;
	rl_iso_info iso;
	rl_write_opts wopts;
	rl_write_result wres;
	char err[256];
	char isize[32], dsize[32];
	const char *image, *target;
	int force = 0, yes = 0, verify = 0, raw = 0, dry = 0;
	int opt, index = 0, rc = 1, tty;
	struct stat ist;

	while ((opt = getopt_long(argc, argv, "fyvrnh", longopts, &index)) != -1) {
		switch (opt) {
		case 'f': force = 1; break;
		case 'y': yes = 1; break;
		case 'v': verify = 1; break;
		case 'r': raw = 1; break;
		case 'n': dry = 1; break;
		case 'h': usage(stdout); return 0;
		default:  usage(stderr); return 1;
		}
	}
	if (argc - optind < 2) {
		fprintf(stderr, "rufus-linux: 'write' needs an image and a device\n\n");
		usage(stderr);
		return 1;
	}
	image = argv[optind];
	target = argv[optind + 1];

	if (stat(image, &ist) != 0 || !S_ISREG(ist.st_mode)) {
		fprintf(stderr, "rufus-linux: '%s' is not a regular file\n", image);
		return 1;
	}
	if (rl_iso_probe(image, &iso, err, sizeof(err)) != 0) {
		fprintf(stderr, "rufus-linux: %s\n", err);
		return 1;
	}

	if (rl_devices_scan(&list) != 0) {
		fprintf(stderr, "rufus-linux: cannot enumerate block devices\n");
		return 1;
	}
	dev = (rl_device *)rl_device_find(&list, target);
	if (dev == NULL) {
		fprintf(stderr, "rufus-linux: no such block device: %s\n", target);
		fprintf(stderr, "Run 'rufus-linux list' to see the available devices.\n");
		goto out;
	}

	if (dev->is_system) {
		fprintf(stderr,
		        "rufus-linux: refusing to write to %s: it holds the running system.\n",
		        dev->path);
		goto out;
	}
	if (dev->read_only) {
		fprintf(stderr, "rufus-linux: %s is write-protected.\n", dev->path);
		goto out;
	}
	if (device_holds_dev(dev, ist.st_dev)) {
		fprintf(stderr, "rufus-linux: the image lives on the target device itself.\n");
		goto out;
	}
	if (!iso.is_hybrid && !raw) {
		fprintf(stderr,
		        "rufus-linux: %s is not isohybrid; a raw copy would not boot.\n"
		        "             %s\n"
		        "             Use --raw to write it anyway.\n", image, iso.advice);
		goto out;
	}
	if (!iso.is_hybrid) {
		fprintf(stderr,
		        "rufus-linux: warning: %s is not isohybrid; --raw forces a "
		        "verbatim copy.\n", image);
	}
	if (dev->mounted && !force) {
		fprintf(stderr,
		        "rufus-linux: %s has mounted volumes; close them or pass --force.\n",
		        dev->path);
		goto out;
	}

	rl_format_bytes(iso.size_bytes, isize, sizeof(isize));
	rl_format_bytes(dev->size_bytes, dsize, sizeof(dsize));

	printf("Image    : %s (%s, %s)\n", image, isize, rl_iso_fs_name(iso.fs));
	printf("Label    : %s\n", iso.label[0] ? iso.label : "-");
	printf("Target   : %s (%s, %s, %s)\n", dev->path, dsize,
	       dev->transport, dev->model[0] ? dev->model : "unknown");
	printf("Mode     : raw (%s)%s\n", iso.is_hybrid ? "isohybrid" : "forced",
	       iso.efi_bootable ? ", EFI bootable" : "");
	printf("Verify   : %s\n", verify ? "yes (SHA-256 read-back)" : "no");
	printf("Warning  : all data on %s will be destroyed.\n", dev->path);

	if (dry) {
		printf("\nDry run: nothing written.\n");
		rc = 0;
		goto out;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "rufus-linux: writing requires root (try: sudo ...)\n");
		goto out;
	}

	if (!yes) {
		char line[16];

		printf("\nType YES to continue: ");
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL) {
			fprintf(stderr, "\nAborted.\n");
			goto out;
		}
		rl_trim(line);
		if (strcmp(line, "YES") != 0) {
			printf("Aborted.\n");
			goto out;
		}
	}

	if (dev->mounted) {
		printf("\nUnmounting volumes on %s...\n", dev->path);
		if (rl_device_unmount(dev, err, sizeof(err)) != 0) {
			fprintf(stderr, "rufus-linux: %s\n", err);
			goto out;
		}
	}

	memset(&wopts, 0, sizeof(wopts));
	tty = isatty(STDERR_FILENO);
	wopts.verify = verify;
	wopts.progress = progress_cb;
	wopts.user = &tty;

	printf("Writing %s to %s...\n", image, dev->path);
	if (rl_image_write(image, dev, &wopts, &wres, err, sizeof(err)) != 0) {
		fprintf(stderr, "\nrufus-linux: %s\n", err);
		goto out;
	}
	if (tty)
		fprintf(stderr, "\n");

	printf("\nWrote %llu bytes in %.1f s.\n",
	       (unsigned long long)wres.bytes_written, wres.seconds);
	printf("SHA-256  : %s\n", wres.image_sha256);
	if (wres.verified == 1)
		printf("Verified : OK (device matches image)\n");
	else if (wres.verified < 0)
		printf("Verified : FAILED (device differs from image!)\n");
	if (wres.verified < 0) {
		rc = 2;
		goto out;
	}
	rc = 0;

out:
	rl_devices_free(&list);
	return rc;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stdout);
		return 0;
	}
	if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "help") == 0) {
		usage(stdout);
		return 0;
	}
	if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0 ||
	    strcmp(argv[1], "version") == 0) {
		printf("rufus-linux %s\n", RL_VERSION);
		return 0;
	}
	if (strcmp(argv[1], "list") == 0)
		return cmd_list();
	if (strcmp(argv[1], "info") == 0) {
		if (argc < 3) {
			fprintf(stderr, "rufus-linux: 'info' needs an image path\n");
			return 1;
		}
		return cmd_info(argv[2]);
	}
	if (strcmp(argv[1], "write") == 0)
		return cmd_write(argc - 1, argv + 1);

	fprintf(stderr, "rufus-linux: unknown command '%s'\n\n", argv[1]);
	usage(stderr);
	return 1;
}
