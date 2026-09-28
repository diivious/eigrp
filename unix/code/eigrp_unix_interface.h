// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix host interface inventory for standalone EIGRP development.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_UNIX_EIGRP_UNIX_INTERFACE_H_
#define EIGRP_UNIX_EIGRP_UNIX_INTERFACE_H_

#include "eigrp_sys.h"

typedef struct eigrp_unix_interface eigrp_unix_interface_t;

typedef void (*eigrp_unix_interface_walk_cb)(
	const eigrp_unix_interface_t *interface, void *arg);
typedef void (*eigrp_unix_interface_address_walk_cb)(
	const eigrp_prefix_t *address, bool secondary, void *arg);

void eigrp_unix_interface_model_reset(void);
eigrp_result_t eigrp_unix_interface_create(
	const char *name, eigrp_ifindex_t ifindex, bool multicast_capable,
	uint32_t bandwidth, uint32_t mtu, eigrp_unix_interface_t **interface);
eigrp_result_t eigrp_unix_interface_delete(const char *name);
eigrp_result_t eigrp_unix_interface_up(const char *name);
eigrp_result_t eigrp_unix_interface_down(const char *name);
eigrp_result_t eigrp_unix_interface_address_add(
	const char *name, const eigrp_prefix_t *address, bool secondary);
eigrp_result_t eigrp_unix_interface_address_remove(
	const char *name, const eigrp_prefix_t *address);

eigrp_unix_interface_t *eigrp_unix_interface_find(const char *name);
const char *eigrp_unix_interface_name(const eigrp_unix_interface_t *interface);
eigrp_ifindex_t eigrp_unix_interface_ifindex(
	const eigrp_unix_interface_t *interface);
bool eigrp_unix_interface_is_up(const eigrp_unix_interface_t *interface);
bool eigrp_unix_interface_multicast_capable(
	const eigrp_unix_interface_t *interface);
size_t eigrp_unix_interface_address_count(
	const eigrp_unix_interface_t *interface);
void eigrp_unix_interface_walk(eigrp_unix_interface_walk_cb callback,
	void *arg);
void eigrp_unix_interface_address_walk(
	const eigrp_unix_interface_t *interface,
	eigrp_unix_interface_address_walk_cb callback, void *arg);

#endif /* EIGRP_UNIX_EIGRP_UNIX_INTERFACE_H_ */
