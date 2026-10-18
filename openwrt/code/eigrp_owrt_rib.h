// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt Linux kernel RIB integration. Copyright (C) 2026 Donnie V. Savage */
#ifndef EIGRP_OWRT_RIB_H_
#define EIGRP_OWRT_RIB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/netlink.h>
#include "eigrp_rib.h"

/* Start after core initialization; stop/join before core termination.
 * The default VRF and kernel main table are the only supported context. */
bool eigrp_owrt_rib_start(void);
void eigrp_owrt_rib_stop(void);
bool eigrp_owrt_rib_refresh(void);

/* Pure netlink translation. Caller owns the message/storage. These are also
 * exercised by Linux UUT without CAP_NET_ADMIN. */
int eigrp_owrt_rib_message_build(bool add, bool replace,
	const eigrp_rib_route_t *route, void *buffer, size_t capacity);
/* Decode one kernel main-table unicast route. Return 1 for a redistributable
 * source, 0 for ignored routes, and -1 for malformed messages. */
int eigrp_owrt_rib_message_decode(const struct nlmsghdr *msg,
	eigrp_rib_route_t *route, eigrp_rib_nexthop_t *hops,
	size_t hop_capacity);

/* Host-private deterministic reconciliation hook. Inputs are borrowed for the
 * call; state is deep-copied and notifications are emitted on real deltas.
 * Production invokes this only with an authoritative RTM_GETROUTE dump. */
bool eigrp_owrt_rib_snapshot_reconcile(const eigrp_rib_route_t *routes,
    size_t count);

#endif
