// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP redistribution configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_REDISTRIBUTE_H_
#define EIGRPD_EIGRP_REDISTRIBUTE_H_

#include "eigrp_instance.h"
#include "eigrp_metric.h"
#include "eigrp.h"

eigrp_result_t eigrp_redist_add(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map);
eigrp_result_t eigrp_redist_remove(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source);
void eigrp_redist_config_delete_all(eigrp_af_instance_t *af);
void eigrp_redist_policy_update_all(void);

eigrp_result_t eigrp_redist_max_prefix_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_limit_t *limit);
void eigrp_redist_policy_delete_all(eigrp_af_instance_t *af);

#endif /* EIGRPD_EIGRP_REDISTRIBUTE_H_ */
