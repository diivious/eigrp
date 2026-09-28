// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix internal host RIB for standalone EIGRP development.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_UNIX_EIGRP_UNIX_RIB_H_
#define EIGRP_UNIX_EIGRP_UNIX_RIB_H_

#include "eigrp_rib.h"

typedef void (*eigrp_unix_rib_route_walk_cb)(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route, void *arg);
typedef void (*eigrp_unix_rib_source_walk_cb)(
	const eigrp_rib_source_route_t *route, void *arg);

/* Host-originated route lifecycle.  These functions model the Unix RIB; they
 * then notify subscribed EIGRP instances through the public RIB contract. */
eigrp_result_t eigrp_unix_rib_source_route_update(
	const eigrp_rib_source_route_t *route);
eigrp_result_t eigrp_unix_rib_source_route_remove(
	const eigrp_rib_source_route_t *route);

void eigrp_unix_rib_route_walk(eigrp_unix_rib_route_walk_cb callback,
	void *arg);
void eigrp_unix_rib_source_walk(eigrp_unix_rib_source_walk_cb callback,
	void *arg);
size_t eigrp_unix_rib_route_count(void);
size_t eigrp_unix_rib_source_count(void);

#endif /* EIGRP_UNIX_EIGRP_UNIX_RIB_H_ */
