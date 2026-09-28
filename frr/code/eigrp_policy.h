// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR policy adaptation.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_POLICY_H_
#define EIGRPD_EIGRP_POLICY_H_

#include "distribute.h"
#include "eigrp.h"
#include "eigrp_rib.h"

void eigrp_policy_init(void);
void eigrp_policy_finish(void);

eigrp_result_t eigrp_policy_instance_create(eigrp_instance_t *eigrp);
void eigrp_policy_instance_delete(eigrp_instance_t *eigrp);
struct distribute_ctx *eigrp_policy_distribute_context(eigrp_instance_t *eigrp);

eigrp_result_t eigrp_policy_filter_evaluate(
        eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
        const char *name, const eigrp_prefix_t *prefix,
        eigrp_filter_decision_t *decision);

eigrp_result_t eigrp_policy_redistribute_route_map_evaluate(
        eigrp_instance_t *eigrp, const char *name,
        const eigrp_rib_source_route_t *route,
        eigrp_filter_decision_t *decision);

#endif /* EIGRPD_EIGRP_POLICY_H_ */
