// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP daemon classic CLI implementation.
 *
 * Copyright (C) 2019 Network Device Education Foundation, Inc. ("NetDEF")
 *                    Rafael Zalamena
 */

#include <zebra.h>

#include "lib/command.h"
#include "lib/if.h"
#include "lib/log.h"
#include "lib/northbound_cli.h"

#include "eigrp_structs.h"
#include "eigrpd.h"
#include "eigrp_zebra.h"
#include "eigrp_cli_classic.h"
#include "eigrp_cli_named.h"

#ifndef EIGRP_STANDALONE_BUILD
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "eigrpd/eigrp_cli_classic_clippy.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#ifdef EIGRP_STANDALONE_BUILD
/* Standalone compile shim for variables normally supplied by FRR clippy. */
static const char *as_str = "1";
static const char *vrf = NULL;
static const char *addr_str = "0.0.0.0";
static const char *ifname = NULL;
static bool no = false;
static bool disabled = false;
static const char *timer_str = "1";
static const char *variance_str = "1";
static const char *maximum_paths_str = "1";
static int64_t tos = 0;
static const char *tos_str = "0";
static const char *k1_str = "1";
static const char *k2_str = "0";
static const char *k3_str = "1";
static const char *k4_str = "0";
static const char *k5_str = "0";
static const char *k6_str = "0";
static const char *bw_str = "1";
static const char *delay_str = "1";
static const char *rlbt_str = "255";
static const char *load_str = "1";
static const char *mtu_str = "1500";
static const char *hello_str = "5";
static const char *hold_str = "15";
static bool k6 = false;
static const char *prefix_str = "0.0.0.0/0";
static const char *dir = "in";
static const char *name = "stub";
static const char *proto = "connected";
static int64_t route_instance = 0;
static const char *route_map = NULL;
static uint32_t bw = 1;
static uint32_t delay = 1;
static uint8_t rlbt = 255;
static uint8_t load = 1;
static uint32_t mtu = 1500;
static const char *auth_mode = "md5";
#endif

static const struct lyd_node *
eigrp_cli_classic_parent_named(const struct lyd_node *dnode, const char *name)
{
	const struct lyd_node *node = dnode;

	while ((node = lyd_parent(node)) != NULL) {
		if (node->schema && !strcmp(node->schema->name, name))
			return node;
	}

	return NULL;
}

static void eigrp_cli_classic_config_rewind(struct vty *vty)
{
	if (!vty)
		return;
	vty->xpath_index = 0;
	vty->node = CONFIG_NODE;
}

/*
 * Syntax: `router eigrp <1-65535> [vrf NAME]`
 * Mode: Classic router configuration
 * XPath: /frr-eigrpd:eigrpd/instance
 * Target: eigrpd_instance_create() -> eigrp_get()
 */
DEFPY_YANG_NOSH(
	router_eigrp,
	router_eigrp_cmd,
	"router eigrp (1-65535)$as [vrf NAME]",
	ROUTER_STR
	EIGRP_STR
	AS_STR
	VRF_CMD_HELP_STR)
{
	char xpath[XPATH_MAXLEN];
	int rv;

	eigrp_cli_classic_config_rewind(vty);
	snprintf(xpath, sizeof(xpath),
		 "/frr-eigrpd:eigrpd/instance[asn='%s'][vrf='%s']",
		 as_str, vrf ? vrf : VRF_DEFAULT_NAME);

	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);
	if (rv == CMD_SUCCESS)
		VTY_PUSH_XPATH(EIGRP_NODE, xpath);

	return rv;
}

/*
 * Syntax: `no router eigrp <1-65535> [vrf NAME]`
 * Mode: Classic global configuration
 * XPath: /frr-eigrpd:eigrpd/instance
 * Target: eigrpd_instance_destroy() -> eigrp_finish_final()
 */
DEFPY_YANG(
	no_router_eigrp,
	no_router_eigrp_cmd,
	"no router eigrp (1-65535)$as [vrf NAME]",
	NO_STR
	ROUTER_STR
	EIGRP_STR
	AS_STR
	VRF_CMD_HELP_STR)
{
	char xpath[XPATH_MAXLEN];

	eigrp_cli_classic_config_rewind(vty);
	snprintf(xpath, sizeof(xpath),
		 "/frr-eigrpd:eigrpd/instance[asn='%s'][vrf='%s']",
		 as_str, vrf ? vrf : VRF_DEFAULT_NAME);

	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes_clear_pending(vty, NULL);
}

void eigrp_cli_classic_show_header(struct vty *vty, const struct lyd_node *dnode,
			   bool show_defaults)
{
	const char *asn = yang_dnode_get_string(dnode, "asn");
	const char *vrf = yang_dnode_get_string(dnode, "vrf");

	vty_out(vty, "router eigrp %s", asn);
	if (strcmp(vrf, VRF_DEFAULT_NAME))
		vty_out(vty, " vrf %s", vrf);
	vty_out(vty, "\n");
}

void eigrp_cli_classic_show_end_header(struct vty *vty, const struct lyd_node *dnode)
{
	vty_out(vty, "exit\n");
	vty_out(vty, "!\n");
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
 */
/*
 * Syntax: `eigrp router-id A.B.C.D`
 * Mode: Classic router / Named address-family
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/router-id; Named: /frr-eigrpd:eigrpd/named/address-family/router-id
 * Target: eigrp_instance_router_id_set()
 */
DEFPY_YANG(
	eigrp_router_id,
	eigrp_router_id_cmd,
	"eigrp router-id A.B.C.D$addr",
	EIGRP_STR
	"Router ID for this EIGRP process\n"
	"EIGRP Router-ID in IP address format\n")
{
	nb_cli_enqueue_change(vty, "./router-id", NB_OP_MODIFY, addr_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no eigrp router-id [A.B.C.D]`
 * Mode: Classic router / Named address-family
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/router-id; Named: /frr-eigrpd:eigrpd/named/address-family/router-id
 * Target: eigrp_instance_router_id_reset()
 */
DEFPY_YANG(
	no_eigrp_router_id,
	no_eigrp_router_id_cmd,
	"no eigrp router-id [A.B.C.D]",
	NO_STR
	EIGRP_STR
	"Router ID for this EIGRP process\n"
	"EIGRP Router-ID in IP address format\n")
{
	nb_cli_enqueue_change(vty, "./router-id", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_router_id(struct vty *vty, const struct lyd_node *dnode,
			      bool show_defaults)
{
	const char *router_id = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " eigrp router-id %s\n", router_id);
}

/*
 * Syntax: `[no] passive-interface IFNAME`
 * Mode: Classic router configuration
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
 * Target: eigrp_interface_passive_set()
 */
DEFPY_YANG(
	eigrp_passive_interface,
	eigrp_passive_interface_cmd,
	"[no] passive-interface IFNAME",
	NO_STR
	"Suppress routing updates on an interface\n"
	"Interface to suppress on\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), "./passive-interface[.='%s']", ifname);

	if (no)
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	else
		nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_passive_interface(struct vty *vty,
				      const struct lyd_node *dnode,
				      bool show_defaults)
{
	const char *ifname = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " passive-interface %s\n", ifname);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/active-time
 */
/*
 * Syntax: `timers active-time <seconds|disabled>`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/active-time; Named: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Target: Named -> eigrp_timer_active_time_set(); classic runtime remains unsupported
 */
DEFPY_YANG(
	eigrp_timers_active,
	eigrp_timers_active_cmd,
	"timers active-time <(1-65535)$timer|disabled$disabled>",
	"Adjust routing timers\n"
	"Time limit for active state\n"
	"Active state time limit in seconds\n"
	"Disable time limit for active state\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_active_time_apply(vty, disabled, timer_str, false);
	if (disabled)
		nb_cli_enqueue_change(vty, "./active-time", NB_OP_MODIFY, "0");
	else
		nb_cli_enqueue_change(vty, "./active-time",
				      NB_OP_MODIFY, timer_str);

	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no timers active-time`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/active-time; Named: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Target: Named -> eigrp_timer_active_time_reset(); classic runtime remains unsupported
 */
DEFPY_YANG(
	no_eigrp_timers_active,
	no_eigrp_timers_active_cmd,
	"no timers active-time",
	NO_STR
	"Adjust routing timers\n"
	"Time limit for active state\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_active_time_apply(vty, false, NULL, true);
	nb_cli_enqueue_change(vty, "./active-time", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_active_time(struct vty *vty, const struct lyd_node *dnode,
				bool show_defaults)
{
	const char *timer = yang_dnode_get_string(dnode, NULL);

	if (!strcmp(timer, "0"))
		vty_out(vty, " timers active-time disabled\n");
	else
		vty_out(vty, " timers active-time %s\n", timer);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/variance
 */
/*
 * Syntax: `variance multiplier`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/variance; Named: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Target: eigrp_metric_variance_set()
 */
DEFPY_YANG(
	eigrp_variance,
	eigrp_variance_cmd,
	"variance (1-128)$variance",
	"Control load balancing variance\n"
	"Metric variance multiplier\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_variance_apply(vty, variance_str, false);
	nb_cli_enqueue_change(vty, "./variance", NB_OP_MODIFY, variance_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no variance`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/variance; Named: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Target: eigrp_metric_variance_reset()
 */
DEFPY_YANG(
	no_eigrp_variance,
	no_eigrp_variance_cmd,
	"no variance",
	NO_STR
	"Control load balancing variance\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_variance_apply(vty, NULL, true);
	nb_cli_enqueue_change(vty, "./variance", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_variance(struct vty *vty, const struct lyd_node *dnode,
			     bool show_defaults)
{
	const char *variance = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " variance %s\n", variance);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 */
/*
 * Syntax: `maximum-paths paths`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/maximum-paths; Named: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Target: eigrp_topology_maximum_paths_set()
 */
DEFPY_YANG(
	eigrp_maximum_paths,
	eigrp_maximum_paths_cmd,
	"maximum-paths (1-32)$maximum_paths",
	"Forward packets over multiple paths\n"
	"Number of paths\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_maximum_paths_apply(vty, maximum_paths_str, false);
	nb_cli_enqueue_change(vty, "./maximum-paths", NB_OP_MODIFY,
			      maximum_paths_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no maximum-paths [paths]`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/maximum-paths; Named: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Target: eigrp_topology_maximum_paths_reset()
 */
DEFPY_YANG(
	no_eigrp_maximum_paths,
	no_eigrp_maximum_paths_cmd,
	"no maximum-paths [(1-32)]",
	NO_STR
	"Forward packets over multiple paths\n"
	"Number of paths\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_maximum_paths_apply(vty, NULL, true);
	nb_cli_enqueue_change(vty, "./maximum-paths", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_maximum_paths(struct vty *vty, const struct lyd_node *dnode,
				  bool show_defaults)
{
	const char *maximum_paths = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " maximum-paths %s\n", maximum_paths);
}

void eigrp_cli_classic_show_event_log_size(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, " eigrp event-log-size %u\n",
		yang_dnode_get_uint32(dnode, NULL));
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K1
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K2
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K3
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K4
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K5
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K6
 */
/*
 * Syntax: `metric weights tos K1 K2 K3 K4 K5 [K6]`
 * Mode: Classic router / Named address-family
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/metric-weights; Named: /frr-eigrpd:eigrpd/named/address-family/metric-weights
 * Target: eigrp_metric_weights_set()
 */
DEFPY_YANG(
	eigrp_metric_weights,
	eigrp_metric_weights_cmd,
	"metric weights (0-255)$tos (0-255)$k1 (0-255)$k2 (0-255)$k3 (0-255)$k4 (0-255)$k5 [(0-255)$k6]",
	"Modify metrics and parameters for advertisement\n"
	"Modify metric coefficients\n"
	"Type of service (must be 0)\n"
	"K1\n"
	"K2\n"
	"K3\n"
	"K4\n"
	"K5\n"
	"K6\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_metric_weights_apply(
			vty, tos_str, k1_str, k2_str, k3_str, k4_str, k5_str,
			k6_str, false);
	if (tos != 0) {
		vty_out(vty, "%% EIGRP metric weights TOS must be 0\n");
		return CMD_WARNING;
	}
	nb_cli_enqueue_change(vty, "./metric-weights/K1", NB_OP_MODIFY, k1_str);
	nb_cli_enqueue_change(vty, "./metric-weights/K2", NB_OP_MODIFY, k2_str);
	nb_cli_enqueue_change(vty, "./metric-weights/K3", NB_OP_MODIFY, k3_str);
	nb_cli_enqueue_change(vty, "./metric-weights/K4", NB_OP_MODIFY, k4_str);
	nb_cli_enqueue_change(vty, "./metric-weights/K5", NB_OP_MODIFY, k5_str);
	if (k6)
		nb_cli_enqueue_change(vty, "./metric-weights/K6",
				      NB_OP_MODIFY, k6_str);

	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no metric weights [...]`
 * Mode: Classic router / Named address-family
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/metric-weights; Named: /frr-eigrpd:eigrpd/named/address-family/metric-weights
 * Target: eigrp_metric_weights_reset()
 */
DEFPY_YANG(
	no_eigrp_metric_weights,
	no_eigrp_metric_weights_cmd,
	"no metric weights",
	NO_STR
	"Modify metrics and parameters for advertisement\n"
	"Modify metric coefficients\n")
{
	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_metric_weights_apply(
			vty, NULL, NULL, NULL, NULL, NULL, NULL, NULL, true);
	nb_cli_enqueue_change(vty, "./metric-weights/K1", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K2", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K3", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K4", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K5", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K6", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_metrics(struct vty *vty, const struct lyd_node *dnode,
			    bool show_defaults)
{
	const char *k1, *k2, *k3, *k4, *k5, *k6;

	k1 = yang_dnode_exists(dnode, "K1") ?
		yang_dnode_get_string(dnode, "K1") : "0";
	k2 = yang_dnode_exists(dnode, "K2") ?
		yang_dnode_get_string(dnode, "K2") : "0";
	k3 = yang_dnode_exists(dnode, "K3") ?
		yang_dnode_get_string(dnode, "K3") : "0";
	k4 = yang_dnode_exists(dnode, "K4") ?
		yang_dnode_get_string(dnode, "K4") : "0";
	k5 = yang_dnode_exists(dnode, "K5") ?
		yang_dnode_get_string(dnode, "K5") : "0";
	k6 = yang_dnode_exists(dnode, "K6") ?
		yang_dnode_get_string(dnode, "K6") : "0";

	vty_out(vty, " metric weights 0 %s %s %s %s %s",
		k1, k2, k3, k4, k5);
	if (k6)
		vty_out(vty, " %s", k6);
	vty_out(vty, "\n");
}

/*
 * Syntax: `[no] network A.B.C.D/M`
 * Mode: Classic router configuration
 * XPath: /frr-eigrpd:eigrpd/instance/network
 * Target: eigrp_network_create() / eigrp_network_delete()
 */
DEFPY_YANG(
	eigrp_network,
	eigrp_network_cmd,
	"[no] network A.B.C.D/M$prefix",
	NO_STR
	"Enable routing on an IP network\n"
	"EIGRP network prefix\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), "./network[.='%s']", prefix_str);

	if (no)
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	else
		nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_network(struct vty *vty, const struct lyd_node *dnode,
			    bool show_defaults)
{
	const char *prefix = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " network %s\n", prefix);
}

/*
 * Syntax: `[no] neighbor A.B.C.D`
 * Mode: Classic router configuration
 * XPath: /frr-eigrpd:eigrpd/instance/neighbor
 * Target: Classic runtime unsupported; named uses eigrp_neighbor_static_create() / eigrp_neighbor_static_delete()
 */
DEFPY_YANG(
	eigrp_neighbor,
	eigrp_neighbor_cmd,
	"[no] neighbor A.B.C.D$addr",
	NO_STR
	"Specify a neighbor router\n"
	"Neighbor address\n")
{
	char xpath[XPATH_MAXLEN];

	if (eigrp_cli_named_context(vty))
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(xpath, sizeof(xpath), "./neighbor[.='%s']", addr_str);

	if (no)
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	else
		nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_neighbor(struct vty *vty, const struct lyd_node *dnode,
			     bool show_defaults)
{
	const char *prefix = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " neighbor %s\n", prefix);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/distribute-list
 */
/*
 * Syntax: `distribute-list ACCESS-LIST <in|out> [IFNAME]`
 * Mode: Classic router / Named topology
 * XPath: Relative `./distribute-list`; resolves to classic or named topology by VTY context
 * Target: Named -> eigrp_distribute_add()
 */
DEFPY_YANG (eigrp_distribute_list,
       eigrp_distribute_list_cmd,
       "distribute-list ACCESSLIST4_NAME$name <in|out>$dir [WORD$ifname]",
       "Filter networks in routing updates\n"
       "Access-list name\n"
       "Filter incoming routing updates\n"
       "Filter outgoing routing updates\n"
       "Interface name\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath),
		 "./distribute-list[interface='%s']/%s/access-list",
		 ifname ? ifname : "", dir);
	/* nb_cli_enqueue_change(vty, ".", NB_OP_CREATE, NULL); */
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, name);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `distribute-list prefix PREFIX-LIST <in|out> [IFNAME]`
 * Mode: Classic router / Named topology
 * XPath: Relative `./distribute-list`; resolves to classic or named topology by VTY context
 * Target: Named -> eigrp_distribute_add()
 */
DEFPY_YANG (eigrp_distribute_list_prefix,
       eigrp_distribute_list_prefix_cmd,
       "distribute-list prefix PREFIXLIST4_NAME$name <in|out>$dir [WORD$ifname]",
       "Filter networks in routing updates\n"
       "Specify a prefix list\n"
       "Prefix-list name\n"
       "Filter incoming routing updates\n"
       "Filter outgoing routing updates\n"
       "Interface name\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath),
		 "./distribute-list[interface='%s']/%s/prefix-list",
		 ifname ? ifname : "", dir);
	/* nb_cli_enqueue_change(vty, ".", NB_OP_CREATE, NULL); */
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, name);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no distribute-list [ACCESS-LIST] <in|out> [IFNAME]`
 * Mode: Classic router / Named topology
 * XPath: Relative `./distribute-list`; resolves to classic or named topology by VTY context
 * Target: Named -> eigrp_distribute_remove()
 */
DEFPY_YANG (eigrp_no_distribute_list,
       eigrp_no_distribute_list_cmd,
       "no distribute-list [ACCESSLIST4_NAME$name] <in|out>$dir [WORD$ifname]",
       NO_STR
       "Filter networks in routing updates\n"
       "Access-list name\n"
       "Filter incoming routing updates\n"
       "Filter outgoing routing updates\n"
       "Interface name\n")
{
	const struct lyd_node *value_node;
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath),
		 "./distribute-list[interface='%s']/%s/access-list",
		 ifname ? ifname : "", dir);
	/*
	 * See if the user has specified specific list so check it exists.
	 *
	 * NOTE: Other FRR CLI commands do not do this sort of verification and
	 * there may be an official decision not to.
	 */
	if (name) {
		value_node = yang_dnode_getf(vty->candidate_config->dnode, "%s/%s",
					     VTY_CURR_XPATH, xpath);
		if (!value_node || strcmp(name, lyd_get_value(value_node))) {
			vty_out(vty, "distribute list doesn't exist\n");
			return CMD_WARNING_CONFIG_FAILED;
		}
	}
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no distribute-list prefix [PREFIX-LIST] <in|out> [IFNAME]`
 * Mode: Classic router / Named topology
 * XPath: Relative `./distribute-list`; resolves to classic or named topology by VTY context
 * Target: Named -> eigrp_distribute_remove()
 */
DEFPY_YANG (eigrp_no_distribute_list_prefix,
       eigrp_no_distribute_list_prefix_cmd,
       "no distribute-list prefix [PREFIXLIST4_NAME$name] <in|out>$dir [WORD$ifname]",
       NO_STR
       "Filter networks in routing updates\n"
       "Specify a prefix list\n"
       "Prefix-list name\n"
       "Filter incoming routing updates\n"
       "Filter outgoing routing updates\n"
       "Interface name\n")
{
	const struct lyd_node *value_node;
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath),
		 "./distribute-list[interface='%s']/%s/prefix-list",
		 ifname ? ifname : "", dir);
	/*
	 * See if the user has specified specific list so check it exists.
	 *
	 * NOTE: Other FRR CLI commands do not do this sort of verification and
	 * there may be an official decision not to.
	 */
	if (name) {
		value_node = yang_dnode_getf(vty->candidate_config->dnode, "%s/%s",
					     VTY_CURR_XPATH, xpath);
		if (!value_node || strcmp(name, lyd_get_value(value_node))) {
			vty_out(vty, "distribute list doesn't exist\n");
			return CMD_WARNING_CONFIG_FAILED;
		}
	}
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/route-map
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/delay
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/load
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu
 */
/*
 * Syntax: `[no] redistribute PROTOCOL [metric ...] [route-map NAME]`
 * Mode: Classic router / Named topology
 * XPath: Classic: /frr-eigrpd:eigrpd/instance/redistribute; Named: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Target: Named -> eigrp_redistribute_add() / eigrp_redistribute_remove()
 */
DEFPY_YANG(
	eigrp_redistribute_source_metric,
	eigrp_redistribute_source_metric_cmd,
	"[no] redistribute " FRR_REDIST_STR_EIGRPD
	"$proto [(1-65535)$route_instance] [metric (1-4294967295)$bw (0-4294967295)$delay (0-255)$rlbt (1-255)$load (1-65535)$mtu] [route-map WORD$route_map]",
	NO_STR
	REDIST_STR
	FRR_REDIST_HELP_STR_EIGRPD
	"Source route instance (named EIGRP topology only)\n"
	"Metric for redistributed routes\n"
	"Bandwidth metric in Kbits per second\n"
	"EIGRP delay metric, in 10 microsecond units\n"
	"EIGRP reliability metric where 255 is 100% reliable2 ?\n"
	"EIGRP Effective bandwidth metric (Loading) where 255 is 100% loaded\n"
	"EIGRP MTU of the path\n"
	"Route-map\n"
	"Route-map name\n")
{
	char xpath[XPATH_MAXLEN], xpath_metric[XPATH_MAXLEN + 64];

	if (eigrp_cli_named_context(vty))
		return eigrp_cli_named_redistribute_apply(
			vty, proto, route_instance, bw, bw_str, delay, delay_str,
			rlbt, rlbt_str, load, load_str, mtu, mtu_str, route_map,
			no);
	if (route_instance != 0)
		return CMD_WARNING_CONFIG_FAILED;
	if (route_map)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(xpath, sizeof(xpath), "./redistribute[protocol='%s']", proto);

	if (no) {
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}

	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	if (bw == 0 || delay == 0 || rlbt == 0 || load == 0 || mtu == 0)
		return nb_cli_apply_changes(vty, NULL);

	snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/bandwidth",
		 xpath);
	nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, bw_str);
	snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/delay", xpath);
	nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, delay_str);
	snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/reliability",
		 xpath);
	nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, rlbt_str);
	snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/load", xpath);
	nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, load_str);
	snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/mtu", xpath);
	nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, mtu_str);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_redistribute(struct vty *vty, const struct lyd_node *dnode,
				 bool show_defaults)
{
	const char *proto = yang_dnode_get_string(dnode, "protocol");
	const char *bw, *delay, *load, *mtu, *rlbt;

	bw = yang_dnode_exists(dnode, "metrics/bandwidth") ?
		yang_dnode_get_string(dnode, "metrics/bandwidth") : NULL;
	delay = yang_dnode_exists(dnode, "metrics/delay") ?
		yang_dnode_get_string(dnode, "metrics/delay") : NULL;
	rlbt = yang_dnode_exists(dnode, "metrics/reliability") ?
		yang_dnode_get_string(dnode, "metrics/reliability") : NULL;
	load = yang_dnode_exists(dnode, "metrics/load") ?
		yang_dnode_get_string(dnode, "metrics/load") : NULL;
	mtu = yang_dnode_exists(dnode, "metrics/mtu") ?
		yang_dnode_get_string(dnode, "metrics/mtu") : NULL;

	vty_out(vty, " redistribute %s", proto);
	if (bw || rlbt || delay || load || mtu)
		vty_out(vty, " metric %s %s %s %s %s", bw, delay, rlbt, load,
			mtu);
	vty_out(vty, "\n");
}

/*
 * Syntax: `delay <1-16777215>`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/delay
 * Target: lib_interface_eigrp_delay_modify() -> eigrp_interface_runtime_reset()
 */
DEFPY_YANG(
	eigrp_if_delay,
	eigrp_if_delay_cmd,
	"delay (1-16777215)$delay",
	"Specify interface throughput delay\n"
	"Throughput delay (tens of microseconds)\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/delay",
			      NB_OP_MODIFY, delay_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no delay [<1-16777215>]`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/delay
 * Target: lib_interface_eigrp_delay_modify() -> eigrp_interface_runtime_reset()
 */
DEFPY_YANG(
	no_eigrp_if_delay,
	no_eigrp_if_delay_cmd,
	"no delay [(1-16777215)]",
	NO_STR
	"Specify interface throughput delay\n"
	"Throughput delay (tens of microseconds)\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/delay",
			      NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_delay(struct vty *vty, const struct lyd_node *dnode,
			  bool show_defaults)
{
	const char *delay = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " delay %s\n", delay);
}

/*
 * Syntax: `eigrp bandwidth <1-10000000>`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth
 * Target: lib_interface_eigrp_bandwidth_modify() -> eigrp_interface_runtime_reset()
 */
DEFPY_YANG(
	eigrp_if_bandwidth,
	eigrp_if_bandwidth_cmd,
	"eigrp bandwidth (1-10000000)$bw",
	EIGRP_STR
	"Set bandwidth informational parameter\n"
	"Bandwidth in kilobits\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/bandwidth",
			      NB_OP_MODIFY, bw_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no eigrp bandwidth [<1-10000000>]`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth
 * Target: lib_interface_eigrp_bandwidth_modify() -> eigrp_interface_runtime_reset()
 */
DEFPY_YANG(
	no_eigrp_if_bandwidth,
	no_eigrp_if_bandwidth_cmd,
	"no eigrp bandwidth [(1-10000000)]",
	NO_STR
	EIGRP_STR
	"Set bandwidth informational parameter\n"
	"Bandwidth in kilobits\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/bandwidth",
			      NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_bandwidth(struct vty *vty, const struct lyd_node *dnode,
			      bool show_defaults)
{
	const char *bandwidth = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " eigrp bandwidth %s\n", bandwidth);
}

/*
 * Syntax: `ip hello-interval eigrp <1-65535>`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval
 * Target: eigrp_interface_hello_interval_set()
 */
DEFPY_YANG(
	eigrp_if_ip_hellointerval,
	eigrp_if_ip_hellointerval_cmd,
	"ip hello-interval eigrp (1-65535)$hello",
	"Interface Internet Protocol config commands\n"
	"Configures EIGRP hello interval\n"
	EIGRP_STR
	"Seconds between hello transmissions\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/hello-interval",
			      NB_OP_MODIFY, hello_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no ip hello-interval eigrp [<1-65535>]`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval
 * Target: eigrp_interface_hello_interval_set() with YANG default
 */
DEFPY_YANG(
	no_eigrp_if_ip_hellointerval,
	no_eigrp_if_ip_hellointerval_cmd,
	"no ip hello-interval eigrp [(1-65535)]",
	NO_STR
	"Interface Internet Protocol config commands\n"
	"Configures EIGRP hello interval\n"
	EIGRP_STR
	"Seconds between hello transmissions\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/hello-interval",
			      NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}


void eigrp_cli_classic_show_hello_interval(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	const char *hello = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " ip hello-interval eigrp %s\n", hello);
}

/*
 * Syntax: `ip hold-time eigrp <1-65535>`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time
 * Target: eigrp_interface_hold_time_set()
 */
DEFPY_YANG(
	eigrp_if_ip_holdinterval,
	eigrp_if_ip_holdinterval_cmd,
	"ip hold-time eigrp (1-65535)$hold",
	"Interface Internet Protocol config commands\n"
	"Configures EIGRP IPv4 hold time\n"
	EIGRP_STR
	"Seconds before neighbor is considered down\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/hold-time",
			      NB_OP_MODIFY, hold_str);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no ip hold-time eigrp [<1-65535>]`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time
 * Target: eigrp_interface_hold_time_set() with YANG default
 */
DEFPY_YANG(
	no_eigrp_if_ip_holdinterval,
	no_eigrp_if_ip_holdinterval_cmd,
	"no ip hold-time eigrp [(1-65535)]",
	NO_STR
	"Interface Internet Protocol config commands\n"
	"Configures EIGRP IPv4 hold time\n"
	EIGRP_STR
	"Seconds before neighbor is considered down\n")
{
	nb_cli_enqueue_change(vty, "./frr-eigrpd:eigrp/hold-time",
			      NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_hold_time(struct vty *vty, const struct lyd_node *dnode,
			      bool show_defaults)
{
	const char *holdtime = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " ip hold-time eigrp %s\n", holdtime);
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/split-horizon
 */
/* NOT implemented. */

/*
 * Syntax: `ip summary-address eigrp <1-65535> A.B.C.D/M`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses
 * Target: Classic runtime unsupported; named uses eigrp_summary_create()
 */
DEFPY_YANG(
	eigrp_ip_summary_address,
	eigrp_ip_summary_address_cmd,
	"ip summary-address eigrp (1-65535)$as A.B.C.D/M$prefix",
	"Interface Internet Protocol config commands\n"
	"Perform address summarization\n"
	EIGRP_STR
	AS_STR
	"Summary <network>/<length>, e.g. 192.168.0.0/16\n")
{
	char xpath[XPATH_MAXLEN], xpath_auth[XPATH_MAXLEN + 64];

	snprintf(xpath, sizeof(xpath), "./frr-eigrpd:eigrp/instance[asn='%s']",
		 as_str);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	snprintf(xpath_auth, sizeof(xpath_auth),
		 "%s/summarize-addresses[.='%s']", xpath, prefix_str);
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no ip summary-address eigrp <1-65535> A.B.C.D/M`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses
 * Target: Classic cleanup only; named uses eigrp_summary_delete()
 */
DEFPY_YANG(
	no_eigrp_ip_summary_address,
	no_eigrp_ip_summary_address_cmd,
	"no ip summary-address eigrp (1-65535)$as A.B.C.D/M$prefix",
	NO_STR
	"Interface Internet Protocol config commands\n"
	"Perform address summarization\n"
	EIGRP_STR
	AS_STR
	"Summary <network>/<length>, e.g. 192.168.0.0/16\n")
{
	char xpath[XPATH_MAXLEN], xpath_auth[XPATH_MAXLEN + 64];

	snprintf(xpath, sizeof(xpath), "./frr-eigrpd:eigrp/instance[asn='%s']",
		 as_str);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	snprintf(xpath_auth, sizeof(xpath_auth),
		 "%s/summarize-addresses[.='%s']", xpath, prefix_str);
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_summarize_address(struct vty *vty,
				      const struct lyd_node *dnode,
				      bool show_defaults)
{
	const struct lyd_node *instance =
		eigrp_cli_classic_parent_named(dnode, "instance");
	uint16_t asn = yang_dnode_get_uint16(instance, "asn");
	const char *summarize_address = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " ip summary-address eigrp %d %s\n", asn,
		summarize_address);
}

/*
 * Syntax: `ip authentication mode eigrp <1-65535> <md5|hmac-sha-256>`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication
 * Target: Classic FRR auth field; named uses eigrp_auth_mode_set()
 */
DEFPY_YANG(
	eigrp_authentication_mode,
	eigrp_authentication_mode_cmd,
	"ip authentication mode eigrp (1-65535)$as <md5|hmac-sha-256>$auth_mode",
	"Interface Internet Protocol config commands\n"
	"Authentication subcommands\n"
	"Mode\n"
	EIGRP_STR
	AS_STR
	"Keyed message digest\n"
	"HMAC SHA256 algorithm \n")
{
	char xpath[XPATH_MAXLEN], xpath_auth[XPATH_MAXLEN + 64];

	snprintf(xpath, sizeof(xpath), "./frr-eigrpd:eigrp/instance[asn='%s']",
		 as_str);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	snprintf(xpath_auth, sizeof(xpath_auth), "%s/authentication", xpath);
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_MODIFY, auth_mode);

	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no ip authentication mode eigrp <1-65535> [md5|hmac-sha-256]`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication
 * Target: Classic FRR auth field; named uses eigrp_auth_mode_reset()
 */
DEFPY_YANG(
	no_eigrp_authentication_mode,
	no_eigrp_authentication_mode_cmd,
	"no ip authentication mode eigrp (1-65535)$as [<md5|hmac-sha-256>]",
	NO_STR
	"Interface Internet Protocol config commands\n"
	"Authentication subcommands\n"
	"Mode\n"
	EIGRP_STR
	AS_STR
	"Keyed message digest\n"
	"HMAC SHA256 algorithm \n")
{
	char xpath[XPATH_MAXLEN], xpath_auth[XPATH_MAXLEN + 64];

	snprintf(xpath, sizeof(xpath), "./frr-eigrpd:eigrp/instance[asn='%s']",
		 as_str);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	snprintf(xpath_auth, sizeof(xpath_auth), "%s/authentication", xpath);
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_MODIFY, "none");

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_authentication(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	const struct lyd_node *instance =
		eigrp_cli_classic_parent_named(dnode, "instance");
	uint16_t asn = yang_dnode_get_uint16(instance, "asn");
	const char *crypt = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " ip authentication mode eigrp %d %s\n", asn, crypt);
}

/*
 * Syntax: `ip authentication key-chain eigrp <1-65535> WORD`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 * Target: Classic FRR keychain field; named uses eigrp_auth_keychain_set()
 */
DEFPY_YANG(
	eigrp_authentication_keychain,
	eigrp_authentication_keychain_cmd,
	"ip authentication key-chain eigrp (1-65535)$as WORD$name",
	"Interface Internet Protocol config commands\n"
	"Authentication subcommands\n"
	"Key-chain\n"
	EIGRP_STR
	AS_STR
	"Name of key-chain\n")
{
	char xpath[XPATH_MAXLEN], xpath_auth[XPATH_MAXLEN + 64];

	snprintf(xpath, sizeof(xpath), "./frr-eigrpd:eigrp/instance[asn='%s']",
		 as_str);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	snprintf(xpath_auth, sizeof(xpath_auth), "%s/keychain", xpath);
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_MODIFY, name);

	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no ip authentication key-chain eigrp <1-65535> [WORD]`
 * Mode: Classic interface configuration
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 * Target: Classic FRR keychain field; named uses eigrp_auth_keychain_reset()
 */
DEFPY_YANG(
	no_eigrp_authentication_keychain,
	no_eigrp_authentication_keychain_cmd,
	"no ip authentication key-chain eigrp (1-65535)$as [WORD]",
	NO_STR
	"Interface Internet Protocol config commands\n"
	"Authentication subcommands\n"
	"Key-chain\n"
	EIGRP_STR
	AS_STR
	"Name of key-chain\n")
{
	char xpath[XPATH_MAXLEN], xpath_auth[XPATH_MAXLEN + 64];

	snprintf(xpath, sizeof(xpath), "./frr-eigrpd:eigrp/instance[asn='%s']",
		 as_str);
	snprintf(xpath_auth, sizeof(xpath_auth), "%s/keychain", xpath);
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_classic_show_keychain(struct vty *vty, const struct lyd_node *dnode,
			     bool show_defaults)
{
	const struct lyd_node *instance =
		eigrp_cli_classic_parent_named(dnode, "instance");
	uint16_t asn = yang_dnode_get_uint16(instance, "asn");
	const char *keychain = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, " ip authentication key-chain eigrp %d %s\n", asn,
		keychain);
}


/*
 * CLI installation procedures.
 */
static int eigrp_config_write(struct vty *vty);
static struct cmd_node eigrp_node = {
	.name = "eigrp",
	.node = EIGRP_NODE,
	.parent_node = CONFIG_NODE,
	.prompt = "%s(config-router)# ",
	.config_write = eigrp_config_write,
};

static int eigrp_config_write(struct vty *vty)
{
	const struct lyd_node *dnode;
	int written = 0;

	dnode = yang_dnode_get(running_config->dnode, "/frr-eigrpd:eigrpd");
	if (dnode) {
		nb_cli_show_dnode_cmds(vty, dnode, false);
		written = 1;
	}

	return written;
}

void
eigrp_cli_classic_init(void)
{
	install_element(CONFIG_NODE, &router_eigrp_cmd);
	install_element(CONFIG_NODE, &no_router_eigrp_cmd);

	install_node(&eigrp_node);
	/* `router eigrp ...` is a top-level configuration command.  Accept it
	 * while already in EIGRP mode so the callback can rewind to CONFIG_NODE
	 * before selecting the requested process.
	 */
	install_element(EIGRP_NODE, &router_eigrp_cmd);
	install_default(EIGRP_NODE);

	install_element(EIGRP_NODE, &eigrp_router_id_cmd);
	install_element(EIGRP_NODE, &no_eigrp_router_id_cmd);
	install_element(EIGRP_NODE, &eigrp_passive_interface_cmd);
	install_element(EIGRP_NODE, &eigrp_timers_active_cmd);
	install_element(EIGRP_NODE, &no_eigrp_timers_active_cmd);
	install_element(EIGRP_NODE, &eigrp_variance_cmd);
	install_element(EIGRP_NODE, &no_eigrp_variance_cmd);
	install_element(EIGRP_NODE, &eigrp_maximum_paths_cmd);
	install_element(EIGRP_NODE, &no_eigrp_maximum_paths_cmd);
	install_element(EIGRP_NODE, &eigrp_metric_weights_cmd);
	install_element(EIGRP_NODE, &no_eigrp_metric_weights_cmd);
	install_element(EIGRP_NODE, &eigrp_network_cmd);
	install_element(EIGRP_NODE, &eigrp_neighbor_cmd);
	install_element(EIGRP_NODE, &eigrp_distribute_list_cmd);
	install_element(EIGRP_NODE, &eigrp_distribute_list_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_no_distribute_list_cmd);
	install_element(EIGRP_NODE, &eigrp_no_distribute_list_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_redistribute_source_metric_cmd);

	vrf_cmd_init(NULL);

	if_cmd_init_default();

	install_element(INTERFACE_NODE, &eigrp_if_delay_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_if_delay_cmd);
	install_element(INTERFACE_NODE, &eigrp_if_bandwidth_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_if_bandwidth_cmd);
	install_element(INTERFACE_NODE, &eigrp_if_ip_hellointerval_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_if_ip_hellointerval_cmd);
	install_element(INTERFACE_NODE, &eigrp_if_ip_holdinterval_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_if_ip_holdinterval_cmd);
	install_element(INTERFACE_NODE, &eigrp_ip_summary_address_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_ip_summary_address_cmd);
	install_element(INTERFACE_NODE, &eigrp_authentication_mode_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_authentication_mode_cmd);
	install_element(INTERFACE_NODE, &eigrp_authentication_keychain_cmd);
	install_element(INTERFACE_NODE, &no_eigrp_authentication_keychain_cmd);
}
