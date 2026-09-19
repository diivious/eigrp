// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR policy adaptation.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_POLICY_H_
#define EIGRPD_EIGRP_POLICY_H_

#include "distribute.h"
#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"

void eigrp_policy_init(void);
void eigrp_policy_finish(void);

eigrp_result_t eigrp_policy_instance_create(eigrp_instance_t *eigrp);
void eigrp_policy_instance_delete(eigrp_instance_t *eigrp);
struct distribute_ctx *eigrp_policy_distribute_context(eigrp_instance_t *eigrp);

eigrp_result_t eigrp_policy_filter_evaluate(
        eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
        const char *name, const eigrp_prefix_t *prefix,
        eigrp_filter_decision_t *decision);

#endif /* EIGRPD_EIGRP_POLICY_H_ */
