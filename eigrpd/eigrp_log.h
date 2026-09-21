// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP portable logging contract. Copyright (C) 2026 Donnie V. Savage */
#ifndef _EIGRP_LOG_H_
#define _EIGRP_LOG_H_

typedef enum eigrp_log_level {
	EIGRP_LOG_DEBUG = 0,
	EIGRP_LOG_INFO,
	EIGRP_LOG_NOTICE,
	EIGRP_LOG_WARNING,
	EIGRP_LOG_ERROR
} eigrp_log_level_t;

void eigrp_log(eigrp_log_level_t level, const char *format, ...);

#endif /* _EIGRP_LOG_H_ */
