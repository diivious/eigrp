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
#include "eigrp.h"

eigrp_result_t eigrp_summary_create(
	eigrp_interface_context_t *context, const eigrp_prefix_t *prefix,
	const eigrp_summary_options_t *options);
eigrp_result_t eigrp_summary_delete(
	eigrp_interface_context_t *context, const eigrp_prefix_t *prefix);
void eigrp_summary_delete_all(eigrp_interface_config_t *interface);

eigrp_result_t eigrp_summary_auto_set(eigrp_instance_context_t *context);
eigrp_result_t eigrp_summary_auto_reset(eigrp_instance_context_t *context);
eigrp_result_t eigrp_summary_metric_set(eigrp_instance_context_t *context,
					   const eigrp_prefix_t *prefix,
					   const eigrp_summary_metric_config_t *config);
eigrp_result_t eigrp_summary_metric_reset(eigrp_instance_context_t *context,
					   const eigrp_prefix_t *prefix);
void eigrp_summary_state_delete_all(eigrp_address_family_config_t *af);

#endif /* EIGRPD_EIGRP_SUMMARY_H_ */
