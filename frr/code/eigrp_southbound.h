// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR southbound adapter declarations.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _FRR_EIGRP_SOUTHBOUND_H_
#define _FRR_EIGRP_SOUTHBOUND_H_

#include "eigrp.h"

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

#endif /* _FRR_EIGRP_SOUTHBOUND_H_ */
