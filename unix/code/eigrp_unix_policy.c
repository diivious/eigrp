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
#ifdef EIGRP_UUT_TEST
#include <string.h>
#include "eigrp_prefix.h"

/* Only the UUT executable enables this deterministic host ACL fixture. */
static eigrp_prefix_t uut_denied_prefix;
static bool uut_denied_active;

void eigrp_unix_policy_uut_deny_set(const eigrp_prefix_t *prefix)
{
    uut_denied_active = prefix != NULL;
    if (prefix) {
        uut_denied_prefix = *prefix;
        eigrp_prefix_normalize(&uut_denied_prefix);
    }
}
#endif

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
#ifdef EIGRP_UUT_TEST
    if (uut_denied_active && name && prefix &&
        type == EIGRP_DISTRIBUTE_ACCESS_LIST &&
        strcmp(name, "uut-inbound-deny") == 0 &&
        prefix->address.afi == uut_denied_prefix.address.afi &&
        prefix->prefix_length == uut_denied_prefix.prefix_length &&
        memcmp(prefix->address.bytes, uut_denied_prefix.address.bytes,
               prefix->address.afi == EIGRP_AFI_IPV4 ? 4U : 16U) == 0)
        *decision = EIGRP_FILTER_DECISION_DENY;
#endif
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
