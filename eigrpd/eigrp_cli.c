// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP daemon CLI implementation.
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
#include "eigrp_cli.h"

#ifndef EIGRP_STANDALONE_BUILD
/*
 * FRR clippy generates this file during the real FRR build.  Generated
 * parser wrappers are not EIGRP-owned source style, so suppress warnings
 * that can be emitted by clippy formatting rather than by eigrp_cli.c.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "eigrpd/eigrp_cli_clippy.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#ifdef EIGRP_STANDALONE_BUILD
/*
 * Standalone compile shim for FRR clippy parsed variables.  The real FRR
 * build generates eigrp_cli_clippy.c and passes these as handler arguments.
 */
static const char *as_str = "1";
static const char *vrf = NULL;
static const char *addr_str = "0.0.0.0";
static const char *ifname = NULL;
static bool no = false;
static bool disabled = false;
static const char *timer_str = "1";
static const char *variance_str = "1";
static const char *maximum_paths_str = "1";
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
static uint32_t bw = 1;
static uint32_t delay = 1;
static uint8_t rlbt = 255;
static uint8_t load = 1;
static uint32_t mtu = 1500;
#define crypt "md5"
#endif

/*
 * XPath: /frr-eigrpd:eigrpd/instance
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

	snprintf(xpath, sizeof(xpath),
		 "/frr-eigrpd:eigrpd/instance[asn='%s'][vrf='%s']",
		 as_str, vrf ? vrf : VRF_DEFAULT_NAME);

	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);

	vty_out(vty,
		"EIGRP-NB DEBUG: router xpath=%s rv=%d candidate-exists=%s\n",
		xpath, rv,
		(vty->candidate_config && vty->candidate_config->dnode &&
		 yang_dnode_exists(vty->candidate_config->dnode, xpath))
			? "yes"
			: "no");

	if (rv == CMD_SUCCESS)
		VTY_PUSH_XPATH(EIGRP_NODE, xpath);

	return rv;
}

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

	snprintf(xpath, sizeof(xpath),
		 "/frr-eigrpd:eigrpd/instance[asn='%s'][vrf='%s']",
		 as_str, vrf ? vrf : VRF_DEFAULT_NAME);

	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes_clear_pending(vty, NULL);
}

static eigrp_instance_t *eigrp_cli_dnode_instance(const struct lyd_node *dnode)
{
	const char *asn = yang_dnode_get_string(dnode, "asn");
	const char *vrf_name = yang_dnode_get_string(dnode, "vrf");
	struct vrf *vrf = vrf_lookup_by_name(vrf_name);
	eigrp_instance_t *eigrp;
	struct listnode *node, *nnode;
	uint32_t as;

	if (!asn || !vrf)
		return NULL;

	as = strtoul(asn, NULL, 10);
	for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->AS == as && eigrp->vrf_id == vrf->vrf_id)
			return eigrp;
	}

	return NULL;
}

static const char *eigrp_cli_dnode_instance_name(const struct lyd_node *dnode)
{
	eigrp_instance_t *eigrp = eigrp_cli_dnode_instance(dnode);

	if (eigrp && eigrp->name)
		return eigrp->name;

	return yang_dnode_get_string(dnode, "asn");
}

static void eigrp_cli_show_named_af_interfaces(struct vty *vty,
					       const struct lyd_node *dnode)
{
	eigrp_instance_t *eigrp = eigrp_cli_dnode_instance(dnode);
	eigrp_interface_t *ei;
	struct listnode *node;

	if (!eigrp)
		return;

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		bool wrote = false;

		if (!ei || !ei->ifp)
			continue;

		if (ei->params.v_hello == EIGRP_HELLO_INTERVAL_DEFAULT
		    && ei->params.v_wait == EIGRP_HOLD_INTERVAL_DEFAULT
		    && ei->params.delay == EIGRP_DELAY_DEFAULT
		    && ei->params.passive_interface == EIGRP_INTF_ACTIVE
		    && ei->params.auth_type == EIGRP_AUTH_TYPE_NONE
		    && ei->params.auth_keychain == NULL)
			continue;

		vty_out(vty, "  af-interface %s\n", ei->ifp->name);
		wrote = true;

		if (ei->params.v_hello != EIGRP_HELLO_INTERVAL_DEFAULT)
			vty_out(vty, "   hello-interval %u\n", ei->params.v_hello);
		if (ei->params.v_wait != EIGRP_HOLD_INTERVAL_DEFAULT)
			vty_out(vty, "   hold-time %u\n", ei->params.v_wait);
		if (ei->params.delay != EIGRP_DELAY_DEFAULT)
			vty_out(vty, "   delay %u\n", ei->params.delay);
		if (ei->params.passive_interface == EIGRP_INTF_PASSIVE)
			vty_out(vty, "   passive-interface\n");
		if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5)
			vty_out(vty, "   authentication mode md5\n");
		else if (ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256)
			vty_out(vty, "   authentication mode hmac-sha-256\n");
		if (ei->params.auth_keychain)
			vty_out(vty, "   authentication key-chain %s\n",
				ei->params.auth_keychain);

		if (wrote)
			vty_out(vty, "  exit-af-interface\n");
	}
}

void eigrp_cli_show_header(struct vty *vty, const struct lyd_node *dnode,
			   bool show_defaults)
{
	const char *asn = yang_dnode_get_string(dnode, "asn");
	const char *vrf = yang_dnode_get_string(dnode, "vrf");

	vty_out(vty, "router eigrp %s\n", eigrp_cli_dnode_instance_name(dnode));
	vty_out(vty, " address-family ipv4 unicast");
	if (strcmp(vrf, VRF_DEFAULT_NAME))
		vty_out(vty, " vrf %s", vrf);
	vty_out(vty, " autonomous-system %s\n", asn);
}

void eigrp_cli_show_end_header(struct vty *vty, const struct lyd_node *dnode)
{
	eigrp_cli_show_named_af_interfaces(vty, dnode);
	vty_out(vty, " exit-address-family\n");
	vty_out(vty, "exit\n");
	vty_out(vty, "!\n");
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
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

void eigrp_cli_show_router_id(struct vty *vty, const struct lyd_node *dnode,
			      bool show_defaults)
{
	const char *router_id = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  eigrp router-id %s\n", router_id);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
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

void eigrp_cli_show_passive_interface(struct vty *vty,
				      const struct lyd_node *dnode,
				      bool show_defaults)
{
	const char *ifname = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  passive-interface %s\n", ifname);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/active-time
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
	if (disabled)
		nb_cli_enqueue_change(vty, "./active-time", NB_OP_MODIFY, "0");
	else
		nb_cli_enqueue_change(vty, "./active-time",
				      NB_OP_MODIFY, timer_str);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY_YANG(
	no_eigrp_timers_active,
	no_eigrp_timers_active_cmd,
	"no timers active-time [<(1-65535)|disabled>]",
	NO_STR
	"Adjust routing timers\n"
	"Time limit for active state\n"
	"Active state time limit in seconds\n"
	"Disable time limit for active state\n")
{
	nb_cli_enqueue_change(vty, "./active-time", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_show_active_time(struct vty *vty, const struct lyd_node *dnode,
				bool show_defaults)
{
	const char *timer = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  timers active-time %s\n", timer);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/variance
 */
DEFPY_YANG(
	eigrp_variance,
	eigrp_variance_cmd,
	"variance (1-128)$variance",
	"Control load balancing variance\n"
	"Metric variance multiplier\n")
{
	nb_cli_enqueue_change(vty, "./variance", NB_OP_MODIFY, variance_str);
	return nb_cli_apply_changes(vty, NULL);
}

DEFPY_YANG(
	no_eigrp_variance,
	no_eigrp_variance_cmd,
	"no variance [(1-128)]",
	NO_STR
	"Control load balancing variance\n"
	"Metric variance multiplier\n")
{
	nb_cli_enqueue_change(vty, "./variance", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_show_variance(struct vty *vty, const struct lyd_node *dnode,
			     bool show_defaults)
{
	const char *variance = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  variance %s\n", variance);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 */
DEFPY_YANG(
	eigrp_maximum_paths,
	eigrp_maximum_paths_cmd,
	"maximum-paths (1-32)$maximum_paths",
	"Forward packets over multiple paths\n"
	"Number of paths\n")
{
	nb_cli_enqueue_change(vty, "./maximum-paths", NB_OP_MODIFY,
			      maximum_paths_str);
	return nb_cli_apply_changes(vty, NULL);
}

DEFPY_YANG(
	no_eigrp_maximum_paths,
	no_eigrp_maximum_paths_cmd,
	"no maximum-paths [(1-32)]",
	NO_STR
	"Forward packets over multiple paths\n"
	"Number of paths\n")
{
	nb_cli_enqueue_change(vty, "./maximum-paths", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_show_maximum_paths(struct vty *vty, const struct lyd_node *dnode,
				  bool show_defaults)
{
	const char *maximum_paths = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  maximum-paths %s\n", maximum_paths);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K1
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K2
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K3
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K4
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K5
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K6
 */
DEFPY_YANG(
	eigrp_metric_weights,
	eigrp_metric_weights_cmd,
	"metric weights (0-255)$k1 (0-255)$k2 (0-255)$k3 (0-255)$k4 (0-255)$k5 [(0-255)$k6]",
	"Modify metrics and parameters for advertisement\n"
	"Modify metric coefficients\n"
	"K1\n"
	"K2\n"
	"K3\n"
	"K4\n"
	"K5\n"
	"K6\n")
{
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

DEFPY_YANG(
	no_eigrp_metric_weights,
	no_eigrp_metric_weights_cmd,
	"no metric weights [(0-255) (0-255) (0-255) (0-255) (0-255) (0-255)]",
	NO_STR
	"Modify metrics and parameters for advertisement\n"
	"Modify metric coefficients\n"
	"K1\n"
	"K2\n"
	"K3\n"
	"K4\n"
	"K5\n"
	"K6\n")
{
	nb_cli_enqueue_change(vty, "./metric-weights/K1", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K2", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K3", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K4", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K5", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./metric-weights/K6", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_show_metrics(struct vty *vty, const struct lyd_node *dnode,
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

	vty_out(vty, "  metric weights %s %s %s %s %s",
		k1, k2, k3, k4, k5);
	if (k6)
		vty_out(vty, " %s", k6);
	vty_out(vty, "\n");
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/network
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

void eigrp_cli_show_network(struct vty *vty, const struct lyd_node *dnode,
			    bool show_defaults)
{
	const char *prefix = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  network %s\n", prefix);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/neighbor
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

	snprintf(xpath, sizeof(xpath), "./neighbor[.='%s']", addr_str);

	if (no)
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	else
		nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

void eigrp_cli_show_neighbor(struct vty *vty, const struct lyd_node *dnode,
			     bool show_defaults)
{
	const char *prefix = yang_dnode_get_string(dnode, NULL);

	vty_out(vty, "  neighbor %s\n", prefix);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/distribute-list
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
DEFPY_YANG(
	eigrp_redistribute_source_metric,
	eigrp_redistribute_source_metric_cmd,
	"[no] redistribute " FRR_REDIST_STR_EIGRPD
	"$proto [metric (1-4294967295)$bw (0-4294967295)$delay (0-255)$rlbt (1-255)$load (1-65535)$mtu]",
	NO_STR
	REDIST_STR
	FRR_REDIST_HELP_STR_EIGRPD
	"Metric for redistributed routes\n"
	"Bandwidth metric in Kbits per second\n"
	"EIGRP delay metric, in 10 microsecond units\n"
	"EIGRP reliability metric where 255 is 100% reliable2 ?\n"
	"EIGRP Effective bandwidth metric (Loading) where 255 is 100% loaded\n"
	"EIGRP MTU of the path\n")
{
	char xpath[XPATH_MAXLEN], xpath_metric[XPATH_MAXLEN + 64];

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

void eigrp_cli_show_redistribute(struct vty *vty, const struct lyd_node *dnode,
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

	vty_out(vty, "  redistribute %s", proto);
	if (bw || rlbt || delay || load || mtu)
		vty_out(vty, " metric %s %s %s %s %s", bw, delay, rlbt, load,
			mtu);
	vty_out(vty, "\n");
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/delay
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

void eigrp_cli_show_delay(struct vty *vty, const struct lyd_node *dnode,
			  bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth
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

void eigrp_cli_show_bandwidth(struct vty *vty, const struct lyd_node *dnode,
			      bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval
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


void eigrp_cli_show_hello_interval(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time
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

void eigrp_cli_show_hold_time(struct vty *vty, const struct lyd_node *dnode,
			      bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/split-horizon
 */
/* NOT implemented. */

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses
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

void eigrp_cli_show_summarize_address(struct vty *vty,
				      const struct lyd_node *dnode,
				      bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication
 */
DEFPY_YANG(
	eigrp_authentication_mode,
	eigrp_authentication_mode_cmd,
	"ip authentication mode eigrp (1-65535)$as <md5|hmac-sha-256>$crypt",
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
	nb_cli_enqueue_change(vty, xpath_auth, NB_OP_MODIFY, crypt);

	return nb_cli_apply_changes(vty, NULL);
}

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

void eigrp_cli_show_authentication(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
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

void eigrp_cli_show_keychain(struct vty *vty, const struct lyd_node *dnode,
			     bool show_defaults)
{
	(void)vty;
	(void)dnode;
	(void)show_defaults;
	/* Named-mode interface config is emitted from the EIGRP AF writer. */
}



static const char *eigrp_cli_token_value(const struct cmd_token *token)
{
	if (!token)
		return NULL;
	return token->arg ? token->arg : token->text;
}

static const char *eigrp_cli_token_after(int argc, struct cmd_token *argv[],
					 const char *keyword)
{
	int i;

	for (i = 0; i < argc - 1; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);

		if (value && strcmp(value, keyword) == 0)
			return eigrp_cli_token_value(argv[i + 1]);
	}

	return NULL;
}

static const char *eigrp_cli_token_last(int argc, struct cmd_token *argv[])
{
	if (argc <= 0)
		return NULL;
	return eigrp_cli_token_value(argv[argc - 1]);
}

static bool eigrp_cli_xpath_get(const char *xpath, const char *key, char *buf,
					size_t buflen)
{
	char pattern[64];
	const char *start;
	const char *end;
	size_t len;

	if (!xpath || !key || !buf || buflen == 0)
		return false;

	snprintf(pattern, sizeof(pattern), "%s='", key);
	start = strstr(xpath, pattern);
	if (!start)
		return false;

	start += strlen(pattern);
	end = strchr(start, '\'');
	if (!end)
		return false;

	len = end - start;
	if (len >= buflen)
		len = buflen - 1;

	memcpy(buf, start, len);
	buf[len] = '\0';
	return true;
}

static bool eigrp_cli_current_as_vrf(struct vty *vty, char *asn,
					     size_t asn_len, char *vrf,
					     size_t vrf_len)
{
	return eigrp_cli_xpath_get(VTY_CURR_XPATH, "asn", asn, asn_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "vrf", vrf, vrf_len);
}

static const char *eigrp_cli_vrf_name(const char *vrf)
{
	return vrf ? vrf : VRF_DEFAULT_NAME;
}

static eigrp_instance_t *eigrp_cli_instance_lookup_by_as_vrf(const char *asn,
						     const char *vrf_name)
{
	struct vrf *vrf;
	eigrp_instance_t *eigrp;
	struct listnode *node, *nnode;
	uint32_t as;

	if (!asn || !vrf_name)
		return NULL;

	vrf = vrf_lookup_by_name(vrf_name);
	if (!vrf)
		return NULL;

	as = strtoul(asn, NULL, 10);
	for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->AS == as && eigrp->vrf_id == vrf->vrf_id)
			return eigrp;
	}

	return NULL;
}

static void eigrp_cli_current_name(struct vty *vty, char *name, size_t name_len)
{
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	eigrp_instance_t *eigrp;

	if (eigrp_cli_xpath_get(VTY_CURR_XPATH, "name", name, name_len))
		return;

	if (eigrp_cli_current_as_vrf(vty, asn, sizeof(asn), vrf_name,
				       sizeof(vrf_name))) {
		eigrp = eigrp_cli_instance_lookup_by_as_vrf(asn, vrf_name);
		if (eigrp && eigrp->name) {
			snprintf(name, name_len, "%s", eigrp->name);
			return;
		}
	}

	snprintf(name, name_len, "EIGRP");
}

static bool eigrp_cli_parse_uint16(const char *value, uint16_t *out)
{
	char *end = NULL;
	unsigned long parsed;

	if (!value || !out)
		return false;

	parsed = strtoul(value, &end, 10);
	if (end == value || *end != '\0' || parsed == 0 || parsed > 65535)
		return false;

	*out = parsed;
	return true;
}

int eigrp_cli_not_configured(struct vty *vty, const char *command)
{
	zlog_debug("EIGRP named-mode command '%s' is not configured yet", command);
	vty_out(vty, "%% EIGRP named-mode command '%s' is not configured yet\n",
		command);
	return CMD_SUCCESS;
}

static int eigrp_cli_push_named_root(struct vty *vty, const char *name)
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath),
		 "/frr-eigrpd:eigrpd/eigrp-named[name='%s']",
		 name && name[0] ? name : "EIGRP");
	VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return CMD_SUCCESS;
}

static void eigrp_cli_instance_xpath(char *xpath, size_t xpath_len,
				     const char *asn, const char *vrf_name)
{
	snprintf(xpath, xpath_len,
		 "/frr-eigrpd:eigrpd/instance[asn='%s'][vrf='%s']", asn,
		 eigrp_cli_vrf_name(vrf_name));
}

static int eigrp_cli_named_instance_set(struct vty *vty, const char *name,
					const char *asn, const char *vrf_name)
{
	char xpath[XPATH_MAXLEN];
	uint16_t as;
	struct vrf *vrf;
	eigrp_instance_t *eigrp;
	int rv;

	if (!eigrp_cli_parse_uint16(asn, &as)) {
		vty_out(vty, "%% Invalid autonomous-system %s\n", asn ? asn : "");
		return CMD_WARNING;
	}

	eigrp_cli_instance_xpath(xpath, sizeof(xpath), asn,
				 eigrp_cli_vrf_name(vrf_name));
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);
	if (rv != CMD_SUCCESS)
		return rv;

	vrf = vrf_lookup_by_name(eigrp_cli_vrf_name(vrf_name));
	eigrp = eigrp_get(as, vrf ? vrf->vrf_id : VRF_DEFAULT);
	eigrp_name_set(eigrp, name);
	VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return CMD_SUCCESS;
}

static int eigrp_cli_named_instance_unset(struct vty *vty, const char *asn,
					  const char *vrf_name)
{
	char xpath[XPATH_MAXLEN];

	eigrp_cli_instance_xpath(xpath, sizeof(xpath), asn,
				 eigrp_cli_vrf_name(vrf_name));
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes_clear_pending(vty, NULL);
}

static int eigrp_cli_network_prefix_from_address(const char *addr,
						 char *prefix, size_t prefix_len)
{
	struct in_addr in;
	uint32_t host;
	uint8_t plen;

	if (!addr || inet_aton(addr, &in) == 0)
		return 0;

	host = ntohl(in.s_addr);
	if (host == 0)
		plen = 0;
	else if ((host & 0x80000000U) == 0)
		plen = 8;
	else if ((host & 0xC0000000U) == 0x80000000U)
		plen = 16;
	else
		plen = 24;

	snprintf(prefix, prefix_len, "%s/%u", addr, plen);
	return 1;
}

static int eigrp_cli_network_prefix_from_wildcard(const char *addr,
						  const char *wildcard,
						  char *prefix,
						  size_t prefix_len)
{
	struct in_addr in, wc;
	uint32_t wildcard_host;
	uint32_t mask;
	uint32_t value;
	uint8_t plen = 0;
	int bit;

	if (!addr || !wildcard || inet_aton(addr, &in) == 0
	    || inet_aton(wildcard, &wc) == 0)
		return 0;

	wildcard_host = ntohl(wc.s_addr);
	for (bit = 31; bit >= 0; bit--) {
		if (wildcard_host & (1U << bit))
			break;
		plen++;
	}

	mask = plen == 0 ? 0 : (0xffffffffU << (32 - plen));
	value = ntohl(in.s_addr) & mask;
	in.s_addr = htonl(value);
	snprintf(prefix, prefix_len, "%s/%u", inet_ntoa(in), plen);
	return 1;
}

static int eigrp_cli_af_interface_path(struct vty *vty, char *ifname,
				       size_t ifname_len, char *asn, size_t asn_len)
{
	char vrf_name[VRF_NAMSIZ];

	if (!eigrp_cli_xpath_get(VTY_CURR_XPATH, "ifname", ifname, ifname_len)
	    && !eigrp_cli_xpath_get(VTY_CURR_XPATH, "interface", ifname,
				      ifname_len)) {
		vty_out(vty, "%% Enter named EIGRP af-interface mode first\n");
		return 0;
	}

	if (!eigrp_cli_current_as_vrf(vty, asn, asn_len, vrf_name, sizeof(vrf_name))) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return 0;
	}

	return 1;
}


static void eigrp_cli_interface_eigrp_xpath(char *xpath, size_t xpath_len,
					    const char *ifname, const char *leaf)
{
	snprintf(xpath, xpath_len,
		 "/frr-interface:lib/interface[name='%s']/frr-eigrpd:eigrp/%s",
		 ifname, leaf);
}

static void eigrp_cli_interface_instance_xpath(char *xpath, size_t xpath_len,
					       const char *ifname, const char *asn,
					       const char *leaf)
{
	snprintf(xpath, xpath_len,
		 "/frr-interface:lib/interface[name='%s']/frr-eigrpd:eigrp/instance[asn='%s']/%s",
		 ifname, asn, leaf);
}

DEFUN(router_eigrp_named,
      router_eigrp_named_cmd,
      "router eigrp WORD",
      ROUTER_STR
      EIGRP_STR
      "EIGRP named-mode instance name\n")
{
	const char *name = eigrp_cli_token_last(argc, argv);

	return eigrp_cli_push_named_root(vty, name);
}

DEFUN(no_router_eigrp_named,
      no_router_eigrp_named_cmd,
      "no router eigrp WORD",
      NO_STR
      ROUTER_STR
      EIGRP_STR
      "EIGRP named-mode instance name\n")
{
	const char *name = eigrp_cli_token_last(argc, argv);
	struct listnode *node, *nnode;
	eigrp_instance_t *eigrp;
	int matched = 0;

	for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		struct vrf *vrf;
		char xpath[XPATH_MAXLEN];

		if (!eigrp->name || strcmp(eigrp->name, name) != 0)
			continue;

		vrf = vrf_lookup_by_id(eigrp->vrf_id);
		snprintf(xpath, sizeof(xpath),
			 "/frr-eigrpd:eigrpd/instance[asn='%u'][vrf='%s']",
			 eigrp->AS, vrf ? vrf->name : VRF_DEFAULT_NAME);
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
		matched++;
	}

	if (!matched) {
		vty_out(vty, "%% EIGRP named instance %s not found\n", name);
		return CMD_SUCCESS;
	}

	return nb_cli_apply_changes_clear_pending(vty, NULL);
}

DEFUN(eigrp_address_family_ipv4,
      eigrp_address_family_ipv4_cmd,
      "address-family ipv4 [unicast] [vrf NAME] autonomous-system (1-65535)",
      "Enter address-family configuration mode\n"
      "IPv4 address family\n"
      "Unicast address family\n"
      VRF_CMD_HELP_STR
      "EIGRP autonomous system\n"
      AS_STR)
{
	char name[128];
	const char *asn = eigrp_cli_token_after(argc, argv, "autonomous-system");
	const char *vrf_name = eigrp_cli_token_after(argc, argv, "vrf");

	eigrp_cli_current_name(vty, name, sizeof(name));
	return eigrp_cli_named_instance_set(vty, name, asn,
					  eigrp_cli_vrf_name(vrf_name));
}

DEFUN(no_eigrp_address_family_ipv4,
      no_eigrp_address_family_ipv4_cmd,
      "no address-family ipv4 [unicast] [vrf NAME] autonomous-system (1-65535)",
      NO_STR
      "Enter address-family configuration mode\n"
      "IPv4 address family\n"
      "Unicast address family\n"
      VRF_CMD_HELP_STR
      "EIGRP autonomous system\n"
      AS_STR)
{
	const char *asn = eigrp_cli_token_after(argc, argv, "autonomous-system");
	const char *vrf_name = eigrp_cli_token_after(argc, argv, "vrf");

	return eigrp_cli_named_instance_unset(vty, asn,
					    eigrp_cli_vrf_name(vrf_name));
}

DEFUN(eigrp_address_family_ipv6,
      eigrp_address_family_ipv6_cmd,
      "address-family ipv6 [unicast] [vrf NAME] autonomous-system (1-65535)",
      "Enter address-family configuration mode\n"
      "IPv6 address family\n"
      "Unicast address family\n"
      VRF_CMD_HELP_STR
      "EIGRP autonomous system\n"
      AS_STR)
{
	return eigrp_cli_not_configured(vty, "address-family ipv6");
}

DEFUN(no_eigrp_address_family_ipv6,
      no_eigrp_address_family_ipv6_cmd,
      "no address-family ipv6 [unicast] [vrf NAME] autonomous-system (1-65535)",
      NO_STR
      "Enter address-family configuration mode\n"
      "IPv6 address family\n"
      "Unicast address family\n"
      VRF_CMD_HELP_STR
      "EIGRP autonomous system\n"
      AS_STR)
{
	return eigrp_cli_not_configured(vty, "no address-family ipv6");
}

DEFUN(eigrp_exit_address_family,
      eigrp_exit_address_family_cmd,
      "exit-address-family",
      "Exit address-family configuration mode\n")
{
	char name[128];

	eigrp_cli_current_name(vty, name, sizeof(name));
	return eigrp_cli_push_named_root(vty, name);
}

DEFUN(eigrp_no_shutdown,
      eigrp_no_shutdown_cmd,
      "no shutdown",
      NO_STR
      "Shutdown EIGRP address-family or interface\n")
{
	if (strstr(VTY_CURR_XPATH, "af-interface"))
		zlog_debug("EIGRP named-mode af-interface no shutdown accepted as current default behavior");
	else
		zlog_debug("EIGRP named-mode address-family no shutdown accepted as current default behavior");
	return CMD_SUCCESS;
}

DEFUN(eigrp_shutdown,
      eigrp_shutdown_cmd,
      "shutdown",
      "Shutdown EIGRP address-family or interface\n")
{
	if (strstr(VTY_CURR_XPATH, "af-interface"))
		return eigrp_cli_not_configured(vty, "af-interface shutdown");
	return eigrp_cli_not_configured(vty, "shutdown");
}

DEFUN(eigrp_network_address,
      eigrp_network_address_cmd,
      "network A.B.C.D [A.B.C.D]",
      "Enable routing on an IP network\n"
      "Network address\n"
      "Wildcard mask\n")
{
	const char *addr = NULL;
	const char *wildcard = NULL;
	char prefix[INET_ADDRSTRLEN + 4];
	char xpath[XPATH_MAXLEN];
	int i;

	for (i = 0; i < argc; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);
		struct in_addr tmp;

		if (!value || inet_aton(value, &tmp) == 0)
			continue;
		if (!addr)
			addr = value;
		else if (!wildcard)
			wildcard = value;
	}

	if (wildcard) {
		if (!eigrp_cli_network_prefix_from_wildcard(addr, wildcard, prefix,
							 sizeof(prefix))) {
			vty_out(vty, "%% Invalid EIGRP wildcard mask\n");
			return CMD_WARNING;
		}
	} else if (!eigrp_cli_network_prefix_from_address(addr, prefix,
							 sizeof(prefix))) {
		vty_out(vty, "%% Invalid EIGRP network address\n");
		return CMD_WARNING;
	}

	snprintf(xpath, sizeof(xpath), "%s/network[.='%s']", VTY_CURR_XPATH,
		 prefix);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_network_address,
      no_eigrp_network_address_cmd,
      "no network A.B.C.D [A.B.C.D]",
      NO_STR
      "Enable routing on an IP network\n"
      "Network address\n"
      "Wildcard mask\n")
{
	const char *addr = NULL;
	const char *wildcard = NULL;
	char prefix[INET_ADDRSTRLEN + 4];
	char xpath[XPATH_MAXLEN];
	int i;

	for (i = 0; i < argc; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);
		struct in_addr tmp;

		if (!value || inet_aton(value, &tmp) == 0)
			continue;
		if (!addr)
			addr = value;
		else if (!wildcard)
			wildcard = value;
	}

	if (wildcard) {
		if (!eigrp_cli_network_prefix_from_wildcard(addr, wildcard, prefix,
							 sizeof(prefix))) {
			vty_out(vty, "%% Invalid EIGRP wildcard mask\n");
			return CMD_WARNING;
		}
	} else if (!eigrp_cli_network_prefix_from_address(addr, prefix,
							 sizeof(prefix))) {
		vty_out(vty, "%% Invalid EIGRP network address\n");
		return CMD_WARNING;
	}

	snprintf(xpath, sizeof(xpath), "%s/network[.='%s']", VTY_CURR_XPATH,
		 prefix);
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface,
      eigrp_af_interface_cmd,
      "af-interface <default|IFNAME>",
      "Enter address-family interface configuration mode\n"
      "Default interface template\n"
      "Interface name\n")
{
	const char *ifname = eigrp_cli_token_last(argc, argv);
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_current_as_vrf(vty, asn, sizeof(asn), vrf_name,
				       sizeof(vrf_name))) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return CMD_WARNING;
	}

	if (strcmp(ifname, "default") == 0)
		return eigrp_cli_not_configured(vty, "af-interface default");

	snprintf(xpath, sizeof(xpath),
		 "/frr-eigrpd:eigrpd/address-family[asn='%s'][vrf='%s']/af-interface[ifname='%s']",
		 asn, vrf_name, ifname);
	VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return CMD_SUCCESS;
}

DEFUN(eigrp_exit_af_interface,
      eigrp_exit_af_interface_cmd,
      "exit-af-interface",
      "Exit address-family interface configuration mode\n")
{
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_current_as_vrf(vty, asn, sizeof(asn), vrf_name,
				       sizeof(vrf_name)))
		return CMD_SUCCESS;

	eigrp_cli_instance_xpath(xpath, sizeof(xpath), asn, vrf_name);
	VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return CMD_SUCCESS;
}

DEFUN(eigrp_af_interface_hello_interval,
      eigrp_af_interface_hello_interval_cmd,
      "hello-interval (1-65535)",
      "Configures EIGRP hello interval\n"
      "Seconds between hello transmissions\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];
	const char *hello = eigrp_cli_token_last(argc, argv);

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_eigrp_xpath(xpath, sizeof(xpath), ifname,
					 "hello-interval");
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, hello);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_hello_interval,
      no_eigrp_af_interface_hello_interval_cmd,
      "no hello-interval [(1-65535)]",
      NO_STR
      "Configures EIGRP hello interval\n"
      "Seconds between hello transmissions\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_eigrp_xpath(xpath, sizeof(xpath), ifname,
					 "hello-interval");
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_hold_time,
      eigrp_af_interface_hold_time_cmd,
      "hold-time (1-65535)",
      "Configures EIGRP hold time\n"
      "Seconds before neighbor is considered down\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];
	const char *hold = eigrp_cli_token_last(argc, argv);

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_eigrp_xpath(xpath, sizeof(xpath), ifname,
					 "hold-time");
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, hold);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_hold_time,
      no_eigrp_af_interface_hold_time_cmd,
      "no hold-time [(1-65535)]",
      NO_STR
      "Configures EIGRP hold time\n"
      "Seconds before neighbor is considered down\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_eigrp_xpath(xpath, sizeof(xpath), ifname,
					 "hold-time");
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_delay,
      eigrp_af_interface_delay_cmd,
      "delay (1-16777215)",
      "Specify interface throughput delay\n"
      "Throughput delay in tens of microseconds\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];
	const char *delay = eigrp_cli_token_last(argc, argv);

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_eigrp_xpath(xpath, sizeof(xpath), ifname, "delay");
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, delay);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_delay,
      no_eigrp_af_interface_delay_cmd,
      "no delay [(1-16777215)]",
      NO_STR
      "Specify interface throughput delay\n"
      "Throughput delay in tens of microseconds\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_eigrp_xpath(xpath, sizeof(xpath), ifname, "delay");
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_authentication_mode,
      eigrp_af_interface_authentication_mode_cmd,
      "authentication mode <md5|hmac-sha-256>",
      "Authentication subcommands\n"
      "Authentication mode\n"
      "Keyed message digest\n"
      "HMAC SHA256 algorithm\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];
	char instance_xpath[XPATH_MAXLEN];
	const char *mode = eigrp_cli_token_last(argc, argv);

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	snprintf(instance_xpath, sizeof(instance_xpath),
		 "/frr-interface:lib/interface[name='%s']/frr-eigrpd:eigrp/instance[asn='%s']",
		 ifname, asn);
	nb_cli_enqueue_change(vty, instance_xpath, NB_OP_CREATE, NULL);
	eigrp_cli_interface_instance_xpath(xpath, sizeof(xpath), ifname, asn,
					    "authentication");
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, mode);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_authentication_mode,
      no_eigrp_af_interface_authentication_mode_cmd,
      "no authentication mode [<md5|hmac-sha-256>]",
      NO_STR
      "Authentication subcommands\n"
      "Authentication mode\n"
      "Keyed message digest\n"
      "HMAC SHA256 algorithm\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_instance_xpath(xpath, sizeof(xpath), ifname, asn,
					    "authentication");
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, "none");
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_keychain,
      eigrp_af_interface_keychain_cmd,
      "authentication key-chain WORD",
      "Authentication subcommands\n"
      "Key-chain\n"
      "Name of key-chain\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];
	char instance_xpath[XPATH_MAXLEN];
	const char *name = eigrp_cli_token_last(argc, argv);

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	snprintf(instance_xpath, sizeof(instance_xpath),
		 "/frr-interface:lib/interface[name='%s']/frr-eigrpd:eigrp/instance[asn='%s']",
		 ifname, asn);
	nb_cli_enqueue_change(vty, instance_xpath, NB_OP_CREATE, NULL);
	eigrp_cli_interface_instance_xpath(xpath, sizeof(xpath), ifname, asn,
					    "keychain");
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, name);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_keychain,
      no_eigrp_af_interface_keychain_cmd,
      "no authentication key-chain [WORD]",
      NO_STR
      "Authentication subcommands\n"
      "Key-chain\n"
      "Name of key-chain\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_af_interface_path(vty, ifname, sizeof(ifname), asn,
					   sizeof(asn)))
		return CMD_WARNING;

	eigrp_cli_interface_instance_xpath(xpath, sizeof(xpath), ifname, asn,
					    "keychain");
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_passive,
      eigrp_af_interface_passive_cmd,
      "passive-interface",
      "Suppress routing updates on this interface\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_xpath_get(VTY_CURR_XPATH, "ifname", ifname,
				  sizeof(ifname))
	    || !eigrp_cli_current_as_vrf(vty, asn, sizeof(asn), vrf_name,
					 sizeof(vrf_name)))
		return CMD_WARNING;

	eigrp_cli_instance_xpath(xpath, sizeof(xpath), asn, vrf_name);
	snprintf(xpath + strlen(xpath), sizeof(xpath) - strlen(xpath),
		 "/passive-interface[.='%s']", ifname);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_passive,
      no_eigrp_af_interface_passive_cmd,
      "no passive-interface",
      NO_STR
      "Suppress routing updates on this interface\n")
{
	char ifname[IFNAMSIZ];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_xpath_get(VTY_CURR_XPATH, "ifname", ifname,
				  sizeof(ifname))
	    || !eigrp_cli_current_as_vrf(vty, asn, sizeof(asn), vrf_name,
					 sizeof(vrf_name)))
		return CMD_WARNING;

	eigrp_cli_instance_xpath(xpath, sizeof(xpath), asn, vrf_name);
	snprintf(xpath + strlen(xpath), sizeof(xpath) - strlen(xpath),
		 "/passive-interface[.='%s']", ifname);
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_bandwidth_percent,
      eigrp_af_interface_bandwidth_percent_cmd,
      "bandwidth-percent (1-999999)",
      "Set EIGRP bandwidth percentage\n"
      "Percentage of interface bandwidth\n")
{
	return eigrp_cli_not_configured(vty, "bandwidth-percent");
}

DEFUN(no_eigrp_af_interface_bandwidth_percent,
      no_eigrp_af_interface_bandwidth_percent_cmd,
      "no bandwidth-percent [(1-999999)]",
      NO_STR
      "Set EIGRP bandwidth percentage\n"
      "Percentage of interface bandwidth\n")
{
	return eigrp_cli_not_configured(vty, "no bandwidth-percent");
}

DEFUN(eigrp_af_interface_summary_address,
      eigrp_af_interface_summary_address_cmd,
      "summary-address A.B.C.D/M",
      "Perform address summarization\n"
      "Summary prefix\n")
{
	return eigrp_cli_not_configured(vty, "summary-address");
}

DEFUN(no_eigrp_af_interface_summary_address,
      no_eigrp_af_interface_summary_address_cmd,
      "no summary-address A.B.C.D/M",
      NO_STR
      "Perform address summarization\n"
      "Summary prefix\n")
{
	return eigrp_cli_not_configured(vty, "no summary-address");
}

DEFUN(eigrp_af_interface_split_horizon,
      eigrp_af_interface_split_horizon_cmd,
      "split-horizon",
      "Perform split horizon\n")
{
	return eigrp_cli_not_configured(vty, "split-horizon");
}

DEFUN(no_eigrp_af_interface_split_horizon,
      no_eigrp_af_interface_split_horizon_cmd,
      "no split-horizon",
      NO_STR
      "Perform split horizon\n")
{
	return eigrp_cli_not_configured(vty, "no split-horizon");
}

DEFUN(eigrp_topology_base,
      eigrp_topology_base_cmd,
      "topology base",
      "Configure EIGRP topology\n"
      "Base topology\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), "%s", VTY_CURR_XPATH);
	VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return CMD_SUCCESS;
}

DEFUN(eigrp_exit_af_topology,
      eigrp_exit_af_topology_cmd,
      "exit-af-topology",
      "Exit address-family topology mode\n")
{
	return CMD_SUCCESS;
}

DEFUN(eigrp_distance_stub,
      eigrp_distance_stub_cmd,
      "distance eigrp (1-255) (1-255)",
      "Define an administrative distance\n"
      EIGRP_STR
      "Internal route distance\n"
      "External route distance\n")
{
	return eigrp_cli_not_configured(vty, "distance eigrp");
}

DEFUN(eigrp_offset_list_stub,
      eigrp_offset_list_stub_cmd,
      "offset-list WORD <in|out> (0-2147483647) [IFNAME]",
      "Add or subtract offset from EIGRP metrics\n"
      "Access-list name\n"
      "Incoming updates\n"
      "Outgoing updates\n"
      "Metric offset\n"
      "Interface name\n")
{
	return eigrp_cli_not_configured(vty, "offset-list");
}

DEFUN(eigrp_summary_metric_stub,
      eigrp_summary_metric_stub_cmd,
      "summary-metric A.B.C.D/M metric (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)",
      "Configure summary metric\n"
      "Summary prefix\n"
      "Metric values\n"
      "Bandwidth metric in Kbits per second\n"
      "EIGRP delay metric, in 10 microsecond units\n"
      "EIGRP reliability metric where 255 is 100% reliable\n"
      "EIGRP Effective bandwidth metric where 255 is 100% loaded\n"
      "EIGRP MTU of the path\n")
{
	return eigrp_cli_not_configured(vty, "summary-metric");
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
eigrp_cli_init(void)
{
	/* Both EIGRP configuration entry forms are valid.  FRR's CLI matcher
	 * gives the numeric RANGE token priority over WORD, so an ASN selects
	 * classic mode while a non-numeric token selects named mode.
	 */
	install_element(CONFIG_NODE, &router_eigrp_cmd);
	install_element(CONFIG_NODE, &no_router_eigrp_cmd);
	install_element(CONFIG_NODE, &router_eigrp_named_cmd);
	install_element(CONFIG_NODE, &no_router_eigrp_named_cmd);

	install_node(&eigrp_node);
	install_default(EIGRP_NODE);

	install_element(EIGRP_NODE, &eigrp_address_family_ipv4_cmd);
	install_element(EIGRP_NODE, &no_eigrp_address_family_ipv4_cmd);
	install_element(EIGRP_NODE, &eigrp_address_family_ipv6_cmd);
	install_element(EIGRP_NODE, &no_eigrp_address_family_ipv6_cmd);
	install_element(EIGRP_NODE, &eigrp_exit_address_family_cmd);
	install_element(EIGRP_NODE, &eigrp_no_shutdown_cmd);
	install_element(EIGRP_NODE, &eigrp_shutdown_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_cmd);
	install_element(EIGRP_NODE, &eigrp_topology_base_cmd);
	install_element(EIGRP_NODE, &eigrp_exit_af_topology_cmd);

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
	install_element(EIGRP_NODE, &eigrp_network_address_cmd);
	install_element(EIGRP_NODE, &no_eigrp_network_address_cmd);
	install_element(EIGRP_NODE, &eigrp_neighbor_cmd);
	install_element(EIGRP_NODE, &eigrp_distribute_list_cmd);
	install_element(EIGRP_NODE, &eigrp_distribute_list_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_no_distribute_list_cmd);
	install_element(EIGRP_NODE, &eigrp_no_distribute_list_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_redistribute_source_metric_cmd);
	install_element(EIGRP_NODE, &eigrp_distance_stub_cmd);
	install_element(EIGRP_NODE, &eigrp_offset_list_stub_cmd);
	install_element(EIGRP_NODE, &eigrp_summary_metric_stub_cmd);

	vrf_cmd_init(NULL);

	if_cmd_init_default();

	install_element(EIGRP_NODE, &eigrp_exit_af_interface_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_hello_interval_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_hello_interval_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_hold_time_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_hold_time_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_delay_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_delay_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_authentication_mode_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_authentication_mode_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_keychain_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_keychain_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_passive_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_passive_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_bandwidth_percent_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_bandwidth_percent_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_summary_address_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_summary_address_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_split_horizon_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_split_horizon_cmd);
}
