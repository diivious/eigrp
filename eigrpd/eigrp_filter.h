// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Filter Functions.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 *
 */

#ifndef EIGRPD_EIGRP_FILTER_H_
#define EIGRPD_EIGRP_FILTER_H_

#include "distribute.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_result.h"

extern void eigrp_distribute_update(struct distribute_ctx *ctx,
				    struct distribute *dist);
extern void eigrp_distribute_update_all(struct prefix_list *plist);
extern void eigrp_distribute_update_all_wrapper(struct access_list *alist);
extern void eigrp_distribute_timer_process(void *arg);
extern void eigrp_distribute_timer_interface(void *arg);

bool eigrp_filter_prefix_apply(eigrp_instance_t *eigrp,
			       eigrp_interface_t *ei, int direction,
			       const eigrp_prefix_t *prefix);

eigrp_result_t eigrp_offset_update(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name);
eigrp_result_t eigrp_offset_delete(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name);
eigrp_result_t eigrp_distribute_list_update(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name);
eigrp_result_t eigrp_distribute_list_delete(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name);
void eigrp_distribute_list_config_delete_all(eigrp_address_family_config_t *af);

#endif /* EIGRPD_EIGRP_FILTER_H_ */
