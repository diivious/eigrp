// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "eigrp_sys.h"
#include "eigrp_unix.h"

typedef struct signal_state {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned count;
	int fd;
} signal_state_t;

static void state_init(signal_state_t *state)
{
	pthread_mutex_init(&state->lock, NULL);
	pthread_cond_init(&state->cond, NULL);
	state->count = 0;
	state->fd = -1;
}

static int state_wait(signal_state_t *state, unsigned target, unsigned msec)
{
	struct timespec deadline;
	int result = 0;

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += msec / 1000U;
	deadline.tv_nsec += (long)(msec % 1000U) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&state->lock);
	while (state->count < target && result == 0)
		result = pthread_cond_timedwait(&state->cond, &state->lock, &deadline);
	result = state->count >= target;
	pthread_mutex_unlock(&state->lock);
	return result;
}

static void callback(void *arg)
{
	signal_state_t *state = arg;
	char byte;

	if (state->fd >= 0)
		(void)read(state->fd, &byte, sizeof(byte));
	pthread_mutex_lock(&state->lock);
	state->count++;
	pthread_cond_broadcast(&state->cond);
	pthread_mutex_unlock(&state->lock);
}

static eigrp_work_queue_result_t work_callback(eigrp_work_queue_t *queue,
						void *data)
{
	signal_state_t *state = data;

	assert(eigrp_sys_work_queue_instance(queue) != NULL);
	callback(state);
	return EIGRP_WORK_QUEUE_SUCCESS;
}

static void test_events_and_timers(void)
{
	signal_state_t state;
	eigrp_event_t *event = NULL;
	uint64_t before;
	uint64_t after;

	state_init(&state);
	before = eigrp_sys_monotime_msec();
	eigrp_sys_event_add(&event, callback, &state);
	assert(event != NULL);
	assert(state_wait(&state, 1, 1000));
	assert(event == NULL);
	after = eigrp_sys_monotime_msec();
	assert(after >= before);

	eigrp_sys_timer_add(&event, callback, &state, 150);
	assert(event != NULL);
	assert(eigrp_sys_timer_remaining_seconds(event) == 1);
	assert(state_wait(&state, 2, 1000));
	assert(event == NULL);

	eigrp_sys_timer_add(&event, callback, &state, 200);
	eigrp_sys_event_cancel(&event);
	assert(event == NULL);
	{ struct timespec pause = {0, 300000000L}; nanosleep(&pause, NULL); }
	assert(state.count == 2);
}

static void test_read_write_events(void)
{
	int fds[2];
	int sockets[2];
	char byte = 1;
	signal_state_t read_state;
	signal_state_t write_state;
	eigrp_event_t *event = NULL;
	eigrp_instance_t *read_instance = (eigrp_instance_t *)(uintptr_t)0x1;
	eigrp_instance_t *write_instance = (eigrp_instance_t *)(uintptr_t)0x2;

	state_init(&read_state);
	state_init(&write_state);
	assert(pipe(fds) == 0);
	read_state.fd = fds[0];
	eigrp_unix_runtime_fd_set(read_instance, fds[0]);
	eigrp_sys_read_add(&event, read_instance, callback, &read_state);
	assert(event != NULL);
	assert(write(fds[1], &byte, sizeof(byte)) == sizeof(byte));
	assert(state_wait(&read_state, 1, 1000));
	assert(event == NULL);
	eigrp_unix_runtime_fd_clear(read_instance);
	close(fds[0]);
	close(fds[1]);

	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
	eigrp_unix_runtime_fd_set(write_instance, sockets[0]);
	eigrp_sys_write_add(&event, write_instance, callback, &write_state);
	assert(event != NULL);
	assert(state_wait(&write_state, 1, 1000));
	assert(event == NULL);
	eigrp_unix_runtime_fd_clear(write_instance);
	close(sockets[0]);
	close(sockets[1]);
}

static void test_work_queue(void)
{
	signal_state_t first;
	signal_state_t second;
	eigrp_instance_t *instance = (eigrp_instance_t *)(uintptr_t)0x3;
	eigrp_work_queue_t *queue;

	state_init(&first);
	state_init(&second);
	queue = eigrp_sys_work_queue_create(instance, "runtime-test",
					    work_callback, NULL);
	assert(queue != NULL);
	assert(eigrp_sys_work_queue_instance(queue) == instance);
	eigrp_sys_work_queue_enqueue(queue, &first);
	eigrp_sys_work_queue_enqueue(queue, &second);
	assert(state_wait(&first, 1, 1000));
	assert(state_wait(&second, 1, 1000));
	eigrp_sys_work_queue_free(queue);
}

int main(void)
{
	eigrp_sys_runtime_init();
	test_events_and_timers();
	test_read_write_events();
	test_work_queue();
	eigrp_sys_runtime_finish();

	/* Lifecycle calls are idempotent and a stopped runtime accepts no events. */
	eigrp_sys_runtime_finish();
	eigrp_sys_runtime_init();
	eigrp_sys_runtime_finish();
	puts("unix runtime tests: PASS");
	return 0;
}
