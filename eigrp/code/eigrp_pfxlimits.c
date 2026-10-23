// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AF-owned topology maximum-prefix limit. Each eigrp_instance_t is one AF runtime.
 * No DUAL or neighbor teardown occurs inside the receive TLV walk.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdint.h>
#include <stdlib.h>
#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_instance.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_topology.h"
#include "eigrp_packet.h"
#include "eigrp_redistribute.h"
#include "eigrp_timer.h"
#include "eigrp_sys.h"
#include "eigrp_pfxlimits.h"

#define PFXLIMITS_MINUTE_MSEC UINT64_C(60000)
#define PFXLIMITS_Q16 UINT32_C(65536)

static void eigrp_pfxlimits_timers_cancel(eigrp_instance_t *runtime)
{
	eigrp_timer_cancel(runtime, &runtime->t_pfxlimits_af_restart);
	eigrp_timer_cancel(runtime, &runtime->t_pfxlimits_af_reset);
	runtime->eigrp_pfxlimits_af_restart_due_msec = 0;
	runtime->eigrp_pfxlimits_af_reset_due_msec = 0;
}

static void eigrp_pfxlimits_timer_arm(eigrp_instance_t *runtime,
	eigrp_timer_event_t **timer, eigrp_event_callback_t callback, uint64_t due)
{
	uint64_t now = eigrp_sys_monotime_msec();
	uint64_t delta = due > now ? due - now : 1U;
	if (delta > UINT32_MAX)
		delta = UINT32_MAX;
	eigrp_timer_add(runtime, timer, callback, runtime, (uint32_t)delta);
}

static uint32_t eigrp_pfxlimits_penalty_decay(uint32_t score, uint64_t elapsed,
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

/* AF topology-limit suspension is a separate reason for adjacency ineligibility.
 * Never overwrite a peer's own prefix-limit state or its penalty timers. */
static void eigrp_pfxlimits_nbr_update(eigrp_instance_t *runtime, bool release)
{
	eigrp_list_item_t *if_node, *nbr_node;
	eigrp_intf_t *intf;
	eigrp_nbr_t *nbr;
	eigrp_nbr_t **peers = NULL;
	size_t capacity = 0;

	if (!runtime->eiflist)
		return;
	if (!release) {
		/* Allocate before changing any adjacency. On failure, use
		 * complete individual neighbor teardown. */
		for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, if_node, intf)) {
			if (!intf || !intf->nbrs)
				continue;
			for (EIGRP_LIST_ITERATE_RO(intf->nbrs, nbr_node, nbr))
				if (nbr && nbr->published
				    && nbr->limit_state == EIGRP_NBR_LIMIT_NONE)
					++capacity;
		}
		if (capacity)
			peers = calloc(capacity, sizeof(*peers));
	}
	for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, if_node, intf)) {
		if (!intf || !intf->nbrs)
			continue;
		for (EIGRP_LIST_ITERATE_RO(intf->nbrs, nbr_node, nbr)) {
			if (!nbr || !nbr->published)
				continue;
			if (release) {
				if (!nbr->eigrp_pfxlimits_af_suspended)
					continue;
				pthread_mutex_lock(&runtime->peer_lock);
				nbr->eigrp_pfxlimits_af_suspended = false;
				if (++runtime->next_neighbor_generation == 0)
					++runtime->next_neighbor_generation;
				nbr->generation = runtime->next_neighbor_generation;
				pthread_mutex_unlock(&runtime->peer_lock);
			} else {
				if (nbr->eigrp_pfxlimits_af_suspended)
					continue;
				pthread_mutex_lock(&runtime->peer_lock);
				nbr->eigrp_pfxlimits_af_suspended = true;
				if (++runtime->next_neighbor_generation == 0)
					++runtime->next_neighbor_generation;
				nbr->generation = runtime->next_neighbor_generation;
				pthread_mutex_unlock(&runtime->peer_lock);
				/* Already penalized peers have completed DOWN teardown.
				 * Their independent suspension must survive AF limit release. */
				if (nbr->limit_state == EIGRP_NBR_LIMIT_NONE) {
					if (peers)
						eigrp_nbr_down_deferred(nbr);
					else
						eigrp_nbr_down(nbr);
				}
			}
		}
	}
	if (peers) {
		eigrp_topology_suspended_neighbors_down(runtime, peers, capacity);
		free(peers);
	}
}

void eigrp_pfxlimits_state_cleanup(eigrp_instance_t *runtime)
{
	if (!runtime || !runtime->eigrp_pfxlimits_af_cleanup_pending)
		return;
	runtime->eigrp_pfxlimits_af_cleanup_pending = false;
	/* Signal an immediate remote withdrawal before suspending RTP, then
	 * suppress periodic Hellos until the AF prefix limit is released. */
	{
		eigrp_list_item_t *node;
		eigrp_intf_t *intf;
		for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, node, intf))
			if (intf && intf->operative && !eigrp_intf_is_passive(intf))
				eigrp_hello_send(intf, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
	}
	/* Neighbor DOWN enters DUAL once no packet route walker owns descriptors. */
	eigrp_pfxlimits_nbr_update(runtime, false);
	eigrp_redist_routes_withdraw(runtime);
}

static void eigrp_pfxlimits_state_release(eigrp_instance_t *runtime, bool reset)
{
	eigrp_pfxlimits_timers_cancel(runtime);
	runtime->eigrp_pfxlimits_af_state = 0;
	runtime->eigrp_pfxlimits_af_cleanup_pending = false;
	if (reset) {
		runtime->eigrp_pfxlimits_af_restart_cnt = 0;
		runtime->eigrp_pfxlimits_af_penalty_q16 = 0;
		runtime->eigrp_pfxlimits_af_decay_msec = 0;
	}
	eigrp_pfxlimits_nbr_update(runtime, true);
	/* This can synchronously trigger another hard limit violation. */
	if (!runtime->redist_limit_state)
		eigrp_redist_routes_replay(runtime);
}

void eigrp_pfxlimits_restart_expired(void *arg)
{
	eigrp_instance_t *runtime = arg;
	uint64_t now;
	if (!runtime || runtime->eigrp_pfxlimits_af_state != 1 ||
	    !runtime->eigrp_pfxlimits_af_restart_due_msec)
		return;
	now = eigrp_sys_monotime_msec();
	if (now < runtime->eigrp_pfxlimits_af_restart_due_msec) {
		eigrp_pfxlimits_timer_arm(runtime, &runtime->t_pfxlimits_af_restart,
			eigrp_pfxlimits_restart_expired,
			runtime->eigrp_pfxlimits_af_restart_due_msec);
		return;
	}
	/* Preserve reset timer and incident history across automatic restarts. */
	runtime->eigrp_pfxlimits_af_restart_due_msec = 0;
	runtime->eigrp_pfxlimits_af_state = 0;
	eigrp_pfxlimits_nbr_update(runtime, true);
	if (!runtime->redist_limit_state)
		eigrp_redist_routes_replay(runtime);
}

static void eigrp_pfxlimits_reset_expired(void *arg)
{
	eigrp_instance_t *runtime = arg;
	uint64_t now;
	if (!runtime || !runtime->eigrp_pfxlimits_af_reset_due_msec)
		return;
	now = eigrp_sys_monotime_msec();
	if (now < runtime->eigrp_pfxlimits_af_reset_due_msec) {
		eigrp_pfxlimits_timer_arm(runtime, &runtime->t_pfxlimits_af_reset,
			eigrp_pfxlimits_reset_expired,
			runtime->eigrp_pfxlimits_af_reset_due_msec);
		return;
	}
	runtime->eigrp_pfxlimits_af_reset_due_msec = 0;
	if (runtime->eigrp_pfxlimits_af_state != 2) {
		runtime->eigrp_pfxlimits_af_restart_cnt = 0;
		runtime->eigrp_pfxlimits_af_penalty_q16 = 0;
		runtime->eigrp_pfxlimits_af_decay_msec = 0;
	}
}

void eigrp_pfxlimits_prefix_exceeded(eigrp_instance_t *runtime,
	const eigrp_prefix_limit_t *limit)
{
	uint64_t now, base, half_life, interval;
	uint32_t restart, reset, count;
	if (!runtime || !limit || limit->warning_only || runtime->eigrp_pfxlimits_af_state)
		return;
	restart = limit->restart_minutes ? limit->restart_minutes : 5U;
	reset = limit->reset_time_minutes ? limit->reset_time_minutes : 15U;
	count = limit->restart_count ? limit->restart_count : 3U;
	now = eigrp_sys_monotime_msec();
	base = (uint64_t)restart * PFXLIMITS_MINUTE_MSEC;
	half_life = base + base / 2U;
	if (limit->dampened) {
		if (runtime->eigrp_pfxlimits_af_decay_msec &&
		    now > runtime->eigrp_pfxlimits_af_decay_msec)
			runtime->eigrp_pfxlimits_af_penalty_q16 = eigrp_pfxlimits_penalty_decay(
				runtime->eigrp_pfxlimits_af_penalty_q16,
				now - runtime->eigrp_pfxlimits_af_decay_msec, half_life);
		if (runtime->eigrp_pfxlimits_af_penalty_q16 > UINT32_MAX - PFXLIMITS_Q16)
			runtime->eigrp_pfxlimits_af_penalty_q16 = UINT32_MAX;
		else
			runtime->eigrp_pfxlimits_af_penalty_q16 += PFXLIMITS_Q16;
		runtime->eigrp_pfxlimits_af_decay_msec = now;
	} else {
		runtime->eigrp_pfxlimits_af_penalty_q16 = 0;
		runtime->eigrp_pfxlimits_af_decay_msec = 0;
	}
	if (runtime->eigrp_pfxlimits_af_restart_cnt < UINT16_MAX)
		++runtime->eigrp_pfxlimits_af_restart_cnt;
	runtime->eigrp_pfxlimits_af_state = runtime->eigrp_pfxlimits_af_restart_cnt >= count ? 2 : 1;
	eigrp_pfxlimits_timers_cancel(runtime);
	runtime->eigrp_pfxlimits_af_cleanup_pending = true;
	/* Packets must finish their TLV walk before neighbor/DUAL cleanup. */
	if (!runtime->eigrp_pfxlimits_af_pkt_active)
		eigrp_pfxlimits_state_cleanup(runtime);
	if (runtime->eigrp_pfxlimits_af_state == 2)
		return;
	/* Saturating arithmetic prevents long dampened policies from wrapping
	 * into an artificially short restart interval. */
	interval = base;
	if (limit->dampened) {
		uint64_t score = runtime->eigrp_pfxlimits_af_penalty_q16;
		if (score && base > UINT64_MAX / score)
			interval = UINT64_MAX;
		else
			interval = (base * score) / PFXLIMITS_Q16;
	}
	if (interval > UINT64_MAX - now ||
	    (uint64_t)reset * PFXLIMITS_MINUTE_MSEC > UINT64_MAX - now) {
		runtime->eigrp_pfxlimits_af_state = 2;
		return;
	}
	runtime->eigrp_pfxlimits_af_restart_due_msec = now + interval;
	runtime->eigrp_pfxlimits_af_reset_due_msec = now +
		(uint64_t)reset * PFXLIMITS_MINUTE_MSEC;
	eigrp_pfxlimits_timer_arm(runtime, &runtime->t_pfxlimits_af_restart,
		eigrp_pfxlimits_restart_expired, runtime->eigrp_pfxlimits_af_restart_due_msec);
	eigrp_pfxlimits_timer_arm(runtime, &runtime->t_pfxlimits_af_reset,
		eigrp_pfxlimits_reset_expired, runtime->eigrp_pfxlimits_af_reset_due_msec);
	if (!runtime->t_pfxlimits_af_restart || !runtime->t_pfxlimits_af_reset) {
		eigrp_pfxlimits_timers_cancel(runtime);
		runtime->eigrp_pfxlimits_af_state = 2;
	}
}

eigrp_result_t eigrp_pfxlimits_state_clear(eigrp_instance_t *runtime)
{
	if (!runtime)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_instance_thread_dispatch_needed(runtime))
		return EIGRP_RESULT_CONFLICT;
	if (runtime->eigrp_pfxlimits_af_state) {
		eigrp_pfxlimits_state_release(runtime, true);
	} else {
		/* Automatic restart restores admission, not the incident history.
		 * An explicit clear during the reset window must erase that history
		 * without replaying RIB sources or disturbing healthy neighbors. */
		eigrp_pfxlimits_timers_cancel(runtime);
		runtime->eigrp_pfxlimits_af_restart_cnt = 0;
		runtime->eigrp_pfxlimits_af_penalty_q16 = 0;
		runtime->eigrp_pfxlimits_af_decay_msec = 0;
	}
	return EIGRP_RESULT_SUCCESS;
}
