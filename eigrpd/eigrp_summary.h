// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP summarization configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_SUMMARY_H_
#define EIGRPD_EIGRP_SUMMARY_H_

#include "eigrp_cli.h"
#include "eigrp_interface.h"
#include "eigrp_metric.h"
#include "eigrp_structs.h"
#include "eigrp.h"

eigrp_result_t eigrp_summary_create(
	eigrp_intf_context_t *context, const eigrp_prefix_t *prefix,
	const eigrp_summary_options_t *options);
eigrp_result_t eigrp_summary_delete(
	eigrp_intf_context_t *context, const eigrp_prefix_t *prefix);
void eigrp_summary_delete_all(eigrp_intf_config_t *interface);

eigrp_result_t eigrp_summary_auto_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_summary_metric_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_t *prefix,
	const eigrp_summary_metric_config_t *config);
void eigrp_summary_state_delete_all(eigrp_af_instance_t *af);

/* Runtime advertisement helpers used by UPDATE/packetizer paths. */
bool eigrp_summary_route_build(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
			       const eigrp_prefix_descriptor_t *prefix,
			       const eigrp_route_descriptor_t *route,
			       eigrp_prefix_descriptor_t *summary_prefix,
			       eigrp_route_descriptor_t *summary_route);
void eigrp_summary_runtime_update(eigrp_instance_t *eigrp);

#endif /* EIGRPD_EIGRP_SUMMARY_H_ */
