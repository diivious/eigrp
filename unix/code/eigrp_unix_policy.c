// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix host policy-service bindings.
 *
 * The standalone Unix adapter does not yet have native ACL, prefix-list,
 * route-map, or summary leak-map databases. These are the existing neutral
 * outcomes previously supplied by the UUT process, relocated to the owning
 * host adapter so test executables no longer implement eigrp_sys_* hooks.
 *
 * A full Unix policy implementation must replace these neutral evaluations;
 * this file does not represent an implemented Unix policy engine.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrp_sys.h"

void eigrp_sys_policy_init(void)
{
}

void eigrp_sys_policy_finish(void)
{
}

eigrp_result_t eigrp_sys_policy_instance_create(eigrp_instance_t *eigrp)
{
    (void)eigrp;
    return EIGRP_RESULT_SUCCESS;
}

void eigrp_sys_policy_instance_delete(eigrp_instance_t *eigrp)
{
    (void)eigrp;
}

eigrp_result_t eigrp_sys_filter_evaluate(
    eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
    const char *name, const eigrp_prefix_t *prefix,
    eigrp_filter_decision_t *decision)
{
    (void)eigrp;
    (void)type;
    (void)name;
    (void)prefix;
    if (!decision)
        return EIGRP_RESULT_INVALID_ARGUMENT;
    *decision = EIGRP_FILTER_DECISION_PERMIT;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_sys_redistribute_route_map_evaluate(
    eigrp_instance_t *eigrp, const char *name,
    const eigrp_rib_route_t *route, eigrp_filter_decision_t *decision)
{
    (void)eigrp;
    (void)name;
    (void)route;
    if (!decision)
        return EIGRP_RESULT_INVALID_ARGUMENT;
    *decision = EIGRP_FILTER_DECISION_PERMIT;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_sys_summary_leak_map_evaluate(
    eigrp_instance_t *eigrp, const char *name, const eigrp_prefix_t *prefix,
    eigrp_filter_decision_t *decision)
{
    (void)eigrp;
    (void)name;
    (void)prefix;
    if (!decision)
        return EIGRP_RESULT_INVALID_ARGUMENT;
    *decision = EIGRP_FILTER_DECISION_DENY;
    return EIGRP_RESULT_SUCCESS;
}
