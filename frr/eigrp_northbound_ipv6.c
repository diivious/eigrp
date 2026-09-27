// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR IPv6 northbound integration.
 *
 * Code migrated from eigrp_northbound.c during the address-family
 * integration split.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <netinet/in.h>
#include <string.h>

#include "eigrpd/eigrp.h"
#include "eigrp_northbound_internal.h"

bool eigrp_northbound_ipv6_neighbor_address_copy(
	eigrp_address_t *destination, const struct in6_addr *address)
{
	if (!destination || !address)
		return false;

	memset(destination, 0, sizeof(*destination));
	destination->afi = EIGRP_AFI_IPV6;
	memcpy(destination->bytes, address, sizeof(*address));
	return true;
}
