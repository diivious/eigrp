// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR southbound private declarations.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_SOUTHBOUND_INTERNAL_H
#define _EIGRP_SOUTHBOUND_INTERNAL_H

#include "eigrpd/eigrp.h"

int eigrp_southbound_socket_fd_get(const eigrp_instance_t *eigrp);

eigrp_result_t eigrp_southbound_ipv4_socket_configure(int fd);
int eigrp_southbound_ipv4_multicast_interface_update(eigrp_operation_t operation,
                                                   eigrp_instance_t *eigrp,
                                                   eigrp_intf_t *ei);
int eigrp_southbound_ipv4_multicast_join(eigrp_instance_t *eigrp,
                                         eigrp_intf_t *ei);
int eigrp_southbound_ipv4_multicast_leave(eigrp_instance_t *eigrp,
                                          eigrp_intf_t *ei);

eigrp_result_t eigrp_southbound_ipv6_socket_configure(int fd);
int eigrp_southbound_ipv6_multicast_interface_update(eigrp_operation_t operation,
                                                   eigrp_instance_t *eigrp,
                                                   eigrp_intf_t *ei);
int eigrp_southbound_ipv6_multicast_join(eigrp_instance_t *eigrp,
                                         eigrp_intf_t *ei);
int eigrp_southbound_ipv6_multicast_leave(eigrp_instance_t *eigrp,
                                          eigrp_intf_t *ei);

#endif /* _EIGRP_SOUTHBOUND_INTERNAL_H */
