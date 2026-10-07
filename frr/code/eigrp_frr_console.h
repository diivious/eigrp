// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR console, debug, logging, and error integration.
 * Copyright (C) 2013-2018, 2026
 * Authors retain the history from the consolidated source modules.
 */
#ifndef _FRR_EIGRP_CONSOLE_H_
#define _FRR_EIGRP_CONSOLE_H_

#include "lib/ferr.h"
#include "lib_errors.h"
#include "eigrp_debug.h"

struct vty;

enum eigrp_log_refs {
	EC_EIGRP_PACKET = EIGRP_FERR_START,
	EC_EIGRP_CONFIG,
};

void show_ip_eigrp_interface_header(struct vty *, eigrp_instance_t *);
void show_ip_eigrp_neighbor_header(struct vty *, eigrp_instance_t *);
void show_ip_eigrp_topology_header(struct vty *, eigrp_instance_t *);
void show_ip_eigrp_interface_detail(struct vty *, eigrp_instance_t *,
				    eigrp_intf_t *);
void show_ip_eigrp_interface_sub(struct vty *, eigrp_instance_t *,
				 eigrp_intf_t *);
void show_ip_eigrp_neighbor_sub(struct vty *, eigrp_nbr_t *, int);
void show_ip_eigrp_prefix_descriptor(struct vty *,
				     eigrp_prefix_descriptor_t *, bool);
void show_ip_eigrp_route_descriptor(struct vty *, eigrp_instance_t *,
				    eigrp_route_descriptor_t *, bool *first,
				    bool include_serial);

void eigrp_error_init(void);
void eigrp_debug_init(void);

#endif /* _FRR_EIGRP_CONSOLE_H_ */
