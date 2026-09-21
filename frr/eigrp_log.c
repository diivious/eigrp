// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP FRR logging adapter. Copyright (C) 2026 Donnie V. Savage */
#include <stdarg.h>
#include <stdio.h>

#include <zebra.h>
#include "log.h"

#include "eigrpd/eigrp_log.h"

#define EIGRP_FRR_LOG_BUFFER_SIZE 2048U

static void eigrp_log_write(eigrp_log_level_t level, const char *message)
{
	switch (level) {
	case EIGRP_LOG_DEBUG:
		zlog_debug("%s", message);
		break;
	case EIGRP_LOG_INFO:
		zlog_info("%s", message);
		break;
	case EIGRP_LOG_NOTICE:
		zlog_notice("%s", message);
		break;
	case EIGRP_LOG_WARNING:
		zlog_warn("%s", message);
		break;
	case EIGRP_LOG_ERROR:
	default:
		zlog_err("%s", message);
		break;
	}
}

void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{
	char message[EIGRP_FRR_LOG_BUFFER_SIZE];
	va_list ap;

	va_start(ap, format);
	vsnprintf(message, sizeof(message), format, ap);
	va_end(ap);
	eigrp_log_write(level, message);
}
