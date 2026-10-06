// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (C) 2004 6WIND
 *                          <Vincent.Jardin@6WIND.com>
 * All rights reserved.
 *
 * This MD5 code is Big endian and Little Endian compatible.
 */

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 */

#ifndef _EIGRP_MD5_H_
#define _EIGRP_MD5_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MD5_BUFLEN	64

typedef struct {
	union {
		uint32_t md5_state32[4];
		uint8_t md5_state8[16];
	} md5_st;

#define md5_sta		md5_st.md5_state32[0]
#define md5_stb		md5_st.md5_state32[1]
#define md5_stc		md5_st.md5_state32[2]
#define md5_std		md5_st.md5_state32[3]
#define md5_st8		md5_st.md5_state8

	union {
		uint64_t md5_count64;
		uint8_t md5_count8[8];
	} md5_count;
#define md5_n	md5_count.md5_count64
#define md5_n8	md5_count.md5_count8

	unsigned int md5_i;
	uint8_t md5_buf[MD5_BUFLEN];
} eigrp_md5_ctx_t;

extern void eigrp_md5_init(eigrp_md5_ctx_t *ctxt);
extern void eigrp_md5_update(eigrp_md5_ctx_t *ctxt, const void *vinput, unsigned int len);
extern void eigrp_md5_pad(eigrp_md5_ctx_t *ctxt);
extern void eigrp_md5_result(uint8_t *digest, eigrp_md5_ctx_t *ctxt);

extern void eigrp_md5_final(uint8_t *digest, eigrp_md5_ctx_t *ctxt);

/* From RFC 2104 */
void eigrp_hmac_md5(unsigned char *text, int text_len, unsigned char *key,
	      int key_len, uint8_t *digest);

#ifdef __cplusplus
}
#endif

#endif /* ! _EIGRP_MD5_H_*/
