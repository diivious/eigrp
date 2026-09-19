// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Network Related Functions.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 */

#ifndef _ZEBRA_EIGRP_NETWORK_H
#define _ZEBRA_EIGRP_NETWORK_H

#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"

/* Static inline functions */
/* IPv4/IPv6 prefix and address management functions
 * might move to eigrp_addr.h if this grows
 */
static inline const char *
eigrp_print_addr(eigrp_addr_t *addr)
{
    return inet_ntoa(addr->ip.v4);
}

static inline const char *
eigrp_print_routerid(struct in_addr ipv4)
{
    return inet_ntoa(ipv4);
}


/* Prototypes */
eigrp_result_t eigrp_network_create(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix);
eigrp_result_t eigrp_network_delete(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix);
void eigrp_network_config_delete_all(eigrp_address_family_config_t *af);


extern void eigrp_external_routes_refresh(eigrp_instance_t *, int);

#endif /* EIGRP_NETWORK_H_ */
