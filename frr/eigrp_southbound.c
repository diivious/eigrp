// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_southbound.h"

#include "workqueue.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE, "EIGRP work queue");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE_NAME, "EIGRP work queue name");

struct eigrp_work_queue {
	eigrp_instance_t *eigrp;
	struct work_queue *host_queue;
	char *name;
	eigrp_work_queue_func_t workfunc;
	eigrp_work_queue_delete_func_t deletefunc;
};

static wq_item_status eigrp_work_queue_host_run(struct work_queue *host_queue,
						void *data)
{
	eigrp_work_queue_t *queue = host_queue->spec.data;
	eigrp_work_queue_result_t result;

	if (!queue || !queue->workfunc)
		return WQ_SUCCESS;

	result = queue->workfunc(queue, data);
	switch (result) {
	case EIGRP_WORK_QUEUE_REQUEUE:
		return WQ_REQUEUE;
	case EIGRP_WORK_QUEUE_BLOCKED:
		return WQ_QUEUE_BLOCKED;
	case EIGRP_WORK_QUEUE_SUCCESS:
	default:
		return WQ_SUCCESS;
	}
}

static void eigrp_work_queue_host_delete(struct work_queue *host_queue,
						 void *data)
{
	eigrp_work_queue_t *queue = host_queue->spec.data;

	if (queue && queue->deletefunc)
		queue->deletefunc(queue, data);
}

static void eigrp_work_queue_host_create(eigrp_work_queue_t *queue)
{
	queue->host_queue = work_queue_new(eigrpd_event, queue->name);
	queue->host_queue->spec.data = queue;
	queue->host_queue->spec.workfunc = eigrp_work_queue_host_run;
	queue->host_queue->spec.del_item_data = eigrp_work_queue_host_delete;
}

eigrp_work_queue_t *eigrp_work_queue_new(eigrp_instance_t *eigrp,
						 const char *name,
						 eigrp_work_queue_func_t workfunc,
						 eigrp_work_queue_delete_func_t deletefunc)
{
	eigrp_work_queue_t *queue;

	queue = XCALLOC(MTYPE_EIGRP_WORK_QUEUE, sizeof(*queue));
	queue->eigrp = eigrp;
	queue->name = XSTRDUP(MTYPE_EIGRP_WORK_QUEUE_NAME, name);
	queue->workfunc = workfunc;
	queue->deletefunc = deletefunc;

	eigrp_work_queue_host_create(queue);
	return queue;
}

void eigrp_work_queue_free(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;

	if (queue->host_queue)
		work_queue_free_and_null(&queue->host_queue);
	XFREE(MTYPE_EIGRP_WORK_QUEUE_NAME, queue->name);
	XFREE(MTYPE_EIGRP_WORK_QUEUE, queue);
}

void eigrp_work_queue_reset(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;

	if (queue->host_queue)
		work_queue_free_and_null(&queue->host_queue);
	eigrp_work_queue_host_create(queue);
}

void eigrp_work_queue_enqueue(eigrp_work_queue_t *queue, void *data)
{
	if (!queue || !queue->host_queue || !data)
		return;

	work_queue_add(queue->host_queue, data);
}

eigrp_instance_t *eigrp_work_queue_eigrp(eigrp_work_queue_t *queue)
{
	return queue ? queue->eigrp : NULL;
}
