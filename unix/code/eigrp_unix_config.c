// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Standalone Unix EIGRP configuration parser.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_cli.h"
#include "eigrp_unix.h"
#include "eigrp_unix_config.h"

#define EIGRP_UNIX_CONFIG_LINE_MAX 512U
#define EIGRP_UNIX_CONFIG_NAME_MAX 255U

struct eigrp_unix_config {
	char instance_name[EIGRP_UNIX_CONFIG_NAME_MAX + 1U];
	eigrp_afi_t afi;
	uint16_t asn;
	eigrp_af_instance_t *address_family;
};

static char *eigrp_unix_config_trim(char *line)
{
	char *end;

	while (*line && isspace((unsigned char)*line))
		line++;
	end = line + strlen(line);
	while (end > line && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return line;
}

static eigrp_result_t eigrp_unix_config_parent_apply(
	eigrp_unix_config_t *config, const char *line)
{
	const char *name = line + strlen("router eigrp ");
	size_t length = strlen(name);
	eigrp_result_t result;

	if (!length || length > EIGRP_UNIX_CONFIG_NAME_MAX || strchr(name, ' '))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_instance_parent_create(name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	memcpy(config->instance_name, name, length + 1U);
	config->address_family = NULL;
	config->afi = 0;
	config->asn = 0;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_unix_config_af_apply(
	eigrp_unix_config_t *config, const char *line)
{
	char family[16];
	char extra;
	unsigned asn;
	eigrp_afi_t afi;
	eigrp_result_t result;

	if (!config->instance_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (sscanf(line, "address-family %15s autonomous-system %u %c",
		   family, &asn, &extra) != 2
	    && sscanf(line, "address-family %15s unicast autonomous-system %u %c",
		      family, &asn, &extra) != 2)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (strcmp(family, "ipv4") != 0)
		return EIGRP_RESULT_UNSUPPORTED;
	afi = EIGRP_AFI_IPV4;
	if (asn == 0 || asn > UINT16_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	result = eigrp_af_instance_create(
		config->instance_name, afi, EIGRP_UNIX_DEFAULT_VRF_NAME,
		(uint16_t)asn);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	config->address_family = eigrp_af_instance_read(
		config->instance_name, afi, EIGRP_UNIX_DEFAULT_VRF_NAME,
		(uint16_t)asn);
	if (!config->address_family)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	config->afi = afi;
	config->asn = (uint16_t)asn;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_unix_config_router_id_apply(
	eigrp_unix_config_t *config, const char *value)
{
	struct in_addr address;
	eigrp_instance_context_t context = {0};
	eigrp_result_t result;
	uint32_t router_id;

	if (!config->address_family)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (inet_pton(AF_INET, value, &address) != 1)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_af_instance_context_read(
		config->instance_name, config->afi, EIGRP_UNIX_DEFAULT_VRF_NAME,
		config->asn, &context);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	router_id = ntohl(address.s_addr);
	return eigrp_instance_router_id_update(EIGRP_SET, &context, router_id);
}

static eigrp_result_t eigrp_unix_config_line_apply(
	eigrp_unix_config_t *config, char *raw_line)
{
	char *line = eigrp_unix_config_trim(raw_line);

	if (!line[0] || line[0] == '#' || line[0] == '!')
		return EIGRP_RESULT_SUCCESS;
	if (strncmp(line, "router eigrp ", strlen("router eigrp ")) == 0)
		return eigrp_unix_config_parent_apply(config, line);
	if (strncmp(line, "address-family ", strlen("address-family ")) == 0)
		return eigrp_unix_config_af_apply(config, line);
	if (strncmp(line, "eigrp router-id ", strlen("eigrp router-id ")) == 0)
		return eigrp_unix_config_router_id_apply(
			config, line + strlen("eigrp router-id "));
	if (strcmp(line, "shutdown") == 0) {
		if (!config->address_family)
			return EIGRP_RESULT_INVALID_ARGUMENT;
		return eigrp_af_instance_shutdown_update(EIGRP_SET,
			config->address_family);
	}
	if (strcmp(line, "no shutdown") == 0) {
		if (!config->address_family)
			return EIGRP_RESULT_INVALID_ARGUMENT;
		return eigrp_af_instance_shutdown_update(EIGRP_RESET,
			config->address_family);
	}
	if (strcmp(line, "exit-address-family") == 0) {
		config->address_family = NULL;
		config->afi = 0;
		config->asn = 0;
		return EIGRP_RESULT_SUCCESS;
	}
	if (strcmp(line, "exit") == 0) {
		config->instance_name[0] = '\0';
		config->address_family = NULL;
		config->afi = 0;
		config->asn = 0;
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_UNSUPPORTED;
}

eigrp_unix_config_t *eigrp_unix_config_create(void)
{
	return calloc(1, sizeof(eigrp_unix_config_t));
}

void eigrp_unix_config_delete(eigrp_unix_config_t *config)
{
	free(config);
}

eigrp_result_t eigrp_unix_config_apply_file(eigrp_unix_config_t *config,
	FILE *input, unsigned *error_line)
{
	char line[EIGRP_UNIX_CONFIG_LINE_MAX];
	unsigned line_number = 0;
	eigrp_result_t result;

	if (error_line)
		*error_line = 0;
	if (!config || !input)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	while (fgets(line, sizeof(line), input)) {
		line_number++;
		if (!strchr(line, '\n') && !feof(input)) {
			if (error_line)
				*error_line = line_number;
			return EIGRP_RESULT_INVALID_ARGUMENT;
		}
		result = eigrp_unix_config_line_apply(config, line);
		if (result != EIGRP_RESULT_SUCCESS) {
			if (error_line)
				*error_line = line_number;
			return result;
		}
	}
	return ferror(input) ? EIGRP_RESULT_INTERNAL_FAILURE
			     : EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_config_apply_text(eigrp_unix_config_t *config,
	const char *text, unsigned *error_line)
{
	FILE *input;
	eigrp_result_t result;

	if (!config || !text)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	input = tmpfile();
	if (!input)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (fputs(text, input) == EOF || fflush(input) != 0
	    || fseek(input, 0, SEEK_SET) != 0) {
		fclose(input);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	result = eigrp_unix_config_apply_file(config, input, error_line);
	fclose(input);
	return result;
}
