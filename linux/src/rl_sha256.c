/*
 * rufus-linux - SHA-256 (FIPS 180-4)
 * GPL-3.0-or-later
 */

#include "rl_sha256.h"

static const uint32_t K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, hh;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
		       ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = h[0]; b = h[1]; c = h[2]; d = h[3];
	e = h[4]; f = h[5]; g = h[6]; hh = h[7];

	for (i = 0; i < 64; i++) {
		uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t t1 = hh + S1 + ch + K[i] + w[i];
		uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
		uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + mj;

		hh = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void rl_sha256_init(rl_sha256_ctx *ctx)
{
	ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
	ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
	ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
	ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
	ctx->total_len = 0;
	ctx->buf_len = 0;
}

void rl_sha256_update(rl_sha256_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;

	ctx->total_len += (uint64_t)len;

	if (ctx->buf_len > 0) {
		size_t need = 64 - ctx->buf_len;
		size_t take = (len < need) ? len : need;
		memcpy(ctx->buf + ctx->buf_len, p, take);
		ctx->buf_len += take;
		p += take;
		len -= take;
		if (ctx->buf_len == 64) {
			sha256_block(ctx->h, ctx->buf);
			ctx->buf_len = 0;
		}
	}

	while (len >= 64) {
		sha256_block(ctx->h, p);
		p += 64;
		len -= 64;
	}

	if (len > 0) {
		memcpy(ctx->buf, p, len);
		ctx->buf_len = len;
	}
}

void rl_sha256_final(rl_sha256_ctx *ctx, uint8_t out[32])
{
	uint64_t bits = ctx->total_len * 8;
	uint8_t pad[72];
	size_t padlen;
	int i;

	pad[0] = 0x80;
	padlen = (ctx->buf_len < 56) ? (56 - ctx->buf_len) : (120 - ctx->buf_len);
	memset(pad + 1, 0, padlen - 1);
	for (i = 0; i < 8; i++)
		pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
	rl_sha256_update(ctx, pad, padlen + 8);

	for (i = 0; i < 8; i++) {
		out[i * 4]     = (uint8_t)(ctx->h[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(ctx->h[i]);
	}
}

void rl_sha256(const void *data, size_t len, uint8_t out[32])
{
	rl_sha256_ctx ctx;

	rl_sha256_init(&ctx);
	rl_sha256_update(&ctx, data, len);
	rl_sha256_final(&ctx, out);
}

void rl_sha256_hex(const uint8_t digest[32], char out[65])
{
	static const char hex[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 32; i++) {
		out[i * 2]     = hex[digest[i] >> 4];
		out[i * 2 + 1] = hex[digest[i] & 0x0f];
	}
	out[64] = '\0';
}
