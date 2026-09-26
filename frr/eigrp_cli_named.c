// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP daemon named-mode CLI implementation.
 *
 * Copyright (C) 2019 Network Device Education Foundation, Inc. ("NetDEF")
 *                    Rafael Zalamena
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <zebra.h>

#include "memory.h"
#include "lib/command.h"
#include "lib/if.h"
#include "lib/log.h"
#include "lib/northbound_cli.h"
#include "printfrr.h"
#include "zclient.h"
#include "keychain.h"
#include "linklist.h"
#include "distribute.h"

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_cli_named.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_eventlog.h"
#include "eigrpd/eigrp_statistics.h"
#include "eigrpd/eigrp_status.h"
#include "eigrpd/eigrp_timer.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_const.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_northbound.h"

#ifndef EIGRP_STANDALONE_BUILD
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "eigrpd/eigrp_cli_named_clippy.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

static bool eigrp_cli_summary_prefix_display(const char *prefix, char *address,
					      size_t address_len, char *mask,
					      size_t mask_len, bool *ipv6)
{
	char buffer[INET6_ADDRSTRLEN + 4];
	char *slash;
	char *end = NULL;
	unsigned long plen;
	struct in_addr in4;
	struct in_addr mask4;
	uint32_t host_mask;

	if (!prefix || !address || !mask || !ipv6 || strlen(prefix) >= sizeof(buffer))
		return false;
	strlcpy(buffer, prefix, sizeof(buffer));
	slash = strchr(buffer, '/');
	if (!slash)
		return false;
	*slash++ = '\0';
	plen = strtoul(slash, &end, 10);
	if (!end || *end != '\0')
		return false;

	*ipv6 = strchr(buffer, ':') != NULL;
	if (*ipv6) {
		struct in6_addr in6;
		if (plen > 128 || inet_pton(AF_INET6, buffer, &in6) != 1)
			return false;
		strlcpy(address, prefix, address_len);
		mask[0] = '\0';
		return true;
	}

	if (plen > 32 || inet_pton(AF_INET, buffer, &in4) != 1)
		return false;
	if (!inet_ntop(AF_INET, &in4, address, address_len))
		return false;
	host_mask = plen == 0 ? 0 : (0xffffffffU << (32 - plen));
	mask4.s_addr = htonl(host_mask);
	return inet_ntop(AF_INET, &mask4, mask, mask_len) != NULL;
}

void eigrp_cli_named_show_header(struct vty *vty, const struct lyd_node *dnode,
				 bool show_defaults)
{
	const char *name = yang_dnode_get_string(dnode, "name");

	(void)show_defaults;
	vty_out(vty, "router eigrp %s\n", name);
}

void eigrp_cli_named_show_end(struct vty *vty, const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, "exit\n!\n");
}

void eigrp_cli_named_show_address_family(struct vty *vty,
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

void eigrp_cli_named_show_address_family_end(struct vty *vty,
					     const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, " exit-address-family\n");
}

void eigrp_cli_named_show_neighbor(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	const char *address = yang_dnode_get_string(dnode, "address");
	const char *interface_name = yang_dnode_get_string(dnode, "interface");

	(void)show_defaults;
	vty_out(vty, "  neighbor %s %s\n", address, interface_name);
}

void eigrp_cli_named_show_shutdown(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "  shutdown\n");
}


void eigrp_cli_named_show_af_interface(struct vty *vty,
				       const struct lyd_node *dnode,
				       bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "  af-interface %s\n",
		yang_dnode_get_string(dnode, "interface"));
}

void eigrp_cli_named_show_af_interface_end(struct vty *vty,
					   const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, "  exit-af-interface\n");
}

void eigrp_cli_named_show_af_interface_bandwidth_percent(struct vty *vty,
						 const struct lyd_node *dnode,
						 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   bandwidth-percent %u\n",
		yang_dnode_get_uint32(dnode, NULL));
}

void eigrp_cli_named_show_af_interface_bandwidth(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   bandwidth %u\n", yang_dnode_get_uint32(dnode, NULL));
}

void eigrp_cli_named_show_af_interface_delay(struct vty *vty,
				     const struct lyd_node *dnode,
				     bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   delay %u\n", yang_dnode_get_uint32(dnode, NULL));
}

void eigrp_cli_named_show_af_interface_hello(struct vty *vty,
					     const struct lyd_node *dnode,
					     bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   hello-interval %u\n",
		yang_dnode_get_uint16(dnode, NULL));
}

void eigrp_cli_named_show_af_interface_hold(struct vty *vty,
					    const struct lyd_node *dnode,
					    bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   hold-time %u\n", yang_dnode_get_uint16(dnode, NULL));
}

void eigrp_cli_named_show_af_interface_passive(struct vty *vty,
					       const struct lyd_node *dnode,
					       bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "   passive-interface\n");
}

void eigrp_cli_named_show_af_interface_authentication(
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

void eigrp_cli_named_show_af_interface_keychain(struct vty *vty,
						const struct lyd_node *dnode,
						bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   authentication key-chain %s\n",
		yang_dnode_get_string(dnode, NULL));
}

void eigrp_cli_named_show_af_interface_next_hop_self(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
	(void)show_defaults;
	if (!yang_dnode_get_bool(dnode, NULL))
		vty_out(vty, "   no next-hop-self\n");
}

void eigrp_cli_named_show_af_interface_split_horizon(
	struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
	(void)show_defaults;
	if (!yang_dnode_get_bool(dnode, NULL))
		vty_out(vty, "   no split-horizon\n");
}

void eigrp_cli_named_show_af_interface_summary(struct vty *vty,
					       const struct lyd_node *dnode,
					       bool show_defaults)
{
	char address[INET6_ADDRSTRLEN + 4];
	char mask[INET_ADDRSTRLEN];
	bool ipv6;
	const char *prefix = yang_dnode_get_string(dnode, "prefix");

	(void)show_defaults;
	if (!eigrp_cli_summary_prefix_display(prefix, address, sizeof(address), mask,
					      sizeof(mask), &ipv6))
		return;
	if (ipv6)
		vty_out(vty, "   summary-address %s", address);
	else
		vty_out(vty, "   summary-address %s %s", address, mask);
	if (yang_dnode_exists(dnode, "administrative-distance"))
		vty_out(vty, " %u",
			yang_dnode_get_uint8(dnode, "administrative-distance"));
	if (yang_dnode_exists(dnode, "leak-map"))
		vty_out(vty, " leak-map %s",
			yang_dnode_get_string(dnode, "leak-map"));
	vty_out(vty, "\n");
}

void eigrp_cli_named_show_af_interface_shutdown(struct vty *vty,
						const struct lyd_node *dnode,
						bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "   shutdown\n");
}

void eigrp_cli_named_show_topology(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "  topology base\n");
}

void eigrp_cli_named_show_topology_end(struct vty *vty,
				       const struct lyd_node *dnode)
{
	(void)dnode;
	vty_out(vty, "  exit-af-topology\n");
}

void eigrp_cli_named_show_auto_summary(struct vty *vty,
				       const struct lyd_node *dnode,
				       bool show_defaults)
{
	(void)dnode;
	(void)show_defaults;
	vty_out(vty, "   auto-summary\n");
}

static void eigrp_cli_named_show_default_information(
	struct vty *vty, const struct lyd_node *dnode, const char *direction)
{
	vty_out(vty, "   default-information %s", direction);
	if (yang_dnode_exists(dnode, "access-list"))
		vty_out(vty, " %s", yang_dnode_get_string(dnode, "access-list"));
	vty_out(vty, "\n");
}

void eigrp_cli_named_show_default_information_in(struct vty *vty,
						 const struct lyd_node *dnode,
						 bool show_defaults)
{
	(void)show_defaults;
	eigrp_cli_named_show_default_information(vty, dnode, "in");
}

void eigrp_cli_named_show_default_information_out(struct vty *vty,
						  const struct lyd_node *dnode,
						  bool show_defaults)
{
	(void)show_defaults;
	eigrp_cli_named_show_default_information(vty, dnode, "out");
}

void eigrp_cli_named_show_default_metric(struct vty *vty,
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

void eigrp_cli_named_show_distance(struct vty *vty,
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

void eigrp_cli_named_show_maximum_prefix(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	eigrp_cli_show_prefix_limit(vty, dnode, "   maximum-prefix ");
}

void eigrp_cli_named_show_metric_weights(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "  metric weights %u %u %u %u %u %u",
		yang_dnode_get_uint8(dnode, "tos"),
		yang_dnode_get_uint8(dnode, "K1"),
		yang_dnode_get_uint8(dnode, "K2"),
		yang_dnode_get_uint8(dnode, "K3"),
		yang_dnode_get_uint8(dnode, "K4"),
		yang_dnode_get_uint8(dnode, "K5"));
	if (yang_dnode_exists(dnode, "K6"))
		vty_out(vty, " %u", yang_dnode_get_uint8(dnode, "K6"));
	vty_out(vty, "\n");
}

void eigrp_cli_named_show_offset_list(struct vty *vty,
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

void eigrp_cli_named_show_redistribute(struct vty *vty,
				       const struct lyd_node *dnode,
				       bool show_defaults)
{
	const char *protocol = yang_dnode_get_string(dnode, "protocol");
	uint16_t route_instance = yang_dnode_get_uint16(dnode, "route-instance");

	(void)show_defaults;
	vty_out(vty, "   redistribute %s", protocol);
	if (route_instance)
		vty_out(vty, " %u", route_instance);
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

void eigrp_cli_named_show_summary_metric(struct vty *vty,
					 const struct lyd_node *dnode,
					 bool show_defaults)
{
	char address[INET6_ADDRSTRLEN + 4];
	char mask[INET_ADDRSTRLEN];
	bool ipv6;
	const char *prefix = yang_dnode_get_string(dnode, "prefix");

	(void)show_defaults;
	if (!eigrp_cli_summary_prefix_display(prefix, address, sizeof(address), mask,
					      sizeof(mask), &ipv6))
		return;
	if (ipv6)
		vty_out(vty, "   summary-metric %s", address);
	else
		vty_out(vty, "   summary-metric %s %s", address, mask);
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

void eigrp_cli_named_show_neighbor_description(struct vty *vty,
                                                const struct lyd_node *dnode,
                                                bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "  neighbor %s description %s\n",
            yang_dnode_get_string(lyd_parent(dnode), "address"),
            yang_dnode_get_string(dnode, NULL));
}

void eigrp_cli_named_show_neighbor_maximum_prefix(struct vty *vty,
                                                   const struct lyd_node *dnode,
                                                   bool show_defaults)
{
    char prefix[256];
    (void)show_defaults;
    snprintf(prefix, sizeof(prefix), "  neighbor %s maximum-prefix ",
             yang_dnode_get_string(lyd_parent(dnode), "address"));
    eigrp_cli_show_prefix_limit(vty, dnode, prefix);
}

void eigrp_cli_named_show_neighbor_maximum_prefix_all(struct vty *vty,
                                                       const struct lyd_node *dnode,
                                                       bool show_defaults)
{
    (void)show_defaults;
    eigrp_cli_show_prefix_limit(vty, dnode, "  neighbor maximum-prefix ");
}

void eigrp_cli_named_show_log_neighbor_changes(struct vty *vty,
                                                const struct lyd_node *dnode,
                                                bool show_defaults)
{
    (void)show_defaults;
    if (!yang_dnode_get_bool(dnode, NULL))
        vty_out(vty, "  no eigrp log-neighbor-changes\n");
}

void eigrp_cli_named_show_log_neighbor_warnings(struct vty *vty,
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

void eigrp_cli_named_show_maximum_paths(struct vty *vty,
                                         const struct lyd_node *dnode,
                                         bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "   maximum-paths %u\n", yang_dnode_get_uint8(dnode, NULL));
}

void eigrp_cli_named_show_metric_maximum_hops(struct vty *vty,
                                               const struct lyd_node *dnode,
                                               bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "   metric maximum-hops %u\n", yang_dnode_get_uint8(dnode, NULL));
}

void eigrp_cli_named_show_metric_holddown(struct vty *vty,
                                           const struct lyd_node *dnode,
                                           bool show_defaults)
{
    (void)dnode;
    (void)show_defaults;
    vty_out(vty, "   metric holddown\n");
}

void eigrp_cli_named_show_event_log_size(struct vty *vty,
                                          const struct lyd_node *dnode,
                                          bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "   eigrp event-log-size %u\n",
            yang_dnode_get_uint32(dnode, NULL));
}

void eigrp_cli_named_show_redistribute_maximum_prefix(
    struct vty *vty, const struct lyd_node *dnode, bool show_defaults)
{
    (void)show_defaults;
    eigrp_cli_show_prefix_limit(vty, dnode, "   redistribute maximum-prefix ");
}

void eigrp_cli_named_show_distribute_list(struct vty *vty,
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

void eigrp_cli_named_show_active_time(struct vty *vty,
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

void eigrp_cli_named_show_traffic_share_balanced(struct vty *vty,
						 const struct lyd_node *dnode,
						 bool show_defaults)
{
	(void)show_defaults;
	if (yang_dnode_get_bool(dnode, NULL))
		vty_out(vty, "   traffic-share balanced\n");
	else
		vty_out(vty, "   no traffic-share balanced\n");
}

void eigrp_cli_named_show_variance(struct vty *vty,
				   const struct lyd_node *dnode,
				   bool show_defaults)
{
	(void)show_defaults;
	vty_out(vty, "   variance %u\n", yang_dnode_get_uint8(dnode, NULL));
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
 */

void eigrp_cli_named_show_router_id(struct vty *vty,
                                    const struct lyd_node *dnode,
                                    bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "  eigrp router-id %s\n", yang_dnode_get_string(dnode, NULL));
}

void eigrp_cli_named_show_network(struct vty *vty,
                                  const struct lyd_node *dnode,
                                  bool show_defaults)
{
    (void)show_defaults;
    vty_out(vty, "  network %s\n", yang_dnode_get_string(dnode, NULL));
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
	eigrp_list_node_t *node, *nnode;
	uint32_t as;

	if (!asn || !vrf_name)
		return NULL;

	vrf = vrf_lookup_by_name(vrf_name);
	if (!vrf)
		return NULL;

	as = strtoul(asn, NULL, 10);
	for (EIGRP_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
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
	return vty && vty->xpath_index > 0
	       && strstr(VTY_CURR_XPATH, "/named[") != NULL
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "name", name, name_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "afi", afi, afi_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "vrf", vrf_name, vrf_len)
	       && eigrp_cli_xpath_get(VTY_CURR_XPATH, "asn", asn, asn_len);
}

static bool eigrp_cli_named_af_mode(struct vty *vty)
{
	return vty && vty->xpath_index > 0
	       && strstr(VTY_CURR_XPATH, "/named[") != NULL
	       && strstr(VTY_CURR_XPATH, "/address-family[") != NULL
	       && strstr(VTY_CURR_XPATH, "/af-interface[") == NULL
	       && strstr(VTY_CURR_XPATH, "/topology") == NULL;
}

/*
 * Named-mode submodes share FRR's EIGRP_NODE.  The XPath stack therefore
 * carries the real router/address-family/af-interface/topology hierarchy.
 * Mode-entry commands must rewind to an existing parent before entering a
 * sibling.  They may never manufacture a parent that is not already active.
 */
static bool eigrp_cli_named_xpath_rewind(struct vty *vty, const char *xpath)
{
	int index;

	if (!vty || !xpath || !xpath[0])
		return false;

	for (index = vty->xpath_index - 1; index >= 0; index--) {
		if (strcmp(vty->xpath[index], xpath) != 0)
			continue;
		vty->xpath_index = index + 1;
		vty->node = EIGRP_NODE;
		return true;
	}

	return false;
}

static void eigrp_cli_config_rewind(struct vty *vty)
{
	if (!vty)
		return;
	vty->xpath_index = 0;
	vty->node = CONFIG_NODE;
}

static bool eigrp_cli_named_root_rewind(struct vty *vty, char *name,
				       size_t name_len)
{
	char xpath[XPATH_MAXLEN];

	if (!vty || vty->xpath_index <= 0
	    || strstr(VTY_CURR_XPATH, "/named[") == NULL
	    || !eigrp_cli_xpath_get(VTY_CURR_XPATH, "name", name, name_len))
		return false;

	eigrp_cli_named_xpath(xpath, sizeof(xpath), name);
	return eigrp_cli_named_xpath_rewind(vty, xpath);
}

static bool eigrp_cli_named_af_rewind(struct vty *vty, char *name,
				     size_t name_len, char *afi,
				     size_t afi_len, char *vrf_name,
				     size_t vrf_len, char *asn,
				     size_t asn_len)
{
	char xpath[XPATH_MAXLEN];

	if (!eigrp_cli_named_af_context(vty, name, name_len, afi, afi_len,
					vrf_name, vrf_len, asn, asn_len))
		return false;

	eigrp_cli_named_af_xpath(xpath, sizeof(xpath), name, afi, vrf_name, asn);
	return eigrp_cli_named_xpath_rewind(vty, xpath);
}

static int eigrp_cli_named_af_required(struct vty *vty)
{
	if (eigrp_cli_named_af_mode(vty))
		return 1;
	vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
	return 0;
}

/*
 * Syntax: `router eigrp WORD`
 * Mode: Configuration
 * XPath: /frr-eigrpd:eigrpd/named
 * Target: eigrp_instance_parent_create()
 */
DEFUN_NOSH(router_eigrp_named,
           router_eigrp_named_cmd,
      "router eigrp WORD",
      ROUTER_STR
      EIGRP_STR
      "EIGRP named-mode instance name\n")
{
	const char *name = eigrp_cli_token_last(argc, argv);

	/* `router` is a global configuration command even when entered from an
	 * EIGRP submode.  FRR only pops one EIGRP_NODE level while the named
	 * hierarchy can have several XPath levels, so normalize it explicitly.
	 */
	eigrp_cli_config_rewind(vty);
	return eigrp_cli_push_named_root(vty, name);
}

/*
 * Syntax: `no router eigrp WORD`
 * Mode: Configuration
 * XPath: /frr-eigrpd:eigrpd/named
 * Target: eigrp_instance_parent_delete()
 */
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

	eigrp_cli_config_rewind(vty);
	eigrp_cli_named_xpath(xpath, sizeof(xpath), name);
	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes_clear_pending(vty, NULL);
}

/*
 * Syntax: `address-family ipv4 [unicast] [vrf NAME] autonomous-system (1-65535)`
 * Mode: Named router
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Target: eigrp_instance_address_family_create()
 */
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

	if (!eigrp_cli_named_root_rewind(vty, name, sizeof(name))) {
		vty_out(vty, "%% Enter named EIGRP router mode first\n");
		return CMD_WARNING;
	}
	return eigrp_cli_named_address_family_set(vty, name, "ipv4", asn,
						    eigrp_cli_vrf_name(vrf_name));
}

/*
 * Syntax: `no address-family ipv4 [unicast] [vrf NAME] autonomous-system (1-65535)`
 * Mode: Named router
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Target: eigrp_instance_address_family_delete()
 */
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

	if (!eigrp_cli_named_root_rewind(vty, name, sizeof(name))) {
		vty_out(vty, "%% Enter named EIGRP router mode first\n");
		return CMD_WARNING;
	}
	return eigrp_cli_named_address_family_unset(vty, name, "ipv4", asn,
						      eigrp_cli_vrf_name(vrf_name));
}

/*
 * Syntax: `address-family ipv6 [unicast] [vrf NAME] autonomous-system (1-65535)`
 * Mode: Named router
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Target: eigrp_instance_address_family_create()
 */
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

	if (!eigrp_cli_named_root_rewind(vty, name, sizeof(name))) {
		vty_out(vty, "%% Enter named EIGRP router mode first\n");
		return CMD_WARNING;
	}
	return eigrp_cli_named_address_family_set(vty, name, "ipv6", asn,
						    eigrp_cli_vrf_name(vrf_name));
}

/*
 * Syntax: `no address-family ipv6 [unicast] [vrf NAME] autonomous-system (1-65535)`
 * Mode: Named router
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Target: eigrp_instance_address_family_delete()
 */
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

	if (!eigrp_cli_named_root_rewind(vty, name, sizeof(name))) {
		vty_out(vty, "%% Enter named EIGRP router mode first\n");
		return CMD_WARNING;
	}
	return eigrp_cli_named_address_family_unset(vty, name, "ipv6", asn,
						      eigrp_cli_vrf_name(vrf_name));
}

/*
 * Syntax: `exit-address-family`
 * Mode: Named address-family
 * XPath: mode transition only
 * Target: none
 */
DEFUN(eigrp_exit_address_family,
      eigrp_exit_address_family_cmd,
      "exit-address-family",
      "Exit address-family configuration mode\n")
{
	char name[128];

	if (!vty || vty->xpath_index <= 0
	    || strstr(VTY_CURR_XPATH, "/address-family[") == NULL
	    || !eigrp_cli_named_root_rewind(vty, name, sizeof(name))) {
		vty_out(vty, "%% Enter named EIGRP address-family mode first\n");
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

/*
 * Syntax: `no shutdown`
 * Mode: Named parent / address-family / af-interface
 * XPath: parent is direct; address-family and af-interface use their local shutdown leaf
 * Target: parent -> eigrp_instance_parent_shutdown_set(); address-family -> eigrp_instance_address_family_shutdown_set(); af-interface -> eigrp_interface_shutdown_set()
 */
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
			eigrp_instance_parent_shutdown_reset(
				eigrp_instance_parent_read(name)));
	}
	return CMD_WARNING;
}

/*
 * Syntax: `shutdown`
 * Mode: Named parent / address-family / af-interface
 * XPath: parent is direct; address-family and af-interface use their local shutdown leaf
 * Target: parent -> eigrp_instance_parent_shutdown_set(); address-family -> eigrp_instance_address_family_shutdown_set(); af-interface -> eigrp_interface_shutdown_set()
 */
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
			eigrp_instance_parent_shutdown_set(
				eigrp_instance_parent_read(name)));
	}
	return CMD_WARNING;
}

/*
 * Syntax: `network A.B.C.D [A.B.C.D]`
 * Mode: Named IPv4 address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/network
 * Target: eigrp_network_create()
 */
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

/*
 * Syntax: `no network A.B.C.D [A.B.C.D]`
 * Mode: Named IPv4 address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/network
 * Target: eigrp_network_delete()
 */
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

/*
 * Syntax: `neighbor A.B.C.D IFNAME`
 * Mode: Named IPv4 address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Target: eigrp_neighbor_static_create()
 */
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

/*
 * Syntax: `no neighbor A.B.C.D IFNAME`
 * Mode: Named IPv4 address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Target: eigrp_neighbor_static_delete()
 */
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

/*
 * Syntax: `neighbor X:X::X:X IFNAME`
 * Mode: Named IPv6 address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Target: eigrp_neighbor_static_create()
 */
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

/*
 * Syntax: `no neighbor X:X::X:X IFNAME`
 * Mode: Named IPv6 address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Target: eigrp_neighbor_static_delete()
 */
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

static int eigrp_cli_prefix_limit_set(struct vty *vty, int argc,
                                      struct cmd_token *argv[],
                                      const char *keyword,
                                      const char *xpath, bool include_timers,
                                      bool remove);

/*
 * Syntax: `neighbor <A.B.C.D|X:X::X:X> description LINE`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/description
 * Target: eigrp_neighbor_description_set()
 */
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

/*
 * Syntax: `no neighbor <A.B.C.D|X:X::X:X> description [LINE]`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/description
 * Target: eigrp_neighbor_description_reset()
 */
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

/*
 * Syntax: `neighbor <A.B.C.D|X:X::X:X> maximum-prefix (1-4294967295) [(1-100)] [warning-only]`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix
 * Target: eigrp_neighbor_maximum_prefix_set()
 */
DEFUN(eigrp_neighbor_maximum_prefix,
      eigrp_neighbor_maximum_prefix_cmd,
      "neighbor <A.B.C.D|X:X::X:X> maximum-prefix (1-4294967295) [(1-100)] [warning-only]",
      "Specify a neighbor router\n" "Neighbor address\n" "Neighbor address\n"
      "Limit prefixes accepted from this neighbor\n" "Maximum prefixes\n"
      "Warning threshold\n" "Warning only\n")
{
    const char *address = eigrp_cli_token_after(argc, argv, "neighbor");
    char base[XPATH_MAXLEN], xpath[XPATH_MAXLEN];
    if (!eigrp_cli_neighbor_policy_xpath(vty, address, base, sizeof(base)))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, base, NB_OP_CREATE, NULL);
    if (!eigrp_cli_xpath_leaf_build(xpath, sizeof(xpath), base, "maximum-prefix"))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix", xpath, false, false);
}

/*
 * Syntax: `no neighbor <A.B.C.D|X:X::X:X> maximum-prefix`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix
 * Target: eigrp_neighbor_maximum_prefix_reset()
 */
DEFUN(no_eigrp_neighbor_maximum_prefix,
      no_eigrp_neighbor_maximum_prefix_cmd,
      "no neighbor <A.B.C.D|X:X::X:X> maximum-prefix",
      NO_STR "Specify a neighbor router\n" "Neighbor address\n" "Neighbor address\n"
      "Limit prefixes accepted from this neighbor\n")
{
    const char *address = eigrp_cli_token_after(argc, argv, "neighbor");
    char base[XPATH_MAXLEN], xpath[XPATH_MAXLEN];
    if (!eigrp_cli_neighbor_policy_xpath(vty, address, base, sizeof(base)))
        return CMD_WARNING;
    if (!eigrp_cli_xpath_leaf_build(xpath, sizeof(xpath), base, "maximum-prefix"))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix", xpath, false, true);
}

/*
 * Syntax: `neighbor maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix
 * Target: eigrp_neighbor_maximum_prefix_all_set()
 */
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
                                      "./neighbor-maximum-prefix", true, false);
}

/*
 * Syntax: `no neighbor maximum-prefix`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix
 * Target: eigrp_neighbor_maximum_prefix_all_reset()
 */
DEFUN(no_eigrp_neighbor_maximum_prefix_all,
      no_eigrp_neighbor_maximum_prefix_all_cmd,
      "no neighbor maximum-prefix",
      NO_STR "Specify neighbor behavior\n" "Limit prefixes accepted from all neighbors\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./neighbor-maximum-prefix", true, true);
}

/*
 * Syntax: `eigrp log-neighbor-changes`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes
 * Target: eigrp_neighbor_log_set()
 */
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

/*
 * Syntax: `no eigrp log-neighbor-changes`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes
 * Target: eigrp_neighbor_log_reset()
 */
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

/*
 * Syntax: `eigrp log-neighbor-warnings [(1-65535)]`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings
 * Target: eigrp_neighbor_log_set()
 */
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

/*
 * Syntax: `no eigrp log-neighbor-warnings`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings
 * Target: eigrp_neighbor_log_reset()
 */
DEFUN(no_eigrp_log_neighbor_warnings,
      no_eigrp_log_neighbor_warnings_cmd,
      "no eigrp log-neighbor-warnings",
      NO_STR EIGRP_STR "Log EIGRP neighbor warnings\n")
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings", NB_OP_CREATE, NULL);
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings/enabled", NB_OP_MODIFY, "false");
    nb_cli_enqueue_change(vty, "./log-neighbor-warnings/interval", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `af-interface <default|IFNAME>`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface
 * Target: eigrp_interface_config_create()
 */
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

	if (!eigrp_cli_named_af_rewind(vty, name, sizeof(name), afi,
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

/*
 * Syntax: `no af-interface <default|IFNAME>`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface
 * Target: eigrp_interface_config_delete()
 */
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

	if (!eigrp_cli_named_af_rewind(vty, name, sizeof(name), afi,
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

/*
 * Syntax: `exit-af-interface`
 * Mode: Named af-interface
 * XPath: mode transition only
 * Target: none
 */
DEFUN(eigrp_exit_af_interface,
      eigrp_exit_af_interface_cmd,
      "exit-af-interface",
      "Exit address-family interface configuration mode\n")
{
	char name[128];
	char afi[8];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];

	if (!vty || vty->xpath_index <= 0
	    || strstr(VTY_CURR_XPATH, "/af-interface[") == NULL
	    || !eigrp_cli_named_af_rewind(vty, name, sizeof(name), afi,
				      sizeof(afi), vrf_name, sizeof(vrf_name),
				      asn, sizeof(asn))) {
		vty_out(vty, "%% Enter named EIGRP af-interface mode first\n");
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

/*
 * Syntax: `bandwidth-percent (1-999999)`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent
 * Target: eigrp_interface_bandwidth_percent_set()
 */
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

/*
 * Syntax: `no bandwidth-percent`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent
 * Target: eigrp_interface_bandwidth_percent_reset()
 */
DEFUN(no_eigrp_af_interface_bandwidth_percent,
      no_eigrp_af_interface_bandwidth_percent_cmd,
      "no bandwidth-percent",
      NO_STR
      "Set EIGRP bandwidth percentage\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./bandwidth-percent", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `bandwidth (1-10000000)`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth
 * Target: eigrp_interface_bandwidth_set()
 */
DEFUN(eigrp_af_interface_bandwidth,
      eigrp_af_interface_bandwidth_cmd,
      "bandwidth (1-10000000)",
      "Set EIGRP interface bandwidth informational parameter\n"
      "Bandwidth in kilobits\n")
{
	const char *bandwidth = eigrp_cli_token_last(argc, argv);
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./bandwidth", NB_OP_MODIFY, bandwidth);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no bandwidth [(1-10000000)]`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth
 * Target: eigrp_interface_bandwidth_reset()
 */
DEFUN(no_eigrp_af_interface_bandwidth,
      no_eigrp_af_interface_bandwidth_cmd,
      "no bandwidth [(1-10000000)]",
      NO_STR
      "Set EIGRP interface bandwidth informational parameter\n"
      "Bandwidth in kilobits\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./bandwidth", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `delay (1-16777215)`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/delay
 * Target: eigrp_interface_delay_set()
 */
DEFUN(eigrp_af_interface_delay,
      eigrp_af_interface_delay_cmd,
      "delay (1-16777215)",
      "Specify interface throughput delay\n"
      "Throughput delay (tens of microseconds)\n")
{
	const char *delay = eigrp_cli_token_last(argc, argv);
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./delay", NB_OP_MODIFY, delay);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no delay [(1-16777215)]`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/delay
 * Target: eigrp_interface_delay_reset()
 */
DEFUN(no_eigrp_af_interface_delay,
      no_eigrp_af_interface_delay_cmd,
      "no delay [(1-16777215)]",
      NO_STR
      "Specify interface throughput delay\n"
      "Throughput delay (tens of microseconds)\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./delay", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `hello-interval (1-65535)`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval
 * Target: eigrp_interface_hello_interval_set()
 */
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

/*
 * Syntax: `no hello-interval`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval
 * Target: eigrp_interface_hello_interval_reset()
 */
DEFUN(no_eigrp_af_interface_hello_interval,
      no_eigrp_af_interface_hello_interval_cmd,
      "no hello-interval",
      NO_STR
      "Configures EIGRP hello interval\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./hello-interval", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `hold-time (1-65535)`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hold-time
 * Target: eigrp_interface_hold_time_set()
 */
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

/*
 * Syntax: `no hold-time`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hold-time
 * Target: eigrp_interface_hold_time_reset()
 */
DEFUN(no_eigrp_af_interface_hold_time,
      no_eigrp_af_interface_hold_time_cmd,
      "no hold-time",
      NO_STR
      "Configures EIGRP hold time\n")
{
	char interface_name[IFNAMSIZ];

	if (!eigrp_cli_af_interface_path(vty, interface_name,
					 sizeof(interface_name)))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./hold-time", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `authentication mode <md5|hmac-sha-256 <0|7> WORD>`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-mode
 * Target: eigrp_auth_mode_set()
 */
DEFUN(eigrp_af_interface_authentication_mode,
      eigrp_af_interface_authentication_mode_cmd,
      "authentication mode <md5|hmac-sha-256 <0|7> WORD>",
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

/*
 * Syntax: `no authentication mode`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-mode
 * Target: eigrp_auth_mode_reset()
 */
DEFUN(no_eigrp_af_interface_authentication_mode,
      no_eigrp_af_interface_authentication_mode_cmd,
      "no authentication mode",
      NO_STR
      "Authentication subcommands\n"
      "Authentication mode\n")
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

/*
 * Syntax: `authentication key-chain WORD`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-key-chain
 * Target: eigrp_auth_keychain_set()
 */
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

/*
 * Syntax: `no authentication key-chain WORD`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-key-chain
 * Target: eigrp_auth_keychain_reset()
 */
DEFUN(no_eigrp_af_interface_keychain,
      no_eigrp_af_interface_keychain_cmd,
      "no authentication key-chain WORD",
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

/*
 * Syntax: `passive-interface`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/passive-interface
 * Target: eigrp_interface_passive_set()
 */
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

/*
 * Syntax: `no passive-interface`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/passive-interface
 * Target: eigrp_interface_passive_set()
 */
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

/*
 * Syntax: `next-hop-self`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/next-hop-self
 * Target: eigrp_interface_next_hop_self_set()
 */
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

/*
 * Syntax: `no next-hop-self`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/next-hop-self
 * Target: eigrp_interface_next_hop_self_set()
 */
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

/*
 * Syntax: `split-horizon`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/split-horizon
 * Target: eigrp_interface_split_horizon_set()
 */
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

/*
 * Syntax: `no split-horizon`
 * Mode: Named af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/split-horizon
 * Target: eigrp_interface_split_horizon_set()
 */
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

static bool eigrp_cli_ipv4_summary_prefix(const char *address, const char *mask,
                                           char *prefix, size_t prefix_len)
{
    struct in_addr address4;
    struct in_addr mask4;
    struct in_addr network4;
    uint32_t host_mask;
    uint32_t inverse;
    uint8_t plen = 0;
    char network[INET_ADDRSTRLEN];

    if (!address || !mask || !prefix
        || inet_pton(AF_INET, address, &address4) != 1
        || inet_pton(AF_INET, mask, &mask4) != 1)
        return false;
    host_mask = ntohl(mask4.s_addr);
    inverse = ~host_mask;
    if ((inverse & (inverse + 1U)) != 0)
        return false;
    while (host_mask & 0x80000000U) {
        plen++;
        host_mask <<= 1;
    }
    network4.s_addr = address4.s_addr & mask4.s_addr;
    if (!inet_ntop(AF_INET, &network4, network, sizeof(network)))
        return false;
    snprintf(prefix, prefix_len, "%s/%u", network, plen);
    return true;
}

static const char *eigrp_cli_ipv6_prefix(int argc, struct cmd_token *argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        const char *value = eigrp_cli_token_value(argv[i]);
        char address[INET6_ADDRSTRLEN];
        const char *slash;
        char *end = NULL;
        unsigned long plen;
        size_t len;
        struct in6_addr parsed;

        if (!value || !strchr(value, ':') || !(slash = strchr(value, '/')))
            continue;
        len = (size_t)(slash - value);
        if (len == 0 || len >= sizeof(address))
            continue;
        memcpy(address, value, len);
        address[len] = '\0';
        plen = strtoul(slash + 1, &end, 10);
        if (end && *end == '\0' && plen <= 128
            && inet_pton(AF_INET6, address, &parsed) == 1)
            return value;
    }
    return NULL;
}

static int eigrp_cli_af_interface_summary_set(
    struct vty *vty, const char *prefix, const char *distance,
    const char *leak_map, bool remove)
{
    char interface_name[IFNAMSIZ];
    char xpath[XPATH_MAXLEN];
    char child[XPATH_MAXLEN];

    if (!eigrp_cli_af_interface_path(vty, interface_name, sizeof(interface_name)))
        return CMD_WARNING;
    snprintf(xpath, sizeof(xpath), "./summary-address[prefix='%s']", prefix);
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

/*
 * Syntax: `summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]`
 * Mode: Named IPv4 af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address
 * Target: eigrp_summary_create()
 */
DEFUN(eigrp_af_interface_summary_address,
      eigrp_af_interface_summary_address_cmd,
      "summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]",
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
    char prefix[INET_ADDRSTRLEN + 4];
    int i;

    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask)
        || !eigrp_cli_ipv4_summary_prefix(address, mask, prefix, sizeof(prefix)))
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
    return eigrp_cli_af_interface_summary_set(vty, prefix, distance, leak_map,
                                               false);
}

/*
 * Syntax: `summary-address X:X::X:X/M [(1-255) [leak-map WORD]]`
 * Mode: Named IPv6 af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address
 * Target: eigrp_summary_create()
 */
DEFUN(eigrp_af_interface_summary_address_ipv6,
      eigrp_af_interface_summary_address_ipv6_cmd,
      "summary-address X:X::X:X/M [(1-255) [leak-map WORD]]",
      "Perform address summarization\n"
      "Summary IPv6 prefix\n"
      "Administrative distance\n"
      "Leak selected component routes\n"
      "Route-map name\n")
{
    const char *prefix = eigrp_cli_ipv6_prefix(argc, argv);
    const char *distance = NULL;
    const char *leak_map = eigrp_cli_token_after(argc, argv, "leak-map");
    int i;

    if (!prefix)
        return CMD_WARNING;
    for (i = 0; i < argc; i++) {
        const char *value = eigrp_cli_token_value(argv[i]);
        char *end = NULL;
        unsigned long n;
        if (!value || value == prefix)
            continue;
        n = strtoul(value, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 255) {
            distance = value;
            break;
        }
    }
    return eigrp_cli_af_interface_summary_set(vty, prefix, distance, leak_map,
                                               false);
}

/*
 * Syntax: `no summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]`
 * Mode: Named IPv4 af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address
 * Target: eigrp_summary_delete()
 */
DEFUN(no_eigrp_af_interface_summary_address,
      no_eigrp_af_interface_summary_address_cmd,
      "no summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]",
      NO_STR
      "Perform address summarization\n"
      "Summary IPv4 address\n"
      "Summary subnet mask\n"
      "Administrative distance\n"
      "Leak selected component routes\n"
      "Route-map name\n")
{
    const char *address, *mask;
    char prefix[INET_ADDRSTRLEN + 4];

    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask)
        || !eigrp_cli_ipv4_summary_prefix(address, mask, prefix, sizeof(prefix)))
        return CMD_WARNING;
    return eigrp_cli_af_interface_summary_set(vty, prefix, NULL, NULL, true);
}

/*
 * Syntax: `no summary-address X:X::X:X/M [(1-255) [leak-map WORD]]`
 * Mode: Named IPv6 af-interface
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address
 * Target: eigrp_summary_delete()
 */
DEFUN(no_eigrp_af_interface_summary_address_ipv6,
      no_eigrp_af_interface_summary_address_ipv6_cmd,
      "no summary-address X:X::X:X/M [(1-255) [leak-map WORD]]",
      NO_STR
      "Perform address summarization\n"
      "Summary IPv6 prefix\n"
      "Administrative distance\n"
      "Leak selected component routes\n"
      "Route-map name\n")
{
    const char *prefix = eigrp_cli_ipv6_prefix(argc, argv);

    if (!prefix)
        return CMD_WARNING;
    return eigrp_cli_af_interface_summary_set(vty, prefix, NULL, NULL, true);
}

/*
 * Syntax: `topology base`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology
 * Target: eigrp_topology_create()
 */
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

	if (!eigrp_cli_named_af_rewind(vty, name, sizeof(name), afi,
				      sizeof(afi), vrf_name, sizeof(vrf_name),
				      asn, sizeof(asn))) {
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

/*
 * Syntax: `exit-af-topology`
 * Mode: Named topology
 * XPath: mode transition only
 * Target: none
 */
DEFUN(eigrp_exit_af_topology,
      eigrp_exit_af_topology_cmd,
      "exit-af-topology",
      "Exit address-family topology mode\n")
{
	char name[128];
	char afi[8];
	char asn[16];
	char vrf_name[VRF_NAMSIZ];

	if (!vty || vty->xpath_index <= 0
	    || strstr(VTY_CURR_XPATH, "/topology") == NULL
	    || !eigrp_cli_named_af_rewind(vty, name, sizeof(name), afi,
				      sizeof(afi), vrf_name, sizeof(vrf_name),
				      asn, sizeof(asn))) {
		vty_out(vty, "%% Enter named EIGRP topology base mode first\n");
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

/*
 * Syntax: `auto-summary`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/auto-summary
 * Target: eigrp_summary_auto_set()
 */
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

/*
 * Syntax: `no auto-summary`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/auto-summary
 * Target: eigrp_summary_auto_set()
 */
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

/*
 * Syntax: `default-information <in|out> [WORD]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-{in|out}
 * Target: eigrp_topology_default_information_set()
 */
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

/*
 * Syntax: `no default-information <in|out> [WORD]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-{in|out}
 * Target: eigrp_topology_default_information_set()
 */
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

/*
 * Syntax: `default-metric (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric
 * Target: eigrp_metric_default_set()
 */
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

/*
 * Syntax: `no default-metric (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric
 * Target: eigrp_metric_default_reset()
 */
DEFUN(no_eigrp_default_metric,
      no_eigrp_default_metric_cmd,
      "no default-metric (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)",
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

/*
 * Syntax: `distance eigrp (1-255) (1-255)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distance
 * Target: eigrp_instance_distance_set()
 */
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

/*
 * Syntax: `no distance eigrp`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distance
 * Target: eigrp_instance_distance_reset()
 */
DEFUN(no_eigrp_distance,
      no_eigrp_distance_cmd,
      "no distance eigrp",
      NO_STR
      "Define an administrative distance\n"
      EIGRP_STR)
{
	if (!eigrp_cli_named_topology_required(vty))
		return CMD_WARNING;
	nb_cli_enqueue_change(vty, "./distance", NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

static int eigrp_cli_prefix_limit_set(struct vty *vty, int argc,
                                      struct cmd_token *argv[],
                                      const char *keyword,
                                      const char *xpath, bool include_timers,
                                      bool remove)
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
    if (include_timers) {
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
    }
#undef EIGRP_PREFIX_LIMIT_DESTROY
#undef EIGRP_PREFIX_LIMIT_MODIFY
#undef EIGRP_PREFIX_LIMIT_EMPTY
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix
 * Target: eigrp_topology_maximum_prefix_set()
 */
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
                                      "./maximum-prefix", true, false);
}

/*
 * Syntax: `no maximum-prefix`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix
 * Target: eigrp_topology_maximum_prefix_reset()
 */
DEFUN(no_eigrp_maximum_prefix,
      no_eigrp_maximum_prefix_cmd,
      "no maximum-prefix",
      NO_STR "Limit prefixes accepted under an EIGRP address family\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./maximum-prefix", true, true);
}

/*
 * Syntax: `metric maximum-hops (1-255)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops
 * Target: eigrp_metric_maximum_hops_set()
 */
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

/*
 * Syntax: `no metric maximum-hops`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops
 * Target: eigrp_metric_maximum_hops_reset()
 */
DEFUN(no_eigrp_metric_maximum_hops,
      no_eigrp_metric_maximum_hops_cmd,
      "no metric maximum-hops",
      NO_STR "Modify EIGRP metric behavior\n" "Maximum hop count\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./metric-maximum-hops", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `metric holddown`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown
 * Target: eigrp_metric_holddown_set()
 */
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

/*
 * Syntax: `no metric holddown`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown
 * Target: eigrp_metric_holddown_reset()
 */
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

/*
 * Syntax: `eigrp event-log-size (0-4294967295)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/event-log-size
 * Target: eigrp_eventlog_size_set()
 */
DEFUN(eigrp_event_log_size,
      eigrp_event_log_size_cmd,
      "eigrp event-log-size (0-4294967295)",
      EIGRP_STR "Set EIGRP event log size\n" "Number of events\n")
{
    if (eigrp_cli_named_context(vty)
        && !eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./event-log-size", NB_OP_MODIFY,
                          eigrp_cli_token_last(argc, argv));
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `no eigrp event-log-size`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/event-log-size
 * Target: eigrp_eventlog_size_reset()
 */
DEFUN(no_eigrp_event_log_size,
      no_eigrp_event_log_size_cmd,
      "no eigrp event-log-size",
      NO_STR EIGRP_STR "Set EIGRP event log size\n")
{
    if (eigrp_cli_named_context(vty)
        && !eigrp_cli_named_topology_required(vty))
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

/*
 * Syntax: `offset-list WORD <in|out> (0-2147483647) [IFNAME]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/offset-list
 * Target: eigrp_offset_add()
 */
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

/*
 * Syntax: `no offset-list WORD <in|out> (0-2147483647) [IFNAME]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/offset-list
 * Target: eigrp_offset_remove()
 */
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

/*
 * Syntax: `redistribute maximum-prefix (1-4294967295) [(1-100)] [dampened] [reset-time (1-65535)] [restart (1-65535)] [restart-count (1-65535)] [warning-only]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix
 * Target: eigrp_redistribute_maximum_prefix_set()
 */
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
                                      "./redistribute-maximum-prefix", true, false);
}

/*
 * Syntax: `no redistribute maximum-prefix`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix
 * Target: eigrp_redistribute_maximum_prefix_reset()
 */
DEFUN(no_eigrp_redistribute_maximum_prefix,
      no_eigrp_redistribute_maximum_prefix_cmd,
      "no redistribute maximum-prefix",
      NO_STR REDIST_STR "Limit redistributed prefixes\n")
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    return eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",
                                      "./redistribute-maximum-prefix", true, true);
}

static int eigrp_cli_summary_metric_set(struct vty *vty,
                                        const char *prefix,
                                        const char **metric,
                                        const char *distance, bool remove)
{
    char xpath[XPATH_MAXLEN];
    char child[XPATH_MAXLEN];
    static const char *leaves[] = {"bandwidth", "delay", "reliability", "load", "mtu"};
    int i;

    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    snprintf(xpath, sizeof(xpath), "./summary-metric[prefix='%s']", prefix);
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

/*
 * Syntax: `summary-metric A.B.C.D A.B.C.D (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535) [distance (1-255)]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrp_summary_metric_set()
 */
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
    char prefix[INET_ADDRSTRLEN + 4];
    int count, i;

    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask)
        || !eigrp_cli_ipv4_summary_prefix(address, mask, prefix, sizeof(prefix)))
        return CMD_WARNING;
    count = eigrp_cli_token_values_after(argc, argv, "summary-metric", args, 7);
    if (count < 7)
        return CMD_WARNING;
    for (i = 0; i < 5; i++)
        metric[i] = args[i + 2];
    return eigrp_cli_summary_metric_set(vty, prefix, metric, distance, false);
}

/*
 * Syntax: `summary-metric X:X::X:X/M (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535) [distance (1-255)]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrp_summary_metric_set()
 */
DEFUN(eigrp_summary_metric_ipv6,
      eigrp_summary_metric_ipv6_cmd,
      "summary-metric X:X::X:X/M (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535) [distance (1-255)]",
      "Configure summary metric\n" "Summary IPv6 prefix\n"
      "Bandwidth metric\n" "Delay metric\n" "Reliability metric\n" "Load metric\n"
      "MTU metric\n" "Administrative distance\n" "Distance\n")
{
    const char *prefix = eigrp_cli_ipv6_prefix(argc, argv);
    const char *args[6] = {0};
    const char *metric[5];
    const char *distance = eigrp_cli_token_after(argc, argv, "distance");
    int count, i;

    if (!prefix)
        return CMD_WARNING;
    count = eigrp_cli_token_values_after(argc, argv, "summary-metric", args, 6);
    if (count < 6)
        return CMD_WARNING;
    for (i = 0; i < 5; i++)
        metric[i] = args[i + 1];
    return eigrp_cli_summary_metric_set(vty, prefix, metric, distance, false);
}

/*
 * Syntax: `summary-metric A.B.C.D A.B.C.D distance (1-255)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrp_summary_metric_set()
 */
DEFUN(eigrp_summary_metric_distance,
      eigrp_summary_metric_distance_cmd,
      "summary-metric A.B.C.D A.B.C.D distance (1-255)",
      "Configure summary metric\n" "Summary IPv4 address\n" "Summary subnet mask\n"
      "Administrative distance\n" "Distance\n")
{
    const char *address, *mask;
    char prefix[INET_ADDRSTRLEN + 4];

    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask)
        || !eigrp_cli_ipv4_summary_prefix(address, mask, prefix, sizeof(prefix)))
        return CMD_WARNING;
    return eigrp_cli_summary_metric_set(vty, prefix, NULL,
                                        eigrp_cli_token_after(argc, argv, "distance"), false);
}

/*
 * Syntax: `summary-metric X:X::X:X/M distance (1-255)`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrp_summary_metric_set()
 */
DEFUN(eigrp_summary_metric_distance_ipv6,
      eigrp_summary_metric_distance_ipv6_cmd,
      "summary-metric X:X::X:X/M distance (1-255)",
      "Configure summary metric\n" "Summary IPv6 prefix\n"
      "Administrative distance\n" "Distance\n")
{
    const char *prefix = eigrp_cli_ipv6_prefix(argc, argv);

    if (!prefix)
        return CMD_WARNING;
    return eigrp_cli_summary_metric_set(vty, prefix, NULL,
                                        eigrp_cli_token_after(argc, argv, "distance"), false);
}

/*
 * Syntax: `no summary-metric A.B.C.D A.B.C.D`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrp_summary_metric_reset()
 */
DEFUN(no_eigrp_summary_metric,
      no_eigrp_summary_metric_cmd,
      "no summary-metric A.B.C.D A.B.C.D",
      NO_STR "Configure summary metric\n" "Summary IPv4 address\n"
      "Summary subnet mask\n")
{
    const char *address, *mask;
    char prefix[INET_ADDRSTRLEN + 4];

    if (!eigrp_cli_ipv4_pair(argc, argv, &address, &mask)
        || !eigrp_cli_ipv4_summary_prefix(address, mask, prefix, sizeof(prefix)))
        return CMD_WARNING;
    return eigrp_cli_summary_metric_set(vty, prefix, NULL, NULL, true);
}

/*
 * Syntax: `no summary-metric X:X::X:X/M`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrp_summary_metric_reset()
 */
DEFUN(no_eigrp_summary_metric_ipv6,
      no_eigrp_summary_metric_ipv6_cmd,
      "no summary-metric X:X::X:X/M",
      NO_STR "Configure summary metric\n" "Summary IPv6 prefix\n")
{
    const char *prefix = eigrp_cli_ipv6_prefix(argc, argv);

    if (!prefix)
        return CMD_WARNING;
    return eigrp_cli_summary_metric_set(vty, prefix, NULL, NULL, true);
}

/*
 * Syntax: `traffic-share balanced`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced
 * Target: eigrp_metric_traffic_share_balanced_set()
 */
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

/*
 * Syntax: `no traffic-share balanced`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced
 * Target: eigrp_metric_traffic_share_balanced_set()
 */
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


bool eigrp_cli_named_context(struct vty *vty)
{
    return vty && strstr(VTY_CURR_XPATH, "/named[") != NULL;
}

/*
 * Syntax: `timers active-time <seconds|disabled>` / `no timers active-time`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Target: eigrp_timer_active_time_set() / eigrp_timer_active_time_reset()
 * Note: the shared parser is declared in eigrp_cli_classic.c and dispatches here when the VTY is in named context.
 */
int eigrp_cli_named_active_time_apply(struct vty *vty, bool disabled,
                                      const char *timer, bool remove)
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./active-time",
                          remove ? NB_OP_DESTROY : NB_OP_MODIFY,
                          remove ? NULL : (disabled ? "0" : timer));
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `variance multiplier` / `no variance`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Target: eigrp_metric_variance_set() / eigrp_metric_variance_reset()
 * Note: the shared parser is declared in eigrp_cli_classic.c and dispatches here when the VTY is in named context.
 */
int eigrp_cli_named_variance_apply(struct vty *vty, const char *variance,
                                   bool remove)
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./variance",
                          remove ? NB_OP_DESTROY : NB_OP_MODIFY,
                          remove ? NULL : variance);
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `maximum-paths paths` / `no maximum-paths`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Target: eigrp_topology_maximum_paths_set() / eigrp_topology_maximum_paths_reset()
 * Note: the shared parser is declared in eigrp_cli_classic.c and dispatches here when the VTY is in named context.
 */
int eigrp_cli_named_maximum_paths_apply(struct vty *vty,
                                        const char *maximum_paths,
                                        bool remove)
{
    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;
    nb_cli_enqueue_change(vty, "./maximum-paths",
                          remove ? NB_OP_DESTROY : NB_OP_MODIFY,
                          remove ? NULL : maximum_paths);
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `metric weights tos K1 K2 K3 K4 K5 [K6]` / `no metric weights`
 * Mode: Named address-family
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights
 * Target: eigrp_metric_weights_set() / eigrp_metric_weights_reset()
 * Note: the shared parser is declared in eigrp_cli_classic.c and dispatches here when the VTY is in named context.
 */
int eigrp_cli_named_metric_weights_apply(struct vty *vty,
                                         const char *tos,
                                         const char *k1,
                                         const char *k2,
                                         const char *k3,
                                         const char *k4,
                                         const char *k5,
                                         const char *k6,
                                         bool remove)
{
    if (!eigrp_cli_named_af_required(vty))
        return CMD_WARNING;
    if (remove) {
        nb_cli_enqueue_change(vty, "./metric-weights", NB_OP_DESTROY, NULL);
        return nb_cli_apply_changes(vty, NULL);
    }
    if (!tos || strcmp(tos, "0") != 0) {
        vty_out(vty, "%% EIGRP metric weights TOS must be 0\n");
        return CMD_WARNING;
    }
    nb_cli_enqueue_change(vty, "./metric-weights", NB_OP_CREATE, NULL);
    nb_cli_enqueue_change(vty, "./metric-weights/tos", NB_OP_MODIFY, tos);
    nb_cli_enqueue_change(vty, "./metric-weights/K1", NB_OP_MODIFY, k1);
    nb_cli_enqueue_change(vty, "./metric-weights/K2", NB_OP_MODIFY, k2);
    nb_cli_enqueue_change(vty, "./metric-weights/K3", NB_OP_MODIFY, k3);
    nb_cli_enqueue_change(vty, "./metric-weights/K4", NB_OP_MODIFY, k4);
    nb_cli_enqueue_change(vty, "./metric-weights/K5", NB_OP_MODIFY, k5);
    if (k6)
        nb_cli_enqueue_change(vty, "./metric-weights/K6", NB_OP_MODIFY, k6);
    else
        nb_cli_enqueue_change(vty, "./metric-weights/K6", NB_OP_DESTROY, NULL);
    return nb_cli_apply_changes(vty, NULL);
}

/*
 * Syntax: `redistribute PROTOCOL [ROUTE-INSTANCE] [metric ...] [route-map NAME]`
 *         / `no redistribute PROTOCOL [ROUTE-INSTANCE]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Target: eigrp_redistribute_add() / eigrp_redistribute_remove()
 * Note: the shared parser is declared in eigrp_cli_classic.c and dispatches here when the VTY is in named context.
 */
int eigrp_cli_named_redistribute_apply(struct vty *vty,
                                       const char *protocol,
                                       uint32_t route_instance,
                                       uint32_t bandwidth,
                                       const char *bandwidth_text,
                                       uint32_t delay,
                                       const char *delay_text,
                                       uint8_t reliability,
                                       const char *reliability_text,
                                       uint8_t load,
                                       const char *load_text,
                                       uint32_t mtu,
                                       const char *mtu_text,
                                       const char *route_map,
                                       bool remove)
{
    char xpath[XPATH_MAXLEN];
    char child[XPATH_MAXLEN + 64];
    bool route_instance_valid = false;

    if (!eigrp_cli_named_topology_required(vty))
        return CMD_WARNING;

    if (protocol && strcmp(protocol, "eigrp") == 0)
        route_instance_valid = route_instance > 0 && route_instance <= UINT16_MAX;
    else if (protocol && strcmp(protocol, "ospf") == 0)
        route_instance_valid = route_instance <= UINT16_MAX;
    else if (protocol
             && (strcmp(protocol, "connected") == 0
                 || strcmp(protocol, "static") == 0
                 || strcmp(protocol, "rip") == 0
                 || strcmp(protocol, "isis") == 0
                 || strcmp(protocol, "bgp") == 0))
        route_instance_valid = route_instance == 0;

    if (!route_instance_valid) {
        vty_out(vty, "%% Unsupported EIGRP redistribution source identity\n");
        return CMD_WARNING_CONFIG_FAILED;
    }

    snprintf(xpath, sizeof(xpath),
             "./redistribute[protocol='%s'][route-instance='%u']",
             protocol, route_instance);
    if (remove) {
        nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);
        return nb_cli_apply_changes(vty, NULL);
    }

    nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);
    snprintf(child, sizeof(child), "%s/metrics", xpath);
    if (!bandwidth_text || !delay_text || !reliability_text || !load_text || !mtu_text) {
        nb_cli_enqueue_change(vty, child, NB_OP_DESTROY, NULL);
    } else {
        nb_cli_enqueue_change(vty, child, NB_OP_CREATE, NULL);
#define EIGRP_NAMED_METRIC_LEAF(_leaf, _value) \
        do { \
            snprintf(child, sizeof(child), "%s/metrics/" _leaf, xpath); \
            nb_cli_enqueue_change(vty, child, NB_OP_MODIFY, _value); \
        } while (0)
        EIGRP_NAMED_METRIC_LEAF("bandwidth", bandwidth_text);
        EIGRP_NAMED_METRIC_LEAF("delay", delay_text);
        EIGRP_NAMED_METRIC_LEAF("reliability", reliability_text);
        EIGRP_NAMED_METRIC_LEAF("load", load_text);
        EIGRP_NAMED_METRIC_LEAF("mtu", mtu_text);
#undef EIGRP_NAMED_METRIC_LEAF
    }
    snprintf(child, sizeof(child), "%s/route-map", xpath);
    nb_cli_enqueue_change(vty, child,
                          route_map ? NB_OP_MODIFY : NB_OP_DESTROY,
                          route_map);
    return nb_cli_apply_changes(vty, NULL);
}


struct eigrp_vty_walk_context {
	const char *ifname;
	const char *detail;
	const char *all;
	const char *target;
	const struct prefix *prefix;
	eigrp_address_family_t address_afi;
	const struct in_addr *ipv4_address;
	const struct in6_addr *ipv6_address;
	bool soft;
	int matched;
	eigrp_result_t result;
};

typedef void (*eigrp_vty_walk_cb)(struct vty *vty, eigrp_instance_t *eigrp,
					 struct eigrp_vty_walk_context *ctx);

static bool eigrp_vty_afi_supported(struct vty *vty, const char *afi,
					   const char *command)
{
	if (afi && strcmp(afi, "ipv4") == 0)
		return true;

	eigrp_cli_result_render(vty, command, EIGRP_RESULT_UNSUPPORTED);
	return false;
}

static struct vrf *eigrp_vty_vrf_lookup(struct vty *vty, const char *vrf_name)
{
	struct vrf *vrf;

	if (vrf_name)
		vrf = vrf_lookup_by_name(vrf_name);
	else
		vrf = vrf_lookup_by_id(VRF_DEFAULT);

	if (!vrf)
		vty_out(vty, "%% VRF %s does not exist\n",
			vrf_name ? vrf_name : VRF_DEFAULT_NAME);

	return vrf;
}

static int eigrp_vty_instance_walk(struct vty *vty, const char *afi,
				   int64_t as, const char *vrf_name,
				   const char *command,
				   eigrp_vty_walk_cb cb,
				   struct eigrp_vty_walk_context *ctx)
{
	struct vrf *vrf;
	eigrp_instance_t *eigrp;
	eigrp_list_node_t *node, *nnode;
	int count = 0;

	if (!eigrp_vty_afi_supported(vty, afi, command))
		return CMD_SUCCESS;

	vrf = eigrp_vty_vrf_lookup(vty, vrf_name);
	if (!vrf)
		return CMD_WARNING;

	if (as > 0) {
		eigrp = eigrp_lookup_by_as_vrf((uint16_t)as, vrf->vrf_id);
		if (!eigrp) {
			vty_out(vty,
				"%% EIGRP address-family ipv4 autonomous-system %ld is not enabled%s%s\n",
				(long)as, vrf_name ? " in VRF " : "",
				vrf_name ? vrf_name : "");
			return CMD_SUCCESS;
		}

		cb(vty, eigrp, ctx);
		return CMD_SUCCESS;
	}

	for (EIGRP_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->vrf_id != vrf->vrf_id)
			continue;

		cb(vty, eigrp, ctx);
		count++;
	}

	if (!count)
		vty_out(vty, "%% EIGRP address-family ipv4 is not enabled%s%s\n",
			vrf_name ? " in VRF " : "", vrf_name ? vrf_name : "");

	return CMD_SUCCESS;
}

static const char *eigrp_vty_afi_name(eigrp_address_family_t afi)
{
	return afi == EIGRP_ADDRESS_FAMILY_IPV6 ? "IPv6" : "IPv4";
}

static const char *eigrp_vty_address_string(const eigrp_address_t *address,
					    char *buffer, size_t length)
{
	int family;

	if (!address || !buffer || length == 0)
		return "<invalid>";
	family = address->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	if (!inet_ntop(family, address->bytes, buffer, length))
		return "<invalid>";
	return buffer;
}

static const char *eigrp_vty_prefix_string(const eigrp_prefix_t *prefix,
					   char *buffer, size_t length)
{
	char address[INET6_ADDRSTRLEN];

	if (!prefix || !buffer || length == 0)
		return "<invalid>";
	eigrp_vty_address_string(&prefix->address, address, sizeof(address));
	snprintf(buffer, length, "%s/%u", address, prefix->prefix_length);
	return buffer;
}

static bool eigrp_vty_destination_parse(const char *text,
					eigrp_address_family_t afi,
					eigrp_prefix_t *destination)
{
	char address[INET6_ADDRSTRLEN + 4];
	char *slash;
	char *end;
	unsigned long prefix_length;
	size_t length;
	int family;
	uint8_t maximum_prefix_length;

	if (!text || !destination)
		return false;
	if (afi == EIGRP_ADDRESS_FAMILY_IPV4) {
		family = AF_INET;
		maximum_prefix_length = 32;
	} else if (afi == EIGRP_ADDRESS_FAMILY_IPV6) {
		family = AF_INET6;
		maximum_prefix_length = 128;
	} else {
		return false;
	}

	length = strlen(text);
	if (length == 0 || length >= sizeof(address))
		return false;
	memcpy(address, text, length + 1);
	slash = strchr(address, '/');
	if (slash) {
		*slash++ = '\0';
		if (!*slash)
			return false;
		prefix_length = strtoul(slash, &end, 10);
		if (*end != '\0' || prefix_length > maximum_prefix_length)
			return false;
	} else {
		prefix_length = maximum_prefix_length;
	}

	memset(destination, 0, sizeof(*destination));
	destination->address.afi = afi;
	destination->prefix_length = (uint8_t)prefix_length;
	return inet_pton(family, address, destination->address.bytes) == 1;
}

static eigrp_instance_t *
eigrp_vty_named_runtime_lookup(eigrp_address_family_config_t *af)
{
	if (!af || af->afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return NULL;

	/*
	 * Named address-family lifecycle owns runtime resolution.  Operational
	 * state consumers use the same bound instance as configuration targets
	 * instead of rediscovering a process by AS/VRF.
	 */
	return af->runtime;
}

typedef eigrp_result_t (*eigrp_vty_named_state_cb)(
	struct vty *vty, const char *instance_name,
	eigrp_address_family_config_t *af, eigrp_instance_t *runtime, void *arg);

struct eigrp_vty_named_state_walk {
	struct vty *vty;
	eigrp_vty_named_state_cb callback;
	void *arg;
};

static eigrp_result_t eigrp_vty_named_state_bridge(
	const char *instance_name, eigrp_address_family_config_t *af, void *arg)
{
	struct eigrp_vty_named_state_walk *walk = arg;

	return walk->callback(walk->vty, instance_name, af,
			      eigrp_vty_named_runtime_lookup(af), walk->arg);
}

static int eigrp_vty_named_state_walk(struct vty *vty,
				      const eigrp_state_request_t *request,
				      const char *operation,
				      eigrp_vty_named_state_cb callback, void *arg)
{
	struct eigrp_vty_named_state_walk walk = {
		.vty = vty,
		.callback = callback,
		.arg = arg,
	};
	eigrp_result_t result;

	result = eigrp_instance_address_family_walk(
		request, eigrp_vty_named_state_bridge, &walk);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty,
			"%% EIGRP %s address-family %s autonomous-system %s is not configured%s%s\n",
			operation, eigrp_vty_afi_name(request->afi),
			request->asn ? "requested" : "any",
			request->vrf_name ? " in VRF " : "",
			request->vrf_name ? request->vrf_name : "");
		return CMD_SUCCESS;
	}
	return eigrp_cli_result_render(vty, operation, result);
}

static void eigrp_vty_named_context_header(struct vty *vty,
					   const char *instance_name,
					   eigrp_address_family_config_t *af,
					   eigrp_instance_t *runtime,
					   const char *subject)
{
	vty_out(vty, "\nEIGRP-%s VR(%s) %s for AS(%u)",
		eigrp_vty_afi_name(af->afi), instance_name, subject, af->asn);
	if (strcmp(af->vrf_name, VRF_DEFAULT_NAME) != 0)
		vty_out(vty, " VRF(%s)", af->vrf_name);
	vty_out(vty, "\n");
	if (!runtime)
		vty_out(vty, "  Runtime state: %s\n",
			af->afi == EIGRP_ADDRESS_FAMILY_IPV6
				? "IPv6 data path not supported"
				: "address-family not active in the runtime");
}

struct eigrp_vty_interface_show {
	struct vty *vty;
	bool detail;
	bool printed_header;
};

static eigrp_result_t eigrp_vty_interface_state_render(
	const eigrp_interface_state_t *state, void *arg)
{
	struct eigrp_vty_interface_show *show = arg;

	if (!show->printed_header) {
		vty_out(show->vty,
			"%-22s %-8s %-7s %-10s %-10s %-7s %-7s %-8s %-7s %-7s\n",
			"Interface", "Source", "Peers", "Bandwidth", "Delay",
			"OutQ", "AckQ", "Hello", "TLV1", "TLV2");
		show->printed_header = true;
	}

	vty_out(show->vty, "%-22s %-8s ", state->interface_name,
		state->runtime_present ? "runtime" : "config");
	if (state->runtime_present)
		vty_out(show->vty,
			"%-7u %-10u %-10u %-7lu %-7lu %-8u %-7u %-7u\n",
			state->peer_count, state->bandwidth, state->delay,
			state->output_queue_count, state->reliable_queue_count,
			state->hello_interval, state->tlv1_peer_count,
			state->tlv2_peer_count);
	else
		vty_out(show->vty,
			"%-7s %-10s %-10s %-7s %-7s %-8s %-7s %-7s\n", "-",
			"-", "-", "-", "-",
			state->hello_interval_configured ? "set" : "-", "-", "-");

	if (show->detail) {
		vty_out(show->vty, "  Hello interval: ");
		if (state->runtime_present || state->hello_interval_configured)
			vty_out(show->vty, "%u seconds\n", state->hello_interval);
		else
			vty_out(show->vty, "default (not explicitly configured)\n");
		vty_out(show->vty, "  Hold time: ");
		if (state->runtime_present || state->hold_time_configured)
			vty_out(show->vty, "%u seconds\n", state->hold_time);
		else
			vty_out(show->vty, "default (not explicitly configured)\n");
		if (state->bandwidth_percent_configured)
			vty_out(show->vty, "  Bandwidth percent: %u%%\n",
				state->bandwidth_percent);
		if (state->runtime_present)
			vty_out(show->vty,
				"  MTU: %u, reliability: %u/255, load: %u/255\n",
				state->mtu, state->reliability, state->load);
		vty_out(show->vty, "  Passive: %s, configured shutdown: %s\n",
			state->passive ? "yes" : "no",
			state->config_present
				? (state->shutdown ? "yes" : "no")
				: "not retained");
		vty_out(show->vty, "  Multicast membership: %s\n",
			state->runtime_present
				? (state->multicast_enabled ? "enabled" : "disabled")
				: "runtime unavailable");
		vty_out(show->vty, "  Authentication: %s\n",
			state->authentication_configured ? "configured" : "not configured");
		vty_out(show->vty, "  Split-horizon: %s\n",
			state->split_horizon ? "enabled" : "disabled");
		if (state->runtime_present) {
			if (state->hello_timer_running)
				vty_out(show->vty, "  Next hello: %u seconds\n",
					state->hello_timer_remaining);
			else
				vty_out(show->vty, "  Next hello: not scheduled\n");
			vty_out(show->vty,
				"  Un/reliable mcasts: %" PRIu64 "/%" PRIu64
				"  Un/reliable ucasts: %" PRIu64 "/%" PRIu64 "\n",
				state->unreliable_multicast_sent,
				state->reliable_multicast_sent,
				state->unreliable_unicast_sent,
				state->reliable_unicast_sent);
			vty_out(show->vty,
				"  Mcast exceptions: %" PRIu64 "  CR packets: %" PRIu64
				"  Retransmissions sent: %" PRIu64 "\n",
				state->multicast_exceptions, state->cr_packets_sent,
				state->retransmissions_sent);
			vty_out(show->vty,
				"  Mean SRTT, pacing/flow timers, ACK suppression, and out-of-sequence counters: n/a (not maintained by the current transport)\n");
		}
	}
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_interface_context {
	const char *ifname;
	bool detail;
};

static eigrp_result_t eigrp_vty_interface_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	struct eigrp_vty_interface_context *options = arg;
	struct eigrp_vty_interface_show show = {
		.vty = vty,
		.detail = options->detail,
	};
	eigrp_result_t result;

	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Address-family Interfaces");
	result = eigrp_interface_state_walk(af, runtime, options->ifname,
					    eigrp_vty_interface_state_render, &show);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "  No EIGRP interfaces matched%s%s\n",
			options->ifname ? " " : "",
			options->ifname ? options->ifname : "");
		return EIGRP_RESULT_SUCCESS;
	}
	if (result != EIGRP_RESULT_SUCCESS)
		eigrp_cli_result_render(vty, "interface state", result);
	return EIGRP_RESULT_SUCCESS;
}

static const char *eigrp_vty_duration_string(uint64_t seconds, char *buffer,
					     size_t size)
{
	uint64_t days = seconds / 86400U;
	uint64_t hours = (seconds % 86400U) / 3600U;
	uint64_t minutes = (seconds % 3600U) / 60U;
	uint64_t secs = seconds % 60U;

	if (days)
		snprintf(buffer, size, "%llud%02lluh", (unsigned long long)days,
			 (unsigned long long)hours);
	else
		snprintf(buffer, size, "%02llu:%02llu:%02llu",
			 (unsigned long long)hours, (unsigned long long)minutes,
			 (unsigned long long)secs);
	return buffer;
}

struct eigrp_vty_neighbor_show {
	struct vty *vty;
	bool detail;
	bool static_only;
	bool printed_header;
	size_t address_width;
};

static eigrp_result_t eigrp_vty_neighbor_state_render(
	const eigrp_neighbor_state_t *state, void *arg)
{
	struct eigrp_vty_neighbor_show *show = arg;
	char address[INET6_ADDRSTRLEN];
	char uptime[32];
	char srtt[16];

	if (!show->printed_header) {
		show->address_width = state->address.afi == EIGRP_ADDRESS_FAMILY_IPV6
					      ? 40U
					      : 23U;
		if (show->static_only) {
			vty_out(show->vty, "%-40s %-22s %-12s\n", "Address",
				"Interface", "State");
		} else {
			vty_out(show->vty,
				"%-*s %-15s %-5s %-8s %-6s %-5s %-3s %-10s\n",
				(int)show->address_width,
				"Address", "Interface", "Hold", "Uptime", "SRTT",
				"RTO", "Q", "Seq");
			vty_out(show->vty, "%*s %-15s %-5s %-8s %-6s %-5s %-3s %-10s\n",
				(int)show->address_width, "", "", "(sec)", "",
				"(ms)", "", "Cnt", "Num");
		}
		show->printed_header = true;
	}

	if (show->static_only) {
		vty_out(show->vty, "%-40s %-22s %-12s\n",
			eigrp_vty_address_string(&state->address, address,
					 sizeof(address)),
			state->interface_name, state->state_name);
		return EIGRP_RESULT_SUCCESS;
	}

	if (state->srtt_valid)
		snprintf(srtt, sizeof(srtt), "%u", state->srtt_msec);
	else
		snprintf(srtt, sizeof(srtt), "n/a");
	eigrp_vty_duration_string(state->uptime_seconds, uptime, sizeof(uptime));
	vty_out(show->vty, "%-*s %-15s %-5u %-8s %-6s %-5u %-3lu %-10u\n",
		(int)show->address_width,
		eigrp_vty_address_string(&state->address, address, sizeof(address)),
		state->interface_name, state->hold_time, uptime, srtt, state->rto_msec,
		state->reliable_queue_count, state->sequence_number);
	if (show->detail) {
		vty_out(show->vty,
			"   Version %u.%u/%u.%u, Retrans: %" PRIu64
			", Retries: %u, Prefixes: %u\n",
			state->os_major, state->os_minor, state->tlv_major,
			state->tlv_minor, state->retransmit_count, state->retry_count,
			state->prefix_count);
	}
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_neighbor_context {
	const char *ifname;
	bool detail;
	bool static_only;
};

static eigrp_result_t eigrp_vty_neighbor_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	struct eigrp_vty_neighbor_context *options = arg;
	struct eigrp_vty_neighbor_show show = {
		.vty = vty,
		.detail = options->detail,
		.static_only = options->static_only,
	};
	eigrp_result_t result;

	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       options->static_only
					       ? "Static Neighbors"
					       : "Address-family Neighbors");
	result = eigrp_neighbor_state_walk(af, runtime, options->ifname,
					   options->static_only,
					   eigrp_vty_neighbor_state_render, &show);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "  No EIGRP %sneighbors matched%s%s\n",
			options->static_only ? "static " : "",
			options->ifname ? " " : "",
			options->ifname ? options->ifname : "");
		return EIGRP_RESULT_SUCCESS;
	}
	if (result != EIGRP_RESULT_SUCCESS)
		eigrp_cli_result_render(vty, "neighbor state", result);
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_topology_show {
	struct vty *vty;
	bool include_serial;
};

static eigrp_result_t eigrp_vty_topology_prefix_render(
	const eigrp_topology_prefix_state_t *state, void *arg)
{
	struct eigrp_vty_topology_show *show = arg;
	char prefix[INET6_ADDRSTRLEN + 8];

	vty_out(show->vty, "%c %s, %u successors, FD is ",
		state->active ? 'A' : 'P',
		eigrp_vty_prefix_string(&state->destination, prefix, sizeof(prefix)),
		state->successor_count);
	if (state->feasible_distance == EIGRP_MAX_METRIC)
		vty_out(show->vty, "Inaccessible");
	else
		vty_out(show->vty, "%u", state->feasible_distance);
	if (show->include_serial)
		vty_out(show->vty, ", serno %" PRIu64, state->serial_number);
	vty_out(show->vty, "\n");
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_topology_route_render(
	const eigrp_topology_route_state_t *state, void *arg)
{
	struct eigrp_vty_topology_show *show = arg;
	char address[INET6_ADDRSTRLEN];

	if (state->connected)
		vty_out(show->vty, "        via Connected, %s\n",
			state->interface_name ? state->interface_name : "<unknown>");
	else
		vty_out(show->vty, "        via %s (%u/%u), %s\n",
			eigrp_vty_address_string(&state->next_hop, address,
						 sizeof(address)),
			state->distance, state->reported_distance,
			state->interface_name ? state->interface_name : "<unknown>");
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_topology_context {
	const eigrp_prefix_t *destination;
	bool all_links;
};

static eigrp_result_t eigrp_vty_topology_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	struct eigrp_vty_topology_context *options = arg;
	struct eigrp_vty_topology_show show = {
		.vty = vty,
		.include_serial = options->all_links,
	};
	eigrp_result_t result;

	if (af)
		eigrp_vty_named_context_header(vty, instance_name, af, runtime,
					       "Topology Table");
	else
		vty_out(vty, "\nEIGRP-%s Topology Table for AS(%u)\n",
			eigrp_vty_afi_name(runtime->af_vectors.afi), runtime->AS);
	vty_out(vty,
		"Codes: P - Passive, A - Active, U - Update, Q - Query, R - Reply,\n"
		"       r - reply Status, s - sia Status\n\n");
	result = eigrp_topology_state_walk(
		af, runtime, options->destination, options->all_links,
		eigrp_vty_topology_prefix_render, eigrp_vty_topology_route_render, &show);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, options->destination ? "%% Network not in table\n"
						  : "  Topology table is empty\n");
		return EIGRP_RESULT_SUCCESS;
	}
	if (result != EIGRP_RESULT_SUCCESS)
		eigrp_cli_result_render(vty, "topology state", result);
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_topology_instance_walk {
	struct vty *vty;
	struct eigrp_vty_topology_context *options;
};

static eigrp_result_t eigrp_vty_topology_instance_render(
	eigrp_instance_t *runtime, void *arg)
{
	struct eigrp_vty_topology_instance_walk *walk = arg;
	eigrp_address_family_config_t *af = eigrp_instance_runtime_config(runtime);

	return eigrp_vty_topology_context_render(
		walk->vty, runtime->name, af, runtime, walk->options);
}

static int eigrp_vty_topology_walk(struct vty *vty,
				   const eigrp_state_request_t *request,
				   struct eigrp_vty_topology_context *options)
{
	struct eigrp_vty_topology_instance_walk walk = {
		.vty = vty,
		.options = options,
	};
	struct vrf *vrf;
	eigrp_result_t result;

	if (request->multicast)
		return eigrp_cli_result_render(vty, "topology",
					       EIGRP_RESULT_NOT_IMPLEMENTED);

	vrf = eigrp_vty_vrf_lookup(vty, request->vrf_name);
	if (!vrf)
		return CMD_WARNING;

	result = eigrp_topology_instance_walk(
		request->afi, vrf->vrf_id, request->asn,
		eigrp_vty_topology_instance_render, &walk);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty,
			"%% EIGRP topology address-family %s autonomous-system %s is not configured%s%s\n",
			eigrp_vty_afi_name(request->afi),
			request->asn ? "requested" : "any",
			request->vrf_name ? " in VRF " : "",
			request->vrf_name ? request->vrf_name : "");
		return CMD_SUCCESS;
	}

	return eigrp_cli_result_render(vty, "topology", result);
}

static bool eigrp_vty_state_request_build(
	const char *afi_text, int64_t asn, const char *vrf_name,
	eigrp_state_request_t *request)
{
	if (!afi_text || !request)
		return false;

	memset(request, 0, sizeof(*request));
	if (strcmp(afi_text, "ipv4") == 0)
		request->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	else if (strcmp(afi_text, "ipv6") == 0)
		request->afi = EIGRP_ADDRESS_FAMILY_IPV6;
	else
		return false;
	request->asn = asn > 0 ? (uint16_t)asn : 0;
	request->vrf_name = vrf_name;
	return true;
}

static bool eigrp_vty_multicast_requested(struct cmd_token *argv[], int argc)
{
	int index = 0;

	return argv_find(argv, argc, "multicast", &index);
}

#ifdef EIGRP_STANDALONE_BUILD
/*
 * The standalone compile harness does not run FRR clippy.  These symbols
 * mirror the parsed variables that clippy normally passes to DEFPY handlers
 * so the command bodies can still be syntax-checked.
 */
static const char *afi = "ipv4";
static const char *vrf = NULL;
static int64_t as = 0;
static const char *as_str = NULL;
static const char *ifname = NULL;
static const char *detail = NULL;
static const char *all = NULL;
static const char *target = NULL;
static const char *soft = NULL;
static const char *vrf_all = NULL;
static const char *ipv4_prefix_str = NULL;
static const char *ipv6_prefix_str = NULL;
static struct in_addr network;
static const char *network_str = NULL;
static struct in_addr mask;
static const char *mask_str = NULL;
static struct in_addr ipv4_addr;
static const char *ipv4_addr_str = NULL;
static struct in6_addr ipv6_addr;
static const char *ipv6_addr_str = NULL;
static int64_t route_instance = 0;
static int64_t bw = 0;
static const char *bw_str = NULL;
static int64_t delay = 0;
static const char *delay_str = NULL;
static int64_t rlbt = 0;
static const char *rlbt_str = NULL;
static int64_t load = 0;
static const char *load_str = NULL;
static int64_t mtu = 0;
static const char *mtu_str = NULL;
static const char *route_map = NULL;
static const char *no = NULL;
#endif

/*
 * Syntax: `[no] redistribute eigrp AS [metric ...] [route-map NAME]`
 * Mode: Named topology
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Target: eigrp_redistribute_add() / eigrp_redistribute_remove()
 */
DEFPY(eigrp_named_redistribute_eigrp,
      eigrp_named_redistribute_eigrp_cmd,
      "[no] redistribute eigrp (1-65535)$route_instance [metric (1-4294967295)$bw (0-4294967295)$delay (0-255)$rlbt (1-255)$load (1-65535)$mtu] [route-map WORD$route_map]",
      NO_STR
      REDIST_STR
      "Enhanced Interior Gateway Routing Protocol (EIGRP)\n"
      "Source EIGRP autonomous system\n"
      "Metric for redistributed routes\n"
      "Bandwidth metric in Kbits per second\n"
      "EIGRP delay metric, in 10 microsecond units\n"
      "EIGRP reliability metric where 255 is 100% reliable\n"
      "EIGRP effective bandwidth metric where 255 is 100% loaded\n"
      "EIGRP MTU of the path\n"
      "Route-map\n"
      "Route-map name\n")
{
    return eigrp_cli_named_redistribute_apply(
        vty, "eigrp", route_instance, bw, bw_str, delay, delay_str, rlbt,
        rlbt_str, load, load_str, mtu, mtu_str, route_map, no);
}


/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] interfaces [IFNAME$ifname] [detail]$detail`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_interface_state_walk()
 */
DEFPY(show_eigrp_interface,
      show_eigrp_interface_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] interfaces [IFNAME$ifname] [detail]$detail",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Display multicast instances\n"
      "Display EIGRP interfaces\n"
      "Interface name\n"
      "Detailed information\n")
{
	eigrp_state_request_t request;
	struct eigrp_vty_interface_context options = {
		.ifname = ifname,
		.detail = detail != NULL,
	};

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_named_state_walk(vty, &request, "interfaces",
					  eigrp_vty_interface_context_render, &options);
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] neighbors [static] [detail]$detail [IFNAME$ifname]`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_neighbor_state_walk()
 */
DEFPY(show_eigrp_neighbor,
      show_eigrp_neighbor_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] neighbors [static] [detail]$detail [IFNAME$ifname]",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Display multicast instances\n"
      "Display EIGRP neighbors\n"
      "Display static neighbors\n"
      "Detailed information\n"
      "Interface name\n")
{
	eigrp_state_request_t request;
	int index = 0;
	struct eigrp_vty_neighbor_context options = {
		.ifname = ifname,
		.detail = detail != NULL,
		.static_only = argv_find(argv, argc, "static", &index),
	};

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_named_state_walk(vty, &request, "neighbors",
					  eigrp_vty_neighbor_context_render, &options);
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [multicast] topology [(1-65535)$as] [all-links]$all`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_topology_state_walk()
 */
DEFPY(show_eigrp_topology_all,
      show_eigrp_topology_all_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [multicast] topology [(1-65535)$as] [all-links]$all",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      "Display multicast instances\n"
      "Display EIGRP topology table\n"
      AS_STR
      "Display all topology links\n")
{
	eigrp_state_request_t request;
	struct eigrp_vty_topology_context options = {
		.all_links = all != NULL,
	};

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_topology_walk(vty, &request, &options);
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [multicast] topology [(1-65535)$as] WORD$target [all-links]$all`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_topology_state_walk()
 */
DEFPY(show_eigrp_topology,
      show_eigrp_topology_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [multicast] topology [(1-65535)$as] WORD$target [all-links]$all",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      "Display multicast instances\n"
      "Display EIGRP topology table\n"
      AS_STR
      "Network address or prefix\n"
      "Display all topology links\n")
{
	eigrp_prefix_t destination;
	eigrp_state_request_t request;
	struct eigrp_vty_topology_context options = {
		.destination = &destination,
		.all_links = all != NULL,
	};

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	if (!eigrp_vty_destination_parse(target, request.afi, &destination)) {
		vty_out(vty, "%% Malformed topology destination: %s\n", target);
		return CMD_WARNING;
	}

	return eigrp_vty_topology_walk(vty, &request, &options);
}

struct eigrp_vty_accounting_show {
	struct vty *vty;
};

static eigrp_result_t eigrp_vty_accounting_state_render(
	const eigrp_statistics_accounting_state_t *state, void *arg)
{
	struct eigrp_vty_accounting_show *show = arg;
	char address[INET6_ADDRSTRLEN];

	const char *code = strcmp(state->neighbor_state, "Up") == 0
				   ? "A"
				   : strcmp(state->neighbor_state, "Waiting for Init") == 0
					     ? "P"
					     : "D";

	vty_out(show->vty, "%-5s %-40s %-22s %-10u %-9s %s\n", code,
		eigrp_vty_address_string(&state->neighbor_address, address,
					 sizeof(address)),
		state->interface_name, state->prefix_count, "n/a", "n/a");
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_accounting_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	eigrp_instance_context_t context = {
		.config = af,
		.runtime = runtime,
		.topology_id = EIGRP_TOPOLOGY_ID_BASE,
	};
	struct eigrp_vty_accounting_show show = {.vty = vty};
	uint32_t total_prefix_count = 0;
	eigrp_result_t result;

	(void)arg;
	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Accounting");
	vty_out(vty, "States: A-Adjacency, P-Pending, D-Down\n");
	vty_out(vty, "%-5s %-40s %-22s %-10s %-9s %s\n", "State",
		"Address/Source", "Interface", "Prefixes", "Restart", "Restart/Reset(s)");
	result = eigrp_statistics_accounting_show(
		&context, &total_prefix_count, eigrp_vty_accounting_state_render, &show);
	if (result == EIGRP_RESULT_SUCCESS) {
		vty_out(vty, "Total Prefix Count: %u\n", total_prefix_count);
		vty_out(vty,
			"Restart fields are n/a until neighbor maximum-prefix enforcement is implemented.\n");
	} else if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "  Runtime accounting data is not available\n");
	} else {
		eigrp_cli_result_render(vty, "accounting", result);
	}
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_traffic_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	eigrp_instance_context_t context = {
		.config = af,
		.runtime = runtime,
		.topology_id = EIGRP_TOPOLOGY_ID_BASE,
	};
	eigrp_statistics_traffic_state_t state;
	eigrp_result_t result;

	(void)arg;
	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Traffic Statistics");
	result = eigrp_statistics_traffic_show(&context, &state);
	if (result == EIGRP_RESULT_SUCCESS) {
		vty_out(vty, "  Hellos sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_hello, state.received_hello);
		vty_out(vty, "  Updates sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_update, state.received_update);
		vty_out(vty, "  Queries sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_query, state.received_query);
		vty_out(vty, "  Replies sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_reply, state.received_reply);
		vty_out(vty, "  Acks sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_ack, state.received_ack);
		vty_out(vty, "  SIA-Queries sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_sia_query, state.received_sia_query);
		vty_out(vty, "  SIA-Replies sent/received: %" PRIu64 "/%" PRIu64 "\n",
			state.sent_sia_reply, state.received_sia_reply);
	} else if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "  Runtime traffic counters are not available\n");
	} else {
		eigrp_cli_result_render(vty, "traffic", result);
	}
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_timer_show {
	struct vty *vty;
	bool printed_header;
};

static eigrp_result_t eigrp_vty_timer_state_render(const eigrp_timer_state_t *state,
						   void *arg)
{
	struct eigrp_vty_timer_show *show = arg;
	char address[INET6_ADDRSTRLEN];

	if (!show->printed_header) {
		vty_out(show->vty, "%-10s %-12s %-22s %s\n", "Process", "Expires",
			"Type", "Detail");
		show->printed_header = true;
	}

	if (state->type == EIGRP_TIMER_STATE_HELLO) {
		vty_out(show->vty, "%-10s %-12u %-22s %s\n", "Hello",
			state->expiration_seconds, "Hello", state->interface_name);
		return EIGRP_RESULT_SUCCESS;
	}

	if (state->type == EIGRP_TIMER_STATE_PEER_HOLD) {
		vty_out(show->vty, "%-10s %-12u %-22s %s via %s\n", "Update",
			state->expiration_seconds, "Peer holding",
			state->neighbor_present
				? eigrp_vty_address_string(&state->neighbor_address, address,
						 sizeof(address))
				: "-",
			state->interface_name ? state->interface_name : "-");
	}
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_timer_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	eigrp_instance_context_t context = {
		.config = af,
		.runtime = runtime,
		.topology_id = EIGRP_TOPOLOGY_ID_BASE,
	};
	struct eigrp_vty_timer_show show = {.vty = vty};
	eigrp_result_t result;

	(void)arg;
	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Address-family Timers");
	result = eigrp_timer_show(&context, eigrp_vty_timer_state_render, &show);
	if (result != EIGRP_RESULT_SUCCESS)
		eigrp_cli_result_render(vty, "timers", result);
	if (!show.printed_header)
		vty_out(vty, "  No active IPv4 EIGRP timers are currently scheduled\n");
	vty_out(vty,
		"  SIA process expiration: n/a until ACTIVE-time enforcement is implemented\n");
	return EIGRP_RESULT_SUCCESS;
}

struct eigrp_vty_eventlog_show {
	struct vty *vty;
};

static eigrp_result_t eigrp_vty_eventlog_entry_render(
	uint32_t event_number, const eigrp_eventlog_entry_t *entry,
	const char *format, void *arg)
{
	struct eigrp_vty_eventlog_show *show = arg;
	char text[256];
	eigrp_result_t result;

	(void)format;
	if (!show || !show->vty || !entry)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_eventlog_entry_format(entry, text, sizeof(text));
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	vty_out(show->vty, "%u %s\n", event_number, text);
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_event_context_render(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	eigrp_instance_context_t context = {
		.config = af,
		.runtime = runtime,
		.topology_id = EIGRP_TOPOLOGY_ID_BASE,
	};
	eigrp_eventlog_state_t state;
	struct eigrp_vty_eventlog_show show = {.vty = vty};
	eigrp_result_t result;

	(void)arg;
	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Events");
	if (!runtime) {
		vty_out(vty, "  Event history is unavailable while the address-family is not running\n");
		return EIGRP_RESULT_SUCCESS;
	}

	result = eigrp_eventlog_state_read(&context, &state);
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_cli_result_render(vty, "event history", result);
		return EIGRP_RESULT_SUCCESS;
	}
	vty_out(vty, "Event information for AS %u:\n", runtime->AS);
	if (!state.count) {
		vty_out(vty, "  No events recorded\n");
		return EIGRP_RESULT_SUCCESS;
	}
	result = eigrp_eventlog_show(&context, eigrp_vty_eventlog_entry_render,
				     &show);
	if (result != EIGRP_RESULT_SUCCESS)
		eigrp_cli_result_render(vty, "event history", result);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] accounting`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_statistics_accounting_show()
 */
DEFPY(show_eigrp_accounting,
      show_eigrp_accounting_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] accounting",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP accounting\n")
{
	eigrp_state_request_t request;

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_named_state_walk(vty, &request, "accounting",
					  eigrp_vty_accounting_context_render, NULL);
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] events`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_eventlog_show()
 */
DEFPY(show_eigrp_event,
      show_eigrp_event_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] events",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP events\n")
{
	eigrp_state_request_t request;

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_named_state_walk(vty, &request, "events",
					  eigrp_vty_event_context_render, NULL);
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] timers`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_timer_show()
 */
DEFPY(show_eigrp_timer,
      show_eigrp_timer_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] timers",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP timers\n")
{
	eigrp_state_request_t request;

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_named_state_walk(vty, &request, "timers",
					  eigrp_vty_timer_context_render, NULL);
}

/*
 * Syntax: `show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] traffic`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_statistics_traffic_show()
 */
DEFPY(show_eigrp_traffic,
      show_eigrp_traffic_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] traffic",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP traffic\n")
{
	eigrp_state_request_t request;

	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	request.multicast = eigrp_vty_multicast_requested(argv, argc);
	return eigrp_vty_named_state_walk(vty, &request, "traffic",
					  eigrp_vty_traffic_context_render, NULL);
}

struct eigrp_vty_protocol_show {
	struct vty *vty;
	bool printed_header;
};

static eigrp_result_t eigrp_vty_protocol_state_render(
	const eigrp_status_protocol_state_t *state, void *arg)
{
	struct eigrp_vty_protocol_show *show = arg;
	eigrp_instance_t *runtime = eigrp_vty_named_runtime_lookup(state->config);
	char router_id[INET_ADDRSTRLEN] = "automatic";
	struct in_addr router_id_address;

	if (!show->printed_header) {
		vty_out(show->vty,
			"%-20s %-6s %-7s %-18s %-10s %-16s\n", "Instance", "AF",
			"AS", "VRF", "State", "Router-ID");
		show->printed_header = true;
	}
	if (state->router_id_configured) {
		router_id_address.s_addr = htonl(state->router_id);
		inet_ntop(AF_INET, &router_id_address, router_id, sizeof(router_id));
	} else if (runtime && runtime->router_id.s_addr != INADDR_ANY) {
		inet_ntop(AF_INET, &runtime->router_id, router_id, sizeof(router_id));
	}
	vty_out(show->vty, "%-20s %-6s %-7u %-18s %-10s %-16s\n",
		state->instance_name, eigrp_vty_afi_name(state->afi), state->asn,
		state->vrf_name,
		state->shutdown ? "shutdown" : (runtime ? "active" : "configured"),
		router_id);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax: `show eigrp protocols`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_status_protocol_show()
 */
DEFPY(show_eigrp_protocol,
      show_eigrp_protocol_cmd,
      "show eigrp protocols",
      SHOW_STR EIGRP_STR "Display EIGRP protocol information\n")
{
	struct eigrp_vty_protocol_show show = {.vty = vty};
	eigrp_result_t result = eigrp_status_protocol_show(
		eigrp_vty_protocol_state_render, &show);

	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "No named EIGRP address families are configured\n");
		return CMD_SUCCESS;
	}
	return eigrp_cli_result_render(vty, "protocols", result);
}

static eigrp_result_t eigrp_vty_tech_support_context(
	const eigrp_status_protocol_state_t *state, void *arg)
{
	struct vty *vty = arg;
	eigrp_instance_t *runtime = eigrp_vty_named_runtime_lookup(state->config);
	struct eigrp_vty_interface_context interface_options = {.detail = true};
	struct eigrp_vty_neighbor_context neighbor_options = {.detail = true};
	struct eigrp_vty_topology_context topology_options = {.all_links = true};

	vty_out(vty, "\n============================================================\n");
	vty_out(vty, "EIGRP technical support: %s %s AS %u VRF %s\n",
		state->instance_name, eigrp_vty_afi_name(state->afi), state->asn,
		state->vrf_name);
	eigrp_vty_interface_context_render(vty, state->instance_name, state->config,
					 runtime, &interface_options);
	eigrp_vty_neighbor_context_render(vty, state->instance_name, state->config,
					runtime, &neighbor_options);
	eigrp_vty_topology_context_render(vty, state->instance_name, state->config,
					runtime, &topology_options);
	eigrp_vty_traffic_context_render(vty, state->instance_name, state->config,
				       runtime, NULL);
	eigrp_vty_timer_context_render(vty, state->instance_name, state->config,
				     runtime, NULL);
	eigrp_vty_accounting_context_render(vty, state->instance_name, state->config,
					  runtime, NULL);
	eigrp_vty_event_context_render(vty, state->instance_name, state->config,
				     runtime, NULL);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax: `show eigrp tech-support`
 * Mode: EXEC
 * XPath: none; read-only
 * Target: eigrp_status_tech_support_show()
 */
DEFPY(show_eigrp_tech_support,
      show_eigrp_tech_support_cmd,
      "show eigrp tech-support",
      SHOW_STR EIGRP_STR "Display EIGRP tech-support information\n")
{
	eigrp_result_t result = eigrp_status_tech_support_show(
		eigrp_vty_tech_support_context, vty);

	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "No named EIGRP address families are configured\n");
		return CMD_SUCCESS;
	}
	return eigrp_cli_result_render(vty, "tech-support", result);
}


static bool eigrp_vty_ipv4_mask_prefix_length(const char *text,
					       uint8_t *prefix_length)
{
	struct in_addr address;
	uint32_t mask;
	uint8_t length = 0;
	bool zero_seen = false;
	int bit;

	if (!text || !prefix_length || inet_pton(AF_INET, text, &address) != 1)
		return false;
	mask = ntohl(address.s_addr);
	for (bit = 31; bit >= 0; bit--) {
		if (mask & (1U << bit)) {
			if (zero_seen)
				return false;
			length++;
		} else {
			zero_seen = true;
		}
	}
	*prefix_length = length;
	return true;
}

struct eigrp_vty_topology_clear_context {
	const eigrp_prefix_t *destination;
	size_t affected;
};

static eigrp_result_t clear_eigrp_topology_context(
	struct vty *vty, const char *instance_name, eigrp_address_family_config_t *af,
	eigrp_instance_t *runtime, void *arg)
{
	struct eigrp_vty_topology_clear_context *clear = arg;
	eigrp_instance_context_t context = {
		.config = af,
		.runtime = runtime,
		.topology_id = EIGRP_TOPOLOGY_ID_BASE,
	};
	eigrp_topology_clear_request_t request = {
		.destination = clear->destination,
	};
	size_t affected = 0;
	eigrp_result_t result;

	(void)vty;
	(void)instance_name;
	result = eigrp_topology_clear(&context, &request, &affected);
	if (result == EIGRP_RESULT_NOT_FOUND && runtime && clear->destination)
		return EIGRP_RESULT_SUCCESS;
	if (result == EIGRP_RESULT_SUCCESS)
		clear->affected += affected;
	return result;
}

static int clear_eigrp_topology_execute(struct vty *vty, const char *afi_text,
					int64_t asn, const char *vrf_name,
					bool all_vrfs,
					const eigrp_prefix_t *destination)
{
	eigrp_state_request_t state_request;
	struct eigrp_vty_topology_clear_context clear = {
		.destination = destination,
	};
	int rv;

	if (!eigrp_vty_state_request_build(afi_text, asn, vrf_name,
					   &state_request))
		return CMD_WARNING;
	state_request.all_vrfs = all_vrfs;
	rv = eigrp_vty_named_state_walk(vty, &state_request, "topology clear",
					clear_eigrp_topology_context, &clear);
	if (rv == CMD_SUCCESS && destination && clear.affected == 0)
		vty_out(vty, "%% Network not in EIGRP topology table\n");
	return rv;
}

/*
 * Syntax: `clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_topology_clear()
 */
DEFPY(clear_eigrp_topology,
      clear_eigrp_topology_cmd,
      "clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology",
      CLEAR_STR
      EIGRP_STR
      AS_STR
      "Virtual Routing and Forwarding\n"
      "VRF name\n"
      "All VRFs\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Clear EIGRP topology entries and relearn them\n")
{
	return clear_eigrp_topology_execute(vty, afi, as, vrf, vrf_all != NULL,
					    NULL);
}

/*
 * Syntax: `clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology <A.B.C.D/M$ipv4_prefix|X:X::X:X/M$ipv6_prefix>`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_topology_clear()
 */
DEFPY(clear_eigrp_topology_prefix,
      clear_eigrp_topology_prefix_cmd,
      "clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology <A.B.C.D/M$ipv4_prefix|X:X::X:X/M$ipv6_prefix>",
      CLEAR_STR
      EIGRP_STR
      AS_STR
      "Virtual Routing and Forwarding\n"
      "VRF name\n"
      "All VRFs\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Clear EIGRP topology entries and relearn them\n"
      "IPv4 prefix\n"
      "IPv6 prefix\n")
{
	eigrp_prefix_t destination;
	const char *prefix_text = ipv4_prefix_str ? ipv4_prefix_str : ipv6_prefix_str;
	eigrp_address_family_t address_family;

	address_family = strcmp(afi, "ipv6") == 0 ? EIGRP_ADDRESS_FAMILY_IPV6
						 : EIGRP_ADDRESS_FAMILY_IPV4;
	if (!prefix_text
	    || !eigrp_vty_destination_parse(prefix_text, address_family,
					    &destination)) {
		vty_out(vty, "%% Prefix does not match the selected EIGRP address family\n");
		return CMD_WARNING;
	}
	return clear_eigrp_topology_execute(vty, afi, as, vrf, vrf_all != NULL,
					    &destination);
}

/*
 * Syntax: `clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology A.B.C.D$network A.B.C.D$mask`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_topology_clear()
 */
DEFPY(clear_eigrp_topology_mask,
      clear_eigrp_topology_mask_cmd,
      "clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology A.B.C.D$network A.B.C.D$mask",
      CLEAR_STR
      EIGRP_STR
      AS_STR
      "Virtual Routing and Forwarding\n"
      "VRF name\n"
      "All VRFs\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Clear EIGRP topology entries and relearn them\n"
      "IPv4 network\n"
      "IPv4 network mask\n")
{
	eigrp_prefix_t destination;
	uint8_t prefix_length;

	(void)network;
	(void)mask;
	if (strcmp(afi, "ipv4") != 0) {
		vty_out(vty, "%% Dotted network masks are valid only for IPv4\n");
		return CMD_WARNING;
	}
	if (!eigrp_vty_destination_parse(network_str, EIGRP_ADDRESS_FAMILY_IPV4,
					 &destination)
	    || !eigrp_vty_ipv4_mask_prefix_length(mask_str, &prefix_length)) {
		vty_out(vty, "%% Invalid IPv4 prefix or network mask\n");
		return CMD_WARNING;
	}
	destination.prefix_length = prefix_length;
	return clear_eigrp_topology_execute(vty, afi, as, vrf, vrf_all != NULL,
					    &destination);
}

static void clear_eigrp_neighbor_render(
	const eigrp_neighbor_clear_state_t *state, void *arg)
{
	struct vty *vty = arg;
	char address[INET6_ADDRSTRLEN];

	vty_time_print(vty, 0);
	vty_out(vty, "Neighbor %s (%s) is %s: manually cleared\n",
		eigrp_vty_address_string(&state->address, address,
					       sizeof(address)),
		state->interface_name ? state->interface_name : "?",
		state->soft ? "resync" : "down");
}

static void clear_eigrp_neighbor_result_apply(
	struct eigrp_vty_walk_context *ctx, eigrp_result_t result,
	size_t affected, bool soft_scope_matches)
{
	if (result == EIGRP_RESULT_NOT_FOUND)
		return;
	if (result != EIGRP_RESULT_SUCCESS) {
		ctx->result = result;
		return;
	}

	ctx->matched += (int)affected;
	/* Preserve the existing soft-clear scope match even when it has no peers. */
	if (ctx->soft && soft_scope_matches && affected == 0)
		ctx->matched++;
}

static void clear_eigrp_neighbor_apply(struct vty *vty,
				       eigrp_instance_t *eigrp,
				       struct eigrp_vty_walk_context *ctx,
				       const eigrp_neighbor_clear_request_t *request,
				       bool soft_scope_matches)
{
	eigrp_result_t result;
	size_t affected = 0;

	result = eigrp_neighbor_clear(eigrp, request, clear_eigrp_neighbor_render,
				      vty, &affected);
	clear_eigrp_neighbor_result_apply(ctx, result, affected,
					  soft_scope_matches);
}

static void clear_eigrp_neighbor_all_cb(struct vty *vty, eigrp_instance_t *eigrp,
					struct eigrp_vty_walk_context *ctx)
{
	const eigrp_neighbor_clear_request_t request = {
		.soft = ctx->soft,
	};

	clear_eigrp_neighbor_apply(vty, eigrp, ctx, &request, true);
}

static void clear_eigrp_neighbor_interface_cb(struct vty *vty,
					      eigrp_instance_t *eigrp,
					      struct eigrp_vty_walk_context *ctx)
{
	eigrp_neighbor_clear_request_t request = {
		.interface_name = ctx->ifname,
		.soft = ctx->soft,
	};

	clear_eigrp_neighbor_apply(vty, eigrp, ctx, &request, true);
}

static void clear_eigrp_neighbor_address_cb(struct vty *vty,
					    eigrp_instance_t *eigrp,
					    struct eigrp_vty_walk_context *ctx)
{
	eigrp_result_t result;
	size_t affected = 0;

	result = eigrp_northbound_neighbor_clear_address(
		eigrp, ctx->address_afi, ctx->ipv4_address, ctx->ipv6_address,
		ctx->soft, clear_eigrp_neighbor_render, vty, &affected);
	clear_eigrp_neighbor_result_apply(ctx, result, affected, false);
}

/*
 * Syntax: `clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors [soft]$soft`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_neighbor_clear()
 */
DEFPY(clear_eigrp_neighbor,
      clear_eigrp_neighbor_cmd,
      "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors [soft]$soft",
      CLEAR_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Clear EIGRP neighbors\n"
      "Resync with peers without adjacency reset\n")
{
	struct eigrp_vty_walk_context ctx = {
		.soft = !!soft,
	};
	int rv;

	rv = eigrp_vty_instance_walk(vty, afi, as, vrf,
				      "clear eigrp address-family neighbors",
				      clear_eigrp_neighbor_all_cb, &ctx);
	if (rv == CMD_SUCCESS && ctx.result != EIGRP_RESULT_SUCCESS)
		return eigrp_cli_result_render(vty,
					       "clear eigrp address-family neighbors",
					       ctx.result);
	if (rv == CMD_SUCCESS && ctx.matched == 0)
		vty_out(vty, "%% No EIGRP neighbors matched\n");
	return rv;
}

/*
 * Syntax: `clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors IFNAME$ifname [soft]$soft`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_neighbor_clear()
 */
DEFPY(clear_eigrp_neighbor_interface,
      clear_eigrp_neighbor_interface_cmd,
      "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors IFNAME$ifname [soft]$soft",
      CLEAR_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Clear EIGRP neighbors\n"
      "Interface name\n"
      "Resync with peers without adjacency reset\n")
{
	struct eigrp_vty_walk_context ctx = {
		.ifname = ifname,
		.soft = !!soft,
	};
	int rv;

	rv = eigrp_vty_instance_walk(vty, afi, as, vrf,
				      "clear eigrp address-family neighbors",
				      clear_eigrp_neighbor_interface_cb, &ctx);
	if (rv == CMD_SUCCESS && ctx.result != EIGRP_RESULT_SUCCESS)
		return eigrp_cli_result_render(vty,
					       "clear eigrp address-family neighbors",
					       ctx.result);
	if (rv == CMD_SUCCESS && ctx.matched == 0)
		vty_out(vty, "%% No EIGRP neighbors matched interface %s\n", ifname);
	return rv;
}

/*
 * Syntax: `clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors <A.B.C.D$ipv4_addr|X:X::X:X$ipv6_addr> [soft]$soft`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_neighbor_clear()
 */
DEFPY(clear_eigrp_neighbor_address,
      clear_eigrp_neighbor_address_cmd,
      "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors <A.B.C.D$ipv4_addr|X:X::X:X$ipv6_addr> [soft]$soft",
      CLEAR_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Clear EIGRP neighbors\n"
      "IPv4 EIGRP neighbor address\n"
      "IPv6 EIGRP neighbor address\n"
      "Resync with peers without adjacency reset\n")
{
	eigrp_address_family_t address_afi;
	const char *address_text;
	struct eigrp_vty_walk_context ctx = {0};
	int rv;

	if (strcmp(afi, "ipv6") == 0) {
		if (!ipv6_addr_str) {
			vty_out(vty,
				"%% IPv6 EIGRP requires an IPv6 neighbor address\n");
			return CMD_WARNING;
		}
		address_afi = EIGRP_ADDRESS_FAMILY_IPV6;
		address_text = ipv6_addr_str;
		ctx.ipv6_address = &ipv6_addr;
	} else {
		if (!ipv4_addr_str) {
			vty_out(vty,
				"%% IPv4 EIGRP requires an IPv4 neighbor address\n");
			return CMD_WARNING;
		}
		address_afi = EIGRP_ADDRESS_FAMILY_IPV4;
		address_text = ipv4_addr_str;
		ctx.ipv4_address = &ipv4_addr;
	}

	ctx.target = address_text;
	ctx.address_afi = address_afi;
	ctx.soft = !!soft;

	rv = eigrp_vty_instance_walk(vty, afi, as, vrf,
				      "clear eigrp address-family neighbors",
				      clear_eigrp_neighbor_address_cb, &ctx);
	if (rv == CMD_SUCCESS && ctx.result != EIGRP_RESULT_SUCCESS)
		return eigrp_cli_result_render(vty,
					       "clear eigrp address-family neighbors",
					       ctx.result);
	if (rv == CMD_SUCCESS && ctx.matched == 0)
		vty_out(vty, "%% No EIGRP neighbor matched %s\n", address_text);
	return rv;
}



static void clear_eigrp_events_cb(struct vty *vty, eigrp_instance_t *eigrp,
				  struct eigrp_vty_walk_context *ctx)
{
	eigrp_instance_context_t context = {
		.runtime = eigrp,
		.topology_id = EIGRP_TOPOLOGY_ID_BASE,
	};

	(void)vty;
	if (eigrp_eventlog_clear(&context) == EIGRP_RESULT_SUCCESS)
		ctx->matched++;
}

/*
 * Syntax: `clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] events`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_eventlog_clear()
 */
DEFPY(clear_eigrp_address_family_events,
      clear_eigrp_address_family_events_cmd,
      "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] events",
      CLEAR_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Clear EIGRP event log\n")
{
	struct eigrp_vty_walk_context ctx = {0};
	int rv;

	rv = eigrp_vty_instance_walk(vty, afi, as, vrf,
				     "clear eigrp address-family events",
				     clear_eigrp_events_cb, &ctx);
	return rv;
}

/*
 * Syntax: `clear eigrp events`
 * Mode: Privileged EXEC
 * XPath: none; operational action
 * Target: eigrp_eventlog_clear()
 */
DEFUN(clear_eigrp_events,
      clear_eigrp_events_cmd,
      "clear eigrp events",
      CLEAR_STR EIGRP_STR "Clear EIGRP event log\n")
{
	eigrp_instance_t *eigrp;
	eigrp_list_node_t *node, *nnode;
	eigrp_instance_context_t context;

	for (EIGRP_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		memset(&context, 0, sizeof(context));
		context.runtime = eigrp;
		context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
		(void)eigrp_eventlog_clear(&context);
	}
	return CMD_SUCCESS;
}


void eigrp_cli_named_init(void)
{
    /* The classic CLI module owns EIGRP_NODE and shared command grammars. */
    install_element(CONFIG_NODE, &router_eigrp_named_cmd);
    install_element(CONFIG_NODE, &no_router_eigrp_named_cmd);
    install_element(EIGRP_NODE, &router_eigrp_named_cmd);

    install_element(ENABLE_NODE, &clear_eigrp_events_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_address_family_events_cmd);

    install_element(EIGRP_NODE, &eigrp_address_family_ipv4_cmd);
    install_element(EIGRP_NODE, &no_eigrp_address_family_ipv4_cmd);
    install_element(EIGRP_NODE, &eigrp_address_family_ipv6_cmd);
    install_element(EIGRP_NODE, &no_eigrp_address_family_ipv6_cmd);
    install_element(EIGRP_NODE, &eigrp_exit_address_family_cmd);
    install_element(EIGRP_NODE, &eigrp_no_shutdown_cmd);
    install_element(EIGRP_NODE, &eigrp_shutdown_cmd);
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
    install_element(EIGRP_NODE, &eigrp_af_interface_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_cmd);
    install_element(EIGRP_NODE, &eigrp_exit_af_interface_cmd);
    install_element(EIGRP_NODE, &eigrp_af_interface_bandwidth_percent_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_bandwidth_percent_cmd);
    install_element(EIGRP_NODE, &eigrp_af_interface_bandwidth_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_bandwidth_cmd);
    install_element(EIGRP_NODE, &eigrp_af_interface_delay_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_delay_cmd);
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
    install_element(EIGRP_NODE, &eigrp_af_interface_next_hop_self_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_next_hop_self_cmd);
    install_element(EIGRP_NODE, &eigrp_af_interface_split_horizon_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_split_horizon_cmd);
    install_element(EIGRP_NODE, &eigrp_af_interface_summary_address_cmd);
    install_element(EIGRP_NODE, &eigrp_af_interface_summary_address_ipv6_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_summary_address_cmd);
    install_element(EIGRP_NODE, &no_eigrp_af_interface_summary_address_ipv6_cmd);
    install_element(EIGRP_NODE, &eigrp_topology_base_cmd);
    install_element(EIGRP_NODE, &eigrp_exit_af_topology_cmd);
    install_element(EIGRP_NODE, &eigrp_auto_summary_cmd);
    install_element(EIGRP_NODE, &no_eigrp_auto_summary_cmd);
    install_element(EIGRP_NODE, &eigrp_default_information_cmd);
    install_element(EIGRP_NODE, &no_eigrp_default_information_cmd);
    install_element(EIGRP_NODE, &eigrp_default_metric_cmd);
    install_element(EIGRP_NODE, &no_eigrp_default_metric_cmd);
    install_element(EIGRP_NODE, &eigrp_distance_cmd);
    install_element(EIGRP_NODE, &no_eigrp_distance_cmd);
    install_element(EIGRP_NODE, &eigrp_maximum_prefix_cmd);
    install_element(EIGRP_NODE, &no_eigrp_maximum_prefix_cmd);
    install_element(EIGRP_NODE, &eigrp_metric_maximum_hops_cmd);
    install_element(EIGRP_NODE, &no_eigrp_metric_maximum_hops_cmd);
    install_element(EIGRP_NODE, &eigrp_metric_holddown_cmd);
    install_element(EIGRP_NODE, &no_eigrp_metric_holddown_cmd);
    install_element(EIGRP_NODE, &eigrp_event_log_size_cmd);
    install_element(EIGRP_NODE, &no_eigrp_event_log_size_cmd);
    install_element(EIGRP_NODE, &eigrp_offset_list_cmd);
    install_element(EIGRP_NODE, &no_eigrp_offset_list_cmd);
    install_element(EIGRP_NODE, &eigrp_named_redistribute_eigrp_cmd);
    install_element(EIGRP_NODE, &eigrp_redistribute_maximum_prefix_cmd);
    install_element(EIGRP_NODE, &no_eigrp_redistribute_maximum_prefix_cmd);
    install_element(EIGRP_NODE, &eigrp_summary_metric_cmd);
    install_element(EIGRP_NODE, &eigrp_summary_metric_ipv6_cmd);
    install_element(EIGRP_NODE, &eigrp_summary_metric_distance_cmd);
    install_element(EIGRP_NODE, &eigrp_summary_metric_distance_ipv6_cmd);
    install_element(EIGRP_NODE, &no_eigrp_summary_metric_cmd);
    install_element(EIGRP_NODE, &no_eigrp_summary_metric_ipv6_cmd);
    install_element(EIGRP_NODE, &eigrp_traffic_share_balanced_cmd);
    install_element(EIGRP_NODE, &no_eigrp_traffic_share_balanced_cmd);

    /* Named-mode EXEC commands. */
    install_element(VIEW_NODE, &show_eigrp_interface_cmd);
    install_element(VIEW_NODE, &show_eigrp_neighbor_cmd);
    install_element(VIEW_NODE, &show_eigrp_topology_cmd);
    install_element(VIEW_NODE, &show_eigrp_topology_all_cmd);
    install_element(VIEW_NODE, &show_eigrp_accounting_cmd);
    install_element(VIEW_NODE, &show_eigrp_event_cmd);
    install_element(VIEW_NODE, &show_eigrp_timer_cmd);
    install_element(VIEW_NODE, &show_eigrp_traffic_cmd);
    install_element(VIEW_NODE, &show_eigrp_protocol_cmd);
    install_element(VIEW_NODE, &show_eigrp_tech_support_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_topology_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_topology_prefix_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_topology_mask_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_neighbor_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_neighbor_interface_cmd);
    install_element(ENABLE_NODE, &clear_eigrp_neighbor_address_cmd);
}
