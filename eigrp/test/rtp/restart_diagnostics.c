// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
/* Exercise the production bounded event ring and restart formatter. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_eventlog.h"
#include "eigrp_sys.h"

static uint64_t clock_msec = 1792446000000ULL;
uint64_t eigrp_sys_wallclock_msec(void) { return clock_msec++; }

/* The test controls the address formatter; ring and event formatting are real. */
bool eigrp_prefix_valid(const eigrp_prefix_t *prefix)
{
	return prefix && prefix->address.afi == EIGRP_AFI_IPV4;
}
int eigrp_prefix_snprintf(char *buf, size_t len, const eigrp_prefix_t *prefix)
{
	(void)prefix;
	return snprintf(buf, len, "10.0.0.2/32");
}

struct output {
	unsigned count;
	char lines[8][512];
};
static eigrp_result_t record(uint32_t number, const eigrp_eventlog_msg_t *entry,
	const char *format, void *arg)
{
	struct output *out = arg;
	(void)format;
	assert(number == ++out->count);
	assert(out->count <= 8);
	assert(eigrp_eventlog_msg_format(entry, out->lines[out->count-1],
		sizeof(out->lines[0])) == EIGRP_RESULT_SUCCESS);
	return EIGRP_RESULT_SUCCESS;
}

int main(void)
{
	eigrp_instance_t runtime = {0};
	eigrp_instance_context_t context = {0};
	eigrp_eventlog_state_t state;
	eigrp_prefix_t peer = {0};
	struct output out = {0};
	char line[512];
	eigrp_eventlog_msg_t event = {0};

	peer.address.afi = EIGRP_AFI_IPV4;
	peer.prefix_length = 32;
	context.runtime = &runtime;
	assert(eigrp_eventlog_init(&runtime, 4) == EIGRP_RESULT_SUCCESS);

	event.addr = peer;
	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_RX;
	event.arg1 = 1; event.arg2 = 1; event.arg3 = 2; event.arg4 = 2;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "RTP INIT received") && strstr(line, "previous 1"));

	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_STATE;
	event.arg1 = 2; event.arg2 = 1; event.arg3 = 1; event.arg4 = 15;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "RTP INIT state") && strstr(line, "hold-armed 1"));

	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_DECISION;
	event.arg2 = EIGRP_EVENTLOG_INIT_REPLAY_ACK; event.arg3 = 1; event.arg4 = 1;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "action replay-ack") && strstr(line, "pending 1"));
	event.arg2 = EIGRP_EVENTLOG_INIT_DUPLICATE_ACK;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "action duplicate-ack"));

	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_EOT;
	event.arg1 = 2; event.arg2 = 1; event.arg3 = 1; event.arg4 = 1;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "accepted-init 1") && strstr(line, "pending 1"));

	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_HOLD;
	event.arg1 = 1; event.arg2 = 15; event.arg3 = 1;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "hold rearm") && strstr(line, "armed 1"));

	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_RESYNC;
	event.arg1 = EIGRP_EVENTLOG_INIT_RESYNC_START;
	event.arg2 = 100; event.arg3 = 0; event.arg4 = 0;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "resync") && strstr(line, "snapshot 100"));

	event.opcode = EIGRP_EVENTLOG_OPCODE_RTP_INIT_SYNC;
	event.arg1 = EIGRP_EVENTLOG_INIT_SYNC_TABLE_START;
	event.arg2 = 0; event.arg3 = 2; event.arg4 = 1;
	assert(eigrp_eventlog_msg_format(&event, line, sizeof(line)) == EIGRP_RESULT_SUCCESS);
	assert(strstr(line, "phase table-start") && strstr(line, "active 1"));

	/* Exercise actual allocation, append, wraparound, and chronological iteration. */
	assert(eigrp_eventlog_msg_add(&runtime, EIGRP_EVENTLOG_OPCODE_RTP_INIT_RX,
		&peer, 1, 1, 0, 2) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_eventlog_msg_add(&runtime, EIGRP_EVENTLOG_OPCODE_RTP_INIT_DECISION,
		&peer, 1, EIGRP_EVENTLOG_INIT_REPLAY_ACK, 1, 1) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_eventlog_msg_add(&runtime, EIGRP_EVENTLOG_OPCODE_RTP_INIT_RESYNC,
		&peer, EIGRP_EVENTLOG_INIT_RESYNC_START, 3, 0, 0) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_eventlog_msg_add(&runtime, EIGRP_EVENTLOG_OPCODE_RTP_INIT_EOT,
		&peer, 2, 1, 1, 1) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_eventlog_msg_add(&runtime, EIGRP_EVENTLOG_OPCODE_RTP_INIT_RESYNC,
		&peer, EIGRP_EVENTLOG_INIT_RESYNC_COMPLETE, 0, 1, 1) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_eventlog_state_read(&context, &state) == EIGRP_RESULT_SUCCESS);
	assert(state.capacity == 4 && state.count == 4 && state.overwritten == 1);
	assert(eigrp_eventlog_msg_iterate(&context, record, &out) == EIGRP_RESULT_SUCCESS);
	assert(out.count == 4);
	assert(strstr(out.lines[0], "phase complete"));
	assert(strstr(out.lines[1], "RTP INIT EOT"));
	assert(strstr(out.lines[2], "phase start"));
	assert(strstr(out.lines[3], "action replay-ack"));
	eigrp_eventlog_delete(&runtime);
	puts("restart diagnostic ring: PASS");
	return 0;
}
