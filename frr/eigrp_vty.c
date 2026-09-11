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
#include "printfrr.h"
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
#include "eigrpd/eigrp_event.h"
#include "eigrpd/eigrp_statistics.h"
#include "eigrpd/eigrp_status.h"
#include "eigrpd/eigrp_timer.h"
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
	struct vrf *vrf;

	if (!af || af->afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return NULL;
	vrf = vrf_lookup_by_name(af->vrf_name);
	if (!vrf)
		return NULL;
	return eigrp_lookup_by_as_vrf(af->asn, vrf->vrf_id);
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
		if (state->bandwidth_configured)
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
		vty_out(show->vty,
			"  SRTT, pacing time, flow timer, and suppression counters: not exposed by the current backend\n");
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

struct eigrp_vty_neighbor_show {
	struct vty *vty;
	bool detail;
	bool static_only;
	bool printed_header;
};

static eigrp_result_t eigrp_vty_neighbor_state_render(
	const eigrp_neighbor_state_t *state, void *arg)
{
	struct eigrp_vty_neighbor_show *show = arg;
	char address[INET6_ADDRSTRLEN];

	if (!show->printed_header) {
		if (show->static_only)
			vty_out(show->vty, "%-40s %-22s %-12s\n", "Address",
				"Interface", "State");
		else
			vty_out(show->vty,
				"%-40s %-22s %-8s %-8s %-12s %-10s\n", "Address",
				"Interface", "Hold", "RelQ", "Seq", "State");
		show->printed_header = true;
	}

	if (show->static_only) {
		vty_out(show->vty, "%-40s %-22s %-12s\n",
			eigrp_vty_address_string(&state->address, address,
						 sizeof(address)),
			state->interface_name, state->state_name);
		return EIGRP_RESULT_SUCCESS;
	}

	vty_out(show->vty, "%-40s %-22s %-8u %-8lu %-12u %-10s\n",
		eigrp_vty_address_string(&state->address, address, sizeof(address)),
		state->interface_name, state->hold_time, state->reliable_queue_count,
		state->sequence_number, state->state_name);
	if (show->detail) {
		vty_out(show->vty,
			"  Version %u.%u/%u.%u, TLV version %u, retransmissions %u\n",
			state->os_major, state->os_minor, state->tlv_major,
			state->tlv_minor, state->tlv_version, state->retransmit_count);
		vty_out(show->vty,
			"  Uptime, SRTT, and adaptive RTO state: not exposed by the current backend\n");
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
};

static eigrp_result_t eigrp_vty_topology_prefix_render(
	const eigrp_topology_prefix_state_t *state, void *arg)
{
	struct eigrp_vty_topology_show *show = arg;
	char prefix[INET6_ADDRSTRLEN + 8];

	vty_out(show->vty, "%c %s, %u successors, FD is %u, serno: %" PRIu64 "\n",
		state->active ? 'A' : 'P',
		eigrp_vty_prefix_string(&state->destination, prefix, sizeof(prefix)),
		state->successor_count, state->feasible_distance, state->serial_number);
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_vty_topology_route_render(
	const eigrp_topology_route_state_t *state, void *arg)
{
	struct eigrp_vty_topology_show *show = arg;
	char address[INET6_ADDRSTRLEN];
	const char *flags = state->successor ? "successor"
			    : state->feasible_successor ? "feasible-successor" : "other";

	if (state->connected)
		vty_out(show->vty, "       via Connected, %s [%s]\n",
			state->interface_name ? state->interface_name : "<unknown>", flags);
	else
		vty_out(show->vty, "       via %s (%u/%u), %s [%s]\n",
			eigrp_vty_address_string(&state->next_hop, address,
						 sizeof(address)),
			state->distance, state->reported_distance,
			state->interface_name ? state->interface_name : "<unknown>", flags);
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
	struct eigrp_vty_topology_show show = {.vty = vty};
	eigrp_result_t result;

	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Topology Table");
	vty_out(vty,
		"Codes: P - Passive, A - Active; route flags identify successors and feasible successors\n");
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
	eigrp_state_request_t request;
	struct eigrp_vty_interface_context options = {
		.ifname = ifname,
		.detail = detail != NULL,
	};

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast interface state",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_vty_named_state_walk(vty, &request, "interfaces",
					  eigrp_vty_interface_context_render, &options);
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
	eigrp_state_request_t request;
	int index = 0;
	struct eigrp_vty_neighbor_context options = {
		.ifname = ifname,
		.detail = detail != NULL,
		.static_only = argv_find(argv, argc, "static", &index),
	};

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast neighbor state",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_vty_named_state_walk(vty, &request, "neighbors",
					  eigrp_vty_neighbor_context_render, &options);
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
	eigrp_state_request_t request;
	struct eigrp_vty_topology_context options = {
		.all_links = all != NULL,
	};

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast topology state",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_vty_named_state_walk(vty, &request, "topology",
					  eigrp_vty_topology_context_render, &options);
}

DEFPY(show_eigrp_topology,
      show_eigrp_topology_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] topology WORD$target [all-links]$all",
      SHOW_STR
      EIGRP_STR
      "Address-family information\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      VRF_CMD_HELP_STR
      AS_STR
      "Display multicast instances\n"
      "Display EIGRP topology table\n"
      "Network address or prefix\n"
      "Display all topology links\n")
{
	eigrp_prefix_t destination;
	eigrp_state_request_t request;
	struct eigrp_vty_topology_context options = {
		.destination = &destination,
		.all_links = all != NULL,
	};

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast topology state",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	if (!eigrp_vty_destination_parse(target, request.afi, &destination)) {
		vty_out(vty, "%% Malformed topology destination: %s\n", target);
		return CMD_WARNING;
	}

	return eigrp_vty_named_state_walk(vty, &request, "topology",
					  eigrp_vty_topology_context_render, &options);
}

struct eigrp_vty_accounting_show {
	struct vty *vty;
};

static eigrp_result_t eigrp_vty_accounting_state_render(
	const eigrp_statistics_accounting_state_t *state, void *arg)
{
	struct eigrp_vty_accounting_show *show = arg;
	char address[INET6_ADDRSTRLEN];

	vty_out(show->vty, "%-10s %-40s %-22s %u\n", state->neighbor_state,
		eigrp_vty_address_string(&state->neighbor_address, address,
					 sizeof(address)),
		state->interface_name, state->prefix_count);
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
	vty_out(vty, "%-10s %-40s %-22s %s\n", "State", "Address/Source",
		"Interface", "Prefixes");
	result = eigrp_statistics_accounting_show(
		&context, &total_prefix_count, eigrp_vty_accounting_state_render, &show);
	if (result == EIGRP_RESULT_SUCCESS) {
		vty_out(vty, "Total Prefix Count: %u\n", total_prefix_count);
		vty_out(vty,
			"Restart count and restart/reset timers: not exposed by the current backend\n");
	} else if (result == EIGRP_RESULT_NOT_FOUND) {
		vty_out(vty, "  Runtime accounting data is not available\n");
	} else {
		eigrp_cli_result_render(vty, "accounting", result);
	}
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_vty_traffic_field_render(struct vty *vty, const char *name,
					   uint16_t field,
					   const eigrp_statistics_traffic_state_t *state,
					   uint64_t sent, uint64_t received)
{
	char sent_text[32];
	char received_text[32];

	if (state->sent_valid & field)
		snprintfrr(sent_text, sizeof(sent_text), "%" PRIu64, sent);
	else
		snprintf(sent_text, sizeof(sent_text), "n/a");
	if (state->received_valid & field)
		snprintfrr(received_text, sizeof(received_text), "%" PRIu64, received);
	else
		snprintf(received_text, sizeof(received_text), "n/a");
	vty_out(vty, "  %-14s %12s %12s\n", name, sent_text, received_text);
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
		vty_out(vty, "                         Sent       Received\n");
		eigrp_vty_traffic_field_render(
			vty, "Hellos", EIGRP_STATISTICS_TRAFFIC_HELLO, &state,
			state.sent_hello, state.received_hello);
		eigrp_vty_traffic_field_render(
			vty, "Updates", EIGRP_STATISTICS_TRAFFIC_UPDATE, &state,
			state.sent_update, state.received_update);
		eigrp_vty_traffic_field_render(
			vty, "Queries", EIGRP_STATISTICS_TRAFFIC_QUERY, &state,
			state.sent_query, state.received_query);
		eigrp_vty_traffic_field_render(
			vty, "Replies", EIGRP_STATISTICS_TRAFFIC_REPLY, &state,
			state.sent_reply, state.received_reply);
		eigrp_vty_traffic_field_render(
			vty, "ACKs", EIGRP_STATISTICS_TRAFFIC_ACK, &state,
			state.sent_ack, state.received_ack);
		eigrp_vty_traffic_field_render(
			vty, "SIA-Queries", EIGRP_STATISTICS_TRAFFIC_SIA_QUERY,
			&state, state.sent_sia_query, state.received_sia_query);
		eigrp_vty_traffic_field_render(
			vty, "SIA-Replies", EIGRP_STATISTICS_TRAFFIC_SIA_REPLY,
			&state, state.sent_sia_reply, state.received_sia_reply);
		vty_out(vty,
			"  n/a means the current packet path does not maintain that counter\n");
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

	if (!show->printed_header) {
		vty_out(show->vty, "%-22s %-10s %-16s %-16s\n", "Interface", "Source",
			"Hello interval", "Hold time");
		show->printed_header = true;
	}
	vty_out(show->vty, "%-22s %-10s ", state->interface_name,
		state->runtime_present ? "runtime" : "config");
	if (state->runtime_present || state->hello_interval_configured)
		vty_out(show->vty, "%-16u ", state->hello_interval);
	else
		vty_out(show->vty, "%-16s ", "default");
	if (state->runtime_present || state->hold_time_configured)
		vty_out(show->vty, "%-16u\n", state->hold_time);
	else
		vty_out(show->vty, "%-16s\n", "default");
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
		vty_out(vty, "  No interface timer state is currently available\n");
	vty_out(vty,
		"  Timer expiration scheduling and SIA process expiration: not exposed by the current portable backend\n");
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

	(void)arg;
	eigrp_vty_named_context_header(vty, instance_name, af, runtime,
				       "Events");
	eigrp_cli_result_render(vty, "event history", eigrp_event_show(&context));
	return EIGRP_RESULT_SUCCESS;
}

DEFPY(show_eigrp_accounting,
      show_eigrp_accounting_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] accounting",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP accounting\n")
{
	eigrp_state_request_t request;

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast accounting",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_vty_named_state_walk(vty, &request, "accounting",
					  eigrp_vty_accounting_context_render, NULL);
}

DEFPY(show_eigrp_event,
      show_eigrp_event_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] events",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP events\n")
{
	eigrp_state_request_t request;

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast events",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_vty_named_state_walk(vty, &request, "events",
					  eigrp_vty_event_context_render, NULL);
}

DEFPY(show_eigrp_timer,
      show_eigrp_timer_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] timers",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP timers\n")
{
	eigrp_state_request_t request;

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast timers",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
	return eigrp_vty_named_state_walk(vty, &request, "timers",
					  eigrp_vty_timer_context_render, NULL);
}

DEFPY(show_eigrp_traffic,
      show_eigrp_traffic_cmd,
      "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] traffic",
      SHOW_STR EIGRP_STR "Address-family information\n"
      "IPv4 address-family\n" "IPv6 address-family\n" VRF_CMD_HELP_STR AS_STR
      "Display multicast instances\n" "Display EIGRP traffic\n")
{
	eigrp_state_request_t request;

	if (eigrp_vty_multicast_requested(argv, argc))
		return eigrp_cli_result_render(vty, "multicast traffic",
					       EIGRP_RESULT_UNSUPPORTED);
	if (!eigrp_vty_state_request_build(afi, as, vrf, &request))
		return CMD_WARNING;
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
