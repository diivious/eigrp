// SPDX-License-Identifier: GPL-2.0-or-later
/* AF-owned topology maximum-prefix enforcement. Copyright (C) 2026 Donnie V. Savage */
#ifndef EIGRP_PFXLIMITS_H_
#define EIGRP_PFXLIMITS_H_
#include "eigrp_types.h"
void eigrp_pfxlimits_prefix_exceeded(eigrp_instance_t *runtime, const eigrp_prefix_limit_t *limit);
void eigrp_pfxlimits_state_cleanup(eigrp_instance_t *runtime);
void eigrp_pfxlimits_restart_expired(void *arg);
eigrp_result_t eigrp_pfxlimits_state_clear(eigrp_instance_t *runtime);
#endif
