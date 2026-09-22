/*
 * EIGRP Authnication functions to support sending and receiving of EIGRP Packets.
 * Copyright (C) 2023
 * Authors:
 *   Donnie Savage
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _ZEBRA_EIGRP_AUTH_H
#define _ZEBRA_EIGRP_AUTH_H

#include "eigrpd/eigrp_cli.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_md5.h"
#include "eigrpd/eigrp_sha256.h"
#include "eigrpd/eigrp.h"

/*
 * These externs need to cleaned up
 */
extern int eigrp_make_md5_digest(eigrp_interface_t *, eigrp_stream_t *, uint8_t);
extern int eigrp_check_md5_digest(eigrp_stream_t *,
				  struct TLV_MD5_Authentication_Type *,
				  eigrp_neighbor_t *, uint8_t);
extern int eigrp_make_sha256_digest(eigrp_interface_t *, eigrp_stream_t *,
				    uint8_t);
extern int eigrp_check_sha256_digest(eigrp_stream_t *,
				     struct TLV_SHA256_Authentication_Type *,
				     eigrp_neighbor_t *, uint8_t);


extern struct TLV_SHA256_Authentication_Type *eigrp_authTLV_SHA256_new(void);
extern void eigrp_authTLV_SHA256_free(struct TLV_SHA256_Authentication_Type *);

extern struct TLV_MD5_Authentication_Type *eigrp_authTLV_MD5_new(void);
extern void eigrp_authTLV_MD5_free(struct TLV_MD5_Authentication_Type *);

extern uint16_t eigrp_add_authTLV_MD5_encode(eigrp_stream_t *,
					     eigrp_interface_t *);
extern uint16_t eigrp_add_authTLV_SHA256_encode(eigrp_stream_t *,
						eigrp_interface_t *);

eigrp_result_t eigrp_auth_mode_set(
	eigrp_interface_context_t *context, eigrp_authentication_mode_t mode,
	const eigrp_auth_hmac_config_t *hmac);
eigrp_result_t eigrp_auth_mode_reset(eigrp_interface_context_t *context);
eigrp_result_t eigrp_auth_keychain_set(eigrp_interface_context_t *context,
					  const char *keychain);
eigrp_result_t eigrp_auth_keychain_reset(eigrp_interface_context_t *context);

#endif /* _ZEBRA_EIGRP_AUTH_H */


