// SPDX-License-Identifier: GPL-2.0-or-later
/* FRR EIGRP debug/VTY presentation adapter. */
#include <zebra.h>
#include "command.h"
#include "vty.h"
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
#include "eigrp_dump.h"

static void eigrp_debug_transmit_write(struct vty *vty, unsigned long state)
{
	if (!state)
		return;
	if ((state & EIGRP_DEBUG_TRANSMIT_ALL) == EIGRP_DEBUG_TRANSMIT_ALL) {
		vty_out(vty, "debug eigrp transmit\n");
		return;
	}
	vty_out(vty, "debug eigrp transmit");
	if (state & EIGRP_DEBUG_TRANSMIT_ACK)
		vty_out(vty, " ack");
	if (state & EIGRP_DEBUG_TRANSMIT_BUILD)
		vty_out(vty, " build");
	if (state & EIGRP_DEBUG_TRANSMIT_DETAIL)
		vty_out(vty, " detail");
	if (state & EIGRP_DEBUG_TRANSMIT_LINK)
		vty_out(vty, " link");
	if (state & EIGRP_DEBUG_TRANSMIT_PACKETIZE)
		vty_out(vty, " packetize");
	if (state & EIGRP_DEBUG_TRANSMIT_PEERDOWN)
		vty_out(vty, " peerdown");
	if (state & EIGRP_DEBUG_TRANSMIT_SIA)
		vty_out(vty, " sia");
	if (state & EIGRP_DEBUG_TRANSMIT_STARTUP)
		vty_out(vty, " startup");
	if (state & EIGRP_DEBUG_TRANSMIT_STRANGE)
		vty_out(vty, " strange");
	vty_out(vty, "\n");
}

static void eigrp_debug_address_family_slot_write(
	struct vty *vty, const eigrp_debug_address_family_state_t *slot)
{
	char address[INET6_ADDRSTRLEN];
	const char *afi;
	const char *suffix = "";

	if (!slot || !slot->used)
		return;
	afi = slot->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? "ipv6" : "ipv4";
	vty_out(vty, "debug eigrp address-family %s", afi);
	if (!slot->all_vrfs && strcmp(slot->vrf_name, "default") != 0)
		vty_out(vty, " vrf %s", slot->vrf_name);
	if (slot->asn)
		vty_out(vty, " %u", slot->asn);

	switch (slot->category) {
	case EIGRP_DEBUG_AF_ROUTE:
		break;
	case EIGRP_DEBUG_AF_NEIGHBOR:
		vty_out(vty, " neighbor");
		if (slot->neighbor_set) {
			int family = slot->afi == EIGRP_ADDRESS_FAMILY_IPV6
					     ? AF_INET6
					     : AF_INET;
			if (inet_ntop(family, slot->neighbor.bytes, address,
				      sizeof(address)))
				vty_out(vty, " %s", address);
		}
		break;
	case EIGRP_DEBUG_AF_NOTIFICATIONS:
		suffix = " notifications";
		break;
	case EIGRP_DEBUG_AF_SUMMARY:
		suffix = " summary";
		break;
	case EIGRP_DEBUG_AF_CATEGORY_MAX:
		break;
	}
	vty_out(vty, "%s\n", suffix);
}

static int config_write_debug(struct vty *vty)
{
	int write = 0;
	unsigned int i;

	if (conf_debug_eigrp & EIGRP_DEBUG_EVENT) {
		vty_out(vty, "debug eigrp event%s\n",
			(conf_debug_eigrp & EIGRP_DEBUG_DETAIL) ? " detail" : "");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_TIMERS) {
		vty_out(vty, "debug eigrp timers\n");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_FSM) {
		vty_out(vty, "debug eigrp fsm\n");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_NSF) {
		vty_out(vty, "debug eigrp nsf\n");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_FAST_REROUTE) {
		vty_out(vty, "debug eigrp frr\n");
		write = 1;
	}
	if (conf_debug_eigrp_nei & EIGRP_DEBUG_NEI) {
		vty_out(vty, "debug eigrp neighbor");
		if (conf_debug_eigrp_nei & EIGRP_DEBUG_NEI_SIATIMER)
			vty_out(vty, " siatimer");
		if (conf_debug_eigrp_nei & EIGRP_DEBUG_NEI_STATIC)
			vty_out(vty, " static");
		vty_out(vty, "\n");
		write = 1;
	}
	if (conf_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB) {
		vty_out(vty, "debug eigrp notifications rib\n");
		write = 1;
	}
	if (conf_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_INTERFACE) {
		vty_out(vty, "debug eigrp notifications interface\n");
		write = 1;
	}
	if (conf_debug_eigrp_transmit) {
		eigrp_debug_transmit_write(vty, conf_debug_eigrp_transmit);
		write = 1;
	}

	for (i = 0; i < eigrp_debug_address_family_state_count(); i++) {
		eigrp_debug_address_family_state_t state;

		if (!eigrp_debug_address_family_state_get(
			    EIGRP_DEBUG_SCOPE_CONFIG, i, &state)
		    || !state.used)
			continue;
		eigrp_debug_address_family_slot_write(vty, &state);
		write = 1;
	}

	/* Persist packet debug categories independently so partial directions survive. */
	for (i = 0; i < EIGRP_DEBUG_PACKET_CATEGORY_MAX; i++) {
		unsigned long state = conf_debug_eigrp_packet[i];
		const char *direction = "";
		const char *detail = "";

		if (!(state & EIGRP_DEBUG_SEND_RECV))
			continue;
		if ((state & EIGRP_DEBUG_SEND_RECV) == EIGRP_DEBUG_SEND)
			direction = " send";
		else if ((state & EIGRP_DEBUG_SEND_RECV) == EIGRP_DEBUG_RECV)
			direction = " receive";
		if (state & EIGRP_DEBUG_PACKET_DETAIL)
			detail = " detail";

		vty_out(vty, "debug eigrp packet %s%s%s\n",
			eigrp_debug_packet_category_cli_name(i), direction, detail);
		write = 1;
	}

	return write;
}

static int eigrp_neighbor_packet_queue_sum(eigrp_interface_t *ei)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node, *nnode;
	int sum;
	sum = 0;

	for (EIGRP_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
		sum += nbr->retrans_queue->count;
	}

	return sum;
}

void show_ip_eigrp_interface_header(struct vty *vty, eigrp_instance_t *eigrp)
{

	vty_out(vty,
		"\nEIGRP interfaces for AS(%d)\n\n %-10s %-10s %-10s %-6s %-12s %-7s %-14s %-12s %-8s %-8s %-8s\n %-39s %-12s %-7s %-14s %-12s %-8s\n",
		eigrp->AS, "Interface", "Bandwidth", "Delay", "Peers",
		"Xmit Queue", "Mean", "Pacing Time", "Multicast", "Pending",
		"Hello", "Holdtime", "", "Un/Reliable", "SRTT", "Un/Reliable",
		"Flow Timer", "Routes");
}

void show_ip_eigrp_interface_sub(struct vty *vty, eigrp_instance_t *eigrp,
				 eigrp_interface_t *ei)
{
	vty_out(vty, "%-11s ", eigrp_intf_name_string(ei));
	vty_out(vty, "%-11u", ei->params.bandwidth);
	vty_out(vty, "%-11u", ei->params.delay);
	vty_out(vty, "%-7u", ei->nbrs->count);
	vty_out(vty, "%u %c %-10u", 0, '/',
		eigrp_neighbor_packet_queue_sum(ei));
	vty_out(vty, "%-7u %-14u %-12u %-8u", 0, 0, 0, 0);
	vty_out(vty, "%-8u %-8u \n", ei->params.v_hello, ei->params.v_wait);
}

void show_ip_eigrp_interface_detail(struct vty *vty, eigrp_instance_t *eigrp,
				    eigrp_interface_t *ei)
{
	vty_out(vty, "%-2s %s %d %-3s \n", "", "Hello interval is ", 0, " sec");
	vty_out(vty, "%-2s %s %s \n", "", "Next xmit serial", "<none>");
	vty_out(vty, "%-2s %s %d %s %d %s %d %s %d \n", "",
		"Un/reliable mcasts: ", 0, "/", 0, "Un/reliable ucasts: ", 0,
		"/", 0);
	vty_out(vty, "%-2s %s %d %s %d %s %d \n", "", "Mcast exceptions: ", 0,
		"  CR packets: ", 0, "  ACKs suppressed: ", 0);
	vty_out(vty, "%-2s %s %d %s %d \n", "", "Retransmissions sent: ", 0,
		"Out-of-sequence rcvd: ", 0);
	vty_out(vty, "%-2s %s %s %s \n", "", "Authentication mode is ", "not",
		"set");
	vty_out(vty, "%-2s TLV peers: v1 %u, v2 %u\n", "",
		ei->tlv1_peer_count, ei->tlv2_peer_count);
	vty_out(vty, "%-2s %s \n", "", "Use multicast");
}

void show_ip_eigrp_neighbor_header(struct vty *vty, eigrp_instance_t *eigrp)
{
	vty_out(vty, "\nIP-EIGRP neighbors for process %u\n", eigrp->AS);
	vty_out(vty,
		"H   Address                 Interface       Hold Uptime   SRTT   RTO  Q  Seq\n");
	vty_out(vty,
		"                                           (sec)          (ms)       Cnt Num\n");
}

static const char *eigrp_dump_duration_string(uint64_t seconds, char *buffer,
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

void show_ip_eigrp_neighbor_sub(struct vty *vty, eigrp_neighbor_t *nbr,
				int detail)
{
	char hold[16];
	char uptime[32];
	char srtt[16];
	uint64_t uptime_seconds = 0;
	uint8_t retry_count = 0;

	if (nbr->t_holddown)
		snprintf(hold, sizeof(hold), "%u",
			 eigrp_sys_timer_remaining_seconds(nbr->t_holddown));
	else
		snprintf(hold, sizeof(hold), "-");
	if (nbr->up_since_msec) {
		uint64_t now = eigrp_sys_monotime_msec();

		if (now >= nbr->up_since_msec)
			uptime_seconds = (now - nbr->up_since_msec) / 1000U;
		eigrp_dump_duration_string(uptime_seconds, uptime, sizeof(uptime));
	} else
		snprintf(uptime, sizeof(uptime), "-");
	if (nbr->retrans_queue && nbr->retrans_queue->tail)
		retry_count = nbr->retrans_queue->tail->retrans_counter;
	if (nbr->srtt_valid)
		snprintf(srtt, sizeof(srtt), "%u", nbr->srtt_msec);
	else
		snprintf(srtt, sizeof(srtt), "n/a");

	vty_out(vty, "%-3s %-23s %-15s %-5s %-8s %-6s %-5u %-3lu %u\n", "-",
		eigrp_print_addr(&nbr->src), eigrp_intf_name_string(nbr->ei), hold, uptime,
		srtt, eigrp_neighbor_rto_get(nbr),
		nbr->retrans_queue ? nbr->retrans_queue->count : 0,
		nbr->recv_sequence_number);


	if (detail) {
		vty_out(vty, "   Version %u.%u/%u.%u", nbr->os_rel_major,
			nbr->os_rel_minor, nbr->tlv_rel_major,
			nbr->tlv_rel_minor);
		vty_out(vty, ", Retrans: %" PRIu64 ", Retries: %u\n",
			nbr->retransmissions, retry_count);
	}
}

/*
 * Print standard header for show EIGRP topology output
 */
void show_ip_eigrp_topology_header(struct vty *vty, eigrp_instance_t *eigrp)
{
	vty_out(vty, "\nIP-EIGRP Topology Table for AS(%d)/ID(%s)\n\n",
		eigrp->AS, eigrp_print_routerid(eigrp->router_id));
	vty_out(vty,
		"Codes: P - Passive, A - Active, U - Update, Q - Query, "
		"R - Reply,\n       r - reply Status, s - sia Status\n\n");
}

void show_ip_eigrp_prefix_descriptor(struct vty *vty,
				     eigrp_prefix_descriptor_t *tn,
				     bool include_serial)
{
	eigrp_list_t *successors = eigrp_topology_get_successor(tn);
	char buffer[EIGRP_PREFIX_STRLEN] = "invalid";

	eigrp_prefix_snprintf(buffer, sizeof(buffer), &tn->destination);
	vty_out(vty, "%c %s, %u successors, FD is ",
		(tn->state > 0) ? 'A' : 'P', buffer,
		(successors) ? successors->count : 0);
	if (tn->fdistance == EIGRP_MAX_METRIC)
		vty_out(vty, "Inaccessible");
	else
		vty_out(vty, "%u", tn->fdistance);
	if (include_serial)
		vty_out(vty, ", serno %" PRIu64, tn->serno);
	vty_out(vty, "\n");

	if (successors)
		eigrp_list_delete(&successors);
}

void show_ip_eigrp_route_descriptor(struct vty *vty, eigrp_instance_t *eigrp,
				    eigrp_route_descriptor_t *te, bool *first,
				    bool include_serial)
{
	if (te->reported_distance == EIGRP_MAX_METRIC)
		return;

	if (*first) {
		show_ip_eigrp_prefix_descriptor(vty, te->prefix, include_serial);
		*first = false;
	}

	if (te->adv_router == eigrp->neighbor_self)
		vty_out(vty, "        via Connected, %s\n", eigrp_intf_name_string(te->ei));
	else
		vty_out(vty, "        via %s (%u/%u), %s\n",
			eigrp_print_addr(&te->adv_router->src), te->distance,
			te->reported_distance, eigrp_intf_name_string(te->ei));
}


DEFUN_NOSH(show_debugging_eigrp, show_debugging_eigrp_cmd,
	   "show debugging [eigrp]", SHOW_STR DEBUG_STR EIGRP_STR)
{
	unsigned int i;

	vty_out(vty, "EIGRP debugging status:\n");
	if (IS_DEBUG_EIGRP(event, EVENT))
		vty_out(vty, "  EIGRP event%s debugging is on\n",
			 IS_DEBUG_EIGRP(event, DETAIL) ? " detail" : "");
	if (IS_DEBUG_EIGRP(event, TIMERS))
		vty_out(vty, "  EIGRP timers debugging is on\n");
	if (IS_DEBUG_EIGRP(event, FSM))
		vty_out(vty, "  EIGRP FSM debugging is on\n");
	if (IS_DEBUG_EIGRP(event, NSF))
		vty_out(vty, "  EIGRP NSF debugging is on\n");
	if (IS_DEBUG_EIGRP(event, FAST_REROUTE))
		vty_out(vty, "  EIGRP fast-reroute debugging is on\n");
	if (term_debug_eigrp_nei & EIGRP_DEBUG_NEI) {
		vty_out(vty, "  EIGRP neighbor debugging is on");
		if (term_debug_eigrp_nei & EIGRP_DEBUG_NEI_SIATIMER)
			vty_out(vty, " (siatimer)");
		if (term_debug_eigrp_nei & EIGRP_DEBUG_NEI_STATIC)
			vty_out(vty, " (static)");
		vty_out(vty, "\n");
	}
	if (term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB)
		vty_out(vty, "  EIGRP RIB notification debugging is on\n");
	if (term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_INTERFACE)
		vty_out(vty, "  EIGRP interface notification debugging is on\n");
	if (term_debug_eigrp_transmit) {
		vty_out(vty, "  EIGRP transmit debugging is on:");
#define EIGRP_SHOW_TRANSMIT(_flag, _name)                                     \
		do {                                                                 \
			if (term_debug_eigrp_transmit & (_flag))                        \
				vty_out(vty, " %s", (_name));                            \
		} while (0)
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_ACK, "ACK");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_BUILD, "BUILD");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_DETAIL, "DETAIL");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_LINK, "LINK");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_PACKETIZE, "PACKETIZE");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_PEERDOWN, "PEERDOWN");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_SIA, "SIA");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_STARTUP, "STARTUP");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_STRANGE, "STRANGE");
#undef EIGRP_SHOW_TRANSMIT
		vty_out(vty, "\n");
	}

	for (i = 0; i < eigrp_debug_address_family_state_count(); i++) {
		eigrp_debug_address_family_state_t state;
		const eigrp_debug_address_family_state_t *slot = &state;
		char address[INET6_ADDRSTRLEN];

		if (!eigrp_debug_address_family_state_get(
			    EIGRP_DEBUG_SCOPE_TERMINAL, i, &state)
		    || !slot->used)
			continue;
		vty_out(vty, "  EIGRP address-family %s",
			slot->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? "ipv6" : "ipv4");
		if (slot->asn)
			vty_out(vty, " AS %u", slot->asn);
		if (!slot->all_vrfs && strcmp(slot->vrf_name, "default") != 0)
			vty_out(vty, " vrf %s", slot->vrf_name);
		switch (slot->category) {
		case EIGRP_DEBUG_AF_ROUTE:
			vty_out(vty, " route");
			break;
		case EIGRP_DEBUG_AF_NEIGHBOR:
			vty_out(vty, " neighbor");
			if (slot->neighbor_set
			    && inet_ntop(slot->afi == EIGRP_ADDRESS_FAMILY_IPV6
						 ? AF_INET6
						 : AF_INET,
					 slot->neighbor.bytes, address, sizeof(address)))
				vty_out(vty, " %s", address);
			break;
		case EIGRP_DEBUG_AF_NOTIFICATIONS:
			vty_out(vty, " notifications");
			break;
		case EIGRP_DEBUG_AF_SUMMARY:
			vty_out(vty, " summary");
			break;
		case EIGRP_DEBUG_AF_CATEGORY_MAX:
			break;
		}
		vty_out(vty, " debugging is on\n");
	}

	for (i = 0; i < EIGRP_DEBUG_PACKET_CATEGORY_MAX; i++) {
		const char *name = eigrp_debug_packet_category_name(i);

		if (IS_DEBUG_EIGRP_PACKET(i, SEND)
		    && IS_DEBUG_EIGRP_PACKET(i, RECV)) {
			vty_out(vty, "  EIGRP packet %s%s debugging is on\n", name,
				IS_DEBUG_EIGRP_PACKET(i, PACKET_DETAIL)
					? " detail"
					: "");
		} else {
			if (IS_DEBUG_EIGRP_PACKET(i, SEND))
				vty_out(vty,
					"  EIGRP packet %s send%s debugging is on\n",
					name,
					IS_DEBUG_EIGRP_PACKET(i, PACKET_DETAIL)
						? " detail"
						: "");
			if (IS_DEBUG_EIGRP_PACKET(i, RECV))
				vty_out(vty,
					"  EIGRP packet %s receive%s debugging is on\n",
					name,
					IS_DEBUG_EIGRP_PACKET(i, PACKET_DETAIL)
						? " detail"
						: "");
		}
	}

	return CMD_SUCCESS;
}


static eigrp_debug_scope_t eigrp_debug_cli_scope(const struct vty *vty)
{
	return vty->node == CONFIG_NODE ? EIGRP_DEBUG_SCOPE_CONFIG
					       : EIGRP_DEBUG_SCOPE_TERMINAL;
}

static int eigrp_debug_cli_result(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS
					      : CMD_WARNING_CONFIG_FAILED;
}

DEFUN(debug_eigrp_event, debug_eigrp_event_cmd,
      "debug eigrp event [detail]",
      DEBUG_STR EIGRP_STR
      "EIGRP event debugging\n"
      "Detailed information\n")
{
	int idx = 0;

	return eigrp_debug_cli_result(eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL,
		EIGRP_DEBUG_EVENT | (argv_find(argv, argc, "detail", &idx)
				     ? EIGRP_DEBUG_DETAIL : 0),
		eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_event, no_debug_eigrp_event_cmd,
      "no debug eigrp event [detail]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP event debugging\n"
      "Detailed information\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL,
			 EIGRP_DEBUG_EVENT | EIGRP_DEBUG_DETAIL,
			 eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_timers, debug_eigrp_timers_cmd,
      "debug eigrp timers",
      DEBUG_STR EIGRP_STR "EIGRP timer debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_TIMERS,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_timers, no_debug_eigrp_timers_cmd,
      "no debug eigrp timers",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP timer debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_TIMERS,
			  eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_fsm, debug_eigrp_fsm_cmd,
      "debug eigrp fsm",
      DEBUG_STR EIGRP_STR "EIGRP DUAL finite-state-machine debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_FSM,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_fsm, no_debug_eigrp_fsm_cmd,
      "no debug eigrp fsm",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP DUAL finite-state-machine debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_FSM,
			  eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_nsf, debug_eigrp_nsf_cmd,
      "debug eigrp nsf",
      DEBUG_STR EIGRP_STR "EIGRP NSF/graceful-restart debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_NSF,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_nsf, no_debug_eigrp_nsf_cmd,
      "no debug eigrp nsf",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP NSF/graceful-restart debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_NSF,
			  eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_frr, debug_eigrp_frr_cmd,
      "debug eigrp frr",
      DEBUG_STR EIGRP_STR "EIGRP fast-reroute debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_FAST_REROUTE,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_frr, no_debug_eigrp_frr_cmd,
      "no debug eigrp frr",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP fast-reroute debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL,
			  EIGRP_DEBUG_FAST_REROUTE,
			  eigrp_debug_cli_scope(vty)));
}

static unsigned long eigrp_debug_neighbor_flags(int argc,
						 struct cmd_token **argv)
{
	unsigned long flags = EIGRP_DEBUG_NEI;
	int idx = 0;

	if (argv_find(argv, argc, "siatimer", &idx))
		flags |= EIGRP_DEBUG_NEI_SIATIMER;
	if (argv_find(argv, argc, "static", &idx))
		flags |= EIGRP_DEBUG_NEI_STATIC;
	return flags;
}

DEFUN(debug_eigrp_neighbor, debug_eigrp_neighbor_cmd,
      "debug eigrp neighbor [siatimer] [static]",
      DEBUG_STR EIGRP_STR
      "EIGRP neighbor debugging\n"
      "Stuck-in-active timer messages\n"
      "Static-neighbor messages\n")
{
	return eigrp_debug_cli_result(eigrp_debug_set(EIGRP_DEBUG_TARGET_NEIGHBOR,
		eigrp_debug_neighbor_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_neighbor, no_debug_eigrp_neighbor_cmd,
      "no debug eigrp neighbor [siatimer] [static]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP neighbor debugging\n"
      "Stuck-in-active timer messages\n"
      "Static-neighbor messages\n")
{
	return eigrp_debug_cli_result(eigrp_debug_reset(EIGRP_DEBUG_TARGET_NEIGHBOR,
		eigrp_debug_neighbor_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_notifications, debug_eigrp_notifications_cmd,
      "debug eigrp notifications <rib|interface>",
      DEBUG_STR EIGRP_STR
      "EIGRP host notifications\n"
      "RIB notifications\n"
      "Interface notifications\n")
{
	int idx = 0;
	unsigned long flags = argv_find(argv, argc, "rib", &idx)
				      ? EIGRP_DEBUG_NOTIFICATION_RIB
				      : EIGRP_DEBUG_NOTIFICATION_INTERFACE;

	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_NOTIFICATIONS, flags,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_notifications, no_debug_eigrp_notifications_cmd,
      "no debug eigrp notifications <rib|interface>",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP host notifications\n"
      "RIB notifications\n"
      "Interface notifications\n")
{
	int idx = 0;
	unsigned long flags = argv_find(argv, argc, "rib", &idx)
				      ? EIGRP_DEBUG_NOTIFICATION_RIB
				      : EIGRP_DEBUG_NOTIFICATION_INTERFACE;

	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_NOTIFICATIONS, flags,
			  eigrp_debug_cli_scope(vty)));
}

static unsigned long eigrp_debug_transmit_flags(int argc,
						 struct cmd_token **argv)
{
	unsigned long flags = 0;
	bool category = false;
	int idx = 0;

#define EIGRP_TRANSMIT_ARG(_name, _flag)                                      \
	do {                                                                     \
		if (argv_find(argv, argc, (_name), &idx)) {                        \
			flags |= (_flag);                                             \
			if ((_flag) != EIGRP_DEBUG_TRANSMIT_DETAIL)                   \
				category = true;                                        \
		}                                                                \
	} while (0)
	EIGRP_TRANSMIT_ARG("ack", EIGRP_DEBUG_TRANSMIT_ACK);
	EIGRP_TRANSMIT_ARG("build", EIGRP_DEBUG_TRANSMIT_BUILD);
	EIGRP_TRANSMIT_ARG("detail", EIGRP_DEBUG_TRANSMIT_DETAIL);
	EIGRP_TRANSMIT_ARG("link", EIGRP_DEBUG_TRANSMIT_LINK);
	EIGRP_TRANSMIT_ARG("packetize", EIGRP_DEBUG_TRANSMIT_PACKETIZE);
	EIGRP_TRANSMIT_ARG("peerdown", EIGRP_DEBUG_TRANSMIT_PEERDOWN);
	EIGRP_TRANSMIT_ARG("sia", EIGRP_DEBUG_TRANSMIT_SIA);
	EIGRP_TRANSMIT_ARG("startup", EIGRP_DEBUG_TRANSMIT_STARTUP);
	EIGRP_TRANSMIT_ARG("strange", EIGRP_DEBUG_TRANSMIT_STRANGE);
#undef EIGRP_TRANSMIT_ARG

	/* Cisco's unqualified command enables the full transmit-debug family.
	 * "detail" by itself is likewise a request for detailed transmit output.
	 */
	if (!category)
		flags = EIGRP_DEBUG_TRANSMIT_ALL;
	return flags;
}

DEFUN(debug_eigrp_transmit, debug_eigrp_transmit_cmd,
      "debug eigrp transmit [ack] [build] [detail] [link] [packetize] [peerdown] [sia] [startup] [strange]",
      DEBUG_STR EIGRP_STR
      "EIGRP transmission events\n"
      "Acknowledgment processing\n"
      "Packet-build processing\n"
      "Detailed information\n"
      "Topology linked-list processing\n"
      "Packetizer processing\n"
      "Peer-down impact on packet generation\n"
      "Stuck-in-active processing\n"
      "Peer startup and initialization\n"
      "Unusual packet-processing events\n")
{
	return eigrp_debug_cli_result(eigrp_debug_set(EIGRP_DEBUG_TARGET_TRANSMIT,
		eigrp_debug_transmit_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_transmit, no_debug_eigrp_transmit_cmd,
      "no debug eigrp transmit [ack] [build] [detail] [link] [packetize] [peerdown] [sia] [startup] [strange]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP transmission events\n"
      "Acknowledgment processing\n"
      "Packet-build processing\n"
      "Detailed information\n"
      "Topology linked-list processing\n"
      "Packetizer processing\n"
      "Peer-down impact on packet generation\n"
      "Stuck-in-active processing\n"
      "Peer startup and initialization\n"
      "Unusual packet-processing events\n")
{
	return eigrp_debug_cli_result(eigrp_debug_reset(EIGRP_DEBUG_TARGET_TRANSMIT,
		eigrp_debug_transmit_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

static bool eigrp_debug_cli_address_family_request_build(
	int argc, struct cmd_token **argv, eigrp_state_request_t *request)
{
	int idx = 0;
	unsigned int i;

	if (!request)
		return false;
	memset(request, 0, sizeof(*request));
	if (argv_find(argv, argc, "ipv6", &idx))
		request->afi = EIGRP_ADDRESS_FAMILY_IPV6;
	else if (argv_find(argv, argc, "ipv4", &idx))
		request->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	else
		return false;

	if (argv_find(argv, argc, "vrf", &idx) && idx + 1 < argc)
		request->vrf_name = argv[idx + 1]->arg;

	for (i = 0; i < (unsigned int)argc; i++) {
		const char *arg = argv[i]->arg;
		char *end = NULL;
		unsigned long value;

		if (!arg || !arg[0] || strspn(arg, "0123456789") != strlen(arg))
			continue;
		value = strtoul(arg, &end, 10);
		if (end && !*end && value > 0 && value <= 65535) {
			request->asn = (uint16_t)value;
			break;
		}
	}
	return true;
}

static bool eigrp_debug_cli_neighbor_address_build(
	const eigrp_state_request_t *request, const char *text,
	eigrp_address_t *address)
{
	int family;

	if (!request || !text || !address)
		return false;
	memset(address, 0, sizeof(*address));
	address->afi = request->afi;
	family = request->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	return inet_pton(family, text, address->bytes) == 1;
}

static int eigrp_debug_cli_address_family_apply(
	struct vty *vty, int argc, struct cmd_token **argv,
	eigrp_debug_address_family_category_t category, bool enable)
{
	eigrp_state_request_t request;
	eigrp_address_t address;
	const eigrp_address_t *neighbor = NULL;
	const char *neighbor_text = NULL;
	eigrp_result_t result;
	int idx = 0;

	if (!eigrp_debug_cli_address_family_request_build(argc, argv, &request))
		return CMD_WARNING_CONFIG_FAILED;
	if (category == EIGRP_DEBUG_AF_NEIGHBOR
	    && argv_find(argv, argc, "neighbor", &idx) && idx + 1 < argc) {
		neighbor_text = argv[idx + 1]->arg;
		if (strcmp(neighbor_text, "vrf") != 0
		    && strcmp(neighbor_text, "notifications") != 0
		    && strcmp(neighbor_text, "summary") != 0) {
			if (!eigrp_debug_cli_neighbor_address_build(&request,
							      neighbor_text,
							      &address)) {
				vty_out(vty, "%% Invalid EIGRP neighbor address %s\n",
					neighbor_text);
				return CMD_WARNING_CONFIG_FAILED;
			}
			neighbor = &address;
		}
	}

	result = enable ? eigrp_debug_address_family_set(
				   &request, category, neighbor, eigrp_debug_cli_scope(vty))
			: eigrp_debug_address_family_reset(
				   &request, category, neighbor, eigrp_debug_cli_scope(vty));
	return eigrp_debug_cli_result(result);
}

DEFUN(debug_eigrp_address_family, debug_eigrp_address_family_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)]",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_ROUTE, true);
}

DEFUN(no_debug_eigrp_address_family, no_debug_eigrp_address_family_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_ROUTE, false);
}

DEFUN(debug_eigrp_address_family_neighbor,
      debug_eigrp_address_family_neighbor_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] neighbor [WORD]",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP neighbor debugging\n"
      "Neighbor address\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NEIGHBOR, true);
}

DEFUN(no_debug_eigrp_address_family_neighbor,
      no_debug_eigrp_address_family_neighbor_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] neighbor [WORD]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP neighbor debugging\n"
      "Neighbor address\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NEIGHBOR, false);
}

DEFUN(debug_eigrp_address_family_notifications,
      debug_eigrp_address_family_notifications_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] notifications",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP event notifications\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NOTIFICATIONS, true);
}

DEFUN(no_debug_eigrp_address_family_notifications,
      no_debug_eigrp_address_family_notifications_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] notifications",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP event notifications\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NOTIFICATIONS, false);
}

DEFUN(debug_eigrp_address_family_summary,
      debug_eigrp_address_family_summary_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] summary",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP summary route processing\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_SUMMARY, true);
}

DEFUN(no_debug_eigrp_address_family_summary,
      no_debug_eigrp_address_family_summary_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] summary",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP summary route processing\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_SUMMARY, false);
}

DEFUN(debug_eigrp_packet, debug_eigrp_packet_cmd,
      "debug eigrp packet <siaquery|siareply|ack|hello|probe|query|reply|request|retry|terse|update|all> [send|receive] [detail]",
      DEBUG_STR EIGRP_STR
      "EIGRP packets\n"
      "EIGRP SIA-Query packets\n"
      "EIGRP SIA-Reply packets\n"
      "EIGRP ack packets\n"
      "EIGRP hello packets\n"
      "EIGRP probe packets\n"
      "EIGRP query packets\n"
      "EIGRP reply packets\n"
      "EIGRP request packets\n"
      "EIGRP retransmissions\n"
      "Display all EIGRP packets except Hellos\n"
      "EIGRP update packets\n"
      "Display all EIGRP packets\n"
      "Send Packets\n"
      "Receive Packets\n"
      "Detail Information\n")
{
	uint32_t type = 0;
	unsigned long flag = EIGRP_DEBUG_SEND_RECV;
	eigrp_debug_scope_t scope = vty->node == CONFIG_NODE
					? EIGRP_DEBUG_SCOPE_CONFIG
					: EIGRP_DEBUG_SCOPE_TERMINAL;
	eigrp_result_t result;
	int idx = 0;

	if (argv_find(argv, argc, "hello", &idx))
		type = EIGRP_DEBUG_HELLO;
	else if (argv_find(argv, argc, "update", &idx))
		type = EIGRP_DEBUG_UPDATE;
	else if (argv_find(argv, argc, "query", &idx))
		type = EIGRP_DEBUG_QUERY;
	else if (argv_find(argv, argc, "ack", &idx))
		type = EIGRP_DEBUG_ACK;
	else if (argv_find(argv, argc, "probe", &idx))
		type = EIGRP_DEBUG_PROBE;
	else if (argv_find(argv, argc, "reply", &idx))
		type = EIGRP_DEBUG_REPLY;
	else if (argv_find(argv, argc, "request", &idx))
		type = EIGRP_DEBUG_REQUEST;
	else if (argv_find(argv, argc, "retry", &idx))
		type = EIGRP_DEBUG_RETRY;
	else if (argv_find(argv, argc, "siaquery", &idx))
		type = EIGRP_DEBUG_SIAQUERY;
	else if (argv_find(argv, argc, "siareply", &idx))
		type = EIGRP_DEBUG_SIAREPLY;
	else if (argv_find(argv, argc, "terse", &idx))
		type = EIGRP_DEBUG_PACKETS_TERSE;
	else if (argv_find(argv, argc, "all", &idx))
		type = EIGRP_DEBUG_PACKETS_ALL;

	if (argv_find(argv, argc, "send", &idx))
		flag = EIGRP_DEBUG_SEND;
	else if (argv_find(argv, argc, "receive", &idx))
		flag = EIGRP_DEBUG_RECV;
	if (argv_find(argv, argc, "detail", &idx))
		flag |= EIGRP_DEBUG_PACKET_DETAIL;

	result = eigrp_debug_packet_set(type, flag, scope);
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS
					      : CMD_WARNING_CONFIG_FAILED;
}

DEFUN(no_debug_eigrp_packet, no_debug_eigrp_packet_cmd,
      "no debug eigrp packet <siaquery|siareply|ack|hello|probe|query|reply|request|retry|terse|update|all> [send|receive] [detail]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP packets\n"
      "EIGRP SIA-Query packets\n"
      "EIGRP SIA-Reply packets\n"
      "EIGRP ack packets\n"
      "EIGRP hello packets\n"
      "EIGRP probe packets\n"
      "EIGRP query packets\n"
      "EIGRP reply packets\n"
      "EIGRP request packets\n"
      "EIGRP retransmissions\n"
      "Display all EIGRP packets except Hellos\n"
      "EIGRP update packets\n"
      "Display all EIGRP packets\n"
      "Send Packets\n"
      "Receive Packets\n"
      "Detailed Information\n")
{
	uint32_t type = 0;
	unsigned long flag = EIGRP_DEBUG_SEND_RECV;
	eigrp_debug_scope_t scope = vty->node == CONFIG_NODE
					? EIGRP_DEBUG_SCOPE_CONFIG
					: EIGRP_DEBUG_SCOPE_TERMINAL;
	eigrp_result_t result;
	int idx = 0;

	if (argv_find(argv, argc, "hello", &idx))
		type = EIGRP_DEBUG_HELLO;
	else if (argv_find(argv, argc, "update", &idx))
		type = EIGRP_DEBUG_UPDATE;
	else if (argv_find(argv, argc, "query", &idx))
		type = EIGRP_DEBUG_QUERY;
	else if (argv_find(argv, argc, "ack", &idx))
		type = EIGRP_DEBUG_ACK;
	else if (argv_find(argv, argc, "probe", &idx))
		type = EIGRP_DEBUG_PROBE;
	else if (argv_find(argv, argc, "reply", &idx))
		type = EIGRP_DEBUG_REPLY;
	else if (argv_find(argv, argc, "request", &idx))
		type = EIGRP_DEBUG_REQUEST;
	else if (argv_find(argv, argc, "retry", &idx))
		type = EIGRP_DEBUG_RETRY;
	else if (argv_find(argv, argc, "siaquery", &idx))
		type = EIGRP_DEBUG_SIAQUERY;
	else if (argv_find(argv, argc, "siareply", &idx))
		type = EIGRP_DEBUG_SIAREPLY;
	else if (argv_find(argv, argc, "terse", &idx))
		type = EIGRP_DEBUG_PACKETS_TERSE;
	else if (argv_find(argv, argc, "all", &idx))
		type = EIGRP_DEBUG_PACKETS_ALL;

	if (argv_find(argv, argc, "send", &idx))
		flag = EIGRP_DEBUG_SEND;
	else if (argv_find(argv, argc, "receive", &idx))
		flag = EIGRP_DEBUG_RECV;
	if (argv_find(argv, argc, "detail", &idx))
		flag |= EIGRP_DEBUG_PACKET_DETAIL;

	result = eigrp_debug_packet_reset(type, flag, scope);
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS
					      : CMD_WARNING_CONFIG_FAILED;
}

/* Debug node. */
static int config_write_debug(struct vty *vty);
static struct cmd_node eigrp_debug_node = {
	.name = "debug",
	.node = DEBUG_NODE,
	.prompt = "",
	.config_write = config_write_debug,
};

/* Initialize debug commands. */
void eigrp_debug_init(void)
{
	install_node(&eigrp_debug_node);

#define EIGRP_INSTALL_DEBUG_NODE(_node)                                       \
	do {                                                                     \
		install_element((_node), &show_debugging_eigrp_cmd);               \
		install_element((_node), &debug_eigrp_event_cmd);                   \
		install_element((_node), &no_debug_eigrp_event_cmd);                \
		install_element((_node), &debug_eigrp_timers_cmd);                  \
		install_element((_node), &no_debug_eigrp_timers_cmd);               \
		install_element((_node), &debug_eigrp_fsm_cmd);                     \
		install_element((_node), &no_debug_eigrp_fsm_cmd);                  \
		install_element((_node), &debug_eigrp_nsf_cmd);                     \
		install_element((_node), &no_debug_eigrp_nsf_cmd);                  \
		install_element((_node), &debug_eigrp_frr_cmd);                     \
		install_element((_node), &no_debug_eigrp_frr_cmd);                  \
		install_element((_node), &debug_eigrp_neighbor_cmd);                \
		install_element((_node), &no_debug_eigrp_neighbor_cmd);             \
		install_element((_node), &debug_eigrp_notifications_cmd);           \
		install_element((_node), &no_debug_eigrp_notifications_cmd);        \
		install_element((_node), &debug_eigrp_packet_cmd);                  \
		install_element((_node), &no_debug_eigrp_packet_cmd);               \
		install_element((_node), &debug_eigrp_transmit_cmd);                \
		install_element((_node), &no_debug_eigrp_transmit_cmd);             \
		install_element((_node), &debug_eigrp_address_family_cmd);          \
		install_element((_node), &no_debug_eigrp_address_family_cmd);       \
		install_element((_node), &debug_eigrp_address_family_neighbor_cmd); \
		install_element((_node),                                            \
				&no_debug_eigrp_address_family_neighbor_cmd);          \
		install_element((_node),                                            \
				&debug_eigrp_address_family_notifications_cmd);        \
		install_element((_node),                                            \
				&no_debug_eigrp_address_family_notifications_cmd);     \
		install_element((_node), &debug_eigrp_address_family_summary_cmd);  \
		install_element((_node),                                            \
				&no_debug_eigrp_address_family_summary_cmd);           \
	} while (0)

	EIGRP_INSTALL_DEBUG_NODE(ENABLE_NODE);
	EIGRP_INSTALL_DEBUG_NODE(CONFIG_NODE);
#undef EIGRP_INSTALL_DEBUG_NODE
}
