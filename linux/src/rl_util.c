/*
 * rufus-linux - small formatting helpers
 * GPL-3.0-or-later
 */

#include "rl_common.h"

void rl_format_bytes(uint64_t bytes, char *out, size_t outlen)
{
	static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
	double v = (double)bytes;
	size_t u = 0;

	while (v >= 1024.0 && u < (sizeof(units) / sizeof(units[0])) - 1) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(out, outlen, "%.0f %s", v, units[u]);
	else
		snprintf(out, outlen, "%.2f %s", v, units[u]);
}

void rl_format_speed(double bytes_per_sec, char *out, size_t outlen)
{
	char b[32];

	rl_format_bytes((uint64_t)(bytes_per_sec < 0 ? 0 : bytes_per_sec), b, sizeof(b));
	snprintf(out, outlen, "%s/s", b);
}

void rl_format_duration(double seconds, char *out, size_t outlen)
{
	if (seconds < 0 || seconds > 359999.0) {
		snprintf(out, outlen, "--:--");
		return;
	}
	if (seconds < 60.0)
		snprintf(out, outlen, "%02d:%02d", 0, (int)seconds);
	else if (seconds < 3600.0)
		snprintf(out, outlen, "%02d:%02d", (int)(seconds / 60), (int)seconds % 60);
	else
		snprintf(out, outlen, "%d:%02d:%02d", (int)(seconds / 3600),
		         (int)(seconds / 60) % 60, (int)seconds % 60);
}

void rl_trim(char *s)
{
	size_t n, i = 0;

	if (s == NULL)
		return;
	while (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')
		i++;
	if (i > 0)
		memmove(s, s + i, strlen(s + i) + 1);
	n = strlen(s);
	while (n > 0) {
		char c = s[n - 1];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		s[--n] = '\0';
	}
}

const char *rl_path_basename(const char *path)
{
	const char *p;

	if (path == NULL)
		return "";
	p = strrchr(path, '/');
#if defined(_WIN32)
	{
		const char *q = strrchr(path, '\\');
		if (q != NULL && (p == NULL || q > p))
			p = q;
	}
#endif
	return (p != NULL) ? p + 1 : path;
}
