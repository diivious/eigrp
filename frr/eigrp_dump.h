// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Dump Functions and Debbuging.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 */

#ifndef _FRR_EIGRP_DUMP_H_
#define _FRR_EIGRP_DUMP_H_

#include "eigrpd/eigrp_debug.h"

struct vty;

extern void show_ip_eigrp_interface_header(struct vty *, eigrp_instance_t *);
extern void show_ip_eigrp_neighbor_header(struct vty *, eigrp_instance_t *);
extern void show_ip_eigrp_topology_header(struct vty *, eigrp_instance_t *);
extern void show_ip_eigrp_interface_detail(struct vty *, eigrp_instance_t *,
                                           eigrp_interface_t *);
extern void show_ip_eigrp_interface_sub(struct vty *, eigrp_instance_t *,
                                        eigrp_interface_t *);
extern void show_ip_eigrp_neighbor_sub(struct vty *, eigrp_neighbor_t *, int);
extern void show_ip_eigrp_prefix_descriptor(struct vty *,
                                             eigrp_prefix_descriptor_t *, bool);
extern void show_ip_eigrp_route_descriptor(struct vty *, eigrp_instance_t *,
                                            eigrp_route_descriptor_t *,
                                            bool *first, bool include_serial);

extern void eigrp_debug_init(void);

#endif /* _FRR_EIGRP_DUMP_H_ */
