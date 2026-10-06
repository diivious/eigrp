// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP compact event log.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_EVENTLOG_H_
#define EIGRPD_EIGRP_EVENTLOG_H_

#include <stddef.h>
#include <stdint.h>

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_instance.h"
#include "eigrp.h"
#include "eigrp_types.h"

#define EIGRP_EVENTLOG_DEFAULT_SIZE 500U

typedef enum eigrp_eventlog_opcode {
	EIGRP_EVENTLOG_OPCODE_NONE = 0,
	EIGRP_EVENTLOG_OPCODE_IPV6_NO_ROUTER_ID,
	EIGRP_EVENTLOG_OPCODE_DUAL_STATE_CHANGE,
	EIGRP_EVENTLOG_OPCODE_NEIGHBOR_STATE_CHANGE,
	EIGRP_EVENTLOG_OPCODE_PACKET_RX,
	EIGRP_EVENTLOG_OPCODE_PACKET_TX,
	EIGRP_EVENTLOG_OPCODE_RTP_ACK,
	EIGRP_EVENTLOG_OPCODE_RTP_RETRANSMIT,
	EIGRP_EVENTLOG_OPCODE_RTP_RETRY_LIMIT,
	EIGRP_EVENTLOG_OPCODE_SUMMARY_METRIC,
	EIGRP_EVENTLOG_OPCODE_SUMMARY_COMPONENT,
	EIGRP_EVENTLOG_OPCODE_SUMMARY_WITHDRAW,
	EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_UPDATE,
	EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_DELETE,
	EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_UPDATE,
	EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_WITHDRAW,
	EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_FILTERED,
	EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_REJECT,
} eigrp_eventlog_opcode_t;

/*
 * Event entries are self-contained snapshots.  addr is the primary address or
 * prefix for the event; scalar arguments must never retain pointers to mutable
 * EIGRP objects.  A logical operation may emit multiple compact records when
 * more context is required.
 */
/* Runtime ring lifecycle. */
eigrp_result_t eigrp_eventlog_init(eigrp_instance_t *eigrp, uint32_t capacity);
void eigrp_eventlog_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_eventlog_resize(eigrp_instance_t *eigrp,
				     uint32_t capacity);

/* Append one self-contained event snapshot to the bounded ring. */
eigrp_result_t eigrp_eventlog_msg_add(eigrp_instance_t *eigrp,
				     uint16_t opcode,
				     const eigrp_prefix_t *addr,
				     eventmsg_arg_t arg1,
				     eventmsg_arg_t arg2,
				     eventmsg_arg_t arg3,
				     eventmsg_arg_t arg4);
void eigrp_eventlog_addr_from_legacy(eigrp_prefix_t *prefix,
				     const eigrp_addr_t *addr);

/* Operational/configuration targets. */
eigrp_result_t eigrp_eventlog_clear(eigrp_instance_context_t *context);
eigrp_result_t eigrp_eventlog_size_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint32_t size);
eigrp_result_t eigrp_eventlog_state_read(
	const eigrp_instance_context_t *context, eigrp_eventlog_state_t *state);
eigrp_result_t eigrp_eventlog_msg_iterate(const eigrp_instance_context_t *context,
				   eigrp_eventlog_msg_cb callback,
				   void *arg);

/* Static opcode-to-format lookup and trusted formatter used by show adapters. */
const char *eigrp_eventlog_format_read(unsigned long opcode);
eigrp_result_t eigrp_eventlog_msg_format(const eigrp_eventlog_msg_t *entry,
					 char *buffer, size_t buffer_size);

#endif /* EIGRPD_EIGRP_EVENTLOG_H_ */
