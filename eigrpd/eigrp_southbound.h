// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_SOUTHBOUND_H_
#define _ZEBRA_EIGRP_SOUTHBOUND_H_

#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"

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

/* Host runtime adaptation for IPv4 network statements. */
eigrp_result_t eigrp_southbound_network_create(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *changed);
eigrp_result_t eigrp_southbound_network_delete(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *changed);

#endif /* _ZEBRA_EIGRP_SOUTHBOUND_H_ */
