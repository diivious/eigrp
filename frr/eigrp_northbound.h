// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP daemon northbound adapter API.
 * Copyright (C) 2026 Donnie V. Savage
 */

#ifndef _FRR_EIGRP_NORTHBOUND_H_
#define _FRR_EIGRP_NORTHBOUND_H_

#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_types.h"

eigrp_result_t eigrp_northbound_neighbor_clear_address(
	eigrp_instance_t *runtime, eigrp_address_family_t afi,
	const struct in_addr *ipv4_address,
	const struct in6_addr *ipv6_address, bool soft,
	eigrp_neighbor_clear_cb callback, void *arg, size_t *affected_count);

#endif /* _FRR_EIGRP_NORTHBOUND_H_ */
