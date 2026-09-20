// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP compact event log.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_eventlog.h"

struct eigrp_eventlog {
	eigrp_eventlog_entry_t *entries;
	uint32_t capacity;
	uint32_t count;
	uint32_t next;
};

/*
 * Event strings are intentionally empty in this pass.  Future event producers
 * add an opcode and its printf-style text here.  The opcode stored in each
 * three-word ring entry is the direct index into this table.
 */
static const char *const eigrp_eventlog_formats[] = {
	NULL,
};

static const eigrp_eventlog_entry_t *
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

/*
 * Event formats are selected only from the private static table above.  Keep
 * the dynamic printf handling here rather than in CLI adapters: vsnprintf()
 * accepts a va_list, so FRR builds with -Wformat-nonliteral remain clean while
 * preserving the compact opcode + two-word event representation.
 */
static int eigrp_eventlog_format_apply(char *buffer, size_t buffer_size,
				       const char *format, ...)
{
	va_list ap;
	int written;

	va_start(ap, format);
	written = vsnprintf(buffer, buffer_size, format, ap);
	va_end(ap);
	return written;
}

eigrp_result_t eigrp_eventlog_entry_format(const eigrp_eventlog_entry_t *entry,
					 char *buffer, size_t buffer_size)
{
	const char *format;

	if (!entry || !buffer || !buffer_size)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	format = eigrp_eventlog_format_read(entry->opcode);
	if (format)
		(void)eigrp_eventlog_format_apply(buffer, buffer_size, format,
					  entry->arg1, entry->arg2);
	else
		(void)snprintf(buffer, buffer_size, "opcode %lu args %lu %lu",
			       entry->opcode, entry->arg1, entry->arg2);

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

void eigrp_eventlog_finish(eigrp_instance_t *eigrp)
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
	eigrp_eventlog_entry_t *entries = NULL;
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
		const eigrp_eventlog_entry_t *entry =
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

eigrp_result_t eigrp_eventlog_record(eigrp_instance_t *eigrp,
				     unsigned long opcode,
				     unsigned long arg1,
				     unsigned long arg2)
{
	eigrp_eventlog_t *log;
	eigrp_eventlog_entry_t *entry;

	if (!eigrp || !opcode)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	log = eigrp->eventlog;
	if (!log || !log->capacity || !log->entries)
		return EIGRP_RESULT_SUCCESS;

	entry = &log->entries[log->next];
	entry->opcode = opcode;
	entry->arg1 = arg1;
	entry->arg2 = arg2;
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
eigrp_result_t eigrp_eventlog_size_update(eigrp_instance_context_t *context,
					  uint32_t size)
{
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
eigrp_result_t eigrp_eventlog_size_delete(eigrp_instance_context_t *context)
{
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
eigrp_result_t eigrp_eventlog_show(const eigrp_instance_context_t *context,
				   eigrp_eventlog_show_cb callback,
				   void *arg)
{
	const eigrp_eventlog_t *log;
	const eigrp_eventlog_entry_t *entry;
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
