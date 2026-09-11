// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP CLI Functions.
 * Copyright (C) 2019
 * Authors:
 *   Donnie Savage
 */

#ifndef _EIGRP_CLI_H_
#define _EIGRP_CLI_H_

#include "eigrp_result.h"

/*Prototypes*/
/* Named-mode FRR/YANG writeback. */
extern void eigrp_cli_show_named_header(struct vty *vty,
				const struct lyd_node *dnode,
				bool show_defaults);
extern void eigrp_cli_show_named_end(struct vty *vty,
			     const struct lyd_node *dnode);
extern void eigrp_cli_show_named_address_family(struct vty *vty,
					const struct lyd_node *dnode,
					bool show_defaults);
extern void eigrp_cli_show_named_address_family_end(
	struct vty *vty, const struct lyd_node *dnode);
extern void eigrp_cli_show_named_neighbor(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_shutdown(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_end(
	struct vty *vty, const struct lyd_node *dnode);
extern void eigrp_cli_show_named_af_interface_bandwidth(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_hello(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_hold(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_passive(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_authentication(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_keychain(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_next_hop_self(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_split_horizon(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_summary(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_af_interface_shutdown(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_topology(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_topology_end(
	struct vty *vty, const struct lyd_node *dnode);
extern void eigrp_cli_show_named_auto_summary(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_default_information_in(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_default_information_out(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_default_metric(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_distance(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_maximum_prefix(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_metric_weights(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_offset_list(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_redistribute(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_summary_metric(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_active_time(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_traffic_share_balanced(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_variance(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_neighbor_description(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_neighbor_maximum_prefix(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_neighbor_maximum_prefix_all(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_log_neighbor_changes(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_log_neighbor_warnings(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_maximum_paths(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_metric_maximum_hops(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_metric_holddown(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_event_log_size(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_redistribute_maximum_prefix(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_show_named_distribute_list(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);

extern void eigrp_cli_show_header(struct vty *vty, const struct lyd_node *dnode,
				  bool show_defaults);
extern void eigrp_cli_show_end_header(struct vty *vty,
				      const struct lyd_node *dnode);
extern void eigrp_cli_show_router_id(struct vty *vty,
				     const struct lyd_node *dnode,
				     bool show_defaults);
extern void eigrp_cli_show_passive_interface(struct vty *vty,
					     const struct lyd_node *dnode,
					     bool show_defaults);
extern void eigrp_cli_show_variance(struct vty *vty,
				    const struct lyd_node *dnode,
				    bool show_defaults);
extern void eigrp_cli_show_maximum_paths(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults);
extern void eigrp_cli_show_metrics(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults);
extern void eigrp_cli_show_network(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults);
extern void eigrp_cli_show_redistribute(struct vty *vty,
					const struct lyd_node *dnode,
					bool show_defaults);
extern int eigrp_cli_result_render(struct vty *vty, const char *operation,
				   eigrp_result_t result);
extern void eigrp_cli_init(void);

#endif /*EIGRP_CLI_H_ */
