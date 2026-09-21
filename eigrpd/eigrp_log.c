// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP portable logging fallback. Copyright (C) 2026 Donnie V. Savage */
#include <stdarg.h>
#include <stdio.h>

#include "eigrpd/eigrp_log.h"

static void eigrp_log_vwrite(const char *level, const char *format, va_list ap)
{
	if (level)
		fprintf(stderr, "EIGRP %s: ", level);
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
}

#define EIGRP_LOG_IMPL(NAME, LEVEL) \
void NAME(const char *format, ...) \
{ \
	va_list ap; \
	va_start(ap, format); \
	eigrp_log_vwrite(LEVEL, format, ap); \
	va_end(ap); \
}

EIGRP_LOG_IMPL(eigrp_log_debug, "debug")
EIGRP_LOG_IMPL(eigrp_log_info, "info")
EIGRP_LOG_IMPL(eigrp_log_notice, "notice")
EIGRP_LOG_IMPL(eigrp_log_warn, "warning")
EIGRP_LOG_IMPL(eigrp_log_error, "error")
