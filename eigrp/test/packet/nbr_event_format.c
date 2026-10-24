// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
/* Execute the production neighbor event formatter and preserve event IDs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_eventlog.h"

bool eigrp_prefix_valid(const eigrp_prefix_t *prefix)
{
	(void)prefix;
	return false;
}

int eigrp_prefix_snprintf(char *buf, size_t len, const eigrp_prefix_t *prefix)
{
	(void)prefix;
	return snprintf(buf, len, "10.0.0.2/32");
}

static void verify(eventmsg_arg_t event, const char *expected)
{
	eigrp_eventlog_msg_t entry = {0};
	char message[512];

	entry.opcode = EIGRP_EVENTLOG_OPCODE_NEIGHBOR_STATE_CHANGE;
	entry.arg1 = EIGRP_NEIGHBOR_UP;
	entry.arg2 = EIGRP_NEIGHBOR_DOWN;
	entry.arg3 = 3;
	entry.arg4 = event;
	assert(eigrp_eventlog_msg_format(&entry, message, sizeof(message))
	       == EIGRP_RESULT_SUCCESS);
	assert(strstr(message, "UP -> DOWN"));
	assert(strstr(message, expected));
	assert(!strstr(message, "unknown"));
}

int main(void)
{
	/* Existing ring entries retain their previous numeric meaning. */
	assert(EIGRP_EVENTLOG_NBR_NONE == 0);
	assert(EIGRP_EVENTLOG_NBR_HOLD_EXPIRED == 1);
	assert(EIGRP_EVENTLOG_NBR_INTF_DOWN == 7);
	assert(EIGRP_EVENTLOG_NBR_SIA == 9);
	assert(EIGRP_EVENTLOG_NBR_PREFIX_LIMIT == 10);

	verify(EIGRP_EVENTLOG_NBR_HOLD_EXPIRED, "holding time expired");
	verify(EIGRP_EVENTLOG_NBR_K_MISMATCH, "K-value mismatch");
	verify(EIGRP_EVENTLOG_NBR_INTF_DOWN, "interface down");
	verify(EIGRP_EVENTLOG_NBR_SIA, "stuck in active");
	verify(EIGRP_EVENTLOG_NBR_PREFIX_LIMIT, "prefix limit exceeded");
	puts("neighbor event format: PASS");
	return 0;
}
