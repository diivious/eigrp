// SPDX-License-Identifier: BSD-2-Clause
/*-
 * Copyright 2005,2007,2009 Colin Percival
 * All rights reserved.
 *
 * $FreeBSD: src/lib/libmd/sha256.h,v 1.2 2006/01/17 15:35:56 phk Exp $
 */

#ifndef _EIGRP_SHA256_H_
#define _EIGRP_SHA256_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SHA256Context {
	uint32_t state[8];
	uint32_t count[2];
	unsigned char buf[64];
} eigrp_sha256_ctx_t;

typedef struct HMAC_SHA256Context {
	eigrp_sha256_ctx_t ictx;
	eigrp_sha256_ctx_t octx;
} eigrp_hmac_sha256_ctx_t;

void eigrp_sha256_init(eigrp_sha256_ctx_t *ctx);
void eigrp_sha256_update(eigrp_sha256_ctx_t *ctx, const void *in, size_t len);
void eigrp_sha256_final(unsigned char digest[32], eigrp_sha256_ctx_t *ctx);
void eigrp_hmac_sha256_init(eigrp_hmac_sha256_ctx_t *ctx, const void *_K, size_t Klen);
void eigrp_hmac_sha256_update(eigrp_hmac_sha256_ctx_t *ctx, const void *in, size_t len);
void eigrp_hmac_sha256_final(unsigned char digest[32], eigrp_hmac_sha256_ctx_t *ctx);

/**
 * eigrp_pbkdf2_sha256(passwd, passwdlen, salt, saltlen, c, buf, dkLen):
 * Compute PBKDF2(passwd, salt, c, dkLen) using HMAC-SHA256 as the PRF, and
 * write the output to buf.  The value dkLen must be at most 32 * (2^32 - 1).
 */
void eigrp_pbkdf2_sha256(const uint8_t *passwd, size_t passwdlen, const uint8_t *salt, size_t saltlen,
		   uint64_t c, uint8_t *buf, size_t dkLen);

#ifdef __cplusplus
}
#endif

#endif /* !_EIGRP_SHA256_H_ */
