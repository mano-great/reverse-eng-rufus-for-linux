/*
 * rufus-linux - list an ISO image's tree.
 * GPL-3.0-or-later
 */

#include "../src/rl_isofs.h"

static int visit(void *user, const rl_iso_entry *e, char *err, size_t errlen)
{
	(void)user; (void)err; (void)errlen;
	printf("%c %12llu  %s%s\n", e->is_dir ? 'd' : (e->is_symlink ? 'l' : 'f'),
	       (unsigned long long)e->size, e->path,
	       e->is_symlink ? " -> " : "");
	if (e->is_symlink)
		printf("                        %s\n", e->link_target);
	return 0;
}

int main(int argc, char **argv)
{
	rl_iso_vol *vol = NULL;
	char err[512];

	if (argc < 2) {
		fprintf(stderr, "usage: %s image.iso\n", argv[0]);
		return 2;
	}
	if (rl_iso_vol_open(&vol, argv[1], err, sizeof(err)) != 0) {
		fprintf(stderr, "error: %s\n", err);
		return 1;
	}
	if (rl_iso_vol_walk(vol, visit, NULL, err, sizeof(err)) != 0) {
		fprintf(stderr, "error: %s\n", err);
		rl_iso_vol_close(vol);
		return 1;
	}
	rl_iso_vol_close(vol);
	return 0;
}
