// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP compact event log.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_eventlog.h"
#include "eigrp_prefix.h"
#include "eigrp_sys.h"

struct eigrp_eventlog {
	eigrp_eventlog_msg_t *entries;
	uint32_t capacity;
	uint32_t count;
	uint32_t next;
};

/* The opcode stored in each fixed ring entry is the direct index into this
 * table. Keep protocol wording here so all management adapters render the
 * same event text.
 */
static const char *const eigrp_eventlog_formats[] = {
	[EIGRP_EVENTLOG_OPCODE_NONE] = NULL,
	[EIGRP_EVENTLOG_OPCODE_IPV6_NO_ROUTER_ID] = "Ignored HELLO, no routerid for IPv6 AS",
	[EIGRP_EVENTLOG_OPCODE_DUAL_STATE_CHANGE] = "DUAL state change",
	[EIGRP_EVENTLOG_OPCODE_NEIGHBOR_STATE_CHANGE] = "Peer state change",
	[EIGRP_EVENTLOG_OPCODE_PACKET_RX] = "Packet received",
	[EIGRP_EVENTLOG_OPCODE_PACKET_TX] = "Packet transmitted",
	[EIGRP_EVENTLOG_OPCODE_RTP_ACK] = "RTP ACK",
	[EIGRP_EVENTLOG_OPCODE_RTP_RETRANSMIT] = "RTP retransmit",
	[EIGRP_EVENTLOG_OPCODE_RTP_RETRY_LIMIT] = "RTP retry limit exceeded",
	[EIGRP_EVENTLOG_OPCODE_SUMMARY_METRIC] = "Summary metric",
	[EIGRP_EVENTLOG_OPCODE_SUMMARY_COMPONENT] = "Summary component",
	[EIGRP_EVENTLOG_OPCODE_SUMMARY_WITHDRAW] = "Summary withdraw",
	[EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_UPDATE] = "RIB route update",
	[EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_DELETE] = "RIB route delete",
	[EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_UPDATE] = "Redistribute route update",
	[EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_WITHDRAW] = "Redistribute route withdraw",
	[EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_FILTERED] = "Redistribute route filtered",
	[EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_REJECT] = "Redistribute route reject",
};

static const eigrp_eventlog_msg_t *
eigrp_eventlog_recent_read(const eigrp_eventlog_t *log, uint32_t recent_index)
{
	uint32_t index;

	if (!log || !log->entries || recent_index >= log->count || !log->capacity)
		return NULL;

	index = (log->next + log->capacity - 1U - recent_index) % log->capacity;
	return &log->entries[index];
}

const char *eigrp_eventlog_format_read(unsigned long opcode)
{
	if (opcode >= (sizeof(eigrp_eventlog_formats)
		      / sizeof(eigrp_eventlog_formats[0])))
		return NULL;
	return eigrp_eventlog_formats[opcode];
}

static const char *eigrp_eventlog_dual_state_name(eventmsg_arg_t state)
{
	switch (state) {
	case EIGRP_FSM_STATE_PASSIVE: return "PASSIVE";
	case EIGRP_FSM_STATE_ACTIVE_0: return "ACTIVE_0";
	case EIGRP_FSM_STATE_ACTIVE_1: return "ACTIVE_1";
	case EIGRP_FSM_STATE_ACTIVE_2: return "ACTIVE_2";
	case EIGRP_FSM_STATE_ACTIVE_3: return "ACTIVE_3";
	default: return "UNKNOWN";
	}
}

static const char *eigrp_eventlog_neighbor_state_name(eventmsg_arg_t state)
{
	switch (state) {
	case EIGRP_NEIGHBOR_DOWN: return "DOWN";
	case EIGRP_NEIGHBOR_PENDING: return "PENDING";
	case EIGRP_NEIGHBOR_UP: return "UP";
	default: return "UNKNOWN";
	}
}

static const char *eigrp_eventlog_redist_reject_name(eventmsg_arg_t reason)
{
	switch (reason) {
	case 1: return "no-metric";
	case 2: return "prefix-limit";
	default: return "unknown";
	}
}

static const char *eigrp_eventlog_redist_source_name(eventmsg_arg_t protocol)
{
	switch (protocol) {
	case EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED: return "CONNECTED";
	case EIGRP_REDISTRIBUTE_PROTOCOL_STATIC: return "STATIC";
	case EIGRP_REDISTRIBUTE_PROTOCOL_RIP: return "RIP";
	case EIGRP_REDISTRIBUTE_PROTOCOL_OSPF: return "OSPF";
	case EIGRP_REDISTRIBUTE_PROTOCOL_ISIS: return "ISIS";
	case EIGRP_REDISTRIBUTE_PROTOCOL_BGP: return "BGP";
	case EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP: return "EIGRP";
	default: return "UNKNOWN";
	}
}

static const char *eigrp_eventlog_opcode_name(eventmsg_arg_t opcode)
{
	switch (opcode) {
	case EIGRP_OPC_UPDATE: return "UPDATE";
	case EIGRP_OPC_QUERY: return "QUERY";
	case EIGRP_OPC_REPLY: return "REPLY";
	case EIGRP_OPC_SIAQUERY: return "SIA-QUERY";
	case EIGRP_OPC_SIAREPLY: return "SIA-REPLY";
	default: return "UNKNOWN";
	}
}

eigrp_result_t eigrp_eventlog_msg_format(const eigrp_eventlog_msg_t *entry,
					 char *buffer, size_t buffer_size)
{
	char addr[EIGRP_PREFIX_STRLEN] = "-";
	char message[384];
	char timebuf[16];
	time_t seconds;
	struct tm local_tm;
	unsigned int msec;

	if (!entry || !buffer || !buffer_size)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_prefix_valid(&entry->addr))
		eigrp_prefix_snprintf(addr, sizeof(addr), &entry->addr);

	message[0] = '\0';
	switch (entry->opcode) {
	case EIGRP_EVENTLOG_OPCODE_IPV6_NO_ROUTER_ID:
		(void)snprintf(message, sizeof(message),
			       "Ignored HELLO, no routerid for IPv6 AS(%lu)",
			       (unsigned long)entry->arg1);
		break;
	case EIGRP_EVENTLOG_OPCODE_DUAL_STATE_CHANGE:
		(void)snprintf(message, sizeof(message), "DUAL state change %s Old: %s New: %s",
			       addr, eigrp_eventlog_dual_state_name(entry->arg1),
			       eigrp_eventlog_dual_state_name(entry->arg2));
		break;
	case EIGRP_EVENTLOG_OPCODE_NEIGHBOR_STATE_CHANGE:
		(void)snprintf(message, sizeof(message), "Peer %s: %s -> %s ifindex %lu",
			       addr, eigrp_eventlog_neighbor_state_name(entry->arg1),
			       eigrp_eventlog_neighbor_state_name(entry->arg2),
			       (unsigned long)entry->arg3);
		break;
	case EIGRP_EVENTLOG_OPCODE_PACKET_RX:
	case EIGRP_EVENTLOG_OPCODE_PACKET_TX:
		(void)snprintf(message, sizeof(message), "%s %s %s seq %lu ack %lu len %lu",
			       entry->opcode == EIGRP_EVENTLOG_OPCODE_PACKET_RX ? "Rcv" : "Send",
			       eigrp_eventlog_opcode_name(entry->arg1), addr,
			       (unsigned long)entry->arg2, (unsigned long)entry->arg3,
			       (unsigned long)entry->arg4);
		break;
	case EIGRP_EVENTLOG_OPCODE_RTP_ACK:
		(void)snprintf(message, sizeof(message), "RTP ACK from %s seq %lu queue %lu",
			       addr, (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_RTP_RETRANSMIT:
		(void)snprintf(message, sizeof(message), "RTP retransmit to %s seq %lu retry %lu",
			       addr, (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_RTP_RETRY_LIMIT:
		(void)snprintf(message, sizeof(message), "RTP retry limit exceeded for %s seq %lu retries %lu",
			       addr, (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_SUMMARY_METRIC:
		(void)snprintf(message, sizeof(message), "Summary metric: %s %lu", addr,
			       (unsigned long)entry->arg1);
		break;
	case EIGRP_EVENTLOG_OPCODE_SUMMARY_COMPONENT:
		(void)snprintf(message, sizeof(message), "Summary component: %s metric %lu", addr,
			       (unsigned long)entry->arg1);
		break;
	case EIGRP_EVENTLOG_OPCODE_SUMMARY_WITHDRAW:
		(void)snprintf(message, sizeof(message), "Summary withdraw: %s", addr);
		break;
	case EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_UPDATE:
		(void)snprintf(message, sizeof(message),
			       "RIB route update %s source %s(%lu) instance %lu",
			       addr, eigrp_eventlog_redist_source_name(entry->arg1),
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_DELETE:
		(void)snprintf(message, sizeof(message),
			       "RIB route delete %s source %s(%lu) instance %lu",
			       addr, eigrp_eventlog_redist_source_name(entry->arg1),
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_UPDATE:
		(void)snprintf(message, sizeof(message),
			       "Redistribute route update %s source %s(%lu) instance %lu delay %lu",
			       addr, eigrp_eventlog_redist_source_name(entry->arg1),
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2,
			       (unsigned long)entry->arg3);
		break;
	case EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_WITHDRAW:
		(void)snprintf(message, sizeof(message),
			       "Redistribute route withdraw %s source %s(%lu) instance %lu",
			       addr, eigrp_eventlog_redist_source_name(entry->arg1),
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_FILTERED:
		(void)snprintf(message, sizeof(message),
			       "Redistribute route filtered %s source %s(%lu) instance %lu",
			       addr, eigrp_eventlog_redist_source_name(entry->arg1),
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2);
		break;
	case EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_REJECT:
		(void)snprintf(message, sizeof(message),
			       "Redistribute route reject %s source %s(%lu) instance %lu reason %s",
			       addr, eigrp_eventlog_redist_source_name(entry->arg1),
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2,
			       eigrp_eventlog_redist_reject_name(entry->arg3));
		break;
	default:
		(void)snprintf(message, sizeof(message),
			       "opcode %u addr %s args %lu %lu %lu %lu", entry->opcode, addr,
			       (unsigned long)entry->arg1, (unsigned long)entry->arg2,
			       (unsigned long)entry->arg3, (unsigned long)entry->arg4);
		break;
	}

	seconds = (time_t)(entry->timestamp / 1000U);
	msec = (unsigned int)(entry->timestamp % 1000U);
	if (!localtime_r(&seconds, &local_tm))
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (!strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &local_tm))
		return EIGRP_RESULT_INTERNAL_FAILURE;
	(void)snprintf(buffer, buffer_size, "%s.%03u %s", timebuf, msec, message);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_eventlog_init(eigrp_instance_t *eigrp, uint32_t capacity)
{
	eigrp_eventlog_t *log;

	if (!eigrp)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp->eventlog)
		return eigrp_eventlog_resize(eigrp, capacity);

	log = calloc(1, sizeof(*log));
	if (!log)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	eigrp->eventlog = log;

	if (!capacity)
		return EIGRP_RESULT_SUCCESS;
	if (capacity && sizeof(*log->entries) > SIZE_MAX / (size_t)capacity) {
		free(log);
		eigrp->eventlog = NULL;
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}

	log->entries = calloc((size_t)capacity, sizeof(*log->entries));
	if (!log->entries) {
		free(log);
		eigrp->eventlog = NULL;
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	log->capacity = capacity;
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_eventlog_delete(eigrp_instance_t *eigrp)
{
	if (!eigrp || !eigrp->eventlog)
		return;

	free(eigrp->eventlog->entries);
	free(eigrp->eventlog);
	eigrp->eventlog = NULL;
}

eigrp_result_t eigrp_eventlog_resize(eigrp_instance_t *eigrp,
				     uint32_t capacity)
{
	eigrp_eventlog_msg_t *entries = NULL;
	eigrp_eventlog_t *log;
	uint32_t keep;
	uint32_t i;

	if (!eigrp)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp->eventlog)
		return eigrp_eventlog_init(eigrp, capacity);
	log = eigrp->eventlog;

	if (capacity == log->capacity)
		return EIGRP_RESULT_SUCCESS;
	if (capacity && sizeof(*entries) > SIZE_MAX / (size_t)capacity)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (capacity) {
		entries = calloc((size_t)capacity, sizeof(*entries));
		if (!entries)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	keep = log->count < capacity ? log->count : capacity;
	/* Copy the retained newest events back in oldest-to-newest order. */
	for (i = 0; i < keep; i++) {
		const eigrp_eventlog_msg_t *entry =
			eigrp_eventlog_recent_read(log, keep - 1U - i);
		if (entry)
			entries[i] = *entry;
	}

	free(log->entries);
	log->entries = entries;
	log->capacity = capacity;
	log->count = keep;
	log->next = capacity ? keep % capacity : 0;
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_eventlog_addr_from_legacy(eigrp_prefix_t *prefix,
				     const eigrp_addr_t *addr)
{
	if (!prefix)
		return;
	memset(prefix, 0, sizeof(*prefix));
	if (!addr)
		return;
	if (addr->afi == AF_INET) {
		prefix->address.afi = EIGRP_AFI_IPV4;
		memcpy(prefix->address.bytes, &addr->ip.v4, 4);
		prefix->prefix_length = 32;
	} else if (addr->afi == AF_INET6) {
		prefix->address.afi = EIGRP_AFI_IPV6;
		memcpy(prefix->address.bytes, &addr->ip.v6, 16);
		prefix->prefix_length = 128;
	}
}

eigrp_result_t eigrp_eventlog_msg_add(eigrp_instance_t *eigrp,
				     uint16_t opcode,
				     const eigrp_prefix_t *addr,
				     eventmsg_arg_t arg1,
				     eventmsg_arg_t arg2,
				     eventmsg_arg_t arg3,
				     eventmsg_arg_t arg4)
{
	eigrp_eventlog_t *log;
	eigrp_eventlog_msg_t *entry;

	if (!eigrp || !opcode)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	log = eigrp->eventlog;
	if (!log || !log->capacity || !log->entries)
		return EIGRP_RESULT_SUCCESS;

	entry = &log->entries[log->next];
	memset(entry, 0, sizeof(*entry));
	entry->timestamp = eigrp_sys_wallclock_msec();
	entry->opcode = opcode;
	if (addr)
		entry->addr = *addr;
	entry->arg1 = arg1;
	entry->arg2 = arg2;
	entry->arg3 = arg3;
	entry->arg4 = arg4;
	log->next = (log->next + 1U) % log->capacity;
	if (log->count < log->capacity)
		log->count++;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   EXEC: `clear eigrp ... events` / `clear eigrp events`
 * Supported: EXEC
 * Placement:
 *   Privileged operational
 * Description:
 * Clears EIGRP event history without changing retained configuration.
 * The operation terminates in the event-log module.
 */
eigrp_result_t eigrp_eventlog_clear(eigrp_instance_context_t *context)
{
	eigrp_eventlog_t *log;

	if (!context || !context->runtime)
		return EIGRP_RESULT_NOT_FOUND;
	log = context->runtime->eventlog;
	if (!log)
		return EIGRP_RESULT_NOT_FOUND;

	/* Logical clear is intentionally O(1); the fixed ring remains allocated. */
	log->count = 0;
	log->next = 0;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `eigrp event-log-size SIZE` / `no eigrp event-log-size`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or restores the EIGRP event-log capacity.
 * The target updates retained configuration and the live EIGRP event log when runtime exists.
 */
typedef struct eigrp_eventlog_message_args {
	eigrp_operation_t operation;
	eigrp_instance_context_t *context;
	uint32_t size;
} eigrp_eventlog_message_args_t;

static eigrp_result_t eigrp_eventlog_message_process(eigrp_instance_t *eigrp,
	void *arg);

eigrp_result_t eigrp_eventlog_size_update(eigrp_operation_t operation, eigrp_instance_context_t *context, uint32_t size)
{
	eigrp_eventlog_message_args_t message = { operation, context, size };

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_eventlog_message_process, &message);
	if (operation == EIGRP_RESET) {
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->runtime) {
		result = eigrp_eventlog_resize(context->runtime,
					       EIGRP_EVENTLOG_DEFAULT_SIZE);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
	}
	if (context->config) {
		context->config->event_log_size = EIGRP_EVENTLOG_DEFAULT_SIZE;
		context->config->event_log_size_configured = false;
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_result_t result = EIGRP_RESULT_SUCCESS;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->runtime) {
		result = eigrp_eventlog_resize(context->runtime, size);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
	}
	if (context->config) {
		context->config->event_log_size = size;
		context->config->event_log_size_configured = true;
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `eigrp event-log-size SIZE` / `no eigrp event-log-size`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or restores the EIGRP event-log capacity.
 * The target updates retained configuration and the live EIGRP event log when runtime exists.
 */


/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... events`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Reads EIGRP event-log state and entries for operational output.
 * FRR formats the portable event records but does not own their storage.
 */
static eigrp_result_t eigrp_eventlog_message_process(eigrp_instance_t *eigrp,
	void *arg)
{
	eigrp_eventlog_message_args_t *message = arg;
	(void)eigrp;
	return eigrp_eventlog_size_update(message->operation, message->context,
		message->size);
}

eigrp_result_t eigrp_eventlog_state_read(
	const eigrp_instance_context_t *context, eigrp_eventlog_state_t *state)
{
	const eigrp_eventlog_t *log;

	if (!state)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	memset(state, 0, sizeof(*state));
	if (!context || !context->runtime || !context->runtime->eventlog)
		return EIGRP_RESULT_NOT_FOUND;

	log = context->runtime->eventlog;
	state->capacity = log->capacity;
	state->count = log->count;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... events`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Reads EIGRP event-log state and entries for operational output.
 * FRR formats the portable event records but does not own their storage.
 */
eigrp_result_t eigrp_eventlog_msg_iterate(const eigrp_instance_context_t *context,
				   eigrp_eventlog_msg_cb callback,
				   void *arg)
{
	const eigrp_eventlog_t *log;
	const eigrp_eventlog_msg_t *entry;
	eigrp_result_t result;
	uint32_t i;

	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || !context->runtime || !context->runtime->eventlog)
		return EIGRP_RESULT_NOT_FOUND;
	log = context->runtime->eventlog;

	for (i = 0; i < log->count; i++) {
		entry = eigrp_eventlog_recent_read(log, i);
		if (!entry)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		result = callback(i + 1U, entry,
				  eigrp_eventlog_format_read(entry->opcode), arg);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
	}
	return EIGRP_RESULT_SUCCESS;
}
