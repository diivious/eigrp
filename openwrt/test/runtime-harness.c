// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt event-runtime UUT. Runs on Linux and macOS without OpenWrt. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "eigrp_sys.h"
#include "eigrp_owrt_runtime.h"

static atomic_int completed;
static atomic_int discarded;
static atomic_int queue_runs;
static int pipefd[2];

static void sleep_msec(long duration)
{
	struct timespec ts = { .tv_sec = duration / 1000,
		.tv_nsec = (duration % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) == -1)
		;
}

static bool wait_count(atomic_int *value, int expected, int max_msec)
{
	int elapsed;
	for (elapsed = 0; elapsed < max_msec; elapsed += 5) {
		if (atomic_load(value) >= expected)
			return true;
		sleep_msec(5);
	}
	return atomic_load(value) >= expected;
}

static void count_cb(void *unused)
{
	(void)unused;
	atomic_fetch_add(&completed, 1);
}

static void read_cb(void *arg)
{
	char byte;
	int fd = *(int *)arg;
	if (read(fd, &byte, sizeof(byte)) == 1)
		atomic_fetch_add(&completed, 1);
}

static eigrp_work_queue_result_t queue_cb(eigrp_work_queue_t *queue, void *arg)
{
	int *phase = arg;
	(void)queue;
	atomic_fetch_add(&queue_runs, 1);
	if (*phase == 0) {
		*phase = 1;
		return EIGRP_WORK_QUEUE_REQUEUE;
	}
	if (*phase == 2)
		return EIGRP_WORK_QUEUE_BLOCKED;
	atomic_fetch_add(&completed, 1);
	return EIGRP_WORK_QUEUE_SUCCESS;
}

static void queue_delete(eigrp_work_queue_t *queue, void *arg)
{
	(void)queue;
	(void)arg;
	atomic_fetch_add(&discarded, 1);
}

int main(void)
{
	eigrp_event_t *event = NULL, *timer = NULL, *read_event = NULL;
	eigrp_event_t *other_event = NULL;
	eigrp_work_queue_t *queue;
	char mock_instance, mock_other;
	eigrp_instance_t *instance = (eigrp_instance_t *)&mock_instance;
	eigrp_instance_t *other = (eigrp_instance_t *)&mock_other;
	int phase = 0, blocked = 2;
	int shared[2], replacement[2];
	uint64_t before;
	uint8_t major, minor;

	assert(eigrp_sys_monotime_msec() > 0);
	assert(eigrp_sys_wallclock_msec() > 0);
	eigrp_sys_software_version(&major, &minor);
	assert(major == 0 && minor == 0);

	eigrp_sys_runtime_init();
	eigrp_sys_runtime_init(); /* idempotent */
	eigrp_sys_event_add(&event, count_cb, NULL);
	assert(wait_count(&completed, 1, 1000));
	assert(event == NULL);

	before = eigrp_sys_monotime_msec();
	eigrp_sys_timer_add(&timer, count_cb, NULL, 75);
	assert(timer != NULL);
	assert(eigrp_sys_timer_remaining_seconds(timer) >= 1);
	assert(wait_count(&completed, 2, 1000));
	assert(eigrp_sys_monotime_msec() >= before + 65);
	assert(timer == NULL);

	eigrp_sys_timer_add(&timer, count_cb, NULL, 500);
	assert(timer != NULL);
	eigrp_sys_event_cancel(&timer);
	assert(timer == NULL);
	sleep_msec(125);
	assert(atomic_load(&completed) == 2);

	assert(pipe(pipefd) == 0);
	assert(eigrp_owrt_runtime_fd_set(instance, pipefd[0]));
	eigrp_sys_read_add(&read_event, instance, read_cb, &pipefd[0]);
	assert(read_event != NULL);
	assert(write(pipefd[1], "x", 1) == 1);
	assert(wait_count(&completed, 3, 1000));
	assert(read_event == NULL);

	eigrp_sys_read_add(&read_event, instance, read_cb, &pipefd[0]);
	assert(read_event);
	eigrp_owrt_runtime_fd_clear(instance);
	assert(read_event == NULL);
	assert(write(pipefd[1], "y", 1) == 1);
	sleep_msec(50);
	assert(atomic_load(&completed) == 3);
	close(pipefd[0]); close(pipefd[1]);

	/* Clearing or rebinding one instance must not cancel an unrelated
	 * instance using the same descriptor. */
	assert(pipe(shared) == 0);
	assert(pipe(replacement) == 0);
	assert(eigrp_owrt_runtime_fd_set(instance, shared[0]));
	assert(eigrp_owrt_runtime_fd_set(other, shared[0]));
	eigrp_sys_read_add(&read_event, instance, read_cb, &shared[0]);
	eigrp_sys_read_add(&other_event, other, read_cb, &shared[0]);
	assert(read_event && other_event);
	assert(eigrp_owrt_runtime_fd_set(instance, replacement[0]));
	assert(read_event == NULL);
	assert(other_event != NULL);
	eigrp_owrt_runtime_fd_clear(other);
	assert(other_event == NULL);
	close(shared[0]); close(shared[1]);
	close(replacement[0]); close(replacement[1]);
	eigrp_owrt_runtime_fd_clear(instance);

	/* The host protocol lock serializes control-plane mutation and callbacks. */
	eigrp_owrt_runtime_enter();
	eigrp_sys_event_add(&event, count_cb, NULL);
	sleep_msec(40);
	assert(atomic_load(&completed) == 3);
	eigrp_owrt_runtime_leave();
	assert(wait_count(&completed, 4, 1000));

	queue = eigrp_sys_work_queue_create(instance, "OW04", queue_cb, queue_delete);
	assert(queue);
	assert(eigrp_sys_work_queue_instance(queue) == instance);
	eigrp_sys_work_queue_enqueue(queue, &phase);
	assert(wait_count(&completed, 5, 1000));
	assert(atomic_load(&queue_runs) >= 2);
	eigrp_sys_work_queue_enqueue(queue, &blocked);
	assert(wait_count(&queue_runs, 3, 1000));
	/* BLOCKED item remains owned by the queue until cleared. */
	eigrp_sys_work_queue_clear(queue);
	assert(atomic_load(&discarded) == 1);
	eigrp_sys_work_queue_free(queue);

	eigrp_sys_timer_add(&timer, count_cb, NULL, 10000);
	assert(timer != NULL);
	eigrp_sys_runtime_finish();
	assert(timer == NULL);
	eigrp_sys_runtime_finish(); /* idempotent */
	eigrp_sys_event_add(&event, count_cb, NULL);
	assert(event == NULL);
	eigrp_sys_runtime_init(); /* restart supported */
	eigrp_sys_event_add(&event, count_cb, NULL);
	assert(wait_count(&completed, 6, 1000));
	eigrp_sys_runtime_finish();

	puts("OpenWrt runtime: event/timer/cancel/fd-rebind/lock/queue/shutdown/restart PASS");
	return 0;
}
