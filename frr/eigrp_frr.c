// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR datatype adaptation.
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <string.h>

#include "eigrp_frr.h"

static eigrp_address_family_t eigrp_frr_address_family_import(uint8_t family)
{
	switch (family) {
	case AF_INET:
		return EIGRP_ADDRESS_FAMILY_IPV4;
	case AF_INET6:
		return EIGRP_ADDRESS_FAMILY_IPV6;
	default:
		return 0;
	}
}

static uint8_t eigrp_frr_address_family_export(eigrp_address_family_t afi)
{
	switch (afi) {
	case EIGRP_ADDRESS_FAMILY_IPV4:
		return AF_INET;
	case EIGRP_ADDRESS_FAMILY_IPV6:
		return AF_INET6;
	default:
		return AF_UNSPEC;
	}
}

eigrp_result_t eigrp_frr_prefix_import(const struct prefix *host,
				       eigrp_prefix_t *prefix)
{
	eigrp_address_family_t afi;
	size_t address_bytes;
	uint8_t max_prefix_length;

	if (!host || !prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	afi = eigrp_frr_address_family_import(host->family);
	if (!afi)
		return EIGRP_RESULT_UNSUPPORTED;

	if (afi == EIGRP_ADDRESS_FAMILY_IPV4) {
		address_bytes = sizeof(host->u.prefix4);
		max_prefix_length = IPV4_MAX_BITLEN;
	} else {
		address_bytes = sizeof(host->u.prefix6);
		max_prefix_length = 128;
	}
	if (host->prefixlen > max_prefix_length)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = afi;
	prefix->prefix_length = host->prefixlen;
	if (afi == EIGRP_ADDRESS_FAMILY_IPV4)
		memcpy(prefix->address.bytes, &host->u.prefix4, address_bytes);
	else
		memcpy(prefix->address.bytes, &host->u.prefix6, address_bytes);

	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_frr_prefix_export(const eigrp_prefix_t *prefix,
				       struct prefix *host)
{
	uint8_t family;
	size_t address_bytes;
	uint8_t max_prefix_length;

	if (!prefix || !host)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	family = eigrp_frr_address_family_export(prefix->address.afi);
	if (family == AF_UNSPEC)
		return EIGRP_RESULT_UNSUPPORTED;

	if (prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV4) {
		address_bytes = sizeof(host->u.prefix4);
		max_prefix_length = IPV4_MAX_BITLEN;
	} else {
		address_bytes = sizeof(host->u.prefix6);
		max_prefix_length = 128;
	}
	if (prefix->prefix_length > max_prefix_length)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	memset(host, 0, sizeof(*host));
	host->family = family;
	host->prefixlen = prefix->prefix_length;
	if (prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV4)
		memcpy(&host->u.prefix4, prefix->address.bytes, address_bytes);
	else
		memcpy(&host->u.prefix6, prefix->address.bytes, address_bytes);

	return EIGRP_RESULT_SUCCESS;
}
