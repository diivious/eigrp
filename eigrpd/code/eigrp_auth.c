/*
 * EIGRP Authnication functions to support sending and receiving of EIGRP Packets.
 * Copyright (C) 2023
 * Authors:
 *   Donnie Savage
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include "eigrpd.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_auth.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
static unsigned char zeropad[16] = {0};

bool eigrp_auth_material_available(const eigrp_intf_t *ei)
{
	return ei && (ei->params.auth_password || ei->params.auth_keychain);
}

static bool eigrp_auth_key_get(eigrp_intf_t *ei, uint32_t *key_id,
			       char *key, size_t key_size)
{
	if (!ei || !key || key_size == 0)
		return false;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256
	    && ei->params.auth_password) {
		size_t len = strlen(ei->params.auth_password);
		if (len >= key_size)
			return false;
		memcpy(key, ei->params.auth_password, len + 1);
		if (key_id)
			*key_id = 0;
		return true;
	}

	return ei->params.auth_keychain
	       && eigrp_sys_auth_key_lookup(ei->params.auth_keychain, key_id,
					    key, key_size);
}

static bool eigrp_auth_sha256_source(eigrp_intf_t *ei, eigrp_nbr_t *nbr,
				     char *text, size_t text_size)
{
	const void *address;
	int af;

	if (nbr) {
		af = nbr->src.afi;
		address = af == AF_INET6 ? (const void *)&nbr->src.ip.v6
					 : (const void *)&nbr->src.ip.v4;
	} else if (ei && ei->address.address.afi == EIGRP_AFI_IPV6) {
		af = AF_INET6;
		address = ei->address.address.bytes;
	} else if (ei) {
		af = AF_INET;
		address = ei->address.address.bytes;
	} else {
		return false;
	}

	return inet_ntop(af, address, text, text_size) != NULL;
}

int eigrp_make_md5_digest(eigrp_intf_t *ei, eigrp_stream_t *s,
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

	auth_TLV = eigrp_auth_tlv_md5_create();

	eigrp_stream_set_getp(s, EIGRP_HEADER_LEN);
	eigrp_stream_get(auth_TLV, s, EIGRP_AUTH_MD5_TLV_SIZE);
	eigrp_stream_set_getp(s, backup_get);

	if (!eigrp_auth_key_get(ei, &key_id, key_string, sizeof(key_string))) {
		eigrp_auth_tlv_md5_delete(auth_TLV);
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

	eigrp_auth_tlv_md5_delete(auth_TLV);
	return EIGRP_AUTH_TYPE_MD5_LEN;
}

int eigrp_check_md5_digest(eigrp_stream_t *s,
			   struct TLV_MD5_Authentication_Type *authTLV,
			   eigrp_nbr_t *nbr, uint8_t flags)
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
		eigrp_log(EIGRP_LOG_WARNING,
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

	if (!eigrp_auth_key_get(nbr->ei, &key_id, key_string, sizeof(key_string))) {
		eigrp_log(EIGRP_LOG_WARNING,
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
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: eigrp_check_md5 checksum mismatch",
			  eigrp_intf_name_string(nbr->ei));
		return 0;
	}

	/* save neighbor's crypt_seqnum */
	nbr->crypt_seqnum = authTLV->key_sequence;

	return 1;
}

int eigrp_make_sha256_digest(eigrp_intf_t *ei, eigrp_stream_t *s,
			     uint8_t flags)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	char source_ip[INET6_ADDRSTRLEN] = {0};
	unsigned char digest[EIGRP_AUTH_TYPE_SHA256_LEN];
	unsigned char key_buffer[1 + PLAINTEXT_LENGTH + INET6_ADDRSTRLEN] = {0};
	eigrp_hmac_sha256_ctx_t ctx;
	size_t backup_get, backup_end, key_len;
	struct TLV_SHA256_Authentication_Type *auth_tlv;

	(void)flags;
	if (!ei || !s || s->endp < EIGRP_HEADER_LEN + EIGRP_AUTH_SHA256_TLV_SIZE)
		return 0;
	backup_end = s->endp;
	backup_get = s->getp;
	auth_tlv = eigrp_auth_tlv_sha256_create();
	if (!auth_tlv)
		return 0;

	eigrp_stream_set_getp(s, EIGRP_HEADER_LEN);
	eigrp_stream_get(auth_tlv, s, EIGRP_AUTH_SHA256_TLV_SIZE);
	eigrp_stream_set_getp(s, backup_get);
	if (!eigrp_auth_key_get(ei, &key_id, key_string, sizeof(key_string))
	    || !eigrp_auth_sha256_source(ei, NULL, source_ip, sizeof(source_ip))) {
		eigrp_auth_tlv_sha256_delete(auth_tlv);
		return 0;
	}

	memset(auth_tlv->digest, 0, sizeof(auth_tlv->digest));
	eigrp_stream_set_endp(s, EIGRP_HEADER_LEN);
	eigrp_stream_put(s, auth_tlv, EIGRP_AUTH_SHA256_TLV_SIZE);
	eigrp_stream_set_endp(s, backup_end);

	key_buffer[0] = '\n';
	key_len = strlen(key_string);
	memcpy(key_buffer + 1, key_string, key_len);
	memcpy(key_buffer + 1 + key_len, source_ip, strlen(source_ip));
	eigrp_hmac_sha256_init(&ctx, key_buffer,
			       1 + key_len + strlen(source_ip));
	eigrp_hmac_sha256_update(&ctx, s->data, backup_end);
	eigrp_hmac_sha256_final(digest, &ctx);

	memcpy(auth_tlv->digest, digest, sizeof(digest));
	eigrp_stream_set_endp(s, EIGRP_HEADER_LEN);
	eigrp_stream_put(s, auth_tlv, EIGRP_AUTH_SHA256_TLV_SIZE);
	eigrp_stream_set_endp(s, backup_end);
	eigrp_auth_tlv_sha256_delete(auth_tlv);
	memset(key_string, 0, sizeof(key_string));
	memset(key_buffer, 0, sizeof(key_buffer));
	return EIGRP_AUTH_TYPE_SHA256_LEN;
}

int eigrp_check_sha256_digest(eigrp_stream_t *s,
			      struct TLV_SHA256_Authentication_Type *authTLV,
			      eigrp_nbr_t *nbr, uint8_t flags)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	char source_ip[INET6_ADDRSTRLEN] = {0};
	unsigned char digest[EIGRP_AUTH_TYPE_SHA256_LEN];
	unsigned char original[EIGRP_AUTH_TYPE_SHA256_LEN];
	unsigned char key_buffer[1 + PLAINTEXT_LENGTH + INET6_ADDRSTRLEN] = {0};
	eigrp_hmac_sha256_ctx_t ctx;
	struct eigrp_header *header;
	uint32_t key_id = 0;
	uint16_t saved_checksum;
	size_t key_len;
	int valid;

	(void)flags;
	if (!s || !authTLV || !nbr || !nbr->ei
	    || s->endp < EIGRP_HEADER_LEN + EIGRP_AUTH_SHA256_TLV_SIZE)
		return 0;
	if (ntohs(authTLV->type) != EIGRP_TLV_AUTH
	    || ntohs(authTLV->length) != EIGRP_AUTH_SHA256_TLV_SIZE
	    || ntohs(authTLV->auth_type) != EIGRP_AUTH_TYPE_SHA256
	    || ntohs(authTLV->auth_length) != EIGRP_AUTH_TYPE_SHA256_LEN)
		return 0;
	if (ntohl(nbr->crypt_seqnum) > ntohl(authTLV->key_sequence))
		return 0;
	if (!eigrp_auth_key_get(nbr->ei, &key_id, key_string, sizeof(key_string))
	    || !eigrp_auth_sha256_source(nbr->ei, nbr, source_ip, sizeof(source_ip)))
		return 0;

	header = (struct eigrp_header *)s->data;
	saved_checksum = header->checksum;
	header->checksum = 0;
	memcpy(original, authTLV->digest, sizeof(original));
	memset(authTLV->digest, 0, sizeof(authTLV->digest));

	key_buffer[0] = '\n';
	key_len = strlen(key_string);
	memcpy(key_buffer + 1, key_string, key_len);
	memcpy(key_buffer + 1 + key_len, source_ip, strlen(source_ip));
	eigrp_hmac_sha256_init(&ctx, key_buffer,
			       1 + key_len + strlen(source_ip));
	eigrp_hmac_sha256_update(&ctx, s->data, s->endp);
	eigrp_hmac_sha256_final(digest, &ctx);

	memcpy(authTLV->digest, original, sizeof(original));
	header->checksum = saved_checksum;
	valid = memcmp(original, digest, sizeof(original)) == 0;
	if (valid)
		nbr->crypt_seqnum = authTLV->key_sequence;
	memset(key_string, 0, sizeof(key_string));
	memset(key_buffer, 0, sizeof(key_buffer));
	return valid;
}

uint16_t eigrp_auth_tlv_md5_encode(eigrp_stream_t *s, eigrp_intf_t *ei)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	struct TLV_MD5_Authentication_Type *authTLV;

	authTLV = eigrp_auth_tlv_md5_create();

	authTLV->type = htons(EIGRP_TLV_AUTH);
	authTLV->length = htons(EIGRP_AUTH_MD5_TLV_SIZE);
	authTLV->auth_type = htons(EIGRP_AUTH_TYPE_MD5);
	authTLV->auth_length = htons(EIGRP_AUTH_TYPE_MD5_LEN);
	authTLV->key_sequence = 0;
	memset(authTLV->Nullpad, 0, sizeof(authTLV->Nullpad));

	if (eigrp_auth_key_get(ei, &key_id, key_string, sizeof(key_string))) {
		authTLV->key_id = htonl(key_id);
		memset(authTLV->digest, 0, EIGRP_AUTH_TYPE_MD5_LEN);
		eigrp_stream_put(s, authTLV,
			   sizeof(struct TLV_MD5_Authentication_Type));
		eigrp_auth_tlv_md5_delete(authTLV);
		return EIGRP_AUTH_MD5_TLV_SIZE;
	}

	eigrp_auth_tlv_md5_delete(authTLV);

	return 0;
}

uint16_t eigrp_auth_tlv_sha256_encode(eigrp_stream_t *s,
					 eigrp_intf_t *ei)
{
	char key_string[PLAINTEXT_LENGTH + 1] = {0};
	uint32_t key_id = 0;
	struct TLV_SHA256_Authentication_Type *authTLV;

	authTLV = eigrp_auth_tlv_sha256_create();

	authTLV->type = htons(EIGRP_TLV_AUTH);
	authTLV->length = htons(EIGRP_AUTH_SHA256_TLV_SIZE);
	authTLV->auth_type = htons(EIGRP_AUTH_TYPE_SHA256);
	authTLV->auth_length = htons(EIGRP_AUTH_TYPE_SHA256_LEN);
	authTLV->key_sequence = 0;
	memset(authTLV->Nullpad, 0, sizeof(authTLV->Nullpad));

	if (eigrp_auth_key_get(ei, &key_id, key_string, sizeof(key_string))) {
		authTLV->key_id = 0;
		memset(authTLV->digest, 0, EIGRP_AUTH_TYPE_SHA256_LEN);
		eigrp_stream_put(s, authTLV,
			   sizeof(struct TLV_SHA256_Authentication_Type));
		eigrp_auth_tlv_sha256_delete(authTLV);
		return EIGRP_AUTH_SHA256_TLV_SIZE;
	}

	eigrp_auth_tlv_sha256_delete(authTLV);

	return 0;
}

struct TLV_MD5_Authentication_Type *eigrp_auth_tlv_md5_create(void)
{
	struct TLV_MD5_Authentication_Type *new;

	new = calloc(1, sizeof(struct TLV_MD5_Authentication_Type));

	return new;
}

void eigrp_auth_tlv_md5_delete(struct TLV_MD5_Authentication_Type *authTLV)
{
	free(authTLV);
}

struct TLV_SHA256_Authentication_Type *eigrp_auth_tlv_sha256_create(void)
{
	struct TLV_SHA256_Authentication_Type *new;

	new = calloc(1, sizeof(struct TLV_SHA256_Authentication_Type));

	return new;
}

void eigrp_auth_tlv_sha256_delete(struct TLV_SHA256_Authentication_Type *authTLV)
{
	free(authTLV);
}


static char *eigrp_auth_string_dup(const char *value)
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
 * MD5 and plaintext direct-password HMAC-SHA-256 have live runtime paths.
 */
eigrp_result_t eigrp_auth_mode_update(eigrp_operation_t operation, eigrp_intf_context_t *context, eigrp_authentication_mode_t mode, const eigrp_auth_hmac_config_t *hmac)
{
	char *config_password = NULL;
	char *runtime_password = NULL;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (operation == EIGRP_RESET) {
		if (context->config) {
			context->config->authentication_mode = EIGRP_AUTHENTICATION_NONE;
			context->config->authentication_mode_configured = false;
			context->config->authentication_encryption_type = 0;
			free(context->config->authentication_password);
			context->config->authentication_password = NULL;
		}
		if (context->runtime) {
			context->runtime->params.auth_type = EIGRP_AUTH_TYPE_NONE;
			if (context->runtime->params.auth_password) {
				memset(context->runtime->params.auth_password, 0,
				       strlen(context->runtime->params.auth_password));
				free(context->runtime->params.auth_password);
				context->runtime->params.auth_password = NULL;
			}
		}
		return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (mode != EIGRP_AUTHENTICATION_MD5
	    && mode != EIGRP_AUTHENTICATION_HMAC_SHA256)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (mode == EIGRP_AUTHENTICATION_HMAC_SHA256) {
		if (context->config
		    && (!hmac || !hmac->password || !hmac->password[0]
			|| strlen(hmac->password) > 32))
			return EIGRP_RESULT_INVALID_ARGUMENT;
		if (context->config && hmac->encryption_type == 7)
			return EIGRP_RESULT_UNSUPPORTED;
		if (context->config && hmac->encryption_type != 0)
			return EIGRP_RESULT_INVALID_ARGUMENT;

		if (context->config) {
			config_password = eigrp_auth_string_dup(hmac->password);
			if (!config_password)
				return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		if (context->runtime && context->config) {
			runtime_password = eigrp_auth_string_dup(hmac->password);
			if (!runtime_password) {
				free(config_password);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
		}
	} else if (hmac) {
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}

	if (context->config) {
		context->config->authentication_mode = (uint8_t)mode;
		context->config->authentication_mode_configured = true;
		free(context->config->authentication_password);
		context->config->authentication_password = config_password;
		context->config->authentication_encryption_type =
			mode == EIGRP_AUTHENTICATION_HMAC_SHA256 ? hmac->encryption_type : 0;
	}
	if (context->runtime) {
		if (context->runtime->params.auth_password) {
			memset(context->runtime->params.auth_password, 0,
			       strlen(context->runtime->params.auth_password));
			free(context->runtime->params.auth_password);
		}
		context->runtime->params.auth_password = runtime_password;
		context->runtime->params.auth_type =
			mode == EIGRP_AUTHENTICATION_HMAC_SHA256
				? EIGRP_AUTH_TYPE_SHA256 : EIGRP_AUTH_TYPE_MD5;

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
 * MD5 and plaintext direct-password HMAC-SHA-256 have live runtime paths.
 */


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
eigrp_result_t eigrp_auth_keychain_update(eigrp_operation_t operation, eigrp_intf_context_t *context, const char *keychain)
{
	if (operation == EIGRP_RESET) {
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

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

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
		config_copy = eigrp_auth_string_dup(keychain);
		if (!config_copy)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	if (context->runtime) {
		runtime_copy = eigrp_auth_string_dup(keychain);
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

