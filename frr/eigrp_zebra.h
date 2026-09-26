// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra connect library for EIGRP.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#ifndef _ZEBRA_EIGRP_ZEBRA_H_
#define _ZEBRA_EIGRP_ZEBRA_H_

#include <zebra.h>
#include "lib/zclient.h"
#include "lib/libfrr.h"

#include "eigrpd/eigrp.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"

extern struct zclient *eigrp_zclient;
extern struct zebra_privs_t eigrpd_privs;

extern void eigrp_zebra_init(void);
extern void eigrp_zebra_stop(void);
extern void eigrp_zebra_instance_delete(eigrp_instance_t *eigrp);

eigrp_result_t eigrp_zebra_route_install(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route);
eigrp_result_t eigrp_zebra_route_remove(eigrp_instance_t *eigrp,
				       const eigrp_prefix_t *prefix);
extern int eigrp_redistribute_set(eigrp_instance_t *, int, struct eigrp_metrics);
extern int eigrp_redistribute_unset(eigrp_instance_t *, int);

eigrp_result_t eigrp_zebra_redistribute_update(
	eigrp_instance_t *eigrp, const eigrp_redistribute_source_t *source);
eigrp_result_t eigrp_zebra_redistribute_delete(
	eigrp_instance_t *eigrp, const eigrp_redistribute_source_t *source);

#endif /* _ZEBRA_EIGRP_ZEBRA_H_ */
