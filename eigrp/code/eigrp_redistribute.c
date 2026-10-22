// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP redistribution configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_redistribute.h"
#include "eigrp_structs.h"
#include "eigrp_table.h"
#include "eigrp_rib.h"
#include "eigrp_topology.h"
#include "eigrp.h"
#include "eigrp_log.h"
#include "eigrp_eventlog.h"
#include "eigrp_instance.h"
#include "eigrp_timer.h"
#include "eigrp_sys.h"
#include <stdint.h>

eigrp_result_t eigrp_topology_redistributed_route_update(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *source_route,
	const eigrp_metrics_t *metric);
eigrp_result_t eigrp_topology_redistributed_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *source_route);
eigrp_result_t eigrp_topology_redistributed_source_remove(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source);

struct eigrp_redist_config {
	eigrp_redist_source_t source;
	bool metric_configured;
	eigrp_metric_values_t metric;
	char *route_map;
	eigrp_redist_config_t *next;
};

struct eigrp_redist_policy_config {
	bool maximum_prefix_configured;
	eigrp_prefix_limit_t maximum_prefix;
};

typedef enum eigrp_redist_metric_origin {
	EIGRP_REDISTRIBUTE_METRIC_NONE = 0,
	EIGRP_REDISTRIBUTE_METRIC_EXPLICIT,
	EIGRP_REDISTRIBUTE_METRIC_ROUTE,
	EIGRP_REDISTRIBUTE_METRIC_DEFAULT,
} eigrp_redist_metric_origin_t;

static char *eigrp_redist_string_dup(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;
	len = strlen(value) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;
	memcpy(copy, value, len);
	return copy;
}

static bool eigrp_redist_source_valid(
	const eigrp_redist_source_t *source)
{
	if (!source)
		return false;
	return source->protocol > EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED
	       && source->protocol <= EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
}

static bool eigrp_redist_source_match(
	const eigrp_redist_source_t *left,
	const eigrp_redist_source_t *right)
{
	return left && right && left->protocol == right->protocol
	       && left->route_instance == right->route_instance;
}

static eigrp_result_t eigrp_redist_validate(
	eigrp_instance_context_t *context,
	const eigrp_redist_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	if (!eigrp_redist_source_valid(source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (metric && (!metric->bandwidth || !metric->load || !metric->mtu))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route_map && !route_map[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_redist_config_t *eigrp_redist_config_lookup(
	eigrp_af_config_t *af,
	const eigrp_redist_source_t *source)
{
	eigrp_redist_config_t *config;

	if (!af || !source)
		return NULL;
	for (config = af->redistributions; config; config = config->next)
		if (eigrp_redist_source_match(&config->source, source))
			return config;
	return NULL;
}

static bool eigrp_redist_runtime_result_committable(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS;
}

static bool eigrp_redist_vecmetric_usable(
	const eigrp_rib_route_t *route)
{
	return route && route->redist.vecmetric.bandwidth != 0
	       && route->redist.vecmetric.delay != 0;
}

static eigrp_redist_metric_origin_t eigrp_redist_metric_select(
	const eigrp_af_config_t *af,
	const eigrp_redist_config_t *config,
	const eigrp_rib_route_t *route, eigrp_metrics_t *metric)
{
	eigrp_metric_values_t values;

	if (!metric)
		return EIGRP_REDISTRIBUTE_METRIC_NONE;
	memset(metric, 0, sizeof(*metric));
	if (!config || !route)
		return EIGRP_REDISTRIBUTE_METRIC_NONE;

	if (config->metric_configured) {
		eigrp_metric_values_convert(&config->metric, metric);
		return EIGRP_REDISTRIBUTE_METRIC_EXPLICIT;
	}
	if (eigrp_redist_vecmetric_usable(route)) {
		*metric = route->redist.vecmetric;
		return EIGRP_REDISTRIBUTE_METRIC_ROUTE;
	}
	if (eigrp_metric_default(af, &values)) {
		eigrp_metric_values_convert(&values, metric);
		return EIGRP_REDISTRIBUTE_METRIC_DEFAULT;
	}
	return EIGRP_REDISTRIBUTE_METRIC_NONE;
}

/*
 * Named redistribution owns retained configuration.  Host subscription state
 * is keyed only by the exact EIGRP-owned {protocol, route-instance} source.
 */
typedef enum eigrp_redist_message_type {
	EIGRP_REDIST_MESSAGE_ADD,
	EIGRP_REDIST_MESSAGE_REMOVE,
	EIGRP_REDIST_MESSAGE_MAX_PREFIX,
} eigrp_redist_message_type_t;

typedef struct eigrp_redist_message_args {
	eigrp_redist_message_type_t type;
	eigrp_operation_t operation;
	eigrp_instance_context_t *context;
	const eigrp_redist_source_t *source;
	const eigrp_metric_values_t *metric;
	const char *route_map;
	const eigrp_prefix_limit_t *limit;
} eigrp_redist_message_args_t;

static eigrp_result_t eigrp_redist_message_process(eigrp_instance_t *eigrp,
	void *arg);

eigrp_result_t eigrp_redist_add(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	eigrp_redist_message_args_t message = {
		.type = EIGRP_REDIST_MESSAGE_ADD, .context = context,
		.source = source, .metric = metric, .route_map = route_map,
	};
	eigrp_redist_config_t *config = NULL;
	eigrp_redist_config_t *new_config = NULL;
	eigrp_metric_values_t old_metric = {0};
	char *new_route_map = NULL;
	char *old_route_map = NULL;
	bool old_metric_configured = false;
	eigrp_result_t result;

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_redist_message_process, &message);

	result = eigrp_redist_validate(context, source, metric, route_map);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (route_map) {
		new_route_map = eigrp_redist_string_dup(route_map);
		if (!new_route_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	/* Register the policy before subscribing: a host can replay existing
	 * source routes synchronously inside eigrp_rib_redistribute_add(). */
	if (context->config) {
		config = eigrp_redist_config_lookup(context->config, source);
		if (config) {
			old_route_map = config->route_map;
			old_metric = config->metric;
			old_metric_configured = config->metric_configured;
			config->route_map = new_route_map;
			new_route_map = NULL;
		} else {
			new_config = calloc(1, sizeof(*new_config));
			if (!new_config) {
				free(new_route_map);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
			new_config->source = *source;
			new_config->route_map = new_route_map;
			new_route_map = NULL;
			new_config->next = context->config->redistributions;
			context->config->redistributions = new_config;
			config = new_config;
		}
		config->metric_configured = metric != NULL;
		memset(&config->metric, 0, sizeof(config->metric));
		if (metric)
			config->metric = *metric;
	}

	result = context->runtime
		? eigrp_rib_redistribute_add(context->runtime, source)
		: EIGRP_RESULT_SUCCESS;
	if (!eigrp_redist_runtime_result_committable(result)) {
		if (new_config) {
			context->config->redistributions = new_config->next;
			free(new_config->route_map);
			free(new_config);
		} else if (config) {
			free(config->route_map);
			config->route_map = old_route_map;
			config->metric = old_metric;
			config->metric_configured = old_metric_configured;
		}
		free(new_route_map);
		return result;
	}

	free(old_route_map);
	free(new_route_map);
	return result;
}

eigrp_result_t eigrp_redist_remove(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source)
{
	eigrp_redist_message_args_t message = {
		.type = EIGRP_REDIST_MESSAGE_REMOVE, .context = context,
		.source = source,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_redist_message_process, &message);
	eigrp_redist_config_t **cursor = NULL;
	eigrp_redist_config_t *config = NULL;
	eigrp_result_t result = EIGRP_RESULT_NOT_FOUND;

	if (!eigrp_redist_source_valid(source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->config) {
		for (cursor = &context->config->redistributions; *cursor;
		     cursor = &(*cursor)->next) {
			if (eigrp_redist_source_match(&(*cursor)->source, source)) {
				config = *cursor;
				break;
			}
		}
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
	}

	if (context->runtime) {
		eigrp_result_t withdraw_result =
			eigrp_topology_redistributed_source_remove(context->runtime, source);

		if (withdraw_result != EIGRP_RESULT_SUCCESS
		    && withdraw_result != EIGRP_RESULT_NOT_FOUND)
			return withdraw_result;
		result = eigrp_rib_redistribute_remove(context->runtime, source);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND)
			return result;
	}

	if (config) {
		*cursor = config->next;
		free(config->route_map);
		free(config);
		if (result == EIGRP_RESULT_NOT_FOUND || !context->runtime)
			result = EIGRP_RESULT_SUCCESS;
	}
	return result;
}

/* Penalty state is scoped to one EIGRP AF runtime, not to a source or peer.
 * Source subscriptions remain live during suspension so withdrawals continue
 * to be processed. Recovery replays the RIB using the existing host API. */
#define REDIST_LIMIT_MINUTE_MSEC UINT64_C(60000)
#define REDIST_LIMIT_Q16 UINT32_C(65536)
#define REDIST_LIMIT_RESTART_DEFAULT 5U
#define REDIST_LIMIT_RESET_DEFAULT 15U
#define REDIST_LIMIT_COUNT_DEFAULT 3U

void eigrp_redist_limit_restart_expired(void *arg);
static void eigrp_redist_limit_reset_expired(void *arg);

static void eigrp_redist_limit_timers_cancel(eigrp_instance_t *runtime)
{
	if (!runtime)
		return;
	eigrp_timer_cancel(runtime, &runtime->t_redist_limit_restart);
	eigrp_timer_cancel(runtime, &runtime->t_redist_limit_reset);
	runtime->redist_limit_restart_due_msec = 0;
	runtime->redist_limit_reset_due_msec = 0;
}

static void eigrp_redist_limit_timer_arm(eigrp_instance_t *runtime,
	eigrp_timer_event_t **timer, eigrp_event_callback_t callback,
	uint64_t deadline)
{
	uint64_t now = eigrp_sys_monotime_msec();
	uint64_t delta = deadline > now ? deadline - now : 1;
	if (delta > UINT32_MAX)
		delta = UINT32_MAX;
	eigrp_timer_add(runtime, timer, callback, runtime, (uint32_t)delta);
}

static uint32_t eigrp_redist_limit_decay(uint32_t score, uint64_t elapsed,
	uint64_t half_life)
{
	static const uint32_t factors[8] = {
		46341U, 55109U, 60097U, 62758U, 64132U, 64830U, 65182U, 65359U
	};
	uint32_t fraction;
	unsigned i;
	if (!score || !elapsed || !half_life)
		return score;
	if (elapsed / half_life >= 32)
		return 0;
	score >>= (unsigned)(elapsed / half_life);
	fraction = (uint32_t)(((elapsed % half_life) * 256U) / half_life);
	for (i = 0; i < 8; ++i)
		if (fraction & (128U >> i))
			score = (uint32_t)(((uint64_t)score * factors[i]) >> 16);
	return score;
}

void eigrp_redist_routes_withdraw(eigrp_instance_t *runtime)
{
	eigrp_af_config_t *af = eigrp_af_config_runtime_read(runtime);
	eigrp_redist_config_t *cfg;
	if (!af)
		return;
	for (cfg = af->redistributions; cfg; cfg = cfg->next)
		(void)eigrp_topology_redistributed_source_remove(runtime, &cfg->source);
}

void eigrp_redist_routes_replay(eigrp_instance_t *runtime)
{
	eigrp_af_config_t *af = eigrp_af_config_runtime_read(runtime);
	eigrp_redist_config_t *cfg;
	if (!af)
		return;
	/* Unsubscribe every source before replay. A replay can synchronously
	 * retrigger the penalty, but every source must remain subscribed so
	 * withdrawals and later recovery continue to be observable. */
	for (cfg = af->redistributions; cfg; cfg = cfg->next)
		(void)eigrp_rib_redistribute_remove(runtime, &cfg->source);
	for (cfg = af->redistributions; cfg; cfg = cfg->next)
		(void)eigrp_rib_redistribute_add(runtime, &cfg->source);
}

static void eigrp_redist_limit_release(eigrp_instance_t *runtime,
	bool reset_history)
{
	if (!runtime)
		return;
	eigrp_redist_limit_timers_cancel(runtime);
	runtime->redist_limit_state = 0;
	if (reset_history) {
		runtime->redist_limit_restarts = 0;
		runtime->redist_limit_penalty_q16 = 0;
		runtime->redist_limit_penalty_updated_msec = 0;
	}
	eigrp_redist_routes_replay(runtime);
}

void eigrp_redist_limit_restart_expired(void *arg)
{
	eigrp_instance_t *runtime = arg;
	uint64_t now;
	if (!runtime || runtime->redist_limit_state != 1 ||
	    !runtime->redist_limit_restart_due_msec)
		return;
	now = eigrp_sys_monotime_msec();
	if (now < runtime->redist_limit_restart_due_msec) {
		eigrp_redist_limit_timer_arm(runtime, &runtime->t_redist_limit_restart,
			eigrp_redist_limit_restart_expired,
			runtime->redist_limit_restart_due_msec);
		return;
	}
	/* Keep the history and reset timer across automatic restarts. */
	runtime->redist_limit_restart_due_msec = 0;
	runtime->redist_limit_state = 0;
	eigrp_redist_routes_replay(runtime);
}

static void eigrp_redist_limit_reset_expired(void *arg)
{
	eigrp_instance_t *runtime = arg;
	uint64_t now;
	if (!runtime || !runtime->redist_limit_reset_due_msec)
		return;
	now = eigrp_sys_monotime_msec();
	if (now < runtime->redist_limit_reset_due_msec) {
		eigrp_redist_limit_timer_arm(runtime, &runtime->t_redist_limit_reset,
			eigrp_redist_limit_reset_expired,
			runtime->redist_limit_reset_due_msec);
		return;
	}
	runtime->redist_limit_reset_due_msec = 0;
	/* Expiry must never release a locked source. */
	if (runtime->redist_limit_state != 2) {
		runtime->redist_limit_restarts = 0;
		runtime->redist_limit_penalty_q16 = 0;
		runtime->redist_limit_penalty_updated_msec = 0;
	}
}

static void eigrp_redist_limit_exceeded(eigrp_instance_t *runtime,
	const eigrp_prefix_limit_t *limit)
{
	eigrp_af_config_t *af = eigrp_af_config_runtime_read(runtime);
	uint64_t now, base, interval, half_life;
	uint32_t count, restart_minutes, reset_minutes;
	bool dampened;
	if (!runtime || !limit || runtime->redist_limit_state)
		return;
	restart_minutes = limit->restart_minutes ? limit->restart_minutes :
		(af && af->topology_maximum_prefix_configured &&
		 af->topology_maximum_prefix.restart_minutes ?
		 af->topology_maximum_prefix.restart_minutes : REDIST_LIMIT_RESTART_DEFAULT);
	reset_minutes = limit->reset_time_minutes ? limit->reset_time_minutes :
		(af && af->topology_maximum_prefix_configured &&
		 af->topology_maximum_prefix.reset_time_minutes ?
		 af->topology_maximum_prefix.reset_time_minutes : REDIST_LIMIT_RESET_DEFAULT);
	count = limit->restart_count ? limit->restart_count :
		(af && af->topology_maximum_prefix_configured &&
		 af->topology_maximum_prefix.restart_count ?
		 af->topology_maximum_prefix.restart_count : REDIST_LIMIT_COUNT_DEFAULT);
	dampened = limit->dampened || (af && af->topology_maximum_prefix_configured &&
		 af->topology_maximum_prefix.dampened);
	now = eigrp_sys_monotime_msec();
	base = (uint64_t)restart_minutes * REDIST_LIMIT_MINUTE_MSEC;
	half_life = base + base / 2U;
	if (dampened) {
		if (runtime->redist_limit_penalty_updated_msec &&
		    now > runtime->redist_limit_penalty_updated_msec)
			runtime->redist_limit_penalty_q16 = eigrp_redist_limit_decay(
				runtime->redist_limit_penalty_q16,
				now - runtime->redist_limit_penalty_updated_msec, half_life);
		if (runtime->redist_limit_penalty_q16 > UINT32_MAX - REDIST_LIMIT_Q16)
			runtime->redist_limit_penalty_q16 = UINT32_MAX;
		else
			runtime->redist_limit_penalty_q16 += REDIST_LIMIT_Q16;
		runtime->redist_limit_penalty_updated_msec = now;
	} else {
		runtime->redist_limit_penalty_q16 = 0;
		runtime->redist_limit_penalty_updated_msec = 0;
	}
	if (runtime->redist_limit_restarts < UINT16_MAX)
		++runtime->redist_limit_restarts;
	/* Set ineligible BEFORE topology withdrawal. */
	runtime->redist_limit_state =
		runtime->redist_limit_restarts >= count ? 2 : 1;
	eigrp_redist_limit_timers_cancel(runtime);
	eigrp_redist_routes_withdraw(runtime);
	if (runtime->redist_limit_state == 2)
		return;
	interval = dampened ? (base * runtime->redist_limit_penalty_q16) /
		REDIST_LIMIT_Q16 : base;
	runtime->redist_limit_restart_due_msec = now + interval;
	runtime->redist_limit_reset_due_msec = now +
		(uint64_t)reset_minutes * REDIST_LIMIT_MINUTE_MSEC;
	eigrp_redist_limit_timer_arm(runtime, &runtime->t_redist_limit_restart,
		eigrp_redist_limit_restart_expired,
		runtime->redist_limit_restart_due_msec);
	eigrp_redist_limit_timer_arm(runtime, &runtime->t_redist_limit_reset,
		eigrp_redist_limit_reset_expired,
		runtime->redist_limit_reset_due_msec);
	if (!runtime->t_redist_limit_restart || !runtime->t_redist_limit_reset) {
		eigrp_redist_limit_timers_cancel(runtime);
		runtime->redist_limit_state = 2;
	}
}

eigrp_result_t eigrp_redist_limit_clear(eigrp_instance_t *runtime)
{
	if (!runtime)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_instance_thread_dispatch_needed(runtime))
		return EIGRP_RESULT_CONFLICT;
	if (runtime->redist_limit_state)
		eigrp_redist_limit_release(runtime, true);
	return EIGRP_RESULT_SUCCESS;
}

static uint32_t eigrp_redist_prefix_count(eigrp_instance_t *runtime)
{
	return runtime ? runtime->redist_prefix_count : 0;
}

bool eigrp_redist_prefix_admit(eigrp_instance_t *runtime,
	const eigrp_rib_route_t *route)
{
	eigrp_af_config_t *af;
	eigrp_prefix_descriptor_t *prefix;
	const eigrp_prefix_limit_t *limit;

	if (!runtime || !route)
		return false;
	if (runtime->redist_limit_state || runtime->process_limit_state)
		return false;
	af = eigrp_af_config_runtime_read(runtime);
	if (!af || !af->redistribute_policy
	    || !af->redistribute_policy->maximum_prefix_configured)
		return true;
	prefix = eigrp_topology_table_lookup(runtime->topology_table, &route->prefix);
	if (eigrp_topology_redist_present(runtime, prefix))
		return true;
	limit = &af->redistribute_policy->maximum_prefix;
	{
		uint32_t count = eigrp_redist_prefix_count(runtime);
		if (eigrp_prefix_limit_threshold_crossed(limit, count))
			eigrp_log(EIGRP_LOG_WARNING,
				  "EIGRP redistribution maximum-prefix threshold reached (%u/%u)",
				  count + 1U, limit->maximum);
		if (!limit->warning_only && count >= limit->maximum) {
			eigrp_redist_limit_exceeded(runtime, limit);
			(void)eigrp_eventlog_msg_add(runtime, EIGRP_EVENTLOG_OPCODE_PREFIX_LIMIT_REJECT,
				&route->prefix, EIGRP_EVENTLOG_PREFIX_LIMIT_REDISTRIBUTION,
				count + 1U, limit->maximum, 0);
			return false;
		}
		return true;
	}
}

enum {
	EIGRP_REDIST_REJECT_NO_METRIC = 1,
	EIGRP_REDIST_REJECT_PREFIX_LIMIT = 2,
};

static void eigrp_redist_event_add(eigrp_instance_t *runtime, uint16_t opcode,
	const eigrp_rib_route_t *route, eventmsg_arg_t arg3)
{
	if (!runtime || !route)
		return;
	(void)eigrp_eventlog_msg_add(runtime, opcode, &route->prefix,
		(eventmsg_arg_t)route->redist.source.protocol,
		(eventmsg_arg_t)route->redist.source.route_instance, arg3, 0);
}

static eigrp_result_t eigrp_redist_source_route_receive(
	eigrp_instance_t *runtime, const eigrp_rib_route_t *route,
	bool importing)
{
	eigrp_af_config_t *af;
	eigrp_redist_config_t *config;
	eigrp_metrics_t metric;

	if (!runtime || !route || !eigrp_redist_source_valid(&route->redist.source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_af_config_runtime_read(runtime);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	config = eigrp_redist_config_lookup(af, &route->redist.source);
	if (!config)
		return EIGRP_RESULT_SUCCESS;

	if (!importing) {
		eigrp_redist_event_add(runtime,
			EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_WITHDRAW, route, 0);
		return eigrp_topology_redistributed_route_remove(runtime, route);
	}

	if (importing && config->route_map) {
		eigrp_filter_decision_t decision = EIGRP_FILTER_DECISION_DENY;
		eigrp_result_t policy_result = eigrp_sys_redistribute_route_map_evaluate(
			runtime, config->route_map, route, &decision);

		/* A configured-but-missing route-map and a no-match both fail closed.
		 * If this prefix had previously been admitted, withdraw it now. */
		if (policy_result != EIGRP_RESULT_SUCCESS
		    || decision == EIGRP_FILTER_DECISION_DENY) {
			eigrp_redist_event_add(runtime,
				EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_FILTERED, route, 0);
			return eigrp_topology_redistributed_route_remove(runtime, route);
		}
	}
	if (eigrp_redist_metric_select(af, config, route, &metric)
	    == EIGRP_REDISTRIBUTE_METRIC_NONE) {
		eigrp_redist_event_add(runtime, EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_REJECT,
			route, EIGRP_REDIST_REJECT_NO_METRIC);
		return EIGRP_RESULT_SUCCESS;
	}
	if (!eigrp_redist_prefix_admit(runtime, route)
	    || !eigrp_topology_prefix_admit(runtime, &route->prefix)) {
		eigrp_redist_event_add(runtime, EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_REJECT,
			route, EIGRP_REDIST_REJECT_PREFIX_LIMIT);
		return EIGRP_RESULT_CONFLICT;
	}
	eigrp_redist_event_add(runtime, EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_UPDATE,
		route, (eventmsg_arg_t)metric.delay);
	return eigrp_topology_redistributed_route_update(runtime, route, &metric);
}

eigrp_result_t eigrp_rib_event_process(
	eigrp_instance_t *runtime, const eigrp_rib_event_t *event)
{
	uint16_t opcode;

	if (!runtime || !event)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	opcode = event->type == EIGRP_RIB_EVENT_DELETE
		 ? EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_DELETE
		 : EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_UPDATE;
	eigrp_redist_event_add(runtime, opcode, &event->route, 0);
	return eigrp_redist_source_route_receive(runtime, &event->route,
		event->type == EIGRP_RIB_EVENT_UPDATE);
}

/* Zebra uses ADD for both first appearance and changed route snapshots. */
eigrp_result_t eigrp_rib_redist_add(
	eigrp_instance_t *runtime, const eigrp_rib_route_t *route)
{
	eigrp_rib_event_t event;

	if (!runtime || !route)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_instance_thread_dispatch_needed(runtime))
		return eigrp_instance_rib_event_enqueue(runtime, EIGRP_RIB_EVENT_UPDATE, route);
	memset(&event, 0, sizeof(event));
	event.type = EIGRP_RIB_EVENT_UPDATE;
	event.route = *route;
	return eigrp_rib_event_process(runtime, &event);
}

eigrp_result_t eigrp_rib_redist_del(
	eigrp_instance_t *runtime, const eigrp_rib_route_t *route)
{
	eigrp_rib_event_t event;

	if (!runtime || !route)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_instance_thread_dispatch_needed(runtime))
		return eigrp_instance_rib_event_enqueue(runtime, EIGRP_RIB_EVENT_DELETE, route);
	memset(&event, 0, sizeof(event));
	event.type = EIGRP_RIB_EVENT_DELETE;
	event.route = *route;
	return eigrp_rib_event_process(runtime, &event);
}

void eigrp_redist_config_delete_all(eigrp_af_config_t *af)
{
	eigrp_redist_config_t *config;
	eigrp_redist_config_t *next;

	if (!af)
		return;
	for (config = af->redistributions; config; config = next) {
		next = config->next;
		free(config->route_map);
		free(config);
	}
	af->redistributions = NULL;
}

void eigrp_redist_policy_update_all(void)
{
	eigrp_virt_router_t *virt_router;
	eigrp_instance_t *runtime;
	eigrp_af_config_t *af;
	eigrp_redist_config_t *config;
	eigrp_list_item_t *vr_node, *af_node;

	if (!eigrp_process.virt_router)
		return;
	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
	for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, runtime)) {
		if (!runtime)
			continue;
		af = eigrp_af_config_runtime_read(runtime);
		if (!af)
			continue;
		for (config = af->redistributions; config; config = config->next) {
			if (!config->route_map)
				continue;
			/* Re-subscription makes Zebra replay the current source RIB so
			 * route-map edits are applied to already-existing IPv4/IPv6
			 * candidates, not only to future route events. */
			(void)eigrp_rib_redistribute_remove(runtime, &config->source);
			(void)eigrp_rib_redistribute_add(runtime, &config->source);
		}
	}
	}
}



/*
 * Syntax:
 *   Named: `redistribute maximum-prefix LIMIT [...]` / `no redistribute maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the redistribution prefix-limit policy.
 * Retained configuration stays here while supported runtime limit semantics are enforced.
 */
eigrp_result_t eigrp_redist_max_prefix_update(eigrp_operation_t operation, eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	eigrp_redist_message_args_t message = {
		.type = EIGRP_REDIST_MESSAGE_MAX_PREFIX, .operation = operation,
		.context = context, .limit = limit,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_redist_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->redistribute_policy) {
		context->config->redistribute_policy->maximum_prefix_configured = false;
		memset(&context->config->redistribute_policy->maximum_prefix, 0,
		       sizeof(context->config->redistribute_policy->maximum_prefix));
	}
	if (context->runtime && context->runtime->redist_limit_state)
		eigrp_redist_limit_release(context->runtime, true);
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (!limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		if (!context->config->redistribute_policy) {
			context->config->redistribute_policy =
				calloc(1, sizeof(*context->config->redistribute_policy));
			if (!context->config->redistribute_policy)
				return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		context->config->redistribute_policy->maximum_prefix = *limit;
		context->config->redistribute_policy->maximum_prefix_configured = true;
	}
	if (context->runtime && context->runtime->redist_limit_state)
		eigrp_redist_limit_release(context->runtime, true);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `redistribute maximum-prefix LIMIT [...]` / `no redistribute maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the redistribution prefix-limit policy.
 * Retained configuration stays here while supported runtime limit semantics are enforced.
 */


static eigrp_result_t eigrp_redist_message_process(eigrp_instance_t *eigrp,
	void *arg)
{
	eigrp_redist_message_args_t *message = arg;

	(void)eigrp;
	switch (message->type) {
	case EIGRP_REDIST_MESSAGE_ADD:
		return eigrp_redist_add(message->context, message->source,
			message->metric, message->route_map);
	case EIGRP_REDIST_MESSAGE_REMOVE:
		return eigrp_redist_remove(message->context, message->source);
	case EIGRP_REDIST_MESSAGE_MAX_PREFIX:
		return eigrp_redist_max_prefix_update(message->operation,
			message->context, message->limit);
	}
	return EIGRP_RESULT_INVALID_ARGUMENT;
}

void eigrp_redist_policy_delete_all(eigrp_af_config_t *af)
{
	if (!af)
		return;
	free(af->redistribute_policy);
	af->redistribute_policy = NULL;
}
