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

#include "eigrp_instance.h"
#include "eigrp_result.h"
#include "eigrp_types.h"

#define EIGRP_EVENTLOG_DEFAULT_SIZE 500U

/*
 * Keep the per-event footprint fixed at exactly three machine words.  The
 * opcode is an index into the static event-format table; arg1 and arg2 are
 * the two values consumed by that format when the event is rendered.
 *
 * Do not add timestamps, pointers, flags, or sequence numbers to this
 * structure.  If future event encodings need additional metadata, encode it
 * into the existing words or keep it outside the per-entry ring storage.
 */
typedef struct eigrp_eventlog_entry {
	unsigned long opcode;
	unsigned long arg1;
	unsigned long arg2;
} eigrp_eventlog_entry_t;

_Static_assert(sizeof(eigrp_eventlog_entry_t) == 3 * sizeof(unsigned long),
	       "EIGRP event log entries must remain exactly three machine words");

typedef struct eigrp_eventlog_state {
	uint32_t capacity;
	uint32_t count;
} eigrp_eventlog_state_t;

typedef eigrp_result_t (*eigrp_eventlog_show_cb)(
	uint32_t event_number, const eigrp_eventlog_entry_t *entry,
	const char *format, void *arg);

/* Runtime ring lifecycle. */
eigrp_result_t eigrp_eventlog_init(eigrp_instance_t *eigrp, uint32_t capacity);
void eigrp_eventlog_finish(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_eventlog_resize(eigrp_instance_t *eigrp,
				     uint32_t capacity);

/* Future event producers use this API; no protocol event sites are wired yet. */
eigrp_result_t eigrp_eventlog_record(eigrp_instance_t *eigrp,
				     unsigned long opcode,
				     unsigned long arg1,
				     unsigned long arg2);

/* Operational/configuration targets. */
eigrp_result_t eigrp_eventlog_clear(eigrp_instance_context_t *context);
eigrp_result_t eigrp_eventlog_size_update(eigrp_instance_context_t *context,
					  uint32_t size);
eigrp_result_t eigrp_eventlog_size_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_eventlog_state_read(
	const eigrp_instance_context_t *context, eigrp_eventlog_state_t *state);
eigrp_result_t eigrp_eventlog_show(const eigrp_instance_context_t *context,
				   eigrp_eventlog_show_cb callback,
				   void *arg);

/* Static opcode-to-format lookup and trusted formatter used by show adapters. */
const char *eigrp_eventlog_format_read(unsigned long opcode);
eigrp_result_t eigrp_eventlog_entry_format(const eigrp_eventlog_entry_t *entry,
					 char *buffer, size_t buffer_size);

#endif /* EIGRPD_EIGRP_EVENTLOG_H_ */
