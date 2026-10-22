// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP address-family process maximum-prefix penalty. Copyright (C) 2026 Donnie V. Savage */
#ifndef EIGRP_PROCESS_LIMIT_H_
#define EIGRP_PROCESS_LIMIT_H_
#include "eigrp_types.h"
void eigrp_process_limit_exceeded(eigrp_instance_t *runtime, const eigrp_prefix_limit_t *limit);
void eigrp_process_limit_cleanup(eigrp_instance_t *runtime);
void eigrp_process_limit_restart_expired(void *arg);
eigrp_result_t eigrp_process_limit_clear(eigrp_instance_t *runtime);
#endif
