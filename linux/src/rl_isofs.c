/*
 * rufus-linux - ISO9660 / Joliet / Rock Ridge tree walker
 * GPL-3.0-or-later
 */

#include "rl_isofs.h"

#define RL_ISO_DIR_MAX  (64u * 1024u * 1024u)
#define RL_ISO_DEPTH    32

struct rl_iso_vol {
	FILE        *f;
	rl_iso_info  info;
	uint64_t     size;
	int          joliet;
	int          susp_skip;
	uint32_t     root_lba;
	uint32_t     root_size;
};

#define ISO_ERR(...) do { if (err != NULL) snprintf(err, errlen, __VA_ARGS__); } while (0)

static int read_at(FILE *f, uint64_t off, void *buf, size_t len)
{
	if (RL_FSEEK(f, off) != 0)
		return -1;
	return (fread(buf, 1, len, f) == len) ? 0 : -1;
}

static int file_size(FILE *f, uint64_t *out)
{
#if defined(_WIN32)
	if (_fseeki64(f, 0, SEEK_END) != 0)
		return -1;
	*out = (uint64_t)_ftelli64(f);
#else
	if (fseeko(f, 0, SEEK_END) != 0)
		return -1;
	*out = (uint64_t)ftello(f);
#endif
	return 0;
}

static size_t utf8_put(uint32_t cp, char *out, size_t o, size_t outlen)
{
	if (cp < 0x80) {
		if (o + 1 < outlen) out[o++] = (char)cp;
	} else if (cp < 0x800) {
		if (o + 2 < outlen) {
			out[o++] = (char)(0xC0 | (cp >> 6));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		}
	} else if (cp < 0x10000) {
		if (o + 3 < outlen) {
			out[o++] = (char)(0xE0 | (cp >> 12));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		}
	} else {
		if (o + 4 < outlen) {
			out[o++] = (char)(0xF0 | (cp >> 18));
			out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	return o;
}

static void joliet_name(const uint8_t *s, size_t len, char *out, size_t outlen)
{
	size_t i, o = 0;

	if (outlen == 0)
		return;
	for (i = 0; i + 1 < len; i += 2) {
		uint32_t cp = ((uint32_t)s[i] << 8) | s[i + 1];
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < len) {
			uint32_t lo = ((uint32_t)s[i + 2] << 8) | s[i + 3];
			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		o = utf8_put(cp, out, o, outlen);
	}
	out[o < outlen ? o : outlen - 1] = '\0';
}

static void iso_name(const uint8_t *s, size_t len, char *out, size_t outlen)
{
	size_t n = len;

	while (n > 0 && s[n - 1] == ' ')
		n--;
	/* Strip the ISO9660 version suffix, e.g. "README.TXT;1". */
	for (size_t i = 0; i < n; i++) {
		if (s[i] == ';') {
			n = i;
			break;
		}
	}
	if (n > 0 && s[n - 1] == '.')
		n--;
	if (n == 0) {
		if (outlen > 1) {
			out[0] = '_';
			out[1] = '\0';
		}
		return;
	}
	if (n >= outlen)
		n = outlen - 1;
	memcpy(out, s, n);
	out[n] = '\0';
}

static int name_is_reserved(const char *name)
{
	static const char *res[] = {
		"CON", "PRN", "AUX", "NUL",
		"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
		"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
	};
	char base[8];
	size_t n = 0;
	size_t i;

	while (name[n] != '\0' && name[n] != '.' && n < sizeof(base) - 1)
		n++;
	for (i = 0; i < n; i++) {
		char c = name[i];
		base[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
	}
	base[n] = '\0';
	for (i = 0; i < sizeof(res) / sizeof(res[0]); i++) {
		if (strcmp(base, res[i]) == 0)
			return 1;
	}
	return 0;
}

static void sanitize_name(char *name)
{
	static const char *bad = "\\:*?\"<>|";
	size_t i, n;

	for (i = 0; name[i] != '\0'; i++) {
		if (strchr(bad, name[i]) != NULL || (unsigned char)name[i] < 0x20)
			name[i] = '_';
	}
	/* FAT does not allow trailing dots or spaces. */
	n = strlen(name);
	while (n > 0 && (name[n - 1] == '.' || name[n - 1] == ' '))
		name[--n] = '\0';
	if (n == 0)
		snprintf(name, 8, "_");
	/* Keep long names well below the 255 UTF-16 unit FAT limit. */
	if (strlen(name) > 200)
		name[200] = '\0';
	if (name_is_reserved(name)) {
		char tmp[210];
		snprintf(tmp, sizeof(tmp), "_%s", name);
		strcpy(name, tmp);
	}
}

/*
 * Walk the SUSP/Rock Ridge entries of one directory record. NM names and SL
 * symlink targets may continue into CE (continuation) areas.
 */
static void parse_susp(rl_iso_vol *vol, const uint8_t *su, size_t len,
		       char *nm, size_t nmlen, int *has_nm,
		       char *sl, size_t sllen, int *has_sl, int depth)
{
	size_t off = 0;

	while (off + 4 <= len) {
		const uint8_t *e = su + off;
		uint8_t elen = e[2];
		const uint8_t *d;
		size_t dlen;

		if (elen < 4 || off + elen > len)
			break;
		d = e + 4;
		dlen = (size_t)elen - 4;

		if (memcmp(e, "ST", 2) == 0)
			break;
		if (memcmp(e, "SP", 2) == 0 && dlen >= 7 &&
		    d[0] == 0xBE && d[1] == 0xEF) {
			vol->susp_skip = d[2];
		} else if (memcmp(e, "NM", 2) == 0 && dlen >= 1) {
			uint8_t flags = d[0];
			if (flags & 0x02) {
				snprintf(nm, nmlen, ".");
				*has_nm = 1;
			} else if (flags & 0x04) {
				snprintf(nm, nmlen, "..");
				*has_nm = 1;
			} else if (dlen > 1) {
				size_t cur = strlen(nm);
				size_t add = dlen - 1;
				if (cur + add >= nmlen)
					add = (cur < nmlen - 1) ? nmlen - 1 - cur : 0;
				memcpy(nm + cur, d + 1, add);
				nm[cur + add] = '\0';
				*has_nm = 1;
			}
		} else if (memcmp(e, "SL", 2) == 0 && dlen >= 1) {
			size_t i = 1;
			while (i + 2 <= dlen) {
				uint8_t cflags = d[i];
				uint8_t clen = d[i + 1];
				const uint8_t *cdata = d + i + 2;
				size_t cur;

				if ((size_t)i + 2 + clen > dlen)
					break;
				cur = strlen(sl);
				if (cflags & 0x08) {          /* root */
					if (cur + 1 < sllen)
						sl[cur++] = '/';
				} else if (cflags & 0x02) {   /* current */
					if (cur + 1 < sllen)
						sl[cur++] = '.';
				} else if (cflags & 0x04) {   /* parent */
					if (cur + 2 < sllen) {
						sl[cur++] = '.';
						sl[cur++] = '.';
					}
				} else if (!(cflags & 0x20)) { /* not hostname */
					if (cur > 0 && sl[cur - 1] != '/' && cur + 1 < sllen)
						sl[cur++] = '/';
					if (cur + clen < sllen) {
						memcpy(sl + cur, cdata, clen);
						cur += clen;
					}
				}
				sl[cur < sllen ? cur : sllen - 1] = '\0';
				*has_sl = 1;
				i += 2 + clen;
			}
		} else if (memcmp(e, "CE", 2) == 0 && dlen >= 24 && depth < 2) {
			uint32_t block = rl_le32(d);
			uint32_t offset = rl_le32(d + 4);
			uint32_t clen = rl_le32(d + 8);
			uint64_t abs = (uint64_t)block * RL_ISO_SECTOR + offset;

			if (clen > 0 && clen <= 65536 && abs + clen <= vol->size) {
				uint8_t *cont = (uint8_t *)malloc(clen);
				if (cont != NULL) {
					if (read_at(vol->f, abs, cont, clen) == 0)
						parse_susp(vol, cont, clen, nm, nmlen,
							   has_nm, sl, sllen, has_sl,
							   depth + 1);
					free(cont);
				}
			}
		}
		off += elen;
	}
}

typedef struct {
	uint32_t lba;
	uint32_t size;
	uint8_t  flags;
	uint8_t  name_len;
	const uint8_t *raw_name;
	char     name[RL_PATH_MAX];
	int      has_nm;
	int      has_sl;
	char     link[RL_PATH_MAX];
} iso_rec;

static void parse_record(rl_iso_vol *vol, const uint8_t *rec, iso_rec *out)
{
	size_t rec_len = rec[0];
	size_t su_off;

	memset(out, 0, sizeof(*out));
	out->lba = rl_le32(rec + 2);
	out->size = rl_le32(rec + 10);
	out->flags = rec[25];
	out->name_len = rec[32];
	out->raw_name = rec + 33;

	if (out->name_len == 1 && out->raw_name[0] == 0x00) {
		snprintf(out->name, sizeof(out->name), ".");
	} else if (out->name_len == 1 && out->raw_name[0] == 0x01) {
		snprintf(out->name, sizeof(out->name), "..");
	} else if (vol->joliet) {
		char *semi;

		joliet_name(out->raw_name, out->name_len, out->name,
			    sizeof(out->name));
		/* Joliet keeps the ISO9660 ";1" version suffix. */
		semi = strchr(out->name, ';');
		if (semi != NULL)
			*semi = '\0';
		{
			size_t n = strlen(out->name);
			if (n > 0 && out->name[n - 1] == '.')
				out->name[n - 1] = '\0';
		}
	} else {
		iso_name(out->raw_name, out->name_len, out->name,
			 sizeof(out->name));
	}

	su_off = 33 + out->name_len;
	if ((out->name_len & 1) == 0)
		su_off++;
	if (su_off + (size_t)vol->susp_skip <= rec_len) {
		const uint8_t *su = rec + su_off + vol->susp_skip;
		size_t sulen = rec_len - su_off - (size_t)vol->susp_skip;

		if (sulen >= 4) {
			parse_susp(vol, su, sulen, out->name, sizeof(out->name),
				   &out->has_nm, out->link, sizeof(out->link),
				   &out->has_sl, 0);
		}
	}
	if (out->has_nm) {
		/* Rock Ridge names are already UTF-8-ish; make them FAT safe. */
	}
	if (strcmp(out->name, ".") != 0 && strcmp(out->name, "..") != 0)
		sanitize_name(out->name);
}

static int walk_dir(rl_iso_vol *vol, uint32_t lba, uint32_t size,
		    const char *prefix, int depth, rl_iso_visit_fn cb,
		    void *user, char *err, size_t errlen)
{
	uint8_t *buf;
	uint64_t abs = (uint64_t)lba * RL_ISO_SECTOR;
	size_t off = 0;
	int rc = 0;

	if (depth > RL_ISO_DEPTH) {
		ISO_ERR("directory tree is too deep at '%s'", prefix);
		return -1;
	}
	if (size == 0)
		return 0;
	if (size > RL_ISO_DIR_MAX || abs + size > vol->size) {
		ISO_ERR("invalid directory extent at '%s'", prefix);
		return -1;
	}
	buf = (uint8_t *)malloc(size);
	if (buf == NULL) {
		ISO_ERR("out of memory reading directory '%s'", prefix);
		return -1;
	}
	if (read_at(vol->f, abs, buf, size) != 0) {
		ISO_ERR("cannot read directory '%s'", prefix);
		free(buf);
		return -1;
	}

	while (off < size) {
		iso_rec rec;
		rl_iso_entry ent;
		char path[RL_PATH_MAX];
		size_t rec_len = buf[off];

		if (rec_len == 0) {
			/* Records never span a sector; skip the padding. */
			off = (off / RL_ISO_SECTOR + 1) * RL_ISO_SECTOR;
			continue;
		}
		if (rec_len < 34 || off + rec_len > size)
			break;

		parse_record(vol, buf + off, &rec);
		if (strcmp(rec.name, ".") == 0 || strcmp(rec.name, "..") == 0) {
			off += rec_len;
			continue;
		}
		if (rec.flags & 0x04) {   /* associated file */
			off += rec_len;
			continue;
		}
		if (prefix[0] == '\0')
			snprintf(path, sizeof(path), "%s", rec.name);
		else
			snprintf(path, sizeof(path), "%s/%s", prefix, rec.name);
		if (strlen(prefix) + strlen(rec.name) + 2 > sizeof(path)) {
			ISO_ERR("path is too long at '%s/%s'", prefix, rec.name);
			free(buf);
			return -1;
		}

		memset(&ent, 0, sizeof(ent));
		snprintf(ent.path, sizeof(ent.path), "%s", path);
		ent.is_dir = (rec.flags & 0x02) != 0;
		ent.is_symlink = rec.has_sl;
		if (rec.has_sl)
			snprintf(ent.link_target, sizeof(ent.link_target), "%s", rec.link);

		if (rec.flags & 0x80) {
			/* Multi-extent file: collect consecutive records. */
			size_t next = off + rec_len;

			while (ent.n_extents < RL_ISO_MAX_EXTENTS) {
				iso_rec more;
				size_t mlen;

				ent.ext[ent.n_extents].lba = rec.lba;
				ent.ext[ent.n_extents].bytes = rec.size;
				ent.n_extents++;
				ent.size += rec.size;

				/* Skip sector padding before the next record. */
				while (next < size && buf[next] == 0)
					next = (next / RL_ISO_SECTOR + 1) * RL_ISO_SECTOR;
				if (next >= size)
					break;
				mlen = buf[next];
				if (mlen < 34 || next + mlen > size)
					break;
				parse_record(vol, buf + next, &more);
				if (more.name_len != rec.name_len ||
				    memcmp(more.raw_name, rec.raw_name, rec.name_len) != 0)
					break;
				rec = more;
				next += mlen;
				if (!(rec.flags & 0x80))
					break;
			}
			off = next;
		} else {
			ent.ext[0].lba = rec.lba;
			ent.ext[0].bytes = rec.size;
			ent.n_extents = 1;
			ent.size = rec.size;
			off += rec_len;
		}

		rc = cb(user, &ent, err, errlen);
		if (rc != 0) {
			free(buf);
			return 1;
		}
		if (ent.is_dir && !ent.is_symlink) {
			rc = walk_dir(vol, ent.ext[0].lba, ent.ext[0].bytes,
				      path, depth + 1, cb, user, err, errlen);
			if (rc != 0) {
				free(buf);
				return rc;
			}
		}
	}

	free(buf);
	return 0;
}

int rl_iso_vol_open(rl_iso_vol **out, const char *path, char *err, size_t errlen)
{
	rl_iso_vol *vol;
	uint8_t sec[RL_ISO_SECTOR];
	uint32_t pvd_root_lba = 0, pvd_root_size = 0;
	uint32_t jol_root_lba = 0, jol_root_size = 0;
	int have_pvd = 0, have_joliet = 0;
	int i;

	if (out == NULL || path == NULL) {
		ISO_ERR("invalid argument");
		return -1;
	}
	*out = NULL;
	vol = (rl_iso_vol *)calloc(1, sizeof(*vol));
	if (vol == NULL) {
		ISO_ERR("out of memory");
		return -1;
	}
	if (rl_iso_probe(path, &vol->info, err, errlen) != 0) {
		free(vol);
		return -1;
	}
	vol->f = fopen(path, "rb");
	if (vol->f == NULL) {
		ISO_ERR("cannot open '%s': %s", path, strerror(errno));
		free(vol);
		return -1;
	}
	if (file_size(vol->f, &vol->size) != 0) {
		ISO_ERR("cannot read size of '%s'", path);
		fclose(vol->f);
		free(vol);
		return -1;
	}

	for (i = 16; i < 16 + 64; i++) {
		const uint8_t *root;

		if (read_at(vol->f, (uint64_t)i * RL_ISO_SECTOR, sec, sizeof(sec)) != 0)
			break;
		if (memcmp(sec + 1, "CD001", 5) != 0)
			continue;
		if (sec[0] == 255)
			break;
		root = sec + 156;
		if (sec[0] == 1 && !have_pvd) {
			pvd_root_lba = rl_le32(root + 2);
			pvd_root_size = rl_le32(root + 10);
			have_pvd = 1;
		} else if (sec[0] == 2 && sec[88] == 0x25 && sec[89] == 0x2F &&
			   (sec[90] == 0x40 || sec[90] == 0x43 || sec[90] == 0x45)) {
			jol_root_lba = rl_le32(root + 2);
			jol_root_size = rl_le32(root + 10);
			have_joliet = 1;
		}
	}
	if (!have_pvd && !have_joliet) {
		ISO_ERR("'%s' has no ISO9660 volume descriptor", path);
		fclose(vol->f);
		free(vol);
		return -1;
	}
	if (have_joliet) {
		vol->joliet = 1;
		vol->root_lba = jol_root_lba;
		vol->root_size = jol_root_size;
	} else {
		vol->root_lba = pvd_root_lba;
		vol->root_size = pvd_root_size;
	}

	/* Parse the root record so a Rock Ridge SP skip is applied everywhere. */
	if (!vol->joliet && vol->root_size >= 34) {
		uint8_t root[512];
		size_t n = vol->root_size < sizeof(root) ? vol->root_size : sizeof(root);
		if (read_at(vol->f, (uint64_t)vol->root_lba * RL_ISO_SECTOR, root, n) == 0 &&
		    root[0] >= 34) {
			iso_rec r;
			parse_record(vol, root, &r);
		}
	}

	*out = vol;
	return 0;
}

void rl_iso_vol_close(rl_iso_vol *vol)
{
	if (vol == NULL)
		return;
	if (vol->f != NULL)
		fclose(vol->f);
	free(vol);
}

const rl_iso_info *rl_iso_vol_info(const rl_iso_vol *vol)
{
	return vol ? &vol->info : NULL;
}

int rl_iso_vol_walk(rl_iso_vol *vol, rl_iso_visit_fn cb, void *user,
		    char *err, size_t errlen)
{
	if (vol == NULL || cb == NULL) {
		ISO_ERR("invalid argument");
		return -1;
	}
	return walk_dir(vol, vol->root_lba, vol->root_size, "", 0, cb, user,
			err, errlen);
}

int rl_iso_vol_pread(rl_iso_vol *vol, uint64_t off, void *buf, size_t len,
		     char *err, size_t errlen)
{
	if (vol == NULL || vol->f == NULL) {
		ISO_ERR("volume is not open");
		return -1;
	}
	if (off > vol->size || len > vol->size - off) {
		ISO_ERR("read past end of image");
		return -1;
	}
	if (read_at(vol->f, off, buf, len) != 0) {
		ISO_ERR("cannot read image at offset %llu", (unsigned long long)off);
		return -1;
	}
	return 0;
}
