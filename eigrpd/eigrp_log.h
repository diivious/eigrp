// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP portable logging contract. Copyright (C) 2026 Donnie V. Savage */
#ifndef _EIGRP_LOG_H_
#define _EIGRP_LOG_H_

void eigrp_log_debug(const char *format, ...);
void eigrp_log_info(const char *format, ...);
void eigrp_log_notice(const char *format, ...);
void eigrp_log_warn(const char *format, ...);
void eigrp_log_error(const char *format, ...);

#endif /* _EIGRP_LOG_H_ */
