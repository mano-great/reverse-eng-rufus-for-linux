/*
 * rufus-linux - SHA-256 (FIPS 180-4)
 * GPL-3.0-or-later
 */

#ifndef RL_SHA256_H
#define RL_SHA256_H

#include "rl_common.h"

typedef struct {
	uint32_t h[8];
	uint64_t total_len;
	uint8_t  buf[64];
	size_t   buf_len;
} rl_sha256_ctx;

void rl_sha256_init(rl_sha256_ctx *ctx);
void rl_sha256_update(rl_sha256_ctx *ctx, const void *data, size_t len);
void rl_sha256_final(rl_sha256_ctx *ctx, uint8_t out[32]);
void rl_sha256(const void *data, size_t len, uint8_t out[32]);
void rl_sha256_hex(const uint8_t digest[32], char out[65]);

#endif /* RL_SHA256_H */
