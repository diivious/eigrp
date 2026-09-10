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

extern struct zclient *eigrp_zclient;

extern void eigrp_zebra_init(void);
extern void eigrp_zebra_stop(void);

extern void eigrp_zebra_route_add(eigrp_instance_t *eigrp, struct prefix *p,
				  struct list *successors, uint32_t distance);
extern void eigrp_zebra_route_delete(eigrp_instance_t *eigrp, struct prefix *);
extern int eigrp_redistribute_set(eigrp_instance_t *, int, struct eigrp_metrics);
extern int eigrp_redistribute_unset(eigrp_instance_t *, int);

#endif /* _ZEBRA_EIGRP_ZEBRA_H_ */
