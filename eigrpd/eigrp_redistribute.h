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
#include "eigrp_result.h"

eigrp_result_t eigrp_redistribute_update(eigrp_instance_context_t *context,
					 const char *protocol,
					 const eigrp_metric_values_t *metric,
					 const char *route_map);
eigrp_result_t eigrp_redistribute_delete(eigrp_instance_context_t *context,
					 const char *protocol);
void eigrp_redistribute_config_delete_all(eigrp_address_family_config_t *af);

eigrp_result_t eigrp_redistribute_maximum_prefix_update(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_redistribute_maximum_prefix_delete(
	eigrp_instance_context_t *context);

#endif /* EIGRPD_EIGRP_REDISTRIBUTE_H_ */
