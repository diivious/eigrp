// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenWrt host event scheduler and work queues for the portable EIGRP API.
 * Linux/poll/pthreads implementation, independent of OpenWrt service, socket,
 * interface and RIB integration. Packet descriptors remain adapter-owned.
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
#include "eigrp_owrt_runtime.h"

typedef enum eigrp_owrt_event_kind {
	EIGRP_OWRT_EVENT_NOW,
	EIGRP_OWRT_EVENT_TIMER,
	EIGRP_OWRT_EVENT_READ,
	EIGRP_OWRT_EVENT_WRITE,
} eigrp_owrt_event_kind_t;

struct eigrp_event {
	eigrp_owrt_event_kind_t kind;
	eigrp_event_callback_t callback;
	void *arg;
	eigrp_event_t **owner;
	uint64_t id;
	uint64_t due_msec;
	int fd;
	eigrp_instance_t *instance; /* Private identity for fd cancellation. */
	struct eigrp_event *next;
};

typedef struct eigrp_owrt_binding {
	eigrp_instance_t *instance;
	int fd;
	struct eigrp_owrt_binding *next;
} eigrp_owrt_binding_t;

typedef struct eigrp_owrt_item {
	void *data;
	struct eigrp_owrt_item *next;
} eigrp_owrt_item_t;

struct eigrp_work_queue {
	eigrp_instance_t *instance;
	char *name;
	eigrp_work_queue_func_t workfunc;
	eigrp_work_queue_delete_func_t deletefunc;
	eigrp_owrt_item_t *head;
	eigrp_owrt_item_t *tail;
	eigrp_event_t *event;
	bool blocked;
};

typedef struct eigrp_owrt_poll_ref {
	eigrp_event_t *event; /* Pointer identity ONLY; never deref after poll. */
	uint64_t id;         /* Prevent reusing an event allocation (ABA). */
} eigrp_owrt_poll_ref_t;

typedef struct eigrp_owrt_scheduler {
	pthread_mutex_t lock;
	pthread_t thread;
	bool running;
	bool started;
	int wake_read;
	int wake_write;
	uint64_t next_id;
	eigrp_event_t *events;
	eigrp_owrt_binding_t *bindings;
} eigrp_owrt_scheduler_t;

static eigrp_owrt_scheduler_t runtime = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.wake_read = -1,
	.wake_write = -1,
};

/* Serializes callbacks with host-side protocol state changes. */
static pthread_mutex_t protocol_lock;
static pthread_once_t protocol_lock_once = PTHREAD_ONCE_INIT;

static void eigrp_owrt_protocol_lock_init(void)
{
	pthread_mutexattr_t attributes;
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&protocol_lock, &attributes);
	pthread_mutexattr_destroy(&attributes);
}

void eigrp_owrt_runtime_enter(void)
{
	pthread_once(&protocol_lock_once, eigrp_owrt_protocol_lock_init);
	pthread_mutex_lock(&protocol_lock);
}

void eigrp_owrt_runtime_leave(void)
{
	pthread_mutex_unlock(&protocol_lock);
}

static uint64_t eigrp_owrt_clock(clockid_t kind)
{
	struct timespec ts;

	if (clock_gettime(kind, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

uint64_t eigrp_sys_monotime_msec(void)
{
	return eigrp_owrt_clock(CLOCK_MONOTONIC);
}

uint64_t eigrp_sys_wallclock_msec(void)
{
	return eigrp_owrt_clock(CLOCK_REALTIME);
}

void eigrp_sys_software_version(uint8_t *major, uint8_t *minor)
{
	if (major)
		*major = 0;
	if (minor)
		*minor = 0;
}

static void eigrp_owrt_wake(void)
{
	uint8_t token = 1;
	ssize_t count;

	/* Descriptor lifetime is synchronized by runtime.lock. */
	pthread_mutex_lock(&runtime.lock);
	if (runtime.wake_write >= 0) {
		count = write(runtime.wake_write, &token, sizeof(token));
		(void)count; /* Nonblocking wake pipe: full means already awake. */
	}
	pthread_mutex_unlock(&runtime.lock);
}

static void eigrp_owrt_drain(int fd)
{
	uint8_t bytes[64];
	while (read(fd, bytes, sizeof(bytes)) > 0)
		;
}

/* Caller holds runtime.lock. */
static void eigrp_owrt_event_destroy(eigrp_event_t *event)
{
	if (event->owner && *event->owner == event)
		*event->owner = NULL;
	free(event);
}

/* Caller holds runtime.lock. */
static bool eigrp_owrt_event_remove(eigrp_event_t *event)
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

void eigrp_sys_event_cancel(eigrp_event_t **owner)
{
	eigrp_event_t *event;
	bool found = false;

	if (!owner)
		return;
	pthread_mutex_lock(&runtime.lock);
	event = *owner;
	if (event) {
		*owner = NULL;
		found = eigrp_owrt_event_remove(event);
		if (found)
			free(event);
	}
	pthread_mutex_unlock(&runtime.lock);
	if (found)
		eigrp_owrt_wake();
}

/* Caller supplies an instance only for fd readiness events. Binding and
 * event installation share runtime.lock to prevent stale-fd registration. */
static int eigrp_owrt_binding_fd(eigrp_instance_t *instance);
static void eigrp_owrt_event_add(eigrp_event_t **owner,
		eigrp_owrt_event_kind_t kind, eigrp_event_callback_t callback,
		void *arg, uint32_t delay_msec, eigrp_instance_t *instance)
{
	eigrp_event_t *event;
	eigrp_event_t *old;

	if (!owner || !callback)
		return;
	event = calloc(1, sizeof(*event));
	if (!event)
		return;
	event->kind = kind;
	event->callback = callback;
	event->arg = arg;
	event->owner = owner;
	event->due_msec = eigrp_sys_monotime_msec() + delay_msec;
	event->fd = -1;
	event->instance = instance;

	pthread_mutex_lock(&runtime.lock);
	if (instance) {
		event->fd = eigrp_owrt_binding_fd(instance);
		if (event->fd < 0) {
			pthread_mutex_unlock(&runtime.lock);
			free(event);
			return;
		}
	}
	/* Replace the existing owner atomically rather than cancel + add. */
	old = *owner;
	if (old && eigrp_owrt_event_remove(old))
		free(old);
	*owner = NULL;
	if (!runtime.running) {
		pthread_mutex_unlock(&runtime.lock);
		free(event);
		return;
	}
	event->id = ++runtime.next_id;
	event->next = runtime.events;
	runtime.events = event;
	*owner = event;
	pthread_mutex_unlock(&runtime.lock);
	eigrp_owrt_wake();
}

void eigrp_sys_event_add(eigrp_event_t **event,
		eigrp_event_callback_t callback, void *arg)
{
	eigrp_owrt_event_add(event, EIGRP_OWRT_EVENT_NOW,
			callback, arg, 0, NULL);
}

void eigrp_sys_timer_add(eigrp_event_t **event,
		eigrp_event_callback_t callback, void *arg, uint32_t delay_msec)
{
	eigrp_owrt_event_add(event, EIGRP_OWRT_EVENT_TIMER,
			callback, arg, delay_msec, NULL);
}

uint32_t eigrp_sys_timer_remaining_seconds(const eigrp_event_t *event)
{
	uint64_t now, delta;
	uint32_t remaining = 0;
	const eigrp_event_t *cursor;
	if (!event)
		return 0;
	pthread_mutex_lock(&runtime.lock);
	for (cursor = runtime.events; cursor; cursor = cursor->next) {
		if (cursor != event || cursor->kind != EIGRP_OWRT_EVENT_TIMER)
			continue;
		now = eigrp_sys_monotime_msec();
		if (cursor->due_msec > now) {
			delta = (cursor->due_msec - now + 999U) / 1000U;
			remaining = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
		}
		break;
	}
	pthread_mutex_unlock(&runtime.lock);
	return remaining;
}

/* Caller holds runtime.lock. */
static int eigrp_owrt_binding_fd(eigrp_instance_t *instance)
{
	eigrp_owrt_binding_t *binding;
	for (binding = runtime.bindings; binding; binding = binding->next)
		if (binding->instance == instance)
			return binding->fd;
	return -1;
}

void eigrp_sys_read_add(eigrp_event_t **event, eigrp_instance_t *instance,
		eigrp_event_callback_t callback, void *arg)
{
	if (instance)
		eigrp_owrt_event_add(event, EIGRP_OWRT_EVENT_READ,
			callback, arg, 0, instance);
}

void eigrp_sys_write_add(eigrp_event_t **event, eigrp_instance_t *instance,
		eigrp_event_callback_t callback, void *arg)
{
	if (instance)
		eigrp_owrt_event_add(event, EIGRP_OWRT_EVENT_WRITE,
			callback, arg, 0, instance);
}

/* Caller holds runtime.lock. */
static void eigrp_owrt_fd_events_clear(eigrp_instance_t *instance)
{
	eigrp_event_t **cursor = &runtime.events;
	while (*cursor) {
		eigrp_event_t *event = *cursor;
		if ((event->kind == EIGRP_OWRT_EVENT_READ ||
		     event->kind == EIGRP_OWRT_EVENT_WRITE) &&
		    event->instance == instance) {
			*cursor = event->next;
			eigrp_owrt_event_destroy(event);
			continue;
		}
		cursor = &event->next;
	}
}

bool eigrp_owrt_runtime_fd_set(eigrp_instance_t *instance, int fd)
{
	eigrp_owrt_binding_t *binding;
	if (!instance || fd < 0)
		return false;
	pthread_mutex_lock(&runtime.lock);
	if (!runtime.running) {
		pthread_mutex_unlock(&runtime.lock);
		return false;
	}
	for (binding = runtime.bindings; binding; binding = binding->next) {
		if (binding->instance != instance)
			continue;
		if (binding->fd != fd) {
			eigrp_owrt_fd_events_clear(instance);
			binding->fd = fd;
		}
		pthread_mutex_unlock(&runtime.lock);
		eigrp_owrt_wake();
		return true;
	}
	binding = calloc(1, sizeof(*binding));
	if (!binding) {
		pthread_mutex_unlock(&runtime.lock);
		return false;
	}
	binding->instance = instance;
	binding->fd = fd;
	binding->next = runtime.bindings;
	runtime.bindings = binding;
	pthread_mutex_unlock(&runtime.lock);
	eigrp_owrt_wake();
	return true;
}

void eigrp_owrt_runtime_fd_clear(eigrp_instance_t *instance)
{
	eigrp_owrt_binding_t **cursor, *binding = NULL;
	if (!instance)
		return;
	pthread_mutex_lock(&runtime.lock);
	for (cursor = &runtime.bindings; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->instance != instance)
			continue;
		binding = *cursor;
		*cursor = binding->next;
		eigrp_owrt_fd_events_clear(instance);
		break;
	}
	pthread_mutex_unlock(&runtime.lock);
	free(binding);
	if (binding)
		eigrp_owrt_wake();
}

/* Caller holds runtime.lock. fd_map entries are never dereferenced once poll
 * begins; only their pointer+generation identities are compared under lock. */
static size_t eigrp_owrt_poll_snapshot(struct pollfd **fds,
		eigrp_owrt_poll_ref_t **refs)
{
	eigrp_event_t *event;
	struct pollfd *out;
	eigrp_owrt_poll_ref_t *map;
	size_t count = 1, i = 1;
	for (event = runtime.events; event; event = event->next)
		if (event->kind == EIGRP_OWRT_EVENT_READ ||
		    event->kind == EIGRP_OWRT_EVENT_WRITE)
			count++;
	out = calloc(count, sizeof(*out));
	map = calloc(count, sizeof(*map));
	if (!out || !map) {
		free(out);
		free(map);
		return 0;
	}
	out[0].fd = runtime.wake_read;
	out[0].events = POLLIN;
	for (event = runtime.events; event; event = event->next) {
		if (event->kind != EIGRP_OWRT_EVENT_READ &&
		    event->kind != EIGRP_OWRT_EVENT_WRITE)
			continue;
		out[i].fd = event->fd;
		out[i].events = event->kind == EIGRP_OWRT_EVENT_READ ? POLLIN : POLLOUT;
		map[i].event = event;
		map[i].id = event->id;
		i++;
	}
	*fds = out;
	*refs = map;
	return count;
}

/* Caller holds runtime.lock. */
static int eigrp_owrt_poll_timeout(void)
{
	eigrp_event_t *event;
	uint64_t due = UINT64_MAX, now, delay;
	for (event = runtime.events; event; event = event->next) {
		if (event->kind == EIGRP_OWRT_EVENT_NOW)
			return 0;
		if (event->kind == EIGRP_OWRT_EVENT_TIMER && event->due_msec < due)
			due = event->due_msec;
	}
	if (due == UINT64_MAX)
		return -1;
	now = eigrp_sys_monotime_msec();
	if (due <= now)
		return 0;
	delay = due - now;
	return delay > (uint64_t)INT_MAX ? INT_MAX : (int)delay;
}

/* Caller holds protocol_lock then runtime.lock. */
static eigrp_event_t *eigrp_owrt_ready_take(struct pollfd *fds,
		eigrp_owrt_poll_ref_t *refs, size_t count)
{
	eigrp_event_t *event, *cursor;
	uint64_t now = eigrp_sys_monotime_msec();
	size_t i;
	for (event = runtime.events; event; event = event->next) {
		if (event->kind == EIGRP_OWRT_EVENT_NOW ||
		    (event->kind == EIGRP_OWRT_EVENT_TIMER && event->due_msec <= now)) {
			(void)eigrp_owrt_event_remove(event);
			if (event->owner && *event->owner == event)
				*event->owner = NULL;
			return event;
		}
	}
	for (i = 1; i < count; ++i) {
		if (!fds[i].revents)
			continue;
		for (cursor = runtime.events; cursor; cursor = cursor->next) {
			if (cursor == refs[i].event && cursor->id == refs[i].id) {
				(void)eigrp_owrt_event_remove(cursor);
				if (cursor->owner && *cursor->owner == cursor)
					*cursor->owner = NULL;
				return cursor;
			}
		}
	}
	return NULL;
}

static void *eigrp_owrt_loop(void *unused)
{
	(void)unused;
	for (;;) {
		struct pollfd *fds = NULL;
		eigrp_owrt_poll_ref_t *refs = NULL;
		eigrp_event_t *event;
		eigrp_event_callback_t callback;
		void *arg;
		size_t count;
		int timeout, status;

		pthread_mutex_lock(&runtime.lock);
		if (!runtime.running) {
			pthread_mutex_unlock(&runtime.lock);
			break;
		}
		timeout = eigrp_owrt_poll_timeout();
		count = eigrp_owrt_poll_snapshot(&fds, &refs);
		pthread_mutex_unlock(&runtime.lock);
		if (!count) {
			fprintf(stderr, "OpenEIGRP OpenWrt runtime: poll allocation failed\n");
			pthread_mutex_lock(&runtime.lock);
			runtime.running = false;
			pthread_mutex_unlock(&runtime.lock);
			break;
		}
		status = poll(fds, count, timeout);
		if (status < 0 && errno != EINTR)
			fprintf(stderr, "OpenEIGRP OpenWrt runtime: poll failed: %d\n", errno);
		if (fds[0].revents & POLLIN)
			eigrp_owrt_drain(fds[0].fd);

		/* Host mutation and handoff must be serialized: canceled callbacks
		 * cannot execute after the host destroys their protocol objects. */
		eigrp_owrt_runtime_enter();
		pthread_mutex_lock(&runtime.lock);
		event = runtime.running ? eigrp_owrt_ready_take(fds, refs, count) : NULL;
		pthread_mutex_unlock(&runtime.lock);
		free(fds);
		free(refs);
		if (!event) {
			eigrp_owrt_runtime_leave();
			continue;
		}
		callback = event->callback;
		arg = event->arg;
		free(event);
		callback(arg);
		eigrp_owrt_runtime_leave();
	}
	return NULL;
}

void eigrp_sys_runtime_init(void)
{
	int pipefd[2], flags;
	pthread_mutex_lock(&runtime.lock);
	if (runtime.started) {
		pthread_mutex_unlock(&runtime.lock);
		return;
	}
	if (pipe(pipefd) != 0) {
		pthread_mutex_unlock(&runtime.lock);
		fprintf(stderr, "OpenEIGRP OpenWrt runtime: pipe failed\n");
		return;
	}
	flags = fcntl(pipefd[0], F_GETFL, 0);
	if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0) {
		close(pipefd[0]); close(pipefd[1]);
		pthread_mutex_unlock(&runtime.lock);
		return;
	}
	flags = fcntl(pipefd[1], F_GETFL, 0);
	if (flags < 0 || fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) != 0) {
		close(pipefd[0]); close(pipefd[1]);
		pthread_mutex_unlock(&runtime.lock);
		return;
	}
	runtime.wake_read = pipefd[0];
	runtime.wake_write = pipefd[1];
	runtime.running = true;
	runtime.started = true;
	if (pthread_create(&runtime.thread, NULL, eigrp_owrt_loop, NULL) != 0) {
		close(pipefd[0]); close(pipefd[1]);
		runtime.wake_read = -1;
		runtime.wake_write = -1;
		runtime.running = false;
		runtime.started = false;
		fprintf(stderr, "OpenEIGRP OpenWrt runtime: thread start failed\n");
	}
	pthread_mutex_unlock(&runtime.lock);
}

void eigrp_sys_runtime_finish(void)
{
	eigrp_event_t *event;
	eigrp_owrt_binding_t *binding;
	pthread_mutex_lock(&runtime.lock);
	if (!runtime.started) {
		pthread_mutex_unlock(&runtime.lock);
		return;
	}
	/* Completion must run on the control thread, not from a callback. */
	if (pthread_equal(pthread_self(), runtime.thread)) {
		pthread_mutex_unlock(&runtime.lock);
		fprintf(stderr, "OpenEIGRP OpenWrt runtime: finish from callback rejected\n");
		return;
	}
	runtime.running = false;
	pthread_mutex_unlock(&runtime.lock);
	eigrp_owrt_wake();
	(void)pthread_join(runtime.thread, NULL);
	pthread_mutex_lock(&runtime.lock);
	while ((event = runtime.events) != NULL) {
		runtime.events = event->next;
		eigrp_owrt_event_destroy(event);
	}
	while ((binding = runtime.bindings) != NULL) {
		runtime.bindings = binding->next;
		free(binding);
	}
	close(runtime.wake_read);
	close(runtime.wake_write);
	runtime.wake_read = -1;
	runtime.wake_write = -1;
	runtime.started = false;
	pthread_mutex_unlock(&runtime.lock);
}

static void eigrp_owrt_work_run(void *arg)
{
	eigrp_work_queue_t *queue = arg;
	eigrp_owrt_item_t *item;
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
		eigrp_sys_event_add(&queue->event, eigrp_owrt_work_run, queue);
}

eigrp_work_queue_t *eigrp_sys_work_queue_create(
		eigrp_instance_t *instance, const char *name,
		eigrp_work_queue_func_t workfunc,
		eigrp_work_queue_delete_func_t deletefunc)
{
	eigrp_work_queue_t *queue;
	if (!instance || !name || !*name || !workfunc)
		return NULL;
	queue = calloc(1, sizeof(*queue));
	if (!queue)
		return NULL;
	queue->name = strdup(name);
	if (!queue->name) {
		free(queue);
		return NULL;
	}
	queue->instance = instance;
	queue->workfunc = workfunc;
	queue->deletefunc = deletefunc;
	return queue;
}

void eigrp_sys_work_queue_clear(eigrp_work_queue_t *queue)
{
	eigrp_owrt_item_t *item;
	if (!queue)
		return;
	eigrp_owrt_runtime_enter();
	eigrp_sys_event_cancel(&queue->event);
	while ((item = queue->head) != NULL) {
		queue->head = item->next;
		if (queue->deletefunc)
			queue->deletefunc(queue, item->data);
		free(item);
	}
	queue->tail = NULL;
	queue->blocked = false;
	eigrp_owrt_runtime_leave();
}

void eigrp_sys_work_queue_free(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;
	eigrp_owrt_runtime_enter();
	eigrp_sys_work_queue_clear(queue);
	free(queue->name);
	free(queue);
	eigrp_owrt_runtime_leave();
}

void eigrp_sys_work_queue_enqueue(eigrp_work_queue_t *queue, void *data)
{
	eigrp_owrt_item_t *item;
	if (!queue || !data)
		return;
	item = calloc(1, sizeof(*item));
	if (!item)
		return;
	item->data = data;
	eigrp_owrt_runtime_enter();
	if (queue->tail)
		queue->tail->next = item;
	else
		queue->head = item;
	queue->tail = item;
	queue->blocked = false;
	if (!queue->event)
		eigrp_sys_event_add(&queue->event, eigrp_owrt_work_run, queue);
	eigrp_owrt_runtime_leave();
}

eigrp_instance_t *eigrp_sys_work_queue_instance(eigrp_work_queue_t *queue)
{
	return queue ? queue->instance : NULL;
}
