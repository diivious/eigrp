// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenWrt-private Linux interface inventory and netlink lifecycle.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_OWRT_INTERFACE_H_
#define EIGRP_OWRT_INTERFACE_H_

#include <stdbool.h>
#include <stdint.h>
#include "eigrp_sys.h"

/* Start AFTER eigrp_init() and BEFORE permitting protocol traffic.
 * Stop and join BEFORE eigrp_terminate(). No implicit core initialization.
 * Startup is idempotent; failure leaves no watcher running. */
bool eigrp_owrt_interface_start(void);
void eigrp_owrt_interface_stop(void);

/* Rescan Linux link/address state immediately and emit real deltas. Useful
 * after host policy/netifd events; asynchronous route-netlink also calls it.
 * Requires eigrp_init() to have completed. */
bool eigrp_owrt_interface_refresh(void);

/* Owns an immutable snapshot of normalized interface state; private to the
 * adapter. Addresses are owned by the snapshot and never cross as pointers. */
typedef struct eigrp_owrt_address {
	eigrp_prefix_t prefix;
	bool secondary;
	struct eigrp_owrt_address *next;
} eigrp_owrt_address_t;

typedef struct eigrp_owrt_link {
	eigrp_ifindex_t ifindex;
	char name[64];
	uint32_t bandwidth; /* kb/s */
	uint32_t mtu;
	bool operative;
	bool multicast;
	eigrp_owrt_address_t *addresses;
	struct eigrp_owrt_link *next;
} eigrp_owrt_link_t;

/* Private snapshot/diff API, also used by deterministic UUT harnesses. */
void eigrp_owrt_interface_snapshot_free(eigrp_owrt_link_t *links);
void eigrp_owrt_interface_snapshot_emit(const eigrp_owrt_link_t *before,
					const eigrp_owrt_link_t *after);

#endif /* EIGRP_OWRT_INTERFACE_H_ */
