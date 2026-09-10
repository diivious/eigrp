// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP VTY Interface.
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
 */
#include <zebra.h>

#include "memory.h"
#include "frrevent.h"
#include "prefix.h"
#include "table.h"
#include "vty.h"
#include "command.h"
#include "plist.h"
#include "log.h"
#include "zclient.h"
#include "keychain.h"
#include "linklist.h"
#include "distribute.h"

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_cli.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_operational.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_const.h"
#ifndef EIGRP_STANDALONE_BUILD
/*
 * FRR clippy generates this file during the real FRR build.  Generated
 * parser wrappers are not EIGRP-owned source style, so suppress warnings
 * that can be emitted by clippy formatting rather than by eigrp_vty.c.
 */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "eigrpd/eigrp_vty_clippy.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

struct eigrp_vty_walk_context {
	const char *ifname;
	const char *detail;
	const char *all;
	const char *target;
	const struct prefix *prefix;
	bool soft;
	int matched;
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
	struct listnode *node, *nnode;
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

	for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
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

static void eigrp_vty_display_prefix_entry(struct vty *vty,
					   eigrp_instance_t *eigrp,
					   eigrp_prefix_descriptor_t *pe,
					   bool all)
{
	bool first = true;
	struct eigrp_route_descriptor *te;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(pe->entries, node, te)) {
		if (all
		    || (((te->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
			 == EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
			|| ((te->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG)
			    == EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG))) {
			show_ip_eigrp_route_descriptor(vty, eigrp, te, &first);
			first = false;
		}
	}
}

static void eigrp_topology_helper(struct vty *vty, eigrp_instance_t *eigrp,
				  const char *all)
{
	eigrp_prefix_descriptor_t *tn;
	struct route_node *rn;

	show_ip_eigrp_topology_header(vty, eigrp);

	for (rn = route_top(eigrp->topology_table); rn; rn = route_next(rn)) {
		if (!rn->info)
			continue;

		tn = rn->info;
		eigrp_vty_display_prefix_entry(vty, eigrp, tn, all ? true : false);
	}
}

static void eigrp_interface_helper(struct vty *vty, eigrp_instance_t *eigrp,
				   const char *ifname, const char *detail)
{
	eigrp_interface_t *ei;
	struct listnode *node;

	if (!ifname)
		show_ip_eigrp_interface_header(vty, eigrp);

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		if (!ifname || strcmp(ei->ifp->name, ifname) == 0) {
			show_ip_eigrp_interface_sub(vty, eigrp, ei);
			if (detail)
				show_ip_eigrp_interface_detail(vty, eigrp, ei);
		}
	}
}

static void eigrp_neighbor_helper(struct vty *vty, eigrp_instance_t *eigrp,
				  const char *ifname, const char *detail)
{
	eigrp_interface_t *ei;
	struct listnode *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;

	show_ip_eigrp_neighbor_header(vty, eigrp);

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		if (!ifname || strcmp(ei->ifp->name, ifname) == 0) {
			for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
				if (detail || nbr->state == EIGRP_NEIGHBOR_UP)
					show_ip_eigrp_neighbor_sub(vty, nbr,
								   !!detail);
			}
		}
	}
}

static void show_eigrp_interface_cb(struct vty *vty, eigrp_instance_t *eigrp,
				    struct eigrp_vty_walk_context *ctx)
{
	eigrp_interface_helper(vty, eigrp, ctx->ifname, ctx->detail);
}

static void show_eigrp_neighbor_cb(struct vty *vty, eigrp_instance_t *eigrp,
				   struct eigrp_vty_walk_context *ctx)
{
	eigrp_neighbor_helper(vty, eigrp, ctx->ifname, ctx->detail);
}

static void show_eigrp_topology_all_cb(struct vty *vty, eigrp_instance_t *eigrp,
				       struct eigrp_vty_walk_context *ctx)
{
	eigrp_topology_helper(vty, eigrp, ctx->all);
}

static void show_eigrp_topology_prefix_cb(struct vty *vty,
					  eigrp_instance_t *eigrp,
					  struct eigrp_vty_walk_context *ctx)
{
	eigrp_prefix_descriptor_t *tn;
	struct route_node *rn;

	show_ip_eigrp_topology_header(vty, eigrp);

	rn = route_node_match(eigrp->topology_table, ctx->prefix);
	if (!rn || !rn->info) {
		vty_out(vty, "%% Network not in table\n");
		if (rn)
			route_unlock_node(rn);
		return;
	}

	tn = rn->info;
	eigrp_vty_display_prefix_entry(vty, eigrp, tn, ctx->all ? true : false);
	route_unlock_node(rn);
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
static struct in_addr address;
static const char *address_str = NULL;
static struct prefix_ipv4 eigrp_standalone_prefix_storage;
static const struct prefix_ipv4 *prefix = &eigrp_standalone_prefix_storage;
static const char *prefix_str = NULL;
static const char *soft = NULL;
static struct in_addr nbr_addr;
static const char *nbr_addr_str = NULL;
#endif

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
	struct eigrp_vty_walk_context ctx = {
		.ifname = ifname,
		.detail = detail,
	};

	return eigrp_vty_instance_walk(vty, afi, as, vrf,
					"show eigrp address-family interfaces",
					show_eigrp_interface_cb, &ctx);
}

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
	struct eigrp_vty_walk_context ctx = {
		.ifname = ifname,
		.detail = detail,
	};

	return eigrp_vty_instance_walk(vty, afi, as, vrf,
					"show eigrp address-family neighbors",
					show_eigrp_neighbor_cb, &ctx);
}

DEFPY(show_eigrp_topology_all,
      show_eigrp_topology_all_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] topology [all-links]$all",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Display multicast instances\n"
      "Display EIGRP topology table\n"
      "Display all topology links\n")
{
	struct eigrp_vty_walk_context ctx = {
		.all = all,
	};

	return eigrp_vty_instance_walk(vty, afi, as, vrf,
					"show eigrp address-family topology",
					show_eigrp_topology_all_cb, &ctx);
}

DEFPY(show_eigrp_topology,
      show_eigrp_topology_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] topology <A.B.C.D$address|A.B.C.D/M$prefix> [all-links]$all",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Display multicast instances\n"
      "Display EIGRP topology table\n"
      "Network address\n"
      "Network prefix\n"
      "Display all topology links\n")
{
	struct prefix p;
	struct eigrp_vty_walk_context ctx = {
		.all = all,
		.prefix = &p,
	};

	(void)address;
	(void)prefix;

	if (address_str) {
		if (str2prefix(address_str, &p) < 0) {
			vty_out(vty, "%% Malformed address\n");
			return CMD_WARNING;
		}
	} else if (prefix_str) {
		if (str2prefix(prefix_str, &p) < 0) {
			vty_out(vty, "%% Malformed prefix\n");
			return CMD_WARNING;
		}
	} else {
		return CMD_WARNING;
	}

	return eigrp_vty_instance_walk(vty, afi, as, vrf,
					"show eigrp address-family topology",
					show_eigrp_topology_prefix_cb, &ctx);
}

static bool eigrp_vty_operational_request_build(
	const char *afi_text, int64_t asn, const char *vrf_name,
	eigrp_operational_request_t *request)
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

DEFPY(show_eigrp_accounting,
      show_eigrp_accounting_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] accounting",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP accounting\n")
{
	eigrp_operational_request_t request;

	if (!eigrp_vty_operational_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_cli_result_render(vty, "accounting",
				       eigrp_accounting_show(&request));
}

DEFPY(show_eigrp_event,
      show_eigrp_event_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] events",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP events\n")
{
	eigrp_operational_request_t request;

	if (!eigrp_vty_operational_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_cli_result_render(vty, "events",
				       eigrp_event_show(&request));
}

DEFPY(show_eigrp_timer,
      show_eigrp_timer_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] timers",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP timers\n")
{
	eigrp_operational_request_t request;

	if (!eigrp_vty_operational_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_cli_result_render(vty, "timers",
				       eigrp_timer_show(&request));
}

DEFPY(show_eigrp_traffic,
      show_eigrp_traffic_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] traffic",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP traffic\n")
{
	eigrp_operational_request_t request;

	if (!eigrp_vty_operational_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_cli_result_render(vty, "traffic",
				       eigrp_traffic_show(&request));
}

DEFPY(show_eigrp_protocol,
      show_eigrp_protocol_cmd,
      "show eigrp protocols",
      SHOW_STR EIGRP_STR "Display EIGRP protocol information\n")
{
	return eigrp_cli_result_render(vty, "protocols", eigrp_protocol_show());
}

DEFPY(show_eigrp_tech_support,
      show_eigrp_tech_support_cmd,
      "show eigrp tech-support",
      SHOW_STR EIGRP_STR "Display EIGRP tech-support information\n")
{
	return eigrp_cli_result_render(vty, "tech-support",
				       eigrp_tech_support_show());
}

static void clear_eigrp_neighbor_all_cb(struct vty *vty, eigrp_instance_t *eigrp,
					struct eigrp_vty_walk_context *ctx)
{
	eigrp_interface_t *ei;
	struct listnode *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;

	if (ctx->soft) {
		eigrp_update_send_process_GR(eigrp, EIGRP_GR_MANUAL, vty);
		ctx->matched++;
		return;
	}

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);

		for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
			if (nbr->state == EIGRP_NEIGHBOR_DOWN)
				continue;

			zlog_debug("Neighbor %pI4 (%s) is down: manually cleared",
				   &nbr->src.ip.v4,
				   ifindex2ifname(nbr->ei->ifp->ifindex,
						  eigrp->vrf_id));
			vty_time_print(vty, 0);
			vty_out(vty,
				"Neighbor %pI4 (%s) is down: manually cleared\n",
				&nbr->src.ip.v4,
				ifindex2ifname(nbr->ei->ifp->ifindex,
					       eigrp->vrf_id));

			eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
			eigrp_nbr_delete(nbr);
			ctx->matched++;
		}
	}
}

static void clear_eigrp_neighbor_interface_cb(struct vty *vty,
					      eigrp_instance_t *eigrp,
					      struct eigrp_vty_walk_context *ctx)
{
	eigrp_interface_t *ei;
	struct listnode *node2, *nnode2;
	eigrp_neighbor_t *nbr;

	ei = eigrp_intf_lookup_by_name(eigrp, ctx->ifname);
	if (!ei)
		return;

	if (ctx->soft) {
		eigrp_update_send_interface_GR(ei, EIGRP_GR_MANUAL, vty);
		ctx->matched++;
		return;
	}

	eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);

	for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
		if (nbr->state == EIGRP_NEIGHBOR_DOWN)
			continue;

		zlog_debug("Neighbor %pI4 (%s) is down: manually cleared",
			   &nbr->src.ip.v4,
			   ifindex2ifname(nbr->ei->ifp->ifindex,
					  eigrp->vrf_id));
		vty_time_print(vty, 0);
		vty_out(vty, "Neighbor %pI4 (%s) is down: manually cleared\n",
			&nbr->src.ip.v4,
			ifindex2ifname(nbr->ei->ifp->ifindex, eigrp->vrf_id));

		eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
		eigrp_nbr_delete(nbr);
		ctx->matched++;
	}
}

static void clear_eigrp_neighbor_address_cb(struct vty *vty,
					    eigrp_instance_t *eigrp,
					    struct eigrp_vty_walk_context *ctx)
{
	struct in_addr addr;
	eigrp_neighbor_t *nbr;

	if (inet_aton(ctx->target, &addr) == 0)
		return;

	nbr = eigrp_nbr_lookup_by_addr_process(eigrp, addr);
	if (!nbr)
		return;

	if (ctx->soft)
		eigrp_update_send_GR(nbr, EIGRP_GR_MANUAL, vty);
	else
		eigrp_nbr_hard_restart(eigrp, nbr, vty);

	ctx->matched++;
}

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
	if (rv == CMD_SUCCESS && ctx.matched == 0)
		vty_out(vty, "%% No EIGRP neighbors matched\n");
	return rv;
}

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
	if (rv == CMD_SUCCESS && ctx.matched == 0)
		vty_out(vty, "%% No EIGRP neighbors matched interface %s\n", ifname);
	return rv;
}

DEFPY(clear_eigrp_neighbor_address,
      clear_eigrp_neighbor_address_cmd,
      "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors A.B.C.D$nbr_addr [soft]$soft",
      CLEAR_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Clear EIGRP neighbors\n"
      "EIGRP neighbor address\n"
      "Resync with peers without adjacency reset\n")
{
	struct eigrp_vty_walk_context ctx = {
		.target = nbr_addr_str,
		.soft = !!soft,
	};
	int rv;

	(void)nbr_addr;

	rv = eigrp_vty_instance_walk(vty, afi, as, vrf,
				      "clear eigrp address-family neighbors",
				      clear_eigrp_neighbor_address_cb, &ctx);
	if (rv == CMD_SUCCESS && ctx.matched == 0)
		vty_out(vty, "%% No EIGRP neighbor matched %s\n", nbr_addr_str);
	return rv;
}

void eigrp_vty_show_init(void)
{
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
}

void eigrp_vty_init(void)
{
	install_element(ENABLE_NODE, &clear_eigrp_neighbor_cmd);
	install_element(ENABLE_NODE, &clear_eigrp_neighbor_interface_cmd);
	install_element(ENABLE_NODE, &clear_eigrp_neighbor_address_cmd);
}
