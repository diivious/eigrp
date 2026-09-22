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
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_eventlog.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_table.h"
#include "eigrpd/eigrp_frr.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_const.h"
#ifndef EIGRP_STANDALONE_BUILD
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "eigrpd/eigrp_vty_clippy.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#ifdef EIGRP_STANDALONE_BUILD
/* Standalone compile shim for variables normally supplied by FRR clippy. */
static const char *vrf = NULL;
static int64_t as = 0;
static const char *as_str = NULL;
static const char *all = NULL;
static const char *address_str = NULL;
static const char *prefix_str = "0.0.0.0/0";
static const char *ifname = NULL;
static const char *detail = NULL;
static struct in_addr nbr_addr;
static const char *nbr_addr_str = "0.0.0.0";
#endif

static void eigrp_vty_display_prefix_entry(struct vty *vty, eigrp_instance_t *eigrp,
					   eigrp_prefix_descriptor_t *pe,
					   bool all)
{
	bool first = true;
	struct eigrp_route_descriptor *te;
	eigrp_list_node_t *node;

	for (EIGRP_LIST_ELEMENTS_RO(pe->entries, node, te)) {
		if (all
		    || (((te->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
			 == EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
			|| ((te->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG)
			    == EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG))) {
			show_ip_eigrp_route_descriptor(vty, eigrp, te, &first, all);
			first = false;
		}
	}
}

static struct vrf *eigrp_vty_get_vrf(struct vty *vty, const char *vrf_name)
{
	struct vrf *vrf;

	if (vrf_name)
		vrf = vrf_lookup_by_name(vrf_name);
	else
		vrf = vrf_lookup_by_id(VRF_DEFAULT);


	if (!vrf) {
		vty_out(vty, "VRF %s specified does not exist",
			vrf_name ? vrf_name : VRF_DEFAULT_NAME);
		return NULL;
	}

	return vrf;
}

static eigrp_instance_t *eigrp_vty_get_eigrp(struct vty *vty, const char *vrf_name)
{
	struct vrf *vrf = eigrp_vty_get_vrf(vty, vrf_name);

	if (!vrf)
		return NULL;

	return eigrp_lookup(vrf->vrf_id);
}

static void eigrp_topology_helper(struct vty *vty, eigrp_instance_t *eigrp,
				  const char *all)
{
	eigrp_prefix_descriptor_t *tn;
	eigrp_table_node_t *rn;

	show_ip_eigrp_topology_header(vty, eigrp);

	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		if (!rn->info)
			continue;

		tn = rn->info;
		eigrp_vty_display_prefix_entry(vty, eigrp, tn,
					       all ? true : false);
	}
}

struct eigrp_vty_topology_walk_context {
	struct vty *vty;
	const char *all;
	const eigrp_prefix_t *prefix;
	const char *vrf_name;
	bool print_vrf;
	bool vrf_printed;
	unsigned int matched;
};

static void eigrp_vty_topology_vrf_header(
	struct eigrp_vty_topology_walk_context *ctx)
{
	if (!ctx->print_vrf || ctx->vrf_printed)
		return;

	vty_out(ctx->vty, "VRF %s:\n", ctx->vrf_name);
	ctx->vrf_printed = true;
}

static eigrp_result_t eigrp_vty_topology_instance_render(
	eigrp_instance_t *eigrp, void *arg)
{
	struct eigrp_vty_topology_walk_context *ctx = arg;

	eigrp_vty_topology_vrf_header(ctx);
	eigrp_topology_helper(ctx->vty, eigrp, ctx->all);
	ctx->matched++;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_topology_prefix_instance_render(
	eigrp_instance_t *eigrp, void *arg)
{
	struct eigrp_vty_topology_walk_context *ctx = arg;
	eigrp_prefix_descriptor_t *tn;
	eigrp_table_node_t *rn;

	eigrp_vty_topology_vrf_header(ctx);
	show_ip_eigrp_topology_header(ctx->vty, eigrp);
	ctx->matched++;

	rn = eigrp_table_node_match(eigrp->topology_table, ctx->prefix);
	if (!rn) {
		vty_out(ctx->vty, "%% Network not in table\n");
		return EIGRP_RESULT_SUCCESS;
	}

	tn = rn->info;
	if (!tn) {
		vty_out(ctx->vty, "%% Network not in table\n");
		return EIGRP_RESULT_SUCCESS;
	}

	eigrp_vty_display_prefix_entry(ctx->vty, eigrp, tn, ctx->all != NULL);
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_topology_walk_vrf(
	struct eigrp_vty_topology_walk_context *ctx, eigrp_vrf_id_t vrf_id,
	uint16_t asn, eigrp_topology_instance_walk_cb callback)
{
	return eigrp_topology_instance_walk(EIGRP_ADDRESS_FAMILY_IPV4, vrf_id,
					    asn, callback, ctx);
}

DEFPY (show_ip_eigrp_topology_all,
       show_ip_eigrp_topology_all_cmd,
       "show ip eigrp [vrf NAME] topology [(1-65535)$as] [all-links$all]",
       SHOW_STR
       IP_STR
       "IP-EIGRP show commands\n"
       VRF_CMD_HELP_STR
       "IP-EIGRP topology\n"
       AS_STR
       "Show all links in topology table\n")
{
	struct eigrp_vty_topology_walk_context ctx = {
		.vty = vty,
		.all = all,
	};
	eigrp_result_t result;
	uint16_t asn = as > 0 ? (uint16_t)as : 0;

	if (vrf && strncmp(vrf, "all", sizeof("all")) == 0) {
		struct vrf *v;

		RB_FOREACH (v, vrf_name_head, &vrfs_by_name) {
			ctx.vrf_name = v->name;
			ctx.print_vrf = true;
			ctx.vrf_printed = false;
			(void)eigrp_vty_topology_walk_vrf(
				&ctx, v->vrf_id, asn,
				eigrp_vty_topology_instance_render);
		}
	} else {
		struct vrf *v = eigrp_vty_get_vrf(vty, vrf);

		if (!v)
			return CMD_WARNING;
		result = eigrp_vty_topology_walk_vrf(
			&ctx, v->vrf_id, asn, eigrp_vty_topology_instance_render);
		if (result == EIGRP_RESULT_NOT_FOUND) {
			vty_out(vty, " EIGRP Routing Process not enabled\n");
			return CMD_SUCCESS;
		}
		if (result != EIGRP_RESULT_SUCCESS)
			return CMD_WARNING;
	}

	if (!ctx.matched)
		vty_out(vty, " EIGRP Routing Process not enabled\n");

	return CMD_SUCCESS;
}

DEFPY (show_ip_eigrp_topology,
       show_ip_eigrp_topology_cmd,
       "show ip eigrp [vrf NAME] topology [(1-65535)$as] <A.B.C.D$address|A.B.C.D/M$prefix>",
       SHOW_STR
       IP_STR
       "IP-EIGRP show commands\n"
       VRF_CMD_HELP_STR
       "IP-EIGRP topology\n"
       AS_STR
       "For a specific address\n"
       "For a specific prefix\n")
{
	struct eigrp_vty_topology_walk_context ctx = {
		.vty = vty,
		.all = NULL,
	};
	eigrp_result_t result;
	struct prefix cmp;
	eigrp_prefix_t eigrp_prefix;
	struct vrf *v;
	uint16_t asn = as > 0 ? (uint16_t)as : 0;

	if (vrf && strncmp(vrf, "all", sizeof("all")) == 0) {
		vty_out(vty, "Specifying vrf `all` for a particular address/prefix makes no sense\n");
		return CMD_SUCCESS;
	}

	if (address_str)
		prefix_str = address_str;

	if (str2prefix(prefix_str, &cmp) < 0) {
		vty_out(vty, "%% Malformed address\n");
		return CMD_WARNING;
	}

	if (eigrp_frr_prefix_import(&cmp, &eigrp_prefix) != EIGRP_RESULT_SUCCESS) {
		vty_out(vty, "%% Unsupported prefix\n");
		return CMD_WARNING;
	}

	ctx.prefix = &eigrp_prefix;
	v = eigrp_vty_get_vrf(vty, vrf);
	if (!v)
		return CMD_WARNING;
	result = eigrp_vty_topology_walk_vrf(
		&ctx, v->vrf_id, asn, eigrp_vty_topology_prefix_instance_render);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}
	if (result != EIGRP_RESULT_SUCCESS)
		return CMD_WARNING;
	return CMD_SUCCESS;
}

static void eigrp_interface_helper(struct vty *vty, eigrp_instance_t *eigrp,
				   const char *ifname, const char *detail)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!ifname)
		show_ip_eigrp_interface_header(vty, eigrp);

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		if (!ifname || strcmp(ei->name, ifname) == 0) {
			show_ip_eigrp_interface_sub(vty, eigrp, ei);
			if (detail)
				show_ip_eigrp_interface_detail(vty, eigrp, ei);
		}
	}
}

DEFPY (show_ip_eigrp_interfaces,
       show_ip_eigrp_interfaces_cmd,
       "show ip eigrp [vrf NAME] interfaces [IFNAME] [detail]$detail",
       SHOW_STR
       IP_STR
       "IP-EIGRP show commands\n"
       VRF_CMD_HELP_STR
       "IP-EIGRP interfaces\n"
       "Interface name to look at\n"
       "Detailed information\n")
{
	eigrp_instance_t *eigrp;

	if (vrf && strncmp(vrf, "all", sizeof("all")) == 0) {
		struct vrf *v;

		RB_FOREACH (v, vrf_name_head, &vrfs_by_name) {
			eigrp = eigrp_lookup(v->vrf_id);
			if (!eigrp)
				continue;

			vty_out(vty, "VRF %s:\n", v->name);

			eigrp_interface_helper(vty, eigrp, ifname, detail);
		}
	} else {
		eigrp = eigrp_vty_get_eigrp(vty, vrf);
		if (eigrp == NULL) {
			vty_out(vty, "EIGRP Routing Process not enabled\n");
			return CMD_SUCCESS;
		}

		eigrp_interface_helper(vty, eigrp, ifname, detail);
	}


	return CMD_SUCCESS;
}

static void eigrp_neighbors_helper(struct vty *vty, eigrp_instance_t *eigrp,
				   const char *ifname, const char *detail)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;

	show_ip_eigrp_neighbor_header(vty, eigrp);

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		if (!ifname || strcmp(ei->name, ifname) == 0) {
			for (EIGRP_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
				if (detail || (nbr->state == EIGRP_NEIGHBOR_UP))
					show_ip_eigrp_neighbor_sub(vty, nbr,
								   !!detail);
			}
		}
	}
}

DEFPY (show_ip_eigrp_neighbors,
       show_ip_eigrp_neighbors_cmd,
       "show ip eigrp [vrf NAME] neighbors [IFNAME] [detail]$detail",
       SHOW_STR
       IP_STR
       "IP-EIGRP show commands\n"
       VRF_CMD_HELP_STR
       "IP-EIGRP neighbors\n"
       "Interface to show on\n"
       "Detailed Information\n")
{
	eigrp_instance_t *eigrp;

	if (vrf && strncmp(vrf, "all", sizeof("all")) == 0) {
		struct vrf *vrf_iter;

		RB_FOREACH (vrf_iter, vrf_name_head, &vrfs_by_name) {
			eigrp = eigrp_lookup(vrf_iter->vrf_id);
			if (!eigrp)
				continue;

			vty_out(vty, "VRF %s:\n", vrf_iter->name);

			eigrp_neighbors_helper(vty, eigrp, ifname, detail);
		}
	} else {
		eigrp = eigrp_vty_get_eigrp(vty, vrf);
		if (eigrp == NULL) {
			vty_out(vty, " EIGRP Routing Process not enabled\n");
			return CMD_SUCCESS;
		}

		eigrp_neighbors_helper(vty, eigrp, ifname, detail);
	}

	return CMD_SUCCESS;
}

static void eigrp_vty_neighbor_clear_render(
	const eigrp_neighbor_clear_state_t *state, void *arg)
{
	struct vty *vty = arg;
	char address[INET6_ADDRSTRLEN];
	int family;

	family = state->address.afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 :
		 state->address.afi == EIGRP_ADDRESS_FAMILY_IPV4 ? AF_INET : AF_UNSPEC;
	if (family == AF_INET6 || family == AF_INET) {
		if (!inet_ntop(family, state->address.bytes, address, sizeof(address)))
			strlcpy(address, "<invalid>", sizeof(address));
	} else {
		strlcpy(address, "<invalid>", sizeof(address));
	}
	vty_time_print(vty, 0);
	vty_out(vty, "Neighbor %s (%s) is %s: manually cleared\n", address,
		state->interface_name ? state->interface_name : "?",
		state->soft ? "resync" : "down");
}

static int eigrp_vty_neighbor_clear_result(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS : CMD_WARNING;
}

/*
 * Execute hard restart for all neighbors
 */
DEFPY (clear_ip_eigrp_neighbors,
       clear_ip_eigrp_neighbors_cmd,
       "clear ip eigrp [vrf NAME] neighbors",
       CLEAR_STR
       IP_STR
       "Clear IP-EIGRP\n"
       VRF_CMD_HELP_STR
       "Clear IP-EIGRP neighbors\n")
{
	eigrp_instance_t *eigrp;
	const eigrp_neighbor_clear_request_t request = {0};
	eigrp_result_t result;

	/* Check if eigrp process is enabled */
	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (eigrp == NULL) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}

	result = eigrp_neighbor_clear(eigrp, &request,
				      eigrp_vty_neighbor_clear_render, vty, NULL);
	return eigrp_vty_neighbor_clear_result(result);
}

/*
 * Execute hard restart for all neighbors on interface
 */
DEFPY (clear_ip_eigrp_neighbors_int,
       clear_ip_eigrp_neighbors_int_cmd,
       "clear ip eigrp [vrf NAME] neighbors IFNAME",
       CLEAR_STR
       IP_STR
       "Clear IP-EIGRP\n"
       VRF_CMD_HELP_STR
       "Clear IP-EIGRP neighbors\n"
       "Interface's name\n")
{
	eigrp_instance_t *eigrp;
	eigrp_neighbor_clear_request_t request = {
		.interface_name = ifname,
	};
	eigrp_result_t result;

	/* Check if eigrp process is enabled */
	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (eigrp == NULL) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}

	result = eigrp_neighbor_clear(eigrp, &request,
				      eigrp_vty_neighbor_clear_render, vty, NULL);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, " Interface (%s) doesn't exist\n", ifname);
		return CMD_WARNING;
	}
	return eigrp_vty_neighbor_clear_result(result);
}

/*
 * Execute hard restart for neighbor specified by IP
 */
DEFPY (clear_ip_eigrp_neighbors_IP,
       clear_ip_eigrp_neighbors_IP_cmd,
       "clear ip eigrp [vrf NAME] neighbors A.B.C.D$nbr_addr",
       CLEAR_STR
       IP_STR
       "Clear IP-EIGRP\n"
       VRF_CMD_HELP_STR
       "Clear IP-EIGRP neighbors\n"
       "IP-EIGRP neighbor address\n")
{
	eigrp_instance_t *eigrp;
	eigrp_address_t address = {
		.afi = EIGRP_ADDRESS_FAMILY_IPV4,
	};
	eigrp_neighbor_clear_request_t request = {
		.address = &address,
	};
	eigrp_result_t result;

	memcpy(address.bytes, &nbr_addr, sizeof(nbr_addr));

	/* Check if eigrp process is enabled */
	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (eigrp == NULL) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}

	result = eigrp_neighbor_clear(eigrp, &request,
				      eigrp_vty_neighbor_clear_render, vty, NULL);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "Neighbor with entered address doesn't exists.\n");
		return CMD_WARNING;
	}
	return eigrp_vty_neighbor_clear_result(result);
}

/*
 * Execute graceful restart for all neighbors
 */
DEFPY (clear_ip_eigrp_neighbors_soft,
       clear_ip_eigrp_neighbors_soft_cmd,
       "clear ip eigrp [vrf NAME] neighbors soft",
       CLEAR_STR
       IP_STR
       "Clear IP-EIGRP\n"
       VRF_CMD_HELP_STR
       "Clear IP-EIGRP neighbors\n"
       "Resync with peers without adjacency reset\n")
{
	eigrp_instance_t *eigrp;
	const eigrp_neighbor_clear_request_t request = {
		.soft = true,
	};
	eigrp_result_t result;

	/* Check if eigrp process is enabled */
	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (eigrp == NULL) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}

	result = eigrp_neighbor_clear(eigrp, &request,
				      eigrp_vty_neighbor_clear_render, vty, NULL);
	return eigrp_vty_neighbor_clear_result(result);
}

/*
 * Execute graceful restart for all neighbors on interface
 */
DEFPY (clear_ip_eigrp_neighbors_int_soft,
       clear_ip_eigrp_neighbors_int_soft_cmd,
       "clear ip eigrp [vrf NAME] neighbors IFNAME soft",
       CLEAR_STR
       IP_STR
       "Clear IP-EIGRP\n"
       VRF_CMD_HELP_STR
       "Clear IP-EIGRP neighbors\n"
       "Interface's name\n"
       "Resync with peer without adjacency reset\n")
{
	eigrp_instance_t *eigrp;
	eigrp_neighbor_clear_request_t request = {
		.interface_name = ifname,
		.soft = true,
	};
	eigrp_result_t result;

	/* Check if eigrp process is enabled */
	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (eigrp == NULL) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}

	result = eigrp_neighbor_clear(eigrp, &request,
				      eigrp_vty_neighbor_clear_render, vty, NULL);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, " Interface (%s) doesn't exist\n", ifname);
		return CMD_WARNING;
	}
	return eigrp_vty_neighbor_clear_result(result);
}

/*
 * Execute graceful restart for neighbor specified by IP
 */
DEFPY (clear_ip_eigrp_neighbors_IP_soft,
       clear_ip_eigrp_neighbors_IP_soft_cmd,
       "clear ip eigrp [vrf NAME] neighbors A.B.C.D$nbr_addr soft",
       CLEAR_STR
       IP_STR
       "Clear IP-EIGRP\n"
       VRF_CMD_HELP_STR
       "Clear IP-EIGRP neighbors\n"
       "IP-EIGRP neighbor address\n"
       "Resync with peer without adjacency reset\n")
{
	eigrp_instance_t *eigrp;
	eigrp_address_t address = {
		.afi = EIGRP_ADDRESS_FAMILY_IPV4,
	};
	eigrp_neighbor_clear_request_t request = {
		.address = &address,
		.soft = true,
	};
	eigrp_result_t result;

	memcpy(address.bytes, &nbr_addr, sizeof(nbr_addr));

	/* Check if eigrp process is enabled */
	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (eigrp == NULL) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}

	result = eigrp_neighbor_clear(eigrp, &request,
				      eigrp_vty_neighbor_clear_render, vty, NULL);
	if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "Neighbor with entered address doesn't exists.\n");
		return CMD_WARNING;
	}
	return eigrp_vty_neighbor_clear_result(result);
}

struct eigrp_vty_eventlog_output {
	struct vty *vty;
};

static eigrp_result_t eigrp_vty_eventlog_output_entry(
	uint32_t event_number, const eigrp_eventlog_entry_t *entry,
	const char *format, void *arg)
{
	struct eigrp_vty_eventlog_output *output = arg;
	char text[256];
	eigrp_result_t result;

	(void)format;
	if (!output || !output->vty || !entry)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_eventlog_entry_format(entry, text, sizeof(text));
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	vty_out(output->vty, "%u %s\n", event_number, text);
	return EIGRP_RESULT_SUCCESS;
}

DEFPY(show_ip_eigrp_events,
      show_ip_eigrp_events_cmd,
      "show ip eigrp [vrf NAME$vrf] events",
      SHOW_STR IP_STR "IP-EIGRP show commands\n" VRF_CMD_HELP_STR
      "Display EIGRP event log\n")
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};
	eigrp_eventlog_state_t state;
	struct eigrp_vty_eventlog_output output = {.vty = vty};
	eigrp_result_t result;

	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (!eigrp) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}
	context.runtime = eigrp;
	context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
	result = eigrp_eventlog_state_read(&context, &state);
	if (result != EIGRP_RESULT_SUCCESS)
		return CMD_WARNING;

	vty_out(vty, "Event information for AS %u:\n", eigrp->AS);
	if (!state.count) {
		vty_out(vty, "  No events recorded\n");
		return CMD_SUCCESS;
	}
	result = eigrp_eventlog_show(&context, eigrp_vty_eventlog_output_entry,
				     &output);
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS : CMD_WARNING;
}

DEFPY(clear_ip_eigrp_events,
      clear_ip_eigrp_events_cmd,
      "clear ip eigrp [vrf NAME$vrf] events",
      CLEAR_STR IP_STR "IP-EIGRP clear commands\n" VRF_CMD_HELP_STR
      "Clear EIGRP event log\n")
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	eigrp = eigrp_vty_get_eigrp(vty, vrf);
	if (!eigrp) {
		vty_out(vty, " EIGRP Routing Process not enabled\n");
		return CMD_SUCCESS;
	}
	context.runtime = eigrp;
	context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return eigrp_eventlog_clear(&context) == EIGRP_RESULT_SUCCESS
		       ? CMD_SUCCESS
		       : CMD_WARNING;
}

void eigrp_vty_show_init(void)
{
	install_element(VIEW_NODE, &show_ip_eigrp_interfaces_cmd);

	install_element(VIEW_NODE, &show_ip_eigrp_neighbors_cmd);

	install_element(VIEW_NODE, &show_ip_eigrp_topology_cmd);
	install_element(VIEW_NODE, &show_ip_eigrp_topology_all_cmd);
	install_element(VIEW_NODE, &show_ip_eigrp_events_cmd);
}

/* Install EIGRP related vty commands. */
void eigrp_vty_init(void)
{
	install_element(ENABLE_NODE, &clear_ip_eigrp_events_cmd);
	/* commands for manual hard restart */
	install_element(ENABLE_NODE, &clear_ip_eigrp_neighbors_cmd);
	install_element(ENABLE_NODE, &clear_ip_eigrp_neighbors_int_cmd);
	install_element(ENABLE_NODE, &clear_ip_eigrp_neighbors_IP_cmd);
	/* commands for manual graceful restart */
	install_element(ENABLE_NODE, &clear_ip_eigrp_neighbors_soft_cmd);
	install_element(ENABLE_NODE, &clear_ip_eigrp_neighbors_int_soft_cmd);
	install_element(ENABLE_NODE, &clear_ip_eigrp_neighbors_IP_soft_cmd);
}
