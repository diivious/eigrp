// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP FRR logging adapter. Copyright (C) 2026 Donnie V. Savage */
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

#include <zebra.h>
#include "log.h"

#include "eigrpd/eigrp_log.h"

#define EIGRP_FRR_LOG_BUFFER_SIZE 2048U

static void eigrp_log_vwrite(int priority, const char *format, va_list ap)
{
	char message[EIGRP_FRR_LOG_BUFFER_SIZE];

	vsnprintf(message, sizeof(message), format, ap);
	switch (priority) {
	case LOG_DEBUG:
		zlog_debug("%s", message);
		break;
	case LOG_INFO:
		zlog_info("%s", message);
		break;
	case LOG_NOTICE:
		zlog_notice("%s", message);
		break;
	case LOG_WARNING:
		zlog_warn("%s", message);
		break;
	case LOG_ERR:
	default:
		zlog_err("%s", message);
		break;
	}
}

#define EIGRP_LOG_IMPL(NAME, PRIORITY)                                        \
	void NAME(const char *format, ...)                                      \
	{                                                                        \
		va_list ap;                                                        \
		va_start(ap, format);                                              \
		eigrp_log_vwrite(PRIORITY, format, ap);                            \
		va_end(ap);                                                        \
	}

EIGRP_LOG_IMPL(eigrp_log_debug, LOG_DEBUG)
EIGRP_LOG_IMPL(eigrp_log_info, LOG_INFO)
EIGRP_LOG_IMPL(eigrp_log_notice, LOG_NOTICE)
EIGRP_LOG_IMPL(eigrp_log_warn, LOG_WARNING)
EIGRP_LOG_IMPL(eigrp_log_error, LOG_ERR)
