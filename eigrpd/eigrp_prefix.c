// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable prefix helpers.
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "eigrpd/eigrp_prefix.h"

static size_t eigrp_prefix_address_bytes(eigrp_address_family_t afi)
{
	switch (afi) {
	case EIGRP_ADDRESS_FAMILY_IPV4:
		return 4;
	case EIGRP_ADDRESS_FAMILY_IPV6:
		return 16;
	default:
		return 0;
	}
}

bool eigrp_prefix_valid(const eigrp_prefix_t *prefix)
{
	if (!prefix)
		return false;

	switch (prefix->address.afi) {
	case EIGRP_ADDRESS_FAMILY_IPV4:
		return prefix->prefix_length <= 32;
	case EIGRP_ADDRESS_FAMILY_IPV6:
		return prefix->prefix_length <= 128;
	default:
		return false;
	}
}

void eigrp_prefix_normalize(eigrp_prefix_t *prefix)
{
	size_t address_bytes;
	size_t full_bytes;
	uint8_t remaining_bits;

	if (!eigrp_prefix_valid(prefix))
		return;

	address_bytes = eigrp_prefix_address_bytes(prefix->address.afi);
	full_bytes = prefix->prefix_length / 8U;
	remaining_bits = prefix->prefix_length % 8U;

	if (remaining_bits && full_bytes < address_bytes) {
		prefix->address.bytes[full_bytes] &=
			(uint8_t)(0xffU << (8U - remaining_bits));
		full_bytes++;
	}

	if (full_bytes < address_bytes)
		memset(prefix->address.bytes + full_bytes, 0,
		       address_bytes - full_bytes);
	if (address_bytes < sizeof(prefix->address.bytes))
		memset(prefix->address.bytes + address_bytes, 0,
		       sizeof(prefix->address.bytes) - address_bytes);
}

int eigrp_prefix_snprintf(char *buf, size_t len, const eigrp_prefix_t *prefix)
{
	char address[INET6_ADDRSTRLEN];
	int family;
	int written;

	if (!buf || !len || !eigrp_prefix_valid(prefix))
		return -1;

	family = prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV4 ? AF_INET
								     : AF_INET6;
	if (!inet_ntop(family, prefix->address.bytes, address, sizeof(address))) {
		buf[0] = '\0';
		return -1;
	}

	written = snprintf(buf, len, "%s/%u", address, prefix->prefix_length);
	if (written < 0 || (size_t)written >= len) {
		buf[0] = '\0';
		return -1;
	}

	return written;
}
