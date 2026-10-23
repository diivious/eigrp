// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Process-wide maximum-prefix penalty. The eigrp_instance_t is one AF runtime.
 * No DUAL or neighbor teardown occurs inside the receive TLV walk.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdint.h>
#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_instance.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_redistribute.h"
#include "eigrp_timer.h"
#include "eigrp_sys.h"
#include "eigrp_process_limit.h"

#define PROCESS_LIMIT_MINUTE_MSEC UINT64_C(60000)
#define PROCESS_LIMIT_Q16 UINT32_C(65536)

static void eigrp_process_limit_timers_cancel(eigrp_instance_t *runtime)
{
	eigrp_timer_cancel(runtime, &runtime->t_process_limit_restart);
	eigrp_timer_cancel(runtime, &runtime->t_process_limit_reset);
	runtime->process_limit_restart_due_msec = 0;
	runtime->process_limit_reset_due_msec = 0;
}

static void eigrp_process_limit_timer_arm(eigrp_instance_t *runtime,
	eigrp_timer_event_t **timer, eigrp_event_callback_t callback, uint64_t due)
{
	uint64_t now = eigrp_sys_monotime_msec();
	uint64_t delta = due > now ? due - now : 1U;
	if (delta > UINT32_MAX)
		delta = UINT32_MAX;
	eigrp_timer_add(runtime, timer, callback, runtime, (uint32_t)delta);
}

static uint32_t eigrp_process_limit_decay(uint32_t score, uint64_t elapsed,
	uint64_t half_life)
{
	static const uint32_t factor[8] = {46341U, 55109U, 60097U, 62758U,
		64132U, 64830U, 65182U, 65359U};
	uint32_t fraction;
	unsigned i;
	if (!score || !elapsed || !half_life)
		return score;
	if (elapsed / half_life >= 32U)
		return 0;
	score >>= (unsigned)(elapsed / half_life);
	fraction = (uint32_t)(((elapsed % half_life) * 256U) / half_life);
	for (i = 0; i < 8; ++i)
		if (fraction & (128U >> i))
			score = (uint32_t)(((uint64_t)score * factor[i]) >> 16);
	return score;
}

/* Process suspension is a separate reason for adjacency ineligibility.
 * Never overwrite a peer's own prefix-limit state or its penalty timers. */
static void eigrp_process_limit_neighbors(eigrp_instance_t *runtime, bool release)
{
	eigrp_list_item_t *if_node, *nbr_node;
	eigrp_intf_t *intf;
	eigrp_nbr_t *nbr;
	if (!runtime->eiflist)
		return;
	for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, if_node, intf)) {
		if (!intf || !intf->nbrs)
			continue;
		for (EIGRP_LIST_ITERATE_RO(intf->nbrs, nbr_node, nbr)) {
			if (!nbr || !nbr->published)
				continue;
			if (release) {
				if (!nbr->process_limit_suspended)
					continue;
				pthread_mutex_lock(&runtime->peer_lock);
				nbr->process_limit_suspended = false;
				if (++runtime->next_neighbor_generation == 0)
					++runtime->next_neighbor_generation;
				nbr->generation = runtime->next_neighbor_generation;
				pthread_mutex_unlock(&runtime->peer_lock);
			} else {
				if (nbr->process_limit_suspended)
					continue;
				pthread_mutex_lock(&runtime->peer_lock);
				nbr->process_limit_suspended = true;
				if (++runtime->next_neighbor_generation == 0)
					++runtime->next_neighbor_generation;
				nbr->generation = runtime->next_neighbor_generation;
				pthread_mutex_unlock(&runtime->peer_lock);
				/* Already penalized peers have completed DOWN teardown.
				 * Their independent suspension must survive process release. */
				if (nbr->limit_state == EIGRP_NBR_LIMIT_NONE)
					eigrp_nbr_down(nbr);
			}
		}
	}
}

void eigrp_process_limit_cleanup(eigrp_instance_t *runtime)
{
	if (!runtime || !runtime->process_limit_cleanup_pending)
		return;
	runtime->process_limit_cleanup_pending = false;
	/* Signal an immediate remote withdrawal before suspending RTP, then
	 * suppress periodic Hellos until the process penalty is released. */
	{
		eigrp_list_item_t *node;
		eigrp_intf_t *intf;
		for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, node, intf))
			if (intf && intf->operative && !eigrp_intf_is_passive(intf))
				eigrp_hello_send(intf, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
	}
	/* Neighbor DOWN enters DUAL once no packet route walker owns descriptors. */
	eigrp_process_limit_neighbors(runtime, false);
	eigrp_redist_routes_withdraw(runtime);
}

static void eigrp_process_limit_release(eigrp_instance_t *runtime, bool reset)
{
	eigrp_process_limit_timers_cancel(runtime);
	runtime->process_limit_state = 0;
	runtime->process_limit_cleanup_pending = false;
	if (reset) {
		runtime->process_limit_restarts = 0;
		runtime->process_limit_penalty_q16 = 0;
		runtime->process_limit_penalty_updated_msec = 0;
	}
	eigrp_process_limit_neighbors(runtime, true);
	/* This can synchronously trigger another hard limit violation. */
	if (!runtime->redist_limit_state)
		eigrp_redist_routes_replay(runtime);
}

void eigrp_process_limit_restart_expired(void *arg)
{
	eigrp_instance_t *runtime = arg;
	uint64_t now;
	if (!runtime || runtime->process_limit_state != 1 ||
	    !runtime->process_limit_restart_due_msec)
		return;
	now = eigrp_sys_monotime_msec();
	if (now < runtime->process_limit_restart_due_msec) {
		eigrp_process_limit_timer_arm(runtime, &runtime->t_process_limit_restart,
			eigrp_process_limit_restart_expired,
			runtime->process_limit_restart_due_msec);
		return;
	}
	/* Preserve reset timer and incident history across automatic restarts. */
	runtime->process_limit_restart_due_msec = 0;
	runtime->process_limit_state = 0;
	eigrp_process_limit_neighbors(runtime, true);
	if (!runtime->redist_limit_state)
		eigrp_redist_routes_replay(runtime);
}

static void eigrp_process_limit_reset_expired(void *arg)
{
	eigrp_instance_t *runtime = arg;
	uint64_t now;
	if (!runtime || !runtime->process_limit_reset_due_msec)
		return;
	now = eigrp_sys_monotime_msec();
	if (now < runtime->process_limit_reset_due_msec) {
		eigrp_process_limit_timer_arm(runtime, &runtime->t_process_limit_reset,
			eigrp_process_limit_reset_expired,
			runtime->process_limit_reset_due_msec);
		return;
	}
	runtime->process_limit_reset_due_msec = 0;
	if (runtime->process_limit_state != 2) {
		runtime->process_limit_restarts = 0;
		runtime->process_limit_penalty_q16 = 0;
		runtime->process_limit_penalty_updated_msec = 0;
	}
}

void eigrp_process_limit_exceeded(eigrp_instance_t *runtime,
	const eigrp_prefix_limit_t *limit)
{
	uint64_t now, base, half_life, interval;
	uint32_t restart, reset, count;
	if (!runtime || !limit || limit->warning_only || runtime->process_limit_state)
		return;
	restart = limit->restart_minutes ? limit->restart_minutes : 5U;
	reset = limit->reset_time_minutes ? limit->reset_time_minutes : 15U;
	count = limit->restart_count ? limit->restart_count : 3U;
	now = eigrp_sys_monotime_msec();
	base = (uint64_t)restart * PROCESS_LIMIT_MINUTE_MSEC;
	half_life = base + base / 2U;
	if (limit->dampened) {
		if (runtime->process_limit_penalty_updated_msec &&
		    now > runtime->process_limit_penalty_updated_msec)
			runtime->process_limit_penalty_q16 = eigrp_process_limit_decay(
				runtime->process_limit_penalty_q16,
				now - runtime->process_limit_penalty_updated_msec, half_life);
		if (runtime->process_limit_penalty_q16 > UINT32_MAX - PROCESS_LIMIT_Q16)
			runtime->process_limit_penalty_q16 = UINT32_MAX;
		else
			runtime->process_limit_penalty_q16 += PROCESS_LIMIT_Q16;
		runtime->process_limit_penalty_updated_msec = now;
	} else {
		runtime->process_limit_penalty_q16 = 0;
		runtime->process_limit_penalty_updated_msec = 0;
	}
	if (runtime->process_limit_restarts < UINT16_MAX)
		++runtime->process_limit_restarts;
	runtime->process_limit_state = runtime->process_limit_restarts >= count ? 2 : 1;
	eigrp_process_limit_timers_cancel(runtime);
	runtime->process_limit_cleanup_pending = true;
	/* Packets must finish their TLV walk before neighbor/DUAL cleanup. */
	if (!runtime->process_limit_packet_active)
		eigrp_process_limit_cleanup(runtime);
	if (runtime->process_limit_state == 2)
		return;
	/* Saturating arithmetic prevents long dampened policies from wrapping
	 * into an artificially short restart interval. */
	interval = base;
	if (limit->dampened) {
		uint64_t score = runtime->process_limit_penalty_q16;
		if (score && base > UINT64_MAX / score)
			interval = UINT64_MAX;
		else
			interval = (base * score) / PROCESS_LIMIT_Q16;
	}
	if (interval > UINT64_MAX - now ||
	    (uint64_t)reset * PROCESS_LIMIT_MINUTE_MSEC > UINT64_MAX - now) {
		runtime->process_limit_state = 2;
		return;
	}
	runtime->process_limit_restart_due_msec = now + interval;
	runtime->process_limit_reset_due_msec = now +
		(uint64_t)reset * PROCESS_LIMIT_MINUTE_MSEC;
	eigrp_process_limit_timer_arm(runtime, &runtime->t_process_limit_restart,
		eigrp_process_limit_restart_expired, runtime->process_limit_restart_due_msec);
	eigrp_process_limit_timer_arm(runtime, &runtime->t_process_limit_reset,
		eigrp_process_limit_reset_expired, runtime->process_limit_reset_due_msec);
	if (!runtime->t_process_limit_restart || !runtime->t_process_limit_reset) {
		eigrp_process_limit_timers_cancel(runtime);
		runtime->process_limit_state = 2;
	}
}

eigrp_result_t eigrp_process_limit_clear(eigrp_instance_t *runtime)
{
	if (!runtime)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_instance_thread_dispatch_needed(runtime))
		return EIGRP_RESULT_CONFLICT;
	if (runtime->process_limit_state) {
		eigrp_process_limit_release(runtime, true);
	} else {
		/* Automatic restart restores admission, not the incident history.
		 * An explicit clear during the reset window must erase that history
		 * without replaying RIB sources or disturbing healthy neighbors. */
		eigrp_process_limit_timers_cancel(runtime);
		runtime->process_limit_restarts = 0;
		runtime->process_limit_penalty_q16 = 0;
		runtime->process_limit_penalty_updated_msec = 0;
	}
	return EIGRP_RESULT_SUCCESS;
}
