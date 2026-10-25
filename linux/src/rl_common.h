/*
 * rufus-linux - native Linux backend for the Rufus fork
 * Copyright (C) 2026  rufus-linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef RL_COMMON_H
#define RL_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define RL_VERSION      "0.1.0"
#define RL_PATH_MAX     512
#define RL_NAME_MAX     64

#if defined(_WIN32)
#  define RL_FSEEK(f, o) _fseeki64((f), (o), SEEK_SET)
#  define RL_FTELL(f)    _ftelli64(f)
#else
#  define RL_FSEEK(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#  define RL_FTELL(f)    ftello(f)
#endif

static inline uint16_t rl_le16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rl_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t rl_le64(const uint8_t *p)
{
	return (uint64_t)rl_le32(p) | ((uint64_t)rl_le32(p + 4) << 32);
}

void rl_format_bytes(uint64_t bytes, char *out, size_t outlen);
void rl_format_speed(double bytes_per_sec, char *out, size_t outlen);
void rl_format_duration(double seconds, char *out, size_t outlen);
void rl_trim(char *s);
const char *rl_path_basename(const char *path);

#endif /* RL_COMMON_H */
