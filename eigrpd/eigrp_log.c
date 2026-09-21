// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP portable logging fallback. Copyright (C) 2026 Donnie V. Savage */
#include <stdarg.h>
#include <stdio.h>

#include "eigrpd/eigrp_log.h"

static const char *eigrp_log_level_name(eigrp_log_level_t level)
{
	switch (level) {
	case EIGRP_LOG_DEBUG:
		return "debug";
	case EIGRP_LOG_INFO:
		return "info";
	case EIGRP_LOG_NOTICE:
		return "notice";
	case EIGRP_LOG_WARNING:
		return "warning";
	case EIGRP_LOG_ERROR:
	default:
		return "error";
	}
}

void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{
	va_list ap;

	fprintf(stderr, "EIGRP %s: ", eigrp_log_level_name(level));
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	fputc('\n', stderr);
}
