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
#include "eigrpd/eigrp_instance.h"
#include "eigrpd.h"
#include "eigrp_zebra.h"
#include "eigrp_cli.h"

static bool eigrp_cli_named_topology_mode(struct vty *vty);
static int eigrp_cli_named_topology_required(struct vty *vty);
static int eigrp_cli_prefix_limit_set(struct vty *vty, int argc,
                                      struct cmd_token *argv[],
                                      const char *keyword,
                                      const char *xpath, bool remove);

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
static const char *route_map = NULL;
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

void eigrp_cli_show_named_header(struct vty *vty, const struct lyd_node *dnode,
				 bool show_defaults)
{
	const char *name = yang_dnode_get_string(dnode, "name");

	(void)show_defaults;
	vty_out(vty, "router eigrp %s\n", name);
}

void eigrp_cli_show_named_end(struct vty *vty, const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, "exit\n!\n");
}

void eigrp_cli_show_named_address_family(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	const char *afi = yang_dnode_get_string(dnode, "afi");
	const char *vrf = yang_dnode_get_string(dnode, "vrf");
	uint16_t asn = yang_dnode_get_uint16(dnode, "asn");

	(void)show_defaults;
	vty_out(vty, " address-family %s unicast", afi);
	if (strcmp(vrf, VRF_DEFAULT_NAME) != 0)
		vty_out(vty, " vrf %s", vrf);
	vty_out(vty, " autonomous-system %u\n", asn);
}

void eigrp_cli_show_named_address_family_end(struct vty *vty,
					     const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, " exit-address-family\n");
}

void eigrp_cli_show_named_neighbor(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	const char *address = yang_dnode_get_string(dnode, "address");
	const char *interface_name = yang_dnode_get_string(dnode, "interface");

	(void)show_defaults;
	vty_out(vty, "  neighbor %s %s\n", address, interface_name);
}

void eigrp_cli_show_named_shutdown(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "  shutdown\n");
}


void eigrp_cli_show_named_af_interface(struct vty *vty,
				       const struct lyd_node *dnode,
				       bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "  af-interface %s\n",
		yang_dnode_get_string(dnode, "interface"));
}

void eigrp_cli_show_named_af_interface_end(struct vty *vty,
					   const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, "  exit-af-interface\n");
}

void eigrp_cli_show_named_af_interface_bandwidth(struct vty *vty,
						 const struct lyd_node *dnode,
						 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   bandwidth-percent %u\n",
		yang_dnode_get_uint32(dnode, NULL));
}

void eigrp_cli_show_named_af_interface_hello(struct vty *vty,
					     const struct lyd_node *dnode,
					     bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   hello-interval %u\n",
		yang_dnode_get_uint16(dnode, NULL));
}

void eigrp_cli_show_named_af_interface_hold(struct vty *vty,
					    const struct lyd_node *dnode,
					    bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   hold-time %u\n", yang_dnode_get_uint16(dnode, NULL));
}

void eigrp_cli_show_named_af_interface_passive(struct vty *vty,
					       const struct lyd_node *dnode,
					       bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "   passive-interface\n");
}

void eigrp_cli_show_named_af_interface_authentication(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
	const char *mode = yang_dnode_get_string(dnode, NULL);
	const struct lyd_node *parent = lyd_parent(dnode);

	(void)show_defaults;
	vty_out(vty, "   authentication mode %s", mode);
	if (mode && strcmp(mode, "hmac-sha-256") == 0
	    && yang_dnode_exists(parent, "authentication-encryption-type")
	    && yang_dnode_exists(parent, "authentication-password"))
		vty_out(vty, " %u %s",
			yang_dnode_get_uint8(parent, "authentication-encryption-type"),
			yang_dnode_get_string(parent, "authentication-password"));
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_af_interface_keychain(struct vty *vty,
						const struct lyd_node *dnode,
						bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   authentication key-chain %s\n",
		yang_dnode_get_string(dnode, NULL));
}

void eigrp_cli_show_named_af_interface_next_hop_self(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
	(void)show_defaults;
	if (!yang_dnode_get_bool(dnode, NULL))
		vty_out(vty, "   no next-hop-self\n");
}

void eigrp_cli_show_named_af_interface_split_horizon(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
	(void)show_defaults;
	if (!yang_dnode_get_bool(dnode, NULL))
		vty_out(vty, "   no split-horizon\n");
}

void eigrp_cli_show_named_af_interface_summary(struct vty *vty,
					       const struct lyd_node *dnode,
					       bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   summary-address %s %s",
		yang_dnode_get_string(dnode, "address"),
		yang_dnode_get_string(dnode, "mask"));
	if (yang_dnode_exists(dnode, "administrative-distance"))
		vty_out(vty, " %u",
			yang_dnode_get_uint8(dnode, "administrative-distance"));
	if (yang_dnode_exists(dnode, "leak-map"))
		vty_out(vty, " leak-map %s",
			yang_dnode_get_string(dnode, "leak-map"));
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_af_interface_shutdown(struct vty *vty,
						const struct lyd_node *dnode,
						bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "   shutdown\n");
}

void eigrp_cli_show_named_topology(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "  topology base\n");
}

void eigrp_cli_show_named_topology_end(struct vty *vty,
				       const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, "  exit-af-topology\n");
}

void eigrp_cli_show_named_auto_summary(struct vty *vty,
				       const struct lyd_node *dnode,
				       bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "   auto-summary\n");
}

static void eigrp_cli_show_named_default_information(
	struct vty *vty, const struct lyd_node *dnode, const char *direction)
{
	vty_out(vty, "   default-information %s", direction);
	if (yang_dnode_exists(dnode, "access-list"))
		vty_out(vty, " %s", yang_dnode_get_string(dnode, "access-list"));
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_default_information_in(struct vty *vty,
						 const struct lyd_node *dnode,
						 bool show_defaults)
{
	(void)show_defaults;
	eigrp_cli_show_named_default_information(vty, dnode, "in");
}

void eigrp_cli_show_named_default_information_out(struct vty *vty,
						  const struct lyd_node *dnode,
						  bool show_defaults)
{
	(void)show_defaults;
	eigrp_cli_show_named_default_information(vty, dnode, "out");
}

void eigrp_cli_show_named_default_metric(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   default-metric %u %u %u %u %u\n",
		yang_dnode_get_uint32(dnode, "bandwidth"),
		yang_dnode_get_uint32(dnode, "delay"),
		yang_dnode_get_uint8(dnode, "reliability"),
		yang_dnode_get_uint8(dnode, "load"),
		yang_dnode_get_uint16(dnode, "mtu"));
}

void eigrp_cli_show_named_distance(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   distance eigrp %u %u\n",
		yang_dnode_get_uint8(dnode, "internal"),
		yang_dnode_get_uint8(dnode, "external"));
}

static void eigrp_cli_show_prefix_limit(struct vty *vty,
				       const struct lyd_node *dnode,
				       const char *prefix)
{
	vty_out(vty, "%s%u", prefix, yang_dnode_get_uint32(dnode, "maximum"));
	if (yang_dnode_exists(dnode, "threshold"))
		vty_out(vty, " %u", yang_dnode_get_uint8(dnode, "threshold"));
	if (yang_dnode_exists(dnode, "dampened"))
		vty_out(vty, " dampened");
	if (yang_dnode_exists(dnode, "reset-time"))
		vty_out(vty, " reset-time %u", yang_dnode_get_uint16(dnode, "reset-time"));
	if (yang_dnode_exists(dnode, "restart"))
		vty_out(vty, " restart %u", yang_dnode_get_uint16(dnode, "restart"));
	if (yang_dnode_exists(dnode, "restart-count"))
		vty_out(vty, " restart-count %u", yang_dnode_get_uint16(dnode, "restart-count"));
	if (yang_dnode_exists(dnode, "warning-only"))
		vty_out(vty, " warning-only");
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_maximum_prefix(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	eigrp_cli_show_prefix_limit(vty, dnode, "   maximum-prefix ");
}

void eigrp_cli_show_named_metric_weights(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   metric weights %u %u %u %u %u %u\n",
		yang_dnode_get_uint8(dnode, "tos"),
		yang_dnode_get_uint8(dnode, "K1"),
		yang_dnode_get_uint8(dnode, "K2"),
		yang_dnode_get_uint8(dnode, "K3"),
		yang_dnode_get_uint8(dnode, "K4"),
		yang_dnode_get_uint8(dnode, "K5"));
}

void eigrp_cli_show_named_offset_list(struct vty *vty,
				      const struct lyd_node *dnode,
				      bool show_defaults)
{
	const char *interface_name = yang_dnode_get_string(dnode, "interface");

	(void)show_defaults;
	vty_out(vty, "   offset-list %s %s %u",
		yang_dnode_get_string(dnode, "access-list"),
		yang_dnode_get_string(dnode, "direction"),
		yang_dnode_get_uint32(dnode, "offset"));
	if (interface_name && interface_name[0])
		vty_out(vty, " %s", interface_name);
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_redistribute(struct vty *vty,
				       const struct lyd_node *dnode,
				       bool show_defaults)
{
	const char *protocol = yang_dnode_get_string(dnode, "protocol");

	(void)show_defaults;
	vty_out(vty, "   redistribute %s", protocol);
	if (yang_dnode_exists(dnode, "metrics"))
		vty_out(vty, " metric %u %u %u %u %u",
			yang_dnode_get_uint32(dnode, "metrics/bandwidth"),
			yang_dnode_get_uint32(dnode, "metrics/delay"),
			yang_dnode_get_uint8(dnode, "metrics/reliability"),
			yang_dnode_get_uint8(dnode, "metrics/load"),
			yang_dnode_get_uint16(dnode, "metrics/mtu"));
	if (yang_dnode_exists(dnode, "route-map"))
		vty_out(vty, " route-map %s", yang_dnode_get_string(dnode, "route-map"));
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_summary_metric(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   summary-metric %s %s",
		yang_dnode_get_string(dnode, "address"),
		yang_dnode_get_string(dnode, "mask"));
	if (yang_dnode_exists(dnode, "bandwidth"))
		vty_out(vty, " %u %u %u %u %u",
			yang_dnode_get_uint32(dnode, "bandwidth"),
			yang_dnode_get_uint32(dnode, "delay"),
			yang_dnode_get_uint8(dnode, "reliability"),
			yang_dnode_get_uint8(dnode, "load"),
			yang_dnode_get_uint16(dnode, "mtu"));
	if (yang_dnode_exists(dnode, "distance"))
		vty_out(vty, " distance %u", yang_dnode_get_uint8(dnode, "distance"));
	vty_out(vty, "\n");
}

void eigrp_cli_show_named_neighbor_description(struct vty *vty,
                                                const struct lyd_node *dnode,
                                                bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "  neighbor %s description %s\n",
            yang_dnode_get_string(lyd_parent(dnode), "address"),
            yang_dnode_get_string(dnode, NULL));
}

void eigrp_cli_show_named_neighbor_maximum_prefix(struct vty *vty,
                                                   const struct lyd_node *dnode,
                                                   bool show_defaults)
{
    char prefix[256];
    (void)show_defaults;
    snprintf(prefix, sizeof(prefix), "  neighbor %s maximum-prefix ",
             yang_dnode_get_string(lyd_parent(dnode), "address"));
    eigrp_cli_show_prefix_limit(vty, dnode, prefix);
}

void eigrp_cli_show_named_neighbor_maximum_prefix_all(struct vty *vty,
                                                       const struct lyd_node *dnode,
                                                       bool show_defaults)
{
    (void)show_defaults;
    eigrp_cli_show_prefix_limit(vty, dnode, "  neighbor maximum-prefix ");
}

void eigrp_cli_show_named_log_neighbor_changes(struct vty *vty,
                                                const struct lyd_node *dnode,
                                                bool show_defaults)
{
    (void)show_defaults;
    if (!yang_dnode_get_bool(dnode, NULL))
        vty_out(vty, "  no eigrp log-neighbor-changes\n");
}

void eigrp_cli_show_named_log_neighbor_warnings(struct vty *vty,
                                                 const struct lyd_node *dnode,
                                                 bool show_defaults)
{
    (void)show_defaults;
    if (!yang_dnode_get_bool(dnode, "enabled")) {
        vty_out(vty, "  no eigrp log-neighbor-warnings\n");
        return;
    }
    vty_out(vty, "  eigrp log-neighbor-warnings");
    if (yang_dnode_exists(dnode, "interval"))
        vty_out(vty, " %u", yang_dnode_get_uint16(dnode, "interval"));
    vty_out(vty, "\n");
}

void eigrp_cli_show_named_maximum_paths(struct vty *vty,
                                         const struct lyd_node *dnode,
                                         bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "   maximum-paths %u\n", yang_dnode_get_uint8(dnode, NULL));
}

void eigrp_cli_show_named_metric_maximum_hops(struct vty *vty,
                                               const struct lyd_node *dnode,
                                               bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "   metric maximum-hops %u\n", yang_dnode_get_uint8(dnode, NULL));
}

void eigrp_cli_show_named_metric_holddown(struct vty *vty,
                                           const struct lyd_node *dnode,
                                           bool show_defaults)
{
    (void)dnode;
    (void)show_defaults;
    vty_out(vty, "   metric holddown\n");
}

void eigrp_cli_show_named_event_log_size(struct vty *vty,
                                          const struct lyd_node *dnode,
                                          bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "   eigrp event-log-size %u\n",
            yang_dnode_get_uint32(dnode, NULL));
}

void eigrp_cli_show_named_redistribute_maximum_prefix(
    struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
    (void)show_defaults;
    eigrp_cli_show_prefix_limit(vty, dnode, "   redistribute maximum-prefix ");
}

void eigrp_cli_show_named_distribute_list(struct vty *vty,
                                           const struct lyd_node *dnode,
                                           bool show_defaults)
{
    const struct lyd_node *direction_node = lyd_parent(dnode);
    const struct lyd_node *list_node = lyd_parent(direction_node);
    const char *ifname = yang_dnode_get_string(list_node, "interface");
    const char *kind = strstr(dnode->schema->name, "prefix-list") ? " prefix" : "";
    const char *direction = direction_node->schema->name;

    (void)show_defaults;
    vty_out(vty, "   distribute-list%s %s %s", kind,
            yang_dnode_get_string(dnode, NULL), direction);
    if (ifname && ifname[0])
        vty_out(vty, " %s", ifname);
    vty_out(vty, "\n");
}

void eigrp_cli_show_named_active_time(struct vty *vty,
				      const struct lyd_node *dnode,
				      bool show_defaults)
{
	uint16_t active_time = yang_dnode_get_uint16(dnode, NULL);

	(void)show_defaults;
	if (active_time == 0)
		vty_out(vty, "   timers active-time disabled\n");
	else
		vty_out(vty, "   timers active-time %u\n", active_time);
}

void eigrp_cli_show_named_traffic_share_balanced(struct vty *vty,
						 const struct lyd_node *dnode,
						 bool show_defaults)
{
	(void)show_defaults;
	if (yang_dnode_get_bool(dnode, NULL))
		vty_out(vty, "   traffic-share balanced\n");
	else
		vty_out(vty, "   no traffic-share balanced\n");
}

void eigrp_cli_show_named_variance(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   variance %u\n", yang_dnode_get_uint8(dnode, NULL));
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
	if (!strstr(VTY_CURR_XPATH, "/named["))
		return CMD_WARNING_CONFIG_FAILED;
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;

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
	if (!strstr(VTY_CURR_XPATH, "/named["))
		return CMD_WARNING_CONFIG_FAILED;
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./active-time", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
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
	if (strstr(VTY_CURR_XPATH, "/named[")
	    && !eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
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
	if (strstr(VTY_CURR_XPATH, "/named[")
	    && !eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
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
	if (strstr(VTY_CURR_XPATH, "/named[")) {
		if (!eigrp_cli_named_topology_required(vty))
			return CMD_WARNING;
		/* Named-mode syntax is: tos, K1, K2, K3, K4, K5. */
		/* k6 is the parsed numeric value, so a valid final K5 value of 0
		 * evaluates false.  k6_str distinguishes an omitted sixth token from
		 * the explicitly configured value 0.
		 */
		if (!k6_str || strcmp(k1_str, "0") != 0) {
			vty_out(vty, "%% EIGRP metric weights TOS must be 0 and K1-K5 are required\n");
			return CMD_WARNING;
		}
		nb_cli_enqueue_change(vty, "./metric-weights", NB_OP_CREATE, NULL);
		nb_cli_enqueue_change(vty, "./metric-weights/tos", NB_OP_MODIFY, k1_str);
		nb_cli_enqueue_change(vty, "./metric-weights/K1", NB_OP_MODIFY, k2_str);
		nb_cli_enqueue_change(vty, "./metric-weights/K2", NB_OP_MODIFY, k3_str);
		nb_cli_enqueue_change(vty, "./metric-weights/K3", NB_OP_MODIFY, k4_str);
		nb_cli_enqueue_change(vty, "./metric-weights/K4", NB_OP_MODIFY, k5_str);
		nb_cli_enqueue_change(vty, "./metric-weights/K5", NB_OP_MODIFY, k6_str);
		return nb_cli_apply_changes(vty, NULL);
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
	if (strstr(VTY_CURR_XPATH, "/named[")) {
		if (!eigrp_cli_named_topology_required(vty))
			return CMD_WARNING;
		nb_cli_enqueue_change(vty, "./metric-weights", NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}

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

/* Classic static-neighbor CLI is intentionally unsupported here.
 * Named mode uses the address-family neighbor commands below. */

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
	"$proto [metric (1-4294967295)$bw (0-4294967295)$delay (0-255)$rlbt (1-255)$load (1-65535)$mtu] [route-map WORD$route_map]",
	NO_STR
	REDIST_STR
	FRR_REDIST_HELP_STR_EIGRPD
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

	if (strstr(VTY_CURR_XPATH, "/named[")) {
		if (!eigrp_cli_named_topology_required(vty))
			return CMD_WARNING;
	} else if (route_map) {
		/* Classic redistribute route-map has no supported backend. */
		return CMD_WARNING_CONFIG_FAILED;
	}

	snprintf(xpath, sizeof(xpath), "./redistribute[protocol='%s']", proto);

	if (no) {
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}

	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics", xpath);
	if (bw == 0 || delay == 0 || rlbt == 0 || load == 0 || mtu == 0) {
		if (strstr(VTY_CURR_XPATH, "/named["))
			nb_cli_enqueue_change(vty, xpath_metric, NB_OP_DESTROY, NULL);
	} else {
		if (strstr(VTY_CURR_XPATH, "/named["))
			nb_cli_enqueue_change(vty, xpath_metric, NB_OP_CREATE, NULL);
		snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/bandwidth", xpath);
		nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, bw_str);
		snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/delay", xpath);
		nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, delay_str);
		snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/reliability", xpath);
		nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, rlbt_str);
		snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/load", xpath);
		nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, load_str);
		snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics/mtu", xpath);
		nb_cli_enqueue_change(vty, xpath_metric, NB_OP_MODIFY, mtu_str);
	}

	if (strstr(VTY_CURR_XPATH, "/named[")) {
		snprintf(xpath_metric, sizeof(xpath_metric), "%s/route-map", xpath);
		nb_cli_enqueue_change(vty, xpath_metric,
				route_map ? NB_OP_MODIFY : NB_OP_DESTROY, route_map);
	}
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
 * Classic interface-mode EIGRP configuration is intentionally not exposed by
 * this project.  Named mode owns interface configuration through af-interface
 * and terminates at the portable eigrp_interface/eigrp_auth/eigrp_summary APIs.
 *
 * The old FRR interface CLI handlers that lived here were never registered in
 * eigrp_cli_init(), and several of their northbound targets had no functional backend.
 * Keep unsupported classic interface configuration out of the adapter rather
 * than carrying unreachable or partially functional commands.
 */

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

static bool eigrp_cli_token_present(int argc, struct cmd_token *argv[],
				    const char *keyword)
{
	int i;

	for (i = 0; i < argc; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);
		if (value && strcmp(value, keyword) == 0)
			return true;
	}
	return false;
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

static bool eigrp_cli_current_afi(struct vty *vty, char *afi, size_t afi_len)
{
	return eigrp_cli_xpath_get(VTY_CURR_XPATH, "afi", afi, afi_len);
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

int eigrp_cli_result_render(struct vty *vty, const char *operation,
			    eigrp_result_t result)
{
	switch (result) {
	case EIGRP_RESULT_SUCCESS:
		return CMD_SUCCESS;
	case EIGRP_RESULT_NOT_IMPLEMENTED:
		vty_out(vty, "%% EIGRP %s is not currently implemented\n", operation);
		return CMD_SUCCESS;
	case EIGRP_RESULT_INVALID_ARGUMENT:
		vty_out(vty, "%% Invalid EIGRP %s configuration\n", operation);
		return CMD_WARNING;
	case EIGRP_RESULT_NOT_FOUND:
		vty_out(vty, "%% EIGRP %s target was not found\n", operation);
		return CMD_WARNING;
	case EIGRP_RESULT_CONFLICT:
		vty_out(vty, "%% EIGRP %s conflicts with existing configuration\n", operation);
		return CMD_WARNING;
	case EIGRP_RESULT_UNSUPPORTED:
		vty_out(vty, "%% EIGRP %s is not supported for this address family or capability\n", operation);
		return CMD_WARNING;
	case EIGRP_RESULT_INTERNAL_FAILURE:
	default:
		vty_out(vty, "%% EIGRP %s failed internally\n", operation);
		return CMD_WARNING;
	}
}

static int eigrp_cli_token_values_after(int argc, struct cmd_token *argv[],
				const char *keyword, const char **values,
				int values_len)
{
	bool found = false;
	int count = 0;
	int i;

	if (!keyword || !values || values_len <= 0)
		return -1;

	for (i = 0; i < argc; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);

		if (!value)
			continue;
		if (!found) {
			if (strcmp(value, keyword) == 0)
				found = true;
			continue;
		}
		if (count < values_len)
			values[count++] = value;
	}

	return found ? count : -1;
}

static bool eigrp_cli_xpath_leaf_build(char *leaf_xpath, size_t leaf_xpath_len,
				       const char *xpath, const char *leaf)
{
	size_t xpath_len;
	size_t leaf_len;
	size_t remaining;

	if (!leaf_xpath || leaf_xpath_len == 0 || !xpath || !leaf)
		return false;

	xpath_len = strlen(xpath);
	leaf_len = strlen(leaf);
	if (xpath_len >= leaf_xpath_len)
		return false;

	remaining = leaf_xpath_len - xpath_len;
	if (remaining < 2 || leaf_len > remaining - 2)
		return false;

	memcpy(leaf_xpath, xpath, xpath_len);
	leaf_xpath[xpath_len] = '/';
	memcpy(leaf_xpath + xpath_len + 1, leaf, leaf_len + 1);
	return true;
}

static void eigrp_cli_named_xpath(char *xpath, size_t xpath_len,
				   const char *name)
{
	snprintf(xpath, xpath_len,
		 "/frr-eigrpd:eigrpd/named[name='%s']", name);
}

static void eigrp_cli_named_af_xpath(char *xpath, size_t xpath_len,
				      const char *name, const char *afi,
				      const char *vrf_name, const char *asn)
{
	snprintf(xpath, xpath_len,
		 "/frr-eigrpd:eigrpd/named[name='%s']/address-family[afi='%s'][vrf='%s'][asn='%s']",
		 name, afi, eigrp_cli_vrf_name(vrf_name), asn);
}

static void eigrp_cli_named_af_interface_xpath(char *xpath, size_t xpath_len,
					       const char *name, const char *afi,
					       const char *vrf_name, const char *asn,
					       const char *interface_name)
{
	eigrp_cli_named_af_xpath(xpath, xpath_len, name, afi, vrf_name, asn);
	snprintf(xpath + strlen(xpath), xpath_len - strlen(xpath),
		 "/af-interface[interface='%s']", interface_name);
}

static void eigrp_cli_named_topology_xpath(char *xpath, size_t xpath_len,
					 const char *name, const char *afi,
					 const char *vrf_name, const char *asn)
{
	eigrp_cli_named_af_xpath(xpath, xpath_len, name, afi, vrf_name, asn);
	snprintf(xpath + strlen(xpath), xpath_len - strlen(xpath), "/topology");
}

static bool eigrp_cli_named_topology_mode(struct vty *vty)
{
	return strstr(VTY_CURR_XPATH, "/named[") != NULL
	       && strstr(VTY_CURR_XPATH, "/address-family[") != NULL
	       && strstr(VTY_CURR_XPATH, "/topology") != NULL;
}

static int eigrp_cli_named_topology_required(struct vty *vty)
{
	if (eigrp_cli_named_topology_mode(vty))
		return 1;
	vty_out(vty, "%% Enter named EIGRP topology base mode first\n");
	return 0;
}

static int eigrp_cli_push_named_root(struct vty *vty, const char *name)
{
	char xpath[XPATH_MAXLEN];
	int rv;

	if (!name || !name[0])
		return CMD_WARNING;

	eigrp_cli_named_xpath(xpath, sizeof(xpath), name);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);
	if (rv == CMD_SUCCESS)
		VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return rv;
}

static int eigrp_cli_named_address_family_set(struct vty *vty,
					      const char *name,
					      const char *afi,
					      const char *asn,
					      const char *vrf_name)
{
	char xpath[XPATH_MAXLEN];
	uint16_t as;
	int rv;

	if (!name || !name[0] || !eigrp_cli_parse_uint16(asn, &as)) {
		vty_out(vty, "%% Invalid EIGRP named address-family configuration\n");
		return CMD_WARNING;
	}

	eigrp_cli_named_af_xpath(xpath, sizeof(xpath), name, afi,
				eigrp_cli_vrf_name(vrf_name), asn);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);
	if (rv == CMD_SUCCESS)
		VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return rv;
}

static int eigrp_cli_named_address_family_unset(struct vty *vty,
						const char *name,
						const char *afi,
						const char *asn,
						const char *vrf_name)
{
	char xpath[XPATH_MAXLEN];

	eigrp_cli_named_af_xpath(xpath, sizeof(xpath), name, afi,
				eigrp_cli_vrf_name(vrf_name), asn);
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

static int eigrp_cli_af_interface_path(struct vty *vty, char *interface_name,
				       size_t interface_name_len)
{
	if (!strstr(VTY_CURR_XPATH, "/named[")
	    || !strstr(VTY_CURR_XPATH, "/address-family[")
	    || !strstr(VTY_CURR_XPATH, "/af-interface[")
	    || !eigrp_cli_xpath_get(VTY_CURR_XPATH, "interface",
				   interface_name, interface_name_len)) {
		vty_out(vty, "%% Enter named EIGRP af-interface mode first\n");
		return 0;
	}

	return 1;
}

static bool eigrp_cli_named_af_context(struct vty *vty, char *name,
				       size_t name_len, char *afi,
				       size_t afi_len, char *vrf_name,
				       size_t vrf_len, char *asn,
				       size_t asn_len)
{
	return strstr(VTY_CURR_XPATH, "/named[") != NULL
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "name", name, name_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "afi", afi, afi_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "vrf", vrf_name, vrf_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "asn", asn, asn_len);
}

static bool eigrp_cli_named_af_mode(struct vty *vty)
{
	return strstr(VTY_CURR_XPATH, "/named[") != NULL
	       && strstr(VTY_CURR_XPATH, "/address-family[") != NULL
	       && strstr(VTY_CURR_XPATH, "/af-interface[") == NULL
	       && strstr(VTY_CURR_XPATH, "/topology") == NULL;
}

static int eigrp_cli_named_af_required(struct vty *vty)
{
	if (eigrp_cli_named_af_mode(vty))
		return 1;
	vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
	return 0;
}

DEFUN_NOSH(router_eigrp_named,
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
	char xpath[XPATH_MAXLEN];

	eigrp_cli_named_xpath(xpath, sizeof(xpath), name);
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
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
	return eigrp_cli_named_address_family_set(vty, name, "ipv4", asn,
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
	char name[128];
	const char *asn = eigrp_cli_token_after(argc, argv, "autonomous-system");
	const char *vrf_name = eigrp_cli_token_after(argc, argv, "vrf");

	eigrp_cli_current_name(vty, name, sizeof(name));
	return eigrp_cli_named_address_family_unset(vty, name, "ipv4", asn,
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
	char name[128];
	const char *asn = eigrp_cli_token_after(argc, argv, "autonomous-system");
	const char *vrf_name = eigrp_cli_token_after(argc, argv, "vrf");

	eigrp_cli_current_name(vty, name, sizeof(name));
	return eigrp_cli_named_address_family_set(vty, name, "ipv6", asn,
						    eigrp_cli_vrf_name(vrf_name));
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
	char name[128];
	const char *asn = eigrp_cli_token_after(argc, argv, "autonomous-system");
	const char *vrf_name = eigrp_cli_token_after(argc, argv, "vrf");

	eigrp_cli_current_name(vty, name, sizeof(name));
	return eigrp_cli_named_address_family_unset(vty, name, "ipv6", asn,
						      eigrp_cli_vrf_name(vrf_name));
}

DEFUN(eigrp_exit_address_family,
      eigrp_exit_address_family_cmd,
      "exit-address-family",
      "Exit address-family configuration mode\n")
{
	if (vty->xpath_index > 1
	    && strstr(VTY_CURR_XPATH, "/address-family[") != NULL)
		vty->xpath_index--;
	vty->node = EIGRP_NODE;
	return CMD_SUCCESS;
}

DEFUN(eigrp_no_shutdown,
      eigrp_no_shutdown_cmd,
      "no shutdown",
      NO_STR
      "Shutdown EIGRP address-family or interface\n")
{
	if (strstr(VTY_CURR_XPATH, "/af-interface[")) {
		nb_cli_enqueue_change(vty, "./shutdown", NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}
	if (strstr(VTY_CURR_XPATH, "/named[")
	    && strstr(VTY_CURR_XPATH, "/address-family[")) {
		nb_cli_enqueue_change(vty, "./shutdown", NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}
	if (strstr(VTY_CURR_XPATH, "/named[")
	    && strstr(VTY_CURR_XPATH, "/address-family[") == NULL) {
		char name[128];

		eigrp_cli_current_name(vty, name, sizeof(name));
		return eigrp_cli_result_render(
			vty, "named process no shutdown",
			eigrp_instance_parent_shutdown_update(
				eigrp_instance_parent_read(name), false));
	}
	return CMD_WARNING;
}

DEFUN(eigrp_shutdown,
      eigrp_shutdown_cmd,
      "shutdown",
      "Shutdown EIGRP address-family or interface\n")
{
	if (strstr(VTY_CURR_XPATH, "/af-interface[")) {
		nb_cli_enqueue_change(vty, "./shutdown", NB_OP_CREATE, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}
	if (strstr(VTY_CURR_XPATH, "/named[")
	    && strstr(VTY_CURR_XPATH, "/address-family[")) {
		nb_cli_enqueue_change(vty, "./shutdown", NB_OP_CREATE, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}
	if (strstr(VTY_CURR_XPATH, "/named[")
	    && strstr(VTY_CURR_XPATH, "/address-family[") == NULL) {
		char name[128];

		eigrp_cli_current_name(vty, name, sizeof(name));
		return eigrp_cli_result_render(
			vty, "named process shutdown",
			eigrp_instance_parent_shutdown_update(
				eigrp_instance_parent_read(name), true));
	}
	return CMD_WARNING;
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
	char afi[8];
	int i;

	if (eigrp_cli_current_afi(vty, afi, sizeof(afi))
	    && strcmp(afi, "ipv4") != 0) {
		vty_out(vty, "%% network is valid only under named IPv4 address-family\n");
		return CMD_WARNING;
	}

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
	char afi[8];
	int i;

	if (eigrp_cli_current_afi(vty, afi, sizeof(afi))
	    && strcmp(afi, "ipv4") != 0) {
		vty_out(vty, "%% network is valid only under named IPv4 address-family\n");
		return CMD_WARNING;
	}

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

static int eigrp_cli_named_neighbor_set(struct vty *vty, const char *address,
					 const char *interface_name, bool remove)
{
	char afi[8];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_current_afi(vty, afi, sizeof(afi))) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return CMD_WARNING;
	}

	snprintf(xpath, sizeof(xpath),
		 "./neighbor[address='%s'][interface='%s']", address,
		 interface_name);
	nb_cli_enqueue_change(vty, xpath,
			      remove ? NB_OP_DESTROY : NB_OP_CREATE, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_named_neighbor_ipv4,
      eigrp_named_neighbor_ipv4_cmd,
      "neighbor A.B.C.D IFNAME",
      "Specify a neighbor router\n"
      "Neighbor IPv4 address\n"
      "Interface used to reach the neighbor\n")
{
	char afi[8];

	if (!eigrp_cli_current_afi(vty, afi, sizeof(afi))
	    || strcmp(afi, "ipv4") != 0) {
		vty_out(vty, "%% IPv4 neighbor is valid only under named IPv4 address-family\n");
		return CMD_WARNING;
	}
	return eigrp_cli_named_neighbor_set(
		vty, eigrp_cli_token_after(argc, argv, "neighbor"),
		eigrp_cli_token_last(argc, argv), false);
}

DEFUN(no_eigrp_named_neighbor_ipv4,
      no_eigrp_named_neighbor_ipv4_cmd,
      "no neighbor A.B.C.D IFNAME",
      NO_STR
      "Specify a neighbor router\n"
      "Neighbor IPv4 address\n"
      "Interface used to reach the neighbor\n")
{
	return eigrp_cli_named_neighbor_set(
		vty, eigrp_cli_token_after(argc, argv, "neighbor"),
		eigrp_cli_token_last(argc, argv), true);
}

DEFUN(eigrp_named_neighbor_ipv6,
      eigrp_named_neighbor_ipv6_cmd,
      "neighbor X:X::X:X IFNAME",
      "Specify a neighbor router\n"
      "Neighbor IPv6 address\n"
      "Interface used to reach the neighbor\n")
{
	char afi[8];

	if (!eigrp_cli_current_afi(vty, afi, sizeof(afi))
	    || strcmp(afi, "ipv6") != 0) {
		vty_out(vty, "%% IPv6 neighbor is valid only under named IPv6 address-family\n");
		return CMD_WARNING;
	}
	return eigrp_cli_named_neighbor_set(
		vty, eigrp_cli_token_after(argc, argv, "neighbor"),
		eigrp_cli_token_last(argc, argv), false);
}

DEFUN(no_eigrp_named_neighbor_ipv6,
      no_eigrp_named_neighbor_ipv6_cmd,
      "no neighbor X:X::X:X IFNAME",
      NO_STR
      "Specify a neighbor router\n"
      "Neighbor IPv6 address\n"
      "Interface used to reach the neighbor\n")
{
	return eigrp_cli_named_neighbor_set(
		vty, eigrp_cli_token_after(argc, argv, "neighbor"),
		eigrp_cli_token_last(argc, argv), true);
}

static int eigrp_cli_neighbor_policy_xpath(struct vty *vty, const char *address,
                                           char *xpath, size_t xpath_len)
{
    if (!eigrp_cli_named_af_required(vty) || !address || !address[0])
        return 0;
    snprintf(xpath, xpath_len, "./neighbor-policy[address='%s']", address);
    return 1;
}

DEFUN(eigrp_neighbor_description,
      eigrp_neighbor_description_cmd,
      "neighbor <A.B.C.D|X:X::X:X> description LINE",
      "Specify a neighbor router\n" "Neighbor address\n" "Neighbor address\n"
      "Neighbor description\n" "Description text\n")
{
    const char *address = eigrp_cli_token_after(argc, argv, "neighbor");
    const char *description = eigrp_cli_token_after(argc, argv, "description");
    char xpath[XPATH_MAXLEN], child[XPATH_MAXLEN];
    if (!eigrp_cli_neighbor_policy_xpath(vty, address, xpath, sizeof(xpath)) || !description)
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
    if (!eigrp_cli_xpath_leaf_build(child, sizeof(child), xpath, "description"))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, child, NB_OP_MODIFY, description);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_neighbor_description,
      no_eigrp_neighbor_description_cmd,
      "no neighbor <A.B.C.D|X:X::X:X> description [LINE]",
      NO_STR "Specify a neighbor router\n" "Neighbor address\n" "Neighbor address\n"
      "Neighbor description\n" "Description text\n")
{
    const char *address = eigrp_cli_token_after(argc, argv, "neighbor");
    char xpath[XPATH_MAXLEN], child[XPATH_MAXLEN];
    if (!eigrp_cli_neighbor_policy_xpath(vty, address, xpath, sizeof(xpath)))
        return CMD_WARNING;
    if (!eigrp_cli_xpath_leaf_build(child, sizeof(child), xpath, "description"))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, child, NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_neighbor_maximum_prefix,
      eigrp_neighbor_maximum_prefix_cmd,
      "neighbor <A.B.C.D|X:X::X:X> maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]",
      "Specify a neighbor router\n" "Neighbor address\n" "Neighbor address\n"
      "Limit prefixes accepted from this neighbor\n" "Maximum prefixes\n"
      "Warning threshold\n" "Dampening\n" "Reset time\n" "Minutes\n"
      "Restart delay\n" "Minutes\n" "Restart count\n" "Count\n" "Warning only\n")
{
    const char *address = eigrp_cli_token_after(argc, argv, "neighbor");
    char base[XPATH_MAXLEN], xpath[XPATH_MAXLEN];
    if (!eigrp_cli_neighbor_policy_xpath(vty, address, base, sizeof(base)))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, base, NB_OP_CREATE, NULL);
    if (!eigrp_cli_xpath_leaf_build(xpath, sizeof(xpath), base, "maximum-prefix"))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix", xpath, false);
}

DEFUN(no_eigrp_neighbor_maximum_prefix,
      no_eigrp_neighbor_maximum_prefix_cmd,
      "no neighbor <A.B.C.D|X:X::X:X> maximum-prefix [(1-4294967295)]",
      NO_STR "Specify a neighbor router\n" "Neighbor address\n" "Neighbor address\n"
      "Limit prefixes accepted from this neighbor\n" "Maximum prefixes\n")
{
    const char *address = eigrp_cli_token_after(argc, argv, "neighbor");
    char base[XPATH_MAXLEN], xpath[XPATH_MAXLEN];
    if (!eigrp_cli_neighbor_policy_xpath(vty, address, base, sizeof(base)))
        return CMD_WARNING;
    if (!eigrp_cli_xpath_leaf_build(xpath, sizeof(xpath), base, "maximum-prefix"))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix", xpath, true);
}

DEFUN(eigrp_neighbor_maximum_prefix_all,
      eigrp_neighbor_maximum_prefix_all_cmd,
      "neighbor maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]",
      "Specify neighbor behavior\n" "Limit prefixes accepted from all neighbors\n"
      "Maximum prefixes\n" "Warning threshold\n" "Dampening\n" "Reset time\n"
      "Minutes\n" "Restart delay\n" "Minutes\n" "Restart count\n" "Count\n"
      "Warning only\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./neighbor-maximum-prefix", false);
}

DEFUN(no_eigrp_neighbor_maximum_prefix_all,
      no_eigrp_neighbor_maximum_prefix_all_cmd,
      "no neighbor maximum-prefix [(1-4294967295)]",
      NO_STR "Specify neighbor behavior\n" "Limit prefixes accepted from all neighbors\n"
      "Maximum prefixes\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./neighbor-maximum-prefix", true);
}

DEFUN(eigrp_log_neighbor_changes,
      eigrp_log_neighbor_changes_cmd,
      "eigrp log-neighbor-changes",
      EIGRP_STR "Log EIGRP neighbor adjacency changes\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./log-neighbor-changes", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_log_neighbor_changes,
      no_eigrp_log_neighbor_changes_cmd,
      "no eigrp log-neighbor-changes",
      NO_STR EIGRP_STR "Log EIGRP neighbor adjacency changes\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./log-neighbor-changes", NB_OP_MODIFY, "false");
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_log_neighbor_warnings,
      eigrp_log_neighbor_warnings_cmd,
      "eigrp log-neighbor-warnings [(1-65535)]",
      EIGRP_STR "Log EIGRP neighbor warnings\n" "Repeat interval in seconds\n")
{
    const char *seconds = eigrp_cli_token_last(argc, argv);
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    if (seconds && strcmp(seconds, "log-neighbor-warnings") == 0) {
        nb_cli_enqueue_change(vty, "./log-neighbor-warnings", NB_OP_DESTROY, NULL);
        return nb_cli_apply_changes(vty, NULL);
    }
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings", NB_OP_CREATE, NULL);
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings/enabled", NB_OP_MODIFY, "true");
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings/interval", NB_OP_MODIFY, seconds);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_log_neighbor_warnings,
      no_eigrp_log_neighbor_warnings_cmd,
      "no eigrp log-neighbor-warnings [(1-65535)]",
      NO_STR EIGRP_STR "Log EIGRP neighbor warnings\n" "Repeat interval in seconds\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings", NB_OP_CREATE, NULL);
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings/enabled", NB_OP_MODIFY, "false");
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings/interval", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface,
      eigrp_af_interface_cmd,
      "af-interface <default|IFNAME>",
      "Enter address-family interface configuration mode\n"
      "Default interface template\n"
      "Interface name\n")
{
	const char *interface_name = eigrp_cli_token_last(argc, argv);
	char name[128];
	char afi[8];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];
	int rv;

	if (!eigrp_cli_named_af_context(vty, name, sizeof(name), afi,
					 sizeof(afi), vrf_name, sizeof(vrf_name),
					 asn, sizeof(asn))) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return CMD_WARNING;
	}

	eigrp_cli_named_af_interface_xpath(xpath, sizeof(xpath), name, afi,
					   vrf_name, asn, interface_name);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);
	if (rv == CMD_SUCCESS)
		VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return rv;
}

DEFUN(no_eigrp_af_interface,
      no_eigrp_af_interface_cmd,
      "no af-interface <default|IFNAME>",
      NO_STR
      "Enter address-family interface configuration mode\n"
      "Default interface template\n"
      "Interface name\n")
{
	const char *interface_name = eigrp_cli_token_last(argc, argv);
	char name[128];
	char afi[8];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_named_af_context(vty, name, sizeof(name), afi,
					 sizeof(afi), vrf_name, sizeof(vrf_name),
					 asn, sizeof(asn))) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return CMD_WARNING;
	}

	eigrp_cli_named_af_interface_xpath(xpath, sizeof(xpath), name, afi,
					   vrf_name, asn, interface_name);
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes_clear_pending(vty, NULL);
}

DEFUN(eigrp_exit_af_interface,
      eigrp_exit_af_interface_cmd,
      "exit-af-interface",
      "Exit address-family interface configuration mode\n")
{
	if (vty->xpath_index > 1
	    && strstr(VTY_CURR_XPATH, "/af-interface[") != NULL)
		vty->xpath_index--;
	vty->node = EIGRP_NODE;
	return CMD_SUCCESS;
}

DEFUN(eigrp_af_interface_bandwidth_percent,
      eigrp_af_interface_bandwidth_percent_cmd,
      "bandwidth-percent (1-999999)",
      "Set EIGRP bandwidth percentage\n"
      "Percentage of interface bandwidth\n")
{
	const char *percent = eigrp_cli_token_last(argc, argv);
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./bandwidth-percent", NB_OP_MODIFY, percent);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_bandwidth_percent,
      no_eigrp_af_interface_bandwidth_percent_cmd,
      "no bandwidth-percent [(1-999999)]",
      NO_STR
      "Set EIGRP bandwidth percentage\n"
      "Percentage of interface bandwidth\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./bandwidth-percent", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_hello_interval,
      eigrp_af_interface_hello_interval_cmd,
      "hello-interval (1-65535)",
      "Configures EIGRP hello interval\n"
      "Seconds between hello transmissions\n")
{
	const char *hello = eigrp_cli_token_last(argc, argv);
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./hello-interval", NB_OP_MODIFY, hello);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_hello_interval,
      no_eigrp_af_interface_hello_interval_cmd,
      "no hello-interval [(1-65535)]",
      NO_STR
      "Configures EIGRP hello interval\n"
      "Seconds between hello transmissions\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./hello-interval", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_hold_time,
      eigrp_af_interface_hold_time_cmd,
      "hold-time (1-65535)",
      "Configures EIGRP hold time\n"
      "Seconds before neighbor is considered down\n")
{
	const char *hold = eigrp_cli_token_last(argc, argv);
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./hold-time", NB_OP_MODIFY, hold);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_hold_time,
      no_eigrp_af_interface_hold_time_cmd,
      "no hold-time [(1-65535)]",
      NO_STR
      "Configures EIGRP hold time\n"
      "Seconds before neighbor is considered down\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./hold-time", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_authentication_mode,
      eigrp_af_interface_authentication_mode_cmd,
      "authentication mode <md5|hmac-sha-256> [<0|7> WORD]",
      "Authentication subcommands\n"
      "Authentication mode\n"
      "Keyed message digest\n"
      "HMAC SHA256 algorithm\n"
      "Unencrypted password\n"
      "Cisco type-7 encoded password\n"
      "Authentication password\n")
{
	const char *mode = NULL;
	const char *encryption = NULL;
	const char *password = NULL;
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name, sizeof(interface_name)))
		return CMD_WARNING;
	if (eigrp_cli_token_present(argc, argv, "md5"))
		mode = "md5";
	else if (eigrp_cli_token_present(argc, argv, "hmac-sha-256")) {
		mode = "hmac-sha-256";
		encryption = eigrp_cli_token_after(argc, argv, "hmac-sha-256");
		if (!encryption || (strcmp(encryption, "0") != 0
				   && strcmp(encryption, "7") != 0))
			return CMD_WARNING;
		password = eigrp_cli_token_after(argc, argv, encryption);
		if (!password || !password[0])
			return CMD_WARNING;
	}
	if (!mode)
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./authentication-mode", NB_OP_MODIFY, mode);
	if (strcmp(mode, "hmac-sha-256") == 0) {
		nb_cli_enqueue_change(vty, "./authentication-encryption-type",
				      NB_OP_MODIFY, encryption);
		nb_cli_enqueue_change(vty, "./authentication-password", NB_OP_MODIFY,
				      password);
	} else {
		nb_cli_enqueue_change(vty, "./authentication-encryption-type",
				      NB_OP_DESTROY, NULL);
		nb_cli_enqueue_change(vty, "./authentication-password", NB_OP_DESTROY,
				      NULL);
	}
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
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./authentication-mode", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./authentication-encryption-type", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./authentication-password", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_keychain,
      eigrp_af_interface_keychain_cmd,
      "authentication key-chain WORD",
      "Authentication subcommands\n"
      "Key-chain\n"
      "Name of key-chain\n")
{
	const char *keychain = eigrp_cli_token_last(argc, argv);
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./authentication-key-chain", NB_OP_MODIFY,
			      keychain);
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
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./authentication-key-chain", NB_OP_DESTROY,
			      NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_passive,
      eigrp_af_interface_passive_cmd,
      "passive-interface",
      "Suppress routing updates on this interface\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./passive-interface", NB_OP_CREATE, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_passive,
      no_eigrp_af_interface_passive_cmd,
      "no passive-interface",
      NO_STR
      "Suppress routing updates on this interface\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./passive-interface", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_next_hop_self,
      eigrp_af_interface_next_hop_self_cmd,
      "next-hop-self",
      "Advertise the local outbound interface as next hop\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	/* true is the documented default, so remove the explicit false leaf. */
	nb_cli_enqueue_change(vty, "./next-hop-self", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_next_hop_self,
      no_eigrp_af_interface_next_hop_self_cmd,
      "no next-hop-self",
      NO_STR
      "Advertise the local outbound interface as next hop\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./next-hop-self", NB_OP_MODIFY, "false");
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_af_interface_split_horizon,
      eigrp_af_interface_split_horizon_cmd,
      "split-horizon",
      "Perform split horizon\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	/* true is the documented default, so remove the explicit false leaf. */
	nb_cli_enqueue_change(vty, "./split-horizon", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_af_interface_split_horizon,
      no_eigrp_af_interface_split_horizon_cmd,
      "no split-horizon",
      NO_STR
      "Perform split horizon\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./split-horizon", NB_OP_MODIFY, "false");
	return nb_cli_apply_changes(vty, NULL);
}

static int eigrp_cli_af_interface_summary_set(
	struct vty *vty, const char *address, const char *mask,
	const char *distance, const char *leak_map, bool remove)
{
	char interface_name[IFNAMSIZ];
	char afi[8];
	char xpath[XPATH_MAXLEN];
	char child[XPATH_MAXLEN];

	if (!eigrp_cli_af_interface_path(vty, interface_name, sizeof(interface_name)))
		return CMD_WARNING;
	if (!eigrp_cli_current_afi(vty, afi, sizeof(afi))
	    || strcmp(afi, "ipv4") != 0) {
		vty_out(vty, "%% summary-address is valid only under named IPv4 address-family\n");
		return CMD_WARNING;
	}
	snprintf(xpath, sizeof(xpath),
		 "./summary-address[address='%s'][mask='%s']", address, mask);
	if (remove) {
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	if (!eigrp_cli_xpath_leaf_build(child, sizeof(child), xpath,
				       "administrative-distance"))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, child, distance ? NB_OP_MODIFY : NB_OP_DESTROY,
			distance);
	if (!eigrp_cli_xpath_leaf_build(child, sizeof(child), xpath, "leak-map"))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, child, leak_map ? NB_OP_MODIFY : NB_OP_DESTROY,
			leak_map);
	return nb_cli_apply_changes(vty, NULL);
}

static bool eigrp_cli_ipv4_pair(int argc, struct cmd_token *argv[],
				const char **address, const char **mask)
{
	int i;

	*address = NULL;
	*mask = NULL;
	for (i = 0; i < argc; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);
		struct in_addr parsed;
		if (!value || inet_aton(value, &parsed) == 0)
			continue;
		if (!*address)
			*address = value;
		else if (!*mask)
			*mask = value;
	}
	return *address && *mask;
}

DEFUN(eigrp_af_interface_summary_address,
      eigrp_af_interface_summary_address_cmd,
      "summary-address A.B.C.D A.B.C.D [(1-255)] [leak-map WORD]",
      "Perform address summarization\n"
      "Summary IPv4 address\n"
      "Summary subnet mask\n"
      "Administrative distance\n"
      "Leak selected component routes\n"
      "Route-map name\n")
{
	const char *address, *mask;
	const char *distance = NULL;
	const char *leak_map = eigrp_cli_token_after(argc, argv, "leak-map");
	int i;

	if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask))
		return CMD_WARNING;
	for (i = 0; i < argc; i++) {
		const char *value = eigrp_cli_token_value(argv[i]);
		char *end = NULL;
		unsigned long n;
		if (!value || value == address || value == mask)
			continue;
		n = strtoul(value, &end, 10);
		if (end && *end == '\0' && n >= 1 && n <= 255) {
			distance = value;
			break;
		}
	}
	return eigrp_cli_af_interface_summary_set(vty, address, mask, distance,
					   leak_map, false);
}

DEFUN(no_eigrp_af_interface_summary_address,
      no_eigrp_af_interface_summary_address_cmd,
      "no summary-address A.B.C.D A.B.C.D [(1-255)] [leak-map WORD]",
      NO_STR
      "Perform address summarization\n"
      "Summary IPv4 address\n"
      "Summary subnet mask\n"
      "Administrative distance\n"
      "Leak selected component routes\n"
      "Route-map name\n")
{
	const char *address, *mask;

	if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask))
		return CMD_WARNING;
	return eigrp_cli_af_interface_summary_set(vty, address, mask, NULL, NULL, true);
}

DEFUN(eigrp_topology_base,
      eigrp_topology_base_cmd,
      "topology base",
      "Configure EIGRP topology\n"
      "Base topology\n")
{
	char name[128];
	char afi[8];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];
	char xpath[XPATH_MAXLEN];
	int rv;

	if (!eigrp_cli_named_af_context(vty, name, sizeof(name), afi,
					 sizeof(afi), vrf_name, sizeof(vrf_name),
					 asn, sizeof(asn))
	    || strstr(VTY_CURR_XPATH, "/af-interface[") != NULL) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return CMD_WARNING;
	}

	eigrp_cli_named_topology_xpath(xpath, sizeof(xpath), name, afi, vrf_name,
				       asn);
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	rv = nb_cli_apply_changes(vty, NULL);
	if (rv == CMD_SUCCESS)
		VTY_PUSH_XPATH(EIGRP_NODE, xpath);
	return rv;
}

DEFUN(eigrp_exit_af_topology,
      eigrp_exit_af_topology_cmd,
      "exit-af-topology",
      "Exit address-family topology mode\n")
{
	if (vty->xpath_index > 1
	    && strstr(VTY_CURR_XPATH, "/topology") != NULL)
		vty->xpath_index--;
	vty->node = EIGRP_NODE;
	return CMD_SUCCESS;
}

DEFUN(eigrp_auto_summary,
      eigrp_auto_summary_cmd,
      "auto-summary",
      "Enable automatic network summarization\n")
{
	char afi[8];

	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	if (!eigrp_cli_current_afi(vty, afi, sizeof(afi))
	    || strcmp(afi, "ipv4") != 0) {
		vty_out(vty, "%% auto-summary is valid only under named IPv4 topology\n");
		return CMD_WARNING;
	}
	nb_cli_enqueue_change(vty, "./auto-summary", NB_OP_CREATE, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_auto_summary,
      no_eigrp_auto_summary_cmd,
      "no auto-summary",
      NO_STR
      "Enable automatic network summarization\n")
{
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./auto-summary", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

static int eigrp_cli_default_information_set(struct vty *vty,
					     const char *direction,
					     const char *access_list,
					     bool remove)
{
	char xpath[96];
	char child[128];

	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	if (!direction
	    || (strcmp(direction, "in") != 0 && strcmp(direction, "out") != 0))
		return CMD_WARNING;
	snprintf(xpath, sizeof(xpath), "./default-information-%s", direction);
	if (remove) {
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}
	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	snprintf(child, sizeof(child), "%s/access-list", xpath);
	nb_cli_enqueue_change(vty, child,
			      access_list ? NB_OP_MODIFY : NB_OP_DESTROY,
			      access_list);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_default_information,
      eigrp_default_information_cmd,
      "default-information <in|out> [WORD]",
      "Control exterior or default routing information\n"
      "Accept exterior or default routing information\n"
      "Advertise exterior or default routing information\n"
      "Access-list name\n")
{
	const char *direction = eigrp_cli_token_present(argc, argv, "in") ? "in" : "out";
	const char *last = eigrp_cli_token_last(argc, argv);
	const char *acl = last && strcmp(last, direction) != 0 ? last : NULL;

	return eigrp_cli_default_information_set(vty, direction, acl, false);
}

DEFUN(no_eigrp_default_information,
      no_eigrp_default_information_cmd,
      "no default-information <in|out> [WORD]",
      NO_STR
      "Control exterior or default routing information\n"
      "Accept exterior or default routing information\n"
      "Advertise exterior or default routing information\n"
      "Access-list name\n")
{
	const char *direction = eigrp_cli_token_present(argc, argv, "in") ? "in" : "out";

	return eigrp_cli_default_information_set(vty, direction, NULL, true);
}

DEFUN(eigrp_default_metric,
      eigrp_default_metric_cmd,
      "default-metric (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)",
      "Set metric for redistributed routes\n"
      "Bandwidth metric in Kbits per second\n"
      "EIGRP delay metric\n"
      "EIGRP reliability metric\n"
      "EIGRP load metric\n"
      "EIGRP MTU\n")
{
	const char *args[5] = {0};

	if (!eigrp_cli_named_topology_required(vty)
	    || eigrp_cli_token_values_after(argc, argv, "default-metric", args, 5) != 5)
		return CMD_WARNING;

	nb_cli_enqueue_change(vty, "./default-metric", NB_OP_CREATE, NULL);
	nb_cli_enqueue_change(vty, "./default-metric/bandwidth", NB_OP_MODIFY,
			      args[0]);
	nb_cli_enqueue_change(vty, "./default-metric/delay", NB_OP_MODIFY,
			      args[1]);
	nb_cli_enqueue_change(vty, "./default-metric/reliability", NB_OP_MODIFY,
			      args[2]);
	nb_cli_enqueue_change(vty, "./default-metric/load", NB_OP_MODIFY,
			      args[3]);
	nb_cli_enqueue_change(vty, "./default-metric/mtu", NB_OP_MODIFY,
			      args[4]);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_default_metric,
      no_eigrp_default_metric_cmd,
      "no default-metric [(1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)]",
      NO_STR
      "Set metric for redistributed routes\n"
      "Bandwidth metric in Kbits per second\n"
      "EIGRP delay metric\n"
      "EIGRP reliability metric\n"
      "EIGRP load metric\n"
      "EIGRP MTU\n")
{
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./default-metric", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_distance,
      eigrp_distance_cmd,
      "distance eigrp (1-255) (1-255)",
      "Define an administrative distance\n"
      EIGRP_STR
      "Internal route distance\n"
      "External route distance\n")
{
	const char *args[2] = {0};

	if (!eigrp_cli_named_topology_required(vty)
	    || eigrp_cli_token_values_after(argc, argv, "eigrp", args, 2) != 2)
		return CMD_WARNING;

	nb_cli_enqueue_change(vty, "./distance", NB_OP_CREATE, NULL);
	nb_cli_enqueue_change(vty, "./distance/internal", NB_OP_MODIFY, args[0]);
	nb_cli_enqueue_change(vty, "./distance/external", NB_OP_MODIFY, args[1]);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_distance,
      no_eigrp_distance_cmd,
      "no distance eigrp [(1-255) (1-255)]",
      NO_STR
      "Define an administrative distance\n"
      EIGRP_STR
      "Internal route distance\n"
      "External route distance\n")
{
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./distance", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

static int eigrp_cli_prefix_limit_set(struct vty *vty, int argc,
                                      struct cmd_token *argv[],
                                      const char *keyword,
                                      const char *xpath, bool remove)
{
    const char *values[16] = {0};
    const char *maximum = NULL;
    const char *threshold = NULL;
    char child[XPATH_MAXLEN];
    int count, i;

    if (remove) {
        nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
        return nb_cli_apply_changes(vty, NULL);
    }
    count = eigrp_cli_token_values_after(argc, argv, keyword, values, 16);
    if (count <= 0)
        return CMD_WARNING;
    for (i = 0; i < count; i++) {
        char *end = NULL;
        unsigned long n;
        if (!values[i])
            continue;
        n = strtoul(values[i], &end, 10);
        if (end == values[i] || *end != '\0')
            continue;
        if (!maximum) {
            maximum = values[i];
            continue;
        }
        if (!threshold && n <= 100
            && (i == 1 || (i > 0 && strcmp(values[i - 1], "maximum-prefix") == 0)))
            threshold = values[i];
    }
    if (!maximum)
        return CMD_WARNING;

    nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
#define EIGRP_PREFIX_LIMIT_MODIFY(leaf, value) do { \
        snprintf(child, sizeof(child), "%s/%s", xpath, leaf); \
        nb_cli_enqueue_change(vty, child, NB_OP_MODIFY, value); \
    } while (0)
#define EIGRP_PREFIX_LIMIT_EMPTY(leaf) do { \
        snprintf(child, sizeof(child), "%s/%s", xpath, leaf); \
        nb_cli_enqueue_change(vty, child, NB_OP_CREATE, NULL); \
    } while (0)
    EIGRP_PREFIX_LIMIT_MODIFY("maximum", maximum);
#define EIGRP_PREFIX_LIMIT_DESTROY(leaf) do { \
        snprintf(child, sizeof(child), "%s/%s", xpath, leaf); \
        nb_cli_enqueue_change(vty, child, NB_OP_DESTROY, NULL); \
    } while (0)
    if (threshold)
        EIGRP_PREFIX_LIMIT_MODIFY("threshold", threshold);
    else
        EIGRP_PREFIX_LIMIT_DESTROY("threshold");
    if (eigrp_cli_token_present(argc, argv, "warning-only"))
        EIGRP_PREFIX_LIMIT_EMPTY("warning-only");
    else
        EIGRP_PREFIX_LIMIT_DESTROY("warning-only");
    if (eigrp_cli_token_present(argc, argv, "dampened"))
        EIGRP_PREFIX_LIMIT_EMPTY("dampened");
    else
        EIGRP_PREFIX_LIMIT_DESTROY("dampened");
    if (eigrp_cli_token_after(argc, argv, "reset-time"))
        EIGRP_PREFIX_LIMIT_MODIFY("reset-time",
                                  eigrp_cli_token_after(argc, argv, "reset-time"));
    else
        EIGRP_PREFIX_LIMIT_DESTROY("reset-time");
    if (eigrp_cli_token_after(argc, argv, "restart"))
        EIGRP_PREFIX_LIMIT_MODIFY("restart",
                                  eigrp_cli_token_after(argc, argv, "restart"));
    else
        EIGRP_PREFIX_LIMIT_DESTROY("restart");
    if (eigrp_cli_token_after(argc, argv, "restart-count"))
        EIGRP_PREFIX_LIMIT_MODIFY("restart-count",
                                  eigrp_cli_token_after(argc, argv, "restart-count"));
    else
        EIGRP_PREFIX_LIMIT_DESTROY("restart-count");
#undef EIGRP_PREFIX_LIMIT_DESTROY
#undef EIGRP_PREFIX_LIMIT_MODIFY
#undef EIGRP_PREFIX_LIMIT_EMPTY
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_maximum_prefix,
      eigrp_maximum_prefix_cmd,
      "maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]",
      "Limit prefixes accepted under an EIGRP address family\n"
      "Maximum number of prefixes\n" "Warning threshold percentage\n"
      "Apply restart-time dampening\n" "Reset restart count\n" "Minutes\n"
      "Restart delay\n" "Minutes\n" "Restart count\n" "Count\n"
      "Warn only when limit is exceeded\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./maximum-prefix", false);
}

DEFUN(no_eigrp_maximum_prefix,
      no_eigrp_maximum_prefix_cmd,
      "no maximum-prefix [(1-4294967295)]",
      NO_STR "Limit prefixes accepted under an EIGRP address family\n"
      "Maximum number of prefixes\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./maximum-prefix", true);
}

DEFUN(eigrp_metric_maximum_hops,
      eigrp_metric_maximum_hops_cmd,
      "metric maximum-hops (1-255)",
      "Modify EIGRP metric behavior\n" "Maximum hop count\n" "Hop count\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./metric-maximum-hops", NB_OP_MODIFY,
                          eigrp_cli_token_last(argc, argv));
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_metric_maximum_hops,
      no_eigrp_metric_maximum_hops_cmd,
      "no metric maximum-hops [(1-255)]",
      NO_STR "Modify EIGRP metric behavior\n" "Maximum hop count\n" "Hop count\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./metric-maximum-hops", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_metric_holddown,
      eigrp_metric_holddown_cmd,
      "metric holddown",
      "Modify EIGRP metric behavior\n" "Enable metric holddown behavior\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./metric-holddown", NB_OP_CREATE, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_metric_holddown,
      no_eigrp_metric_holddown_cmd,
      "no metric holddown",
      NO_STR "Modify EIGRP metric behavior\n" "Enable metric holddown behavior\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./metric-holddown", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_event_log_size,
      eigrp_event_log_size_cmd,
      "eigrp event-log-size (0-4294967295)",
      EIGRP_STR "Set EIGRP event log size\n" "Number of events\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./event-log-size", NB_OP_MODIFY,
                          eigrp_cli_token_last(argc, argv));
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_event_log_size,
      no_eigrp_event_log_size_cmd,
      "no eigrp event-log-size [(0-4294967295)]",
      NO_STR EIGRP_STR "Set EIGRP event log size\n" "Number of events\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./event-log-size", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

static int eigrp_cli_offset_list_change(struct vty *vty, int argc,
					struct cmd_token *argv[], bool remove)
{
	const char *args[4] = {0};
	const char *interface_name;
	char xpath[XPATH_MAXLEN];
	int count;

	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	count = eigrp_cli_token_values_after(argc, argv, "offset-list", args, 4);
	if (count < 3)
		return CMD_WARNING;
	interface_name = count > 3 ? args[3] : "";
	snprintf(xpath, sizeof(xpath),
		 "./offset-list[access-list='%s'][direction='%s'][interface='%s']",
		 args[0], args[1], interface_name);
	if (remove) {
		nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
		return nb_cli_apply_changes(vty, NULL);
	}

	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
	snprintf(xpath, sizeof(xpath),
		 "./offset-list[access-list='%s'][direction='%s'][interface='%s']/offset",
		 args[0], args[1], interface_name);
	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, args[2]);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_offset_list,
      eigrp_offset_list_cmd,
      "offset-list WORD <in|out> (0-2147483647) [IFNAME]",
      "Add or subtract offset from EIGRP metrics\n"
      "Access-list name\n"
      "Incoming updates\n"
      "Outgoing updates\n"
      "Metric offset\n"
      "Interface name\n")
{
	return eigrp_cli_offset_list_change(vty, argc, argv, false);
}

DEFUN(no_eigrp_offset_list,
      no_eigrp_offset_list_cmd,
      "no offset-list WORD <in|out> (0-2147483647) [IFNAME]",
      NO_STR
      "Add or subtract offset from EIGRP metrics\n"
      "Access-list name\n"
      "Incoming updates\n"
      "Outgoing updates\n"
      "Metric offset\n"
      "Interface name\n")
{
	return eigrp_cli_offset_list_change(vty, argc, argv, true);
}

DEFUN(eigrp_redistribute_maximum_prefix,
      eigrp_redistribute_maximum_prefix_cmd,
      "redistribute maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]",
      REDIST_STR "Limit redistributed prefixes\n" "Maximum prefixes\n" "Warning threshold\n"
      "Dampening\n" "Reset time\n" "Minutes\n" "Restart delay\n" "Minutes\n"
      "Restart count\n" "Count\n" "Warning only\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./redistribute-maximum-prefix", false);
}

DEFUN(no_eigrp_redistribute_maximum_prefix,
      no_eigrp_redistribute_maximum_prefix_cmd,
      "no redistribute maximum-prefix [(1-4294967295)]",
      NO_STR REDIST_STR "Limit redistributed prefixes\n" "Maximum prefixes\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./redistribute-maximum-prefix", true);
}

static int eigrp_cli_summary_metric_set(struct vty *vty,
                                        const char *address, const char *mask,
                                        const char **metric,
                                        const char *distance, bool remove)
{
    char afi[8];
    char xpath[XPATH_MAXLEN];
    char child[XPATH_MAXLEN];
    static const char *leaves[] = {"bandwidth", "delay", "reliability", "load", "mtu"};
    int i;

    if (!eigrp_cli_named_topology_required(vty)
        || !eigrp_cli_current_afi(vty, afi, sizeof(afi))
        || strcmp(afi, "ipv4") != 0) {
        vty_out(vty, "%% summary-metric is valid only under named IPv4 topology\n");
        return CMD_WARNING;
    }
    snprintf(xpath, sizeof(xpath), "./summary-metric[address='%s'][mask='%s']",
             address, mask);
    if (remove) {
        nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
        return nb_cli_apply_changes(vty, NULL);
    }
    nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
    for (i = 0; i < 5; i++) {
        if (!eigrp_cli_xpath_leaf_build(child, sizeof(child), xpath, leaves[i]))
            return CMD_WARNING;
        nb_cli_enqueue_change(vty, child, metric ? NB_OP_MODIFY : NB_OP_DESTROY,
                              metric ? metric[i] : NULL);
    }
    if (!eigrp_cli_xpath_leaf_build(child, sizeof(child), xpath, "distance"))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, child, distance ? NB_OP_MODIFY : NB_OP_DESTROY,
                          distance);
    return nb_cli_apply_changes(vty, NULL);
}

DEFUN(eigrp_summary_metric,
      eigrp_summary_metric_cmd,
      "summary-metric A.B.C.D A.B.C.D (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535) [distance (1-255)]",
      "Configure summary metric\n" "Summary IPv4 address\n" "Summary subnet mask\n"
      "Bandwidth metric\n" "Delay metric\n" "Reliability metric\n" "Load metric\n"
      "MTU metric\n" "Administrative distance\n" "Distance\n")
{
    const char *address, *mask;
    const char *args[7] = {0};
    const char *metric[5];
    const char *distance = eigrp_cli_token_after(argc, argv, "distance");
    int count, i;

    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask))
        return CMD_WARNING;
    count = eigrp_cli_token_values_after(argc, argv, "summary-metric", args, 7);
    if (count < 7)
        return CMD_WARNING;
    for (i = 0; i < 5; i++)
        metric[i] = args[i + 2];
    return eigrp_cli_summary_metric_set(vty, address, mask, metric, distance, false);
}

DEFUN(eigrp_summary_metric_distance,
      eigrp_summary_metric_distance_cmd,
      "summary-metric A.B.C.D A.B.C.D distance (1-255)",
      "Configure summary metric\n" "Summary IPv4 address\n" "Summary subnet mask\n"
      "Administrative distance\n" "Distance\n")
{
    const char *address, *mask;
    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask))
        return CMD_WARNING;
    return eigrp_cli_summary_metric_set(vty, address, mask, NULL,
                                        eigrp_cli_token_after(argc, argv, "distance"), false);
}

DEFUN(no_eigrp_summary_metric,
      no_eigrp_summary_metric_cmd,
      "no summary-metric A.B.C.D A.B.C.D",
      NO_STR "Configure summary metric\n" "Summary IPv4 address\n"
      "Summary subnet mask\n")
{
    const char *address, *mask;
    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask))
        return CMD_WARNING;
    return eigrp_cli_summary_metric_set(vty, address, mask, NULL, NULL, true);
}

DEFUN(eigrp_traffic_share_balanced,
      eigrp_traffic_share_balanced_cmd,
      "traffic-share balanced",
      "Control traffic sharing among EIGRP routes\n"
      "Balance traffic in proportion to metrics\n")
{
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	/* Balanced is the protocol default; remove an explicit disabled value. */
	nb_cli_enqueue_change(vty, "./traffic-share-balanced", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN(no_eigrp_traffic_share_balanced,
      no_eigrp_traffic_share_balanced_cmd,
      "no traffic-share balanced",
      NO_STR
      "Control traffic sharing among EIGRP routes\n"
      "Balance traffic in proportion to metrics\n")
{
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./traffic-share-balanced", NB_OP_MODIFY,
			      "false");
	return nb_cli_apply_changes(vty, NULL);
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
	/* Both EIGRP configuration entry forms are valid.  The FRR vtysh
	 * integration patch owns the local EIGRP_NODE transition for both the
	 * numeric and named entry forms.  Keep the named daemon command NOSH so
	 * vtysh does not learn a second overlapping `router eigrp WORD` grammar.
	 * eigrpd still installs the command locally so direct daemon CLI users
	 * can enter named mode.
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
	install_element(EIGRP_NODE, &no_eigrp_af_interface_cmd);
	install_element(EIGRP_NODE, &eigrp_topology_base_cmd);
	install_element(EIGRP_NODE, &eigrp_exit_af_topology_cmd);
	install_element(EIGRP_NODE, &eigrp_auto_summary_cmd);
	install_element(EIGRP_NODE, &no_eigrp_auto_summary_cmd);
	install_element(EIGRP_NODE, &eigrp_default_information_cmd);
	install_element(EIGRP_NODE, &no_eigrp_default_information_cmd);
	install_element(EIGRP_NODE, &eigrp_default_metric_cmd);
	install_element(EIGRP_NODE, &no_eigrp_default_metric_cmd);
	install_element(EIGRP_NODE, &eigrp_maximum_prefix_cmd);
	install_element(EIGRP_NODE, &no_eigrp_maximum_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_metric_maximum_hops_cmd);
	install_element(EIGRP_NODE, &no_eigrp_metric_maximum_hops_cmd);
	install_element(EIGRP_NODE, &eigrp_metric_holddown_cmd);
	install_element(EIGRP_NODE, &no_eigrp_metric_holddown_cmd);
	install_element(EIGRP_NODE, &eigrp_event_log_size_cmd);
	install_element(EIGRP_NODE, &no_eigrp_event_log_size_cmd);
	install_element(EIGRP_NODE, &eigrp_traffic_share_balanced_cmd);
	install_element(EIGRP_NODE, &no_eigrp_traffic_share_balanced_cmd);

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
	install_element(EIGRP_NODE, &eigrp_named_neighbor_ipv4_cmd);
	install_element(EIGRP_NODE, &no_eigrp_named_neighbor_ipv4_cmd);
	install_element(EIGRP_NODE, &eigrp_named_neighbor_ipv6_cmd);
	install_element(EIGRP_NODE, &no_eigrp_named_neighbor_ipv6_cmd);
	install_element(EIGRP_NODE, &eigrp_neighbor_description_cmd);
	install_element(EIGRP_NODE, &no_eigrp_neighbor_description_cmd);
	install_element(EIGRP_NODE, &eigrp_neighbor_maximum_prefix_cmd);
	install_element(EIGRP_NODE, &no_eigrp_neighbor_maximum_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_neighbor_maximum_prefix_all_cmd);
	install_element(EIGRP_NODE, &no_eigrp_neighbor_maximum_prefix_all_cmd);
	install_element(EIGRP_NODE, &eigrp_log_neighbor_changes_cmd);
	install_element(EIGRP_NODE, &no_eigrp_log_neighbor_changes_cmd);
	install_element(EIGRP_NODE, &eigrp_log_neighbor_warnings_cmd);
	install_element(EIGRP_NODE, &no_eigrp_log_neighbor_warnings_cmd);
	install_element(EIGRP_NODE, &eigrp_distribute_list_cmd);
	install_element(EIGRP_NODE, &eigrp_distribute_list_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_no_distribute_list_cmd);
	install_element(EIGRP_NODE, &eigrp_no_distribute_list_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_redistribute_source_metric_cmd);
	install_element(EIGRP_NODE, &eigrp_redistribute_maximum_prefix_cmd);
	install_element(EIGRP_NODE, &no_eigrp_redistribute_maximum_prefix_cmd);
	install_element(EIGRP_NODE, &eigrp_distance_cmd);
	install_element(EIGRP_NODE, &no_eigrp_distance_cmd);
	install_element(EIGRP_NODE, &eigrp_offset_list_cmd);
	install_element(EIGRP_NODE, &no_eigrp_offset_list_cmd);
	install_element(EIGRP_NODE, &eigrp_summary_metric_cmd);
	install_element(EIGRP_NODE, &eigrp_summary_metric_distance_cmd);
	install_element(EIGRP_NODE, &no_eigrp_summary_metric_cmd);

	vrf_cmd_init(NULL);

	if_cmd_init_default();

	install_element(EIGRP_NODE, &eigrp_exit_af_interface_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_hello_interval_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_hello_interval_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_hold_time_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_hold_time_cmd);
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
	install_element(EIGRP_NODE, &eigrp_af_interface_next_hop_self_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_next_hop_self_cmd);
	install_element(EIGRP_NODE, &eigrp_af_interface_split_horizon_cmd);
	install_element(EIGRP_NODE, &no_eigrp_af_interface_split_horizon_cmd);
}
