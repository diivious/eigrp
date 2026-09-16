// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_SOUTHBOUND_H_
#define _ZEBRA_EIGRP_SOUTHBOUND_H_

#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"
#include "eigrpd/eigrp_metric.h"

typedef enum eigrp_work_queue_result {
	EIGRP_WORK_QUEUE_SUCCESS = 0,
	EIGRP_WORK_QUEUE_REQUEUE,
	EIGRP_WORK_QUEUE_BLOCKED,
} eigrp_work_queue_result_t;

typedef eigrp_work_queue_result_t (*eigrp_work_queue_func_t)(
	eigrp_work_queue_t *queue, void *data);
typedef void (*eigrp_work_queue_delete_func_t)(eigrp_work_queue_t *queue,
						       void *data);

eigrp_work_queue_t *eigrp_work_queue_new(eigrp_instance_t *eigrp,
						 const char *name,
						 eigrp_work_queue_func_t workfunc,
						 eigrp_work_queue_delete_func_t deletefunc);
void eigrp_work_queue_free(eigrp_work_queue_t *queue);
void eigrp_work_queue_reset(eigrp_work_queue_t *queue);
void eigrp_work_queue_enqueue(eigrp_work_queue_t *queue, void *data);
eigrp_instance_t *eigrp_work_queue_eigrp(eigrp_work_queue_t *queue);

/* Host runtime lifecycle for a named address-family context. */
eigrp_result_t eigrp_southbound_instance_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, eigrp_instance_t **runtime);
eigrp_result_t eigrp_southbound_instance_delete(
	const char *name, eigrp_instance_t *runtime);
void eigrp_southbound_router_id_refresh(eigrp_instance_t *runtime);
eigrp_result_t eigrp_southbound_address_family_stop(eigrp_instance_t *runtime);
eigrp_result_t eigrp_southbound_address_family_start(eigrp_instance_t *runtime);

/* Host runtime adaptation for IPv4 network statements. */
eigrp_result_t eigrp_southbound_network_create(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *changed);
eigrp_result_t eigrp_southbound_network_delete(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *changed);

/* Host runtime adaptation for route redistribution. */
eigrp_result_t eigrp_southbound_redistribute_update(
	eigrp_instance_t *eigrp, const char *protocol,
	const eigrp_metric_values_t *metric, const char *route_map);
eigrp_result_t eigrp_southbound_redistribute_delete(
	eigrp_instance_t *eigrp, const char *protocol);

/* Host/filter adaptation for distribute-list references. */
eigrp_result_t eigrp_southbound_distribute_list_update(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name);
eigrp_result_t eigrp_southbound_distribute_list_delete(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name);

#endif /* _ZEBRA_EIGRP_SOUTHBOUND_H_ */
