/*
 * EIGRP Authnication functions to support sending and receiving of EIGRP Packets.
 * Copyright (C) 2023
 * Authors:
 *   Donnie Savage
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_southbound.h"
static unsigned char zeropad[16] = {0};

int eigrp_make_md5_digest(eigrp_interface_t *ei, eigrp_stream_t *s,
			  uint8_t flags)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;

	unsigned char digest[EIGRP_AUTH_TYPE_MD5_LEN];
	eigrp_md5_ctx_t ctx;
	uint8_t *ibuf;
	size_t backup_get, backup_end;
	struct TLV_MD5_Authentication_Type *auth_TLV;

	ibuf = s->data;
	backup_end = s->endp;
	backup_get = s->getp;

	auth_TLV = eigrp_authTLV_MD5_new();

	eigrp_stream_set_getp(s, EIGRP_HEADER_LEN);
	eigrp_stream_get(auth_TLV, s, EIGRP_AUTH_MD5_TLV_SIZE);
	eigrp_stream_set_getp(s, backup_get);

	if (!eigrp_southbound_auth_key_lookup(ei->params.auth_keychain, &key_id,
	                                       key_string, sizeof(key_string))) {
		eigrp_authTLV_MD5_free(auth_TLV);
		return EIGRP_AUTH_TYPE_NONE;
	}

	memset(&ctx, 0, sizeof(ctx));
	eigrp_md5_init(&ctx);

	/* Generate a digest. Each situation needs different handling */
	if (flags & EIGRP_AUTH_BASIC_HELLO_FLAG) {
		eigrp_md5_update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
		eigrp_md5_update(&ctx, key_string, strlen(key_string));
		if (strlen(key_string) < 16)
			eigrp_md5_update(&ctx, zeropad, 16 - strlen(key_string));
	} else if (flags & EIGRP_AUTH_UPDATE_INIT_FLAG) {
		eigrp_md5_update(&ctx, ibuf, EIGRP_MD5_UPDATE_INIT_COMPUTE);
	} else if (flags & EIGRP_AUTH_UPDATE_FLAG) {
		eigrp_md5_update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
		eigrp_md5_update(&ctx, key_string, strlen(key_string));
		if (strlen(key_string) < 16)
			eigrp_md5_update(&ctx, zeropad, 16 - strlen(key_string));
		if (backup_end > (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE)) {
			eigrp_md5_update(&ctx,
				  ibuf
					  + (EIGRP_HEADER_LEN
					     + EIGRP_AUTH_MD5_TLV_SIZE),
				  backup_end - 20
					  - (EIGRP_HEADER_LEN
					     + EIGRP_AUTH_MD5_TLV_SIZE));
		}
	}

	eigrp_md5_final(digest, &ctx);

	/* Append md5 digest to the end of the stream. */
	memcpy(auth_TLV->digest, digest, EIGRP_AUTH_TYPE_MD5_LEN);

	eigrp_stream_set_endp(s, EIGRP_HEADER_LEN);
	eigrp_stream_put(s, auth_TLV, EIGRP_AUTH_MD5_TLV_SIZE);
	eigrp_stream_set_endp(s, backup_end);

	eigrp_authTLV_MD5_free(auth_TLV);
	return EIGRP_AUTH_TYPE_MD5_LEN;
}

int eigrp_check_md5_digest(eigrp_stream_t *s,
			   struct TLV_MD5_Authentication_Type *authTLV,
			   eigrp_neighbor_t *nbr, uint8_t flags)
{
	eigrp_md5_ctx_t ctx;
	unsigned char digest[EIGRP_AUTH_TYPE_MD5_LEN];
	unsigned char orig[EIGRP_AUTH_TYPE_MD5_LEN];
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	uint8_t *ibuf;
	size_t backup_end;
	struct TLV_MD5_Authentication_Type *auth_TLV;
	struct eigrp_header *eigrph;
	uint16_t saved_checksum;

	if (ntohl(nbr->crypt_seqnum) > ntohl(authTLV->key_sequence)) {
		eigrp_log_warn(
			"interface %s: eigrp_check_md5 bad sequence %d (expect %d)",
			eigrp_intf_name_string(nbr->ei), ntohl(authTLV->key_sequence),
			ntohl(nbr->crypt_seqnum));
		return 0;
	}

	eigrph = (struct eigrp_header *)s->data;
	saved_checksum = eigrph->checksum;
	eigrph->checksum = 0;

	auth_TLV = (struct TLV_MD5_Authentication_Type *)(s->data
							  + EIGRP_HEADER_LEN);
	memcpy(orig, auth_TLV->digest, EIGRP_AUTH_TYPE_MD5_LEN);
	memset(digest, 0, EIGRP_AUTH_TYPE_MD5_LEN);
	memset(auth_TLV->digest, 0, EIGRP_AUTH_TYPE_MD5_LEN);

	ibuf = s->data;
	backup_end = s->endp;

	if (!eigrp_southbound_auth_key_lookup(nbr->ei->params.auth_keychain, &key_id,
	                                       key_string, sizeof(key_string))) {
		eigrp_log_warn(
			"Interface %s: Expected key value not found in config",
			nbr->ei->name);
		memcpy(auth_TLV->digest, orig, EIGRP_AUTH_TYPE_MD5_LEN);
		eigrph->checksum = saved_checksum;
		return 0;
	}

	memset(&ctx, 0, sizeof(ctx));
	eigrp_md5_init(&ctx);

	/* Generate a digest. Each situation needs different handling */
	if (flags & EIGRP_AUTH_BASIC_HELLO_FLAG) {
		eigrp_md5_update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
		eigrp_md5_update(&ctx, key_string, strlen(key_string));
		if (strlen(key_string) < 16)
			eigrp_md5_update(&ctx, zeropad, 16 - strlen(key_string));
	} else if (flags & EIGRP_AUTH_UPDATE_INIT_FLAG) {
		eigrp_md5_update(&ctx, ibuf, EIGRP_MD5_UPDATE_INIT_COMPUTE);
	} else if (flags & EIGRP_AUTH_UPDATE_FLAG) {
		eigrp_md5_update(&ctx, ibuf, EIGRP_MD5_BASIC_COMPUTE);
		eigrp_md5_update(&ctx, key_string, strlen(key_string));
		if (strlen(key_string) < 16)
			eigrp_md5_update(&ctx, zeropad, 16 - strlen(key_string));
		if (backup_end > (EIGRP_HEADER_LEN + EIGRP_AUTH_MD5_TLV_SIZE)) {
			eigrp_md5_update(&ctx,
				  ibuf
					  + (EIGRP_HEADER_LEN
					     + EIGRP_AUTH_MD5_TLV_SIZE),
				  backup_end - 20
					  - (EIGRP_HEADER_LEN
					     + EIGRP_AUTH_MD5_TLV_SIZE));
		}
	}

	eigrp_md5_final(digest, &ctx);

	/* compare the two */
	memcpy(auth_TLV->digest, orig, EIGRP_AUTH_TYPE_MD5_LEN);
	eigrph->checksum = saved_checksum;

	if (memcmp(orig, digest, EIGRP_AUTH_TYPE_MD5_LEN) != 0) {
		eigrp_log_warn("interface %s: eigrp_check_md5 checksum mismatch",
			  eigrp_intf_name_string(nbr->ei));
		return 0;
	}

	/* save neighbor's crypt_seqnum */
	nbr->crypt_seqnum = authTLV->key_sequence;

	return 1;
}

int eigrp_make_sha256_digest(eigrp_interface_t *ei, eigrp_stream_t *s,
			     uint8_t flags)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	char source_ip[INET_ADDRSTRLEN];

	unsigned char digest[EIGRP_AUTH_TYPE_SHA256_LEN];
	unsigned char buffer[1 + PLAINTEXT_LENGTH + 45 + 1] = {0};

	eigrp_hmac_sha256_ctx_t ctx;
	void *ibuf;
	size_t backup_get, backup_end;
	struct TLV_SHA256_Authentication_Type *auth_TLV;

	ibuf = s->data;
	backup_end = s->endp;
	backup_get = s->getp;

	auth_TLV = eigrp_authTLV_SHA256_new();

	eigrp_stream_set_getp(s, EIGRP_HEADER_LEN);
	eigrp_stream_get(auth_TLV, s, EIGRP_AUTH_SHA256_TLV_SIZE);
	eigrp_stream_set_getp(s, backup_get);

	if (!eigrp_southbound_auth_key_lookup(ei->params.auth_keychain, &key_id,
	                                       key_string, sizeof(key_string))) {
		eigrp_log_warn(
			"Interface %s: Expected key value not found in config",
			ei->name);
		eigrp_authTLV_SHA256_free(auth_TLV);
		return 0;
	}

	inet_ntop(AF_INET, ei->address.address.bytes, source_ip, sizeof(source_ip));

	memset(&ctx, 0, sizeof(ctx));
	buffer[0] = '\n';
	memcpy(buffer + 1, key_string, strlen(key_string));
	memcpy(buffer + 1 + strlen(key_string), source_ip, strlen(source_ip));
	eigrp_hmac_sha256_init(&ctx, buffer,
			  1 + strlen(key_string) + strlen(source_ip));
	eigrp_hmac_sha256_update(&ctx, ibuf, strlen(ibuf));
	eigrp_hmac_sha256_final(digest, &ctx);


	/* Put hmac-sha256 digest to it's place */
	memcpy(auth_TLV->digest, digest, EIGRP_AUTH_TYPE_SHA256_LEN);

	eigrp_stream_set_endp(s, EIGRP_HEADER_LEN);
	eigrp_stream_put(s, auth_TLV, EIGRP_AUTH_SHA256_TLV_SIZE);
	eigrp_stream_set_endp(s, backup_end);

	eigrp_authTLV_SHA256_free(auth_TLV);

	return EIGRP_AUTH_TYPE_SHA256_LEN;
}

int eigrp_check_sha256_digest(eigrp_stream_t *s,
			      struct TLV_SHA256_Authentication_Type *authTLV,
			      eigrp_neighbor_t *nbr, uint8_t flags)
{
	return 1;
}

uint16_t eigrp_add_authTLV_MD5_encode(eigrp_stream_t *s, eigrp_interface_t *ei)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	struct TLV_MD5_Authentication_Type *authTLV;

	authTLV = eigrp_authTLV_MD5_new();

	authTLV->type = htons(EIGRP_TLV_AUTH);
	authTLV->length = htons(EIGRP_AUTH_MD5_TLV_SIZE);
	authTLV->auth_type = htons(EIGRP_AUTH_TYPE_MD5);
	authTLV->auth_length = htons(EIGRP_AUTH_TYPE_MD5_LEN);
	authTLV->key_sequence = 0;
	memset(authTLV->Nullpad, 0, sizeof(authTLV->Nullpad));

	if (eigrp_southbound_auth_key_lookup(ei->params.auth_keychain, &key_id,
	                                      key_string, sizeof(key_string))) {
		authTLV->key_id = htonl(key_id);
		memset(authTLV->digest, 0, EIGRP_AUTH_TYPE_MD5_LEN);
		eigrp_stream_put(s, authTLV,
			   sizeof(struct TLV_MD5_Authentication_Type));
		eigrp_authTLV_MD5_free(authTLV);
		return EIGRP_AUTH_MD5_TLV_SIZE;
	}

	eigrp_authTLV_MD5_free(authTLV);

	return 0;
}

uint16_t eigrp_add_authTLV_SHA256_encode(eigrp_stream_t *s,
					 eigrp_interface_t *ei)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	struct TLV_SHA256_Authentication_Type *authTLV;

	authTLV = eigrp_authTLV_SHA256_new();

	authTLV->type = htons(EIGRP_TLV_AUTH);
	authTLV->length = htons(EIGRP_AUTH_SHA256_TLV_SIZE);
	authTLV->auth_type = htons(EIGRP_AUTH_TYPE_SHA256);
	authTLV->auth_length = htons(EIGRP_AUTH_TYPE_SHA256_LEN);
	authTLV->key_sequence = 0;
	memset(authTLV->Nullpad, 0, sizeof(authTLV->Nullpad));

	if (eigrp_southbound_auth_key_lookup(ei->params.auth_keychain, &key_id,
	                                      key_string, sizeof(key_string))) {
		authTLV->key_id = 0;
		memset(authTLV->digest, 0, EIGRP_AUTH_TYPE_SHA256_LEN);
		eigrp_stream_put(s, authTLV,
			   sizeof(struct TLV_SHA256_Authentication_Type));
		eigrp_authTLV_SHA256_free(authTLV);
		return EIGRP_AUTH_SHA256_TLV_SIZE;
	}

	eigrp_authTLV_SHA256_free(authTLV);

	return 0;
}

struct TLV_MD5_Authentication_Type *eigrp_authTLV_MD5_new(void)
{
	struct TLV_MD5_Authentication_Type *new;

	new = calloc(1, sizeof(struct TLV_MD5_Authentication_Type));

	return new;
}

void eigrp_authTLV_MD5_free(struct TLV_MD5_Authentication_Type *authTLV)
{
	free(authTLV);
}

struct TLV_SHA256_Authentication_Type *eigrp_authTLV_SHA256_new(void)
{
	struct TLV_SHA256_Authentication_Type *new;

	new = calloc(1, sizeof(struct TLV_SHA256_Authentication_Type));

	return new;
}

void eigrp_authTLV_SHA256_free(struct TLV_SHA256_Authentication_Type *authTLV)
{
	free(authTLV);
}


static char *eigrp_auth_string_duplicate(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;
	len = strlen(value) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;
	memcpy(copy, value, len);
	return copy;
}

/*
 * Syntax:
 *   Classic: `ip authentication mode eigrp AS <md5|hmac-sha-256>` / `no ip authentication mode eigrp AS [...]`
 *   Named: `authentication mode <md5|hmac-sha-256 ...>` / `no authentication mode`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode through the FRR northbound adapter
 *   Named: af-interface mode through the same EIGRP target
 * Description:
 * Selects or removes packet authentication for a named EIGRP interface.
 * MD5 has a live runtime path; direct-password HMAC-SHA-256 is retained and reports NOT_IMPLEMENTED until key material and receive validation are implemented.
 */
eigrp_result_t eigrp_auth_mode_update(
	eigrp_interface_context_t *context, eigrp_authentication_mode_t mode,
	const eigrp_auth_hmac_config_t *hmac)
{
	char *password = NULL;

	if (mode != EIGRP_AUTHENTICATION_MD5
	    && mode != EIGRP_AUTHENTICATION_HMAC_SHA256)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (mode == EIGRP_AUTHENTICATION_HMAC_SHA256) {
		/* Named mode carries direct HMAC password details in EIGRP-owned
		 * retained state.  Classic runtime-only configuration uses the host
		 * key-chain attachment and therefore has no direct-password object.
		 */
		if (context->config
		    && (!hmac
			|| (hmac->encryption_type != 0 && hmac->encryption_type != 7)
			|| !hmac->password || !hmac->password[0]
			|| strlen(hmac->password) > 32))
			return EIGRP_RESULT_INVALID_ARGUMENT;
		if (context->config) {
			password = eigrp_auth_string_duplicate(hmac->password);
			if (!password)
				return EIGRP_RESULT_INTERNAL_FAILURE;
		}
	} else if (hmac) {
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}

	if (context->config) {
		context->config->authentication_mode = (uint8_t)mode;
		context->config->authentication_mode_configured = true;
		free(context->config->authentication_password);
		context->config->authentication_password = password;
		context->config->authentication_encryption_type =
			mode == EIGRP_AUTHENTICATION_HMAC_SHA256
				? hmac->encryption_type
				: 0;
	}
	if (context->runtime) {
		/* Named direct-password HMAC still needs runtime key-material
		 * integration.  Classic HMAC uses the existing key-chain runtime.
		 */
		if (mode == EIGRP_AUTHENTICATION_HMAC_SHA256 && context->config)
			return EIGRP_RESULT_NOT_IMPLEMENTED;
		context->runtime->params.auth_type =
			mode == EIGRP_AUTHENTICATION_HMAC_SHA256
				? EIGRP_AUTH_TYPE_SHA256
				: EIGRP_AUTH_TYPE_MD5;
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip authentication mode eigrp AS <md5|hmac-sha-256>` / `no ip authentication mode eigrp AS [...]`
 *   Named: `authentication mode <md5|hmac-sha-256 ...>` / `no authentication mode`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode through the FRR northbound adapter
 *   Named: af-interface mode through the same EIGRP target
 * Description:
 * Selects or removes packet authentication for a named EIGRP interface.
 * MD5 has a live runtime path; direct-password HMAC-SHA-256 is retained and reports NOT_IMPLEMENTED until key material and receive validation are implemented.
 */
eigrp_result_t eigrp_auth_mode_delete(eigrp_interface_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->authentication_mode = EIGRP_AUTHENTICATION_NONE;
		context->config->authentication_mode_configured = false;
		context->config->authentication_encryption_type = 0;
		free(context->config->authentication_password);
		context->config->authentication_password = NULL;
	}
	if (context->runtime)
		context->runtime->params.auth_type = EIGRP_AUTH_TYPE_NONE;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip authentication key-chain eigrp AS NAME` / `no ip authentication key-chain eigrp AS [NAME]`
 *   Named: `authentication key-chain NAME` / `no authentication key-chain NAME`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode through the FRR northbound adapter
 *   Named: af-interface mode through the same EIGRP target
 * Description:
 * Selects or removes the EIGRP authentication key chain.
 * Classic and named adapters converge on this EIGRP-owned target.
 */
eigrp_result_t eigrp_auth_keychain_update(eigrp_interface_context_t *context,
					  const char *keychain)
{
	char *config_copy = NULL;
	char *runtime_copy = NULL;

	if (!keychain || !keychain[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	/* Configuration and runtime have independent ownership.  Allocate both
	 * copies before replacing either value so an allocation failure cannot
	 * leave retained named configuration and the active interface out of
	 * sync. */
	if (context->config) {
		config_copy = eigrp_auth_string_duplicate(keychain);
		if (!config_copy)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	if (context->runtime) {
		runtime_copy = eigrp_auth_string_duplicate(keychain);
		if (!runtime_copy) {
			free(config_copy);
			return EIGRP_RESULT_INTERNAL_FAILURE;
		}
	}

	if (context->config) {
		free(context->config->keychain);
		context->config->keychain = config_copy;
	}
	if (context->runtime) {
		free(context->runtime->params.auth_keychain);
		context->runtime->params.auth_keychain = runtime_copy;
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip authentication key-chain eigrp AS NAME` / `no ip authentication key-chain eigrp AS [NAME]`
 *   Named: `authentication key-chain NAME` / `no authentication key-chain NAME`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode through the FRR northbound adapter
 *   Named: af-interface mode through the same EIGRP target
 * Description:
 * Selects or removes the EIGRP authentication key chain.
 * Classic and named adapters converge on this EIGRP-owned target.
 */
eigrp_result_t eigrp_auth_keychain_delete(eigrp_interface_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		free(context->config->keychain);
		context->config->keychain = NULL;
	}
	if (context->runtime) {
		free(context->runtime->params.auth_keychain);
		context->runtime->params.auth_keychain = NULL;
	}
	return EIGRP_RESULT_SUCCESS;
}
