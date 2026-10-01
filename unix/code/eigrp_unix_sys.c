// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix implementation of EIGRP runtime scheduling services.
 *
 * This file deliberately owns only process/runtime mechanics. Protocol sockets,
 * packet I/O, interface discovery, policy, and RIB integration are separate Unix
 * adapter work and are not faked here.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "eigrp_sys.h"
#include "eigrp_unix.h"

typedef enum eigrp_unix_event_kind {
	EIGRP_UNIX_EVENT_IMMEDIATE,
	EIGRP_UNIX_EVENT_TIMER,
	EIGRP_UNIX_EVENT_READ,
	EIGRP_UNIX_EVENT_WRITE,
} eigrp_unix_event_kind_t;

struct eigrp_event {
	eigrp_unix_event_kind_t kind;
	eigrp_event_callback_t callback;
	void *arg;
	eigrp_event_t **owner;
	uint64_t due_msec;
	int fd;
	struct eigrp_event *next;
};

typedef struct eigrp_unix_fd_binding {
	eigrp_instance_t *eigrp;
	int fd;
	struct eigrp_unix_fd_binding *next;
} eigrp_unix_fd_binding_t;

typedef struct eigrp_unix_work_item {
	void *data;
	struct eigrp_unix_work_item *next;
} eigrp_unix_work_item_t;

struct eigrp_work_queue {
	eigrp_instance_t *eigrp;
	char *name;
	eigrp_work_queue_func_t workfunc;
	eigrp_work_queue_delete_func_t deletefunc;
	eigrp_unix_work_item_t *head;
	eigrp_unix_work_item_t *tail;
	eigrp_event_t *event;
	bool blocked;
};

typedef struct eigrp_unix_runtime {
	pthread_mutex_t lock;
	pthread_t thread;
	bool initialized;
	bool running;
	int wake_read;
	int wake_write;
	eigrp_event_t *events;
	eigrp_unix_fd_binding_t *bindings;
} eigrp_unix_runtime_t;

static eigrp_unix_runtime_t runtime = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.wake_read = -1,
	.wake_write = -1,
};

/* Portable EIGRP callbacks execute on the runtime thread while Unix host
 * management operations may originate on another thread.  Keep those two
 * execution contexts out of portable/core state at the same time. */
static pthread_mutex_t protocol_lock = PTHREAD_MUTEX_INITIALIZER;

void eigrp_unix_runtime_enter(void)
{
	pthread_mutex_lock(&protocol_lock);
}

void eigrp_unix_runtime_leave(void)
{
	pthread_mutex_unlock(&protocol_lock);
}

static void eigrp_unix_log(const char *message)
{
	fprintf(stderr, "EIGRP unix: %s\n", message);
}

static uint64_t eigrp_unix_monotime_msec(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return ((uint64_t)now.tv_sec * 1000U)
	       + ((uint64_t)now.tv_nsec / 1000000U);
}

static void eigrp_unix_wake(void)
{
	char byte = 0;
	ssize_t written;

	if (runtime.wake_write >= 0) {
		written = write(runtime.wake_write, &byte, sizeof(byte));
		(void)written;
	}
}

static void eigrp_unix_wake_drain(void)
{
	char buffer[64];

	while (read(runtime.wake_read, buffer, sizeof(buffer)) > 0)
		;
}

static bool eigrp_unix_event_unlink(eigrp_event_t *event)
{
	eigrp_event_t **cursor;

	for (cursor = &runtime.events; *cursor; cursor = &(*cursor)->next) {
		if (*cursor != event)
			continue;
		*cursor = event->next;
		return true;
	}
	return false;
}

static void eigrp_unix_event_destroy(eigrp_event_t *event)
{
	if (!event)
		return;
	if (event->owner && *event->owner == event)
		*event->owner = NULL;
	free(event);
}

static int eigrp_unix_fd_get_locked(eigrp_instance_t *eigrp)
{
	eigrp_unix_fd_binding_t *binding;

	for (binding = runtime.bindings; binding; binding = binding->next)
		if (binding->eigrp == eigrp)
			return binding->fd;
	return -1;
}

static eigrp_event_t *eigrp_unix_event_prepare(
	eigrp_event_t **owner, eigrp_unix_event_kind_t kind,
	eigrp_event_callback_t callback, void *arg, uint32_t delay_msec,
	int fd)
{
	eigrp_event_t *event;

	if (!owner || !callback)
		return NULL;

	eigrp_sys_event_cancel(owner);
	event = calloc(1, sizeof(*event));
	if (!event)
		return NULL;
	event->kind = kind;
	event->callback = callback;
	event->arg = arg;
	event->owner = owner;
	event->due_msec = eigrp_unix_monotime_msec() + delay_msec;
	event->fd = fd;

	pthread_mutex_lock(&runtime.lock);
	if (!runtime.running) {
		pthread_mutex_unlock(&runtime.lock);
		free(event);
		return NULL;
	}
	event->next = runtime.events;
	runtime.events = event;
	*owner = event;
	pthread_mutex_unlock(&runtime.lock);
	eigrp_unix_wake();
	return event;
}

static int eigrp_unix_poll_timeout_locked(uint64_t now)
{
	eigrp_event_t *event;
	uint64_t nearest = UINT64_MAX;

	for (event = runtime.events; event; event = event->next) {
		if (event->kind == EIGRP_UNIX_EVENT_IMMEDIATE)
			return 0;
		if (event->kind == EIGRP_UNIX_EVENT_TIMER && event->due_msec < nearest)
			nearest = event->due_msec;
	}
	if (nearest == UINT64_MAX)
		return -1;
	if (nearest <= now)
		return 0;
	if (nearest - now > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)(nearest - now);
}

static size_t eigrp_unix_pollfds_build_locked(struct pollfd **pollfds,
					       eigrp_event_t ***events)
{
	eigrp_event_t *event;
	struct pollfd *pfds;
	eigrp_event_t **map;
	size_t count = 1;
	size_t index = 1;

	for (event = runtime.events; event; event = event->next)
		if (event->kind == EIGRP_UNIX_EVENT_READ
		    || event->kind == EIGRP_UNIX_EVENT_WRITE)
			count++;

	pfds = calloc(count, sizeof(*pfds));
	map = calloc(count, sizeof(*map));
	if (!pfds || !map) {
		free(pfds);
		free(map);
		*pollfds = NULL;
		*events = NULL;
		return 0;
	}
	pfds[0].fd = runtime.wake_read;
	pfds[0].events = POLLIN;
	for (event = runtime.events; event; event = event->next) {
		if (event->kind != EIGRP_UNIX_EVENT_READ
		    && event->kind != EIGRP_UNIX_EVENT_WRITE)
			continue;
		pfds[index].fd = event->fd;
		pfds[index].events = event->kind == EIGRP_UNIX_EVENT_READ
					 ? POLLIN : POLLOUT;
		map[index] = event;
		index++;
	}
	*pollfds = pfds;
	*events = map;
	return count;
}

static eigrp_event_t *eigrp_unix_ready_take_locked(
	struct pollfd *pollfds, eigrp_event_t **map, size_t count, uint64_t now)
{
	eigrp_event_t *event;
	size_t index;

	for (event = runtime.events; event; event = event->next) {
		if (event->kind == EIGRP_UNIX_EVENT_IMMEDIATE
		    || (event->kind == EIGRP_UNIX_EVENT_TIMER
			&& event->due_msec <= now)) {
			if (eigrp_unix_event_unlink(event))
				return event;
		}
	}
	for (index = 1; index < count; index++) {
		if (!map[index] || !pollfds[index].revents)
			continue;
		event = map[index];
		if (eigrp_unix_event_unlink(event))
			return event;
	}
	return NULL;
}

static void *eigrp_unix_runtime_loop(void *unused)
{
	(void)unused;
	for (;;) {
		struct pollfd *pollfds = NULL;
		eigrp_event_t **map = NULL;
		eigrp_event_t *event;
		eigrp_event_callback_t callback;
		void *arg;
		size_t count;
		int timeout;
		int poll_result;

		pthread_mutex_lock(&runtime.lock);
		if (!runtime.running) {
			pthread_mutex_unlock(&runtime.lock);
			break;
		}
		timeout = eigrp_unix_poll_timeout_locked(eigrp_unix_monotime_msec());
		count = eigrp_unix_pollfds_build_locked(&pollfds, &map);
		pthread_mutex_unlock(&runtime.lock);
		if (count == 0) {
			eigrp_unix_log("unable to allocate poll state");
			break;
		}

		poll_result = poll(pollfds, count, timeout);
		if (poll_result < 0 && errno != EINTR)
			eigrp_unix_log("poll failed");
		if (pollfds[0].revents & POLLIN)
			eigrp_unix_wake_drain();

		/* Serialize the ready-event handoff with host-side protocol mutations.
		 * Taking an event and only then waiting for the protocol lock leaves an
		 * uncancellable in-flight callback: interface-down can remove the state
		 * that callback owns before it actually runs.  Hold the protocol lock
		 * before unlinking the event so cancellation either wins first or waits
		 * for the callback to finish. */
		eigrp_unix_runtime_enter();
		pthread_mutex_lock(&runtime.lock);
		event = eigrp_unix_ready_take_locked(pollfds, map, count,
						      eigrp_unix_monotime_msec());
		if (event && event->owner && *event->owner == event)
			*event->owner = NULL;
		pthread_mutex_unlock(&runtime.lock);
		free(map);
		free(pollfds);
		if (!event) {
			eigrp_unix_runtime_leave();
			continue;
		}

		callback = event->callback;
		arg = event->arg;
		free(event);
		callback(arg);
		eigrp_unix_runtime_leave();
	}
	return NULL;
}

void eigrp_sys_runtime_init(void)
{
	int wake_pipe[2];

	pthread_mutex_lock(&runtime.lock);
	if (runtime.initialized) {
		pthread_mutex_unlock(&runtime.lock);
		return;
	}
	if (pipe(wake_pipe) != 0) {
		pthread_mutex_unlock(&runtime.lock);
		eigrp_unix_log("unable to create runtime wake pipe");
		return;
	}
	runtime.wake_read = wake_pipe[0];
	runtime.wake_write = wake_pipe[1];
	(void)fcntl(runtime.wake_read, F_SETFL,
		    fcntl(runtime.wake_read, F_GETFL, 0) | O_NONBLOCK);
	(void)fcntl(runtime.wake_write, F_SETFL,
		    fcntl(runtime.wake_write, F_GETFL, 0) | O_NONBLOCK);
	runtime.running = true;
	runtime.initialized = true;
	pthread_mutex_unlock(&runtime.lock);

	if (pthread_create(&runtime.thread, NULL, eigrp_unix_runtime_loop, NULL) != 0) {
		pthread_mutex_lock(&runtime.lock);
		runtime.running = false;
		runtime.initialized = false;
		close(runtime.wake_read);
		close(runtime.wake_write);
		runtime.wake_read = -1;
		runtime.wake_write = -1;
		pthread_mutex_unlock(&runtime.lock);
		eigrp_unix_log("unable to start runtime thread");
	}
}

void eigrp_sys_runtime_finish(void)
{
	eigrp_event_t *event;
	eigrp_unix_fd_binding_t *binding;

	pthread_mutex_lock(&runtime.lock);
	if (!runtime.initialized) {
		pthread_mutex_unlock(&runtime.lock);
		return;
	}
	runtime.running = false;
	pthread_mutex_unlock(&runtime.lock);
	eigrp_unix_wake();
	(void)pthread_join(runtime.thread, NULL);

	pthread_mutex_lock(&runtime.lock);
	while ((event = runtime.events) != NULL) {
		runtime.events = event->next;
		eigrp_unix_event_destroy(event);
	}
	while ((binding = runtime.bindings) != NULL) {
		runtime.bindings = binding->next;
		free(binding);
	}
	close(runtime.wake_read);
	close(runtime.wake_write);
	runtime.wake_read = -1;
	runtime.wake_write = -1;
	runtime.initialized = false;
	pthread_mutex_unlock(&runtime.lock);
}

void eigrp_sys_event_cancel(eigrp_event_t **owner)
{
	eigrp_event_t *event;

	if (!owner)
		return;
	pthread_mutex_lock(&runtime.lock);
	event = *owner;
	if (event) {
		*owner = NULL;
		eigrp_unix_event_unlink(event);
	}
	pthread_mutex_unlock(&runtime.lock);
	if (event) {
		free(event);
		eigrp_unix_wake();
	}
}

void eigrp_sys_event_add(eigrp_event_t **owner,
			 eigrp_event_callback_t callback, void *arg)
{
	(void)eigrp_unix_event_prepare(owner, EIGRP_UNIX_EVENT_IMMEDIATE,
				       callback, arg, 0, -1);
}

void eigrp_sys_timer_add(eigrp_event_t **owner,
			 eigrp_event_callback_t callback, void *arg,
			 uint32_t delay_msec)
{
	(void)eigrp_unix_event_prepare(owner, EIGRP_UNIX_EVENT_TIMER,
				       callback, arg, delay_msec, -1);
}

static void eigrp_unix_io_add(eigrp_event_t **owner, eigrp_instance_t *eigrp,
			      eigrp_event_callback_t callback, void *arg,
			      eigrp_unix_event_kind_t kind)
{
	int fd;

	if (!eigrp)
		return;
	pthread_mutex_lock(&runtime.lock);
	fd = eigrp_unix_fd_get_locked(eigrp);
	pthread_mutex_unlock(&runtime.lock);
	if (fd < 0)
		return;
	(void)eigrp_unix_event_prepare(owner, kind, callback, arg, 0, fd);
}

void eigrp_sys_read_add(eigrp_event_t **owner, eigrp_instance_t *eigrp,
			eigrp_event_callback_t callback, void *arg)
{
	eigrp_unix_io_add(owner, eigrp, callback, arg, EIGRP_UNIX_EVENT_READ);
}

void eigrp_sys_write_add(eigrp_event_t **owner, eigrp_instance_t *eigrp,
			 eigrp_event_callback_t callback, void *arg)
{
	eigrp_unix_io_add(owner, eigrp, callback, arg, EIGRP_UNIX_EVENT_WRITE);
}

uint32_t eigrp_sys_timer_remaining_seconds(const eigrp_event_t *event)
{
	uint64_t now;
	uint64_t remaining;

	if (!event || event->kind != EIGRP_UNIX_EVENT_TIMER)
		return 0;
	now = eigrp_unix_monotime_msec();
	if (event->due_msec <= now)
		return 0;
	remaining = event->due_msec - now;
	return (uint32_t)((remaining + 999U) / 1000U);
}

uint64_t eigrp_sys_monotime_msec(void)
{
	return eigrp_unix_monotime_msec();
}

void eigrp_sys_software_version(uint8_t *major, uint8_t *minor)
{
	if (major)
		*major = 0;
	if (minor)
		*minor = 0;
}

void eigrp_unix_runtime_fd_set(eigrp_instance_t *eigrp, int fd)
{
	eigrp_unix_fd_binding_t *binding;

	if (!eigrp || fd < 0)
		return;
	pthread_mutex_lock(&runtime.lock);
	for (binding = runtime.bindings; binding; binding = binding->next) {
		if (binding->eigrp == eigrp) {
			binding->fd = fd;
			pthread_mutex_unlock(&runtime.lock);
			eigrp_unix_wake();
			return;
		}
	}
	binding = calloc(1, sizeof(*binding));
	if (binding) {
		binding->eigrp = eigrp;
		binding->fd = fd;
		binding->next = runtime.bindings;
		runtime.bindings = binding;
	}
	pthread_mutex_unlock(&runtime.lock);
	eigrp_unix_wake();
}

void eigrp_unix_runtime_fd_clear(eigrp_instance_t *eigrp)
{
	eigrp_unix_fd_binding_t **cursor;
	eigrp_unix_fd_binding_t *binding = NULL;
	eigrp_event_t **event_cursor;

	if (!eigrp)
		return;
	pthread_mutex_lock(&runtime.lock);
	for (cursor = &runtime.bindings; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp)
			continue;
		binding = *cursor;
		*cursor = binding->next;
		break;
	}
	if (binding) {
		for (event_cursor = &runtime.events; *event_cursor;) {
			eigrp_event_t *event = *event_cursor;
			if ((event->kind == EIGRP_UNIX_EVENT_READ
			     || event->kind == EIGRP_UNIX_EVENT_WRITE)
			    && event->fd == binding->fd) {
				*event_cursor = event->next;
				eigrp_unix_event_destroy(event);
				continue;
			}
			event_cursor = &event->next;
		}
	}
	pthread_mutex_unlock(&runtime.lock);
	free(binding);
	eigrp_unix_wake();
}

static void eigrp_unix_work_queue_run(void *arg)
{
	eigrp_work_queue_t *queue = arg;
	eigrp_unix_work_item_t *item;
	eigrp_work_queue_result_t result;

	if (!queue || !queue->workfunc)
		return;
	item = queue->head;
	if (!item)
		return;
	result = queue->workfunc(queue, item->data);
	if (result == EIGRP_WORK_QUEUE_BLOCKED) {
		queue->blocked = true;
		return;
	}
	if (result == EIGRP_WORK_QUEUE_REQUEUE) {
		item = queue->head;
		queue->head = item->next;
		if (!queue->head)
			queue->tail = NULL;
		item->next = NULL;
		if (queue->tail)
			queue->tail->next = item;
		else
			queue->head = item;
		queue->tail = item;
	} else {
		queue->head = item->next;
		if (!queue->head)
			queue->tail = NULL;
		free(item);
	}
	if (queue->head)
		eigrp_sys_event_add(&queue->event, eigrp_unix_work_queue_run, queue);
}

eigrp_work_queue_t *eigrp_sys_work_queue_create(
	eigrp_instance_t *eigrp, const char *name,
	eigrp_work_queue_func_t workfunc,
	eigrp_work_queue_delete_func_t deletefunc)
{
	eigrp_work_queue_t *queue;

	if (!eigrp || !name || !name[0] || !workfunc)
		return NULL;
	queue = calloc(1, sizeof(*queue));
	if (!queue)
		return NULL;
	queue->name = strdup(name);
	if (!queue->name) {
		free(queue);
		return NULL;
	}
	queue->eigrp = eigrp;
	queue->workfunc = workfunc;
	queue->deletefunc = deletefunc;
	return queue;
}

void eigrp_sys_work_queue_clear(eigrp_work_queue_t *queue)
{
	eigrp_unix_work_item_t *item;

	if (!queue)
		return;
	eigrp_sys_event_cancel(&queue->event);
	while ((item = queue->head) != NULL) {
		queue->head = item->next;
		if (queue->deletefunc)
			queue->deletefunc(queue, item->data);
		free(item);
	}
	queue->tail = NULL;
	queue->blocked = false;
}

void eigrp_sys_work_queue_free(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;
	eigrp_sys_work_queue_clear(queue);
	free(queue->name);
	free(queue);
}

void eigrp_sys_work_queue_enqueue(eigrp_work_queue_t *queue, void *data)
{
	eigrp_unix_work_item_t *item;

	if (!queue || !data)
		return;
	item = calloc(1, sizeof(*item));
	if (!item)
		return;
	item->data = data;
	if (queue->tail)
		queue->tail->next = item;
	else
		queue->head = item;
	queue->tail = item;
	queue->blocked = false;
	if (!queue->event)
		eigrp_sys_event_add(&queue->event, eigrp_unix_work_queue_run, queue);
}

eigrp_instance_t *eigrp_sys_work_queue_instance(eigrp_work_queue_t *queue)
{
	return queue ? queue->eigrp : NULL;
}

eigrp_result_t eigrp_sys_vrf_resolve(const char *vrf_name,
				      eigrp_vrf_id_t *vrf_id)
{
	if (!vrf_id)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (vrf_name && vrf_name[0]
	    && strcmp(vrf_name, EIGRP_UNIX_DEFAULT_VRF_NAME) != 0)
		return EIGRP_RESULT_UNSUPPORTED;
	*vrf_id = EIGRP_VRF_DEFAULT;
	return EIGRP_RESULT_SUCCESS;
}
