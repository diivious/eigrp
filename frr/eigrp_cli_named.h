// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Named-Mode CLI Functions.
 * Copyright (C) 2019
 * Authors:
 *   Donnie Savage
 */

#ifndef _EIGRP_CLI_NAMED_H_
#define _EIGRP_CLI_NAMED_H_

#include "eigrp_result.h"

/* Named-mode FRR/YANG writeback. */
extern void eigrp_cli_named_show_header(struct vty *vty,
				const struct lyd_node *dnode,
				bool show_defaults);
extern void eigrp_cli_named_show_end(struct vty *vty,
			     const struct lyd_node *dnode);
extern void eigrp_cli_named_show_address_family(struct vty *vty,
					const struct lyd_node *dnode,
					bool show_defaults);
extern void eigrp_cli_named_show_address_family_end(
	struct vty *vty, const struct lyd_node *dnode);
extern void eigrp_cli_named_show_neighbor(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_shutdown(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_end(
	struct vty *vty, const struct lyd_node *dnode);
extern void eigrp_cli_named_show_af_interface_bandwidth(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_hello(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_hold(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_passive(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_authentication(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_keychain(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_next_hop_self(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_split_horizon(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_summary(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_af_interface_shutdown(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_topology(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_topology_end(
	struct vty *vty, const struct lyd_node *dnode);
extern void eigrp_cli_named_show_auto_summary(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_default_information_in(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_default_information_out(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_default_metric(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_distance(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_maximum_prefix(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_metric_weights(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_offset_list(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_redistribute(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_summary_metric(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_active_time(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_traffic_share_balanced(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_variance(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_neighbor_description(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_neighbor_maximum_prefix(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_neighbor_maximum_prefix_all(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_log_neighbor_changes(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_log_neighbor_warnings(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_maximum_paths(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_metric_maximum_hops(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_metric_holddown(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_event_log_size(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_redistribute_maximum_prefix(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_distribute_list(struct vty *vty,
	const struct lyd_node *dnode, bool show_defaults);

extern void eigrp_cli_named_show_router_id(struct vty *vty,
    const struct lyd_node *dnode, bool show_defaults);
extern void eigrp_cli_named_show_network(struct vty *vty,
    const struct lyd_node *dnode, bool show_defaults);

extern bool eigrp_cli_named_context(struct vty *vty);
extern int eigrp_cli_named_active_time_apply(struct vty *vty, bool disabled,
    const char *timer, bool remove);
extern int eigrp_cli_named_variance_apply(struct vty *vty,
    const char *variance, bool remove);
extern int eigrp_cli_named_maximum_paths_apply(struct vty *vty,
    const char *maximum_paths, bool remove);
extern int eigrp_cli_named_metric_weights_apply(struct vty *vty,
    const char *tos, const char *k1, const char *k2, const char *k3,
    const char *k4, const char *k5, bool has_k5, bool remove);
extern int eigrp_cli_named_redistribute_apply(struct vty *vty,
    const char *protocol, uint32_t bandwidth, const char *bandwidth_text,
    uint32_t delay, const char *delay_text, uint8_t reliability,
    const char *reliability_text, uint8_t load, const char *load_text,
    uint32_t mtu, const char *mtu_text, const char *route_map, bool remove);
extern int eigrp_cli_result_render(struct vty *vty, const char *operation,
    eigrp_result_t result);
extern void eigrp_cli_named_init(void);

#endif /* _EIGRP_CLI_NAMED_H_ */
