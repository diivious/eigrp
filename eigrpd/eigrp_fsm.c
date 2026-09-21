#include <assert.h>
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRPd Finite State Machine (DUAL).
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 * This file contains functions for executing logic of finite state machine
 *
 *
 *                                +------------ +
 *                                |     (7)     |
 *                                |             v
 *                    +=====================================+
 *                    |                                     |
 *                    |              Passive                |
 *                    |                                     |
 *                    +=====================================+
 *                        ^     |     ^     ^     ^    |
 *                     (3)|     |  (1)|     |  (1)|    |
 *                        |  (0)|     |  (3)|     | (2)|
 *                        |     |     |     |     |    +---------------+
 *                        |     |     |     |     |                     \
 *              +--------+      |     |     |     +-----------------+    \
 *            /                /     /      |                        \    \
 *          /                /     /        +----+                    \    \
 *         |                |     |               |                    |    |
 *         |                v     |               |                    |    v
 *    +===========+   (6)  +===========+       +===========+   (6)   +===========+
 *    |           |------->|           |  (5)  |           |-------->|           |
 *    |           |   (4)  |           |------>|           |   (4)   |           |
 *    | ACTIVE 0  |<-------| ACTIVE 1  |       | ACTIVE 2  |<--------| ACTIVE 3  |
 * +--|           |     +--|           |    +--|           |      +--|           |
 * |  +===========+     |  +===========+    |  +===========+      |  +===========+
 * |       ^  |(5)      |      ^            |    ^    ^           |         ^
 * |       |  +---------|------|------------|----+    |           |         |
 * +-------+            +------+            +---------+           +---------+
 *    (7)                 (7)                  (7)                   (7)
 *
 * 0- input event other than query from successor, FC not satisfied
 * 1- last reply, FD is reset
 * 2- query from successor, FC not satisfied
 * 3- last reply, FC satisfied with current value of FDij
 * 4- distance increase while in active state
 * 5- query from successor while in active state
 * 6- last reply, FC not satisfied with current value of FDij
 * 7- state not changed, usually by receiving not last reply
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_debug.h"

/*
 * Prototypes
 */
int eigrp_fsm_event_keep_state(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_nq_fcn(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_q_fcn(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_lr(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_dinc(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_lr_fcs(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_lr_fcn(eigrp_fsm_action_message_t *);
int eigrp_fsm_event_qact(eigrp_fsm_action_message_t *);

//---------------------------------------------------------------------

/*
 * NSM - field of fields of struct containing one function each.
 * Which function is used depends on actual state of FSM and occurred
 * event(arrow in diagram). Usage:
 * NSM[actual/starting state][occurred event].func
 * Functions are should be executed within separate event.
 */
const struct {
	int (*func)(eigrp_fsm_action_message_t *);
} NSM[EIGRP_FSM_STATE_MAX][EIGRP_FSM_EVENT_MAX] = {
	{
		// PASSIVE STATE
		{eigrp_fsm_event_nq_fcn},     /* Event 0 */
		{eigrp_fsm_event_keep_state}, /* Event 1 */
		{eigrp_fsm_event_q_fcn},      /* Event 2 */
		{eigrp_fsm_event_keep_state}, /* Event 3 */
		{eigrp_fsm_event_keep_state}, /* Event 4 */
		{eigrp_fsm_event_keep_state}, /* Event 5 */
		{eigrp_fsm_event_keep_state}, /* Event 6 */
		{eigrp_fsm_event_keep_state}, /* Event 7 */
	},
	{
		// Active 0 state
		{eigrp_fsm_event_keep_state}, /* Event 0 */
		{eigrp_fsm_event_keep_state}, /* Event 1 */
		{eigrp_fsm_event_keep_state}, /* Event 2 */
		{eigrp_fsm_event_lr_fcs},     /* Event 3 */
		{eigrp_fsm_event_keep_state}, /* Event 4 */
		{eigrp_fsm_event_qact},	      /* Event 5 */
		{eigrp_fsm_event_lr_fcn},     /* Event 6 */
		{eigrp_fsm_event_keep_state}, /* Event 7 */
	},
	{
		// Active 1 state
		{eigrp_fsm_event_keep_state}, /* Event 0 */
		{eigrp_fsm_event_lr},	      /* Event 1 */
		{eigrp_fsm_event_keep_state}, /* Event 2 */
		{eigrp_fsm_event_keep_state}, /* Event 3 */
		{eigrp_fsm_event_dinc},	      /* Event 4 */
		{eigrp_fsm_event_qact},	      /* Event 5 */
		{eigrp_fsm_event_keep_state}, /* Event 6 */
		{eigrp_fsm_event_keep_state}, /* Event 7 */
	},
	{
		// Active 2 state
		{eigrp_fsm_event_keep_state}, /* Event 0 */
		{eigrp_fsm_event_keep_state}, /* Event 1 */
		{eigrp_fsm_event_keep_state}, /* Event 2 */
		{eigrp_fsm_event_lr_fcs},     /* Event 3 */
		{eigrp_fsm_event_keep_state}, /* Event 4 */
		{eigrp_fsm_event_keep_state}, /* Event 5 */
		{eigrp_fsm_event_lr_fcn},     /* Event 6 */
		{eigrp_fsm_event_keep_state}, /* Event 7 */
	},
	{
		// Active 3 state
		{eigrp_fsm_event_keep_state}, /* Event 0 */
		{eigrp_fsm_event_lr},	      /* Event 1 */
		{eigrp_fsm_event_keep_state}, /* Event 2 */
		{eigrp_fsm_event_keep_state}, /* Event 3 */
		{eigrp_fsm_event_dinc},	      /* Event 4 */
		{eigrp_fsm_event_keep_state}, /* Event 5 */
		{eigrp_fsm_event_keep_state}, /* Event 6 */
		{eigrp_fsm_event_keep_state}, /* Event 7 */
	},
};

static const char *packet_type2str(uint8_t packet_type)
{
	if (packet_type == EIGRP_OPC_UPDATE)
		return "Update";
	if (packet_type == EIGRP_OPC_REQUEST)
		return "Request";
	if (packet_type == EIGRP_OPC_QUERY)
		return "Query";
	if (packet_type == EIGRP_OPC_REPLY)
		return "Reply";
	if (packet_type == EIGRP_OPC_HELLO)
		return "Hello";
	if (packet_type == EIGRP_OPC_IPXSAP)
		return "IPXSAP";
	if (packet_type == EIGRP_OPC_ACK)
		return "Ack";
	if (packet_type == EIGRP_OPC_SIAQUERY)
		return "SIA Query";
	if (packet_type == EIGRP_OPC_SIAREPLY)
		return "SIA Reply";

	return "Unknown";
}

static const char *prefix_state2str(enum eigrp_fsm_states state)
{
	switch (state) {
	case EIGRP_FSM_STATE_PASSIVE:
		return "Passive";
	case EIGRP_FSM_STATE_ACTIVE_0:
		return "Active oij0";
	case EIGRP_FSM_STATE_ACTIVE_1:
		return "Active oij1";
	case EIGRP_FSM_STATE_ACTIVE_2:
		return "Active oij2";
	case EIGRP_FSM_STATE_ACTIVE_3:
		return "Active oij3";
	}

	return "Unknown";
}

static const char *fsm_state2str(enum eigrp_fsm_events event)
{
	switch (event) {
	case EIGRP_FSM_KEEP_STATE:
		return "Keep State Event";
	case EIGRP_FSM_EVENT_NQ_FCN:
		return "Non Query Event Feasability not satisfied";
	case EIGRP_FSM_EVENT_LR:
		return "Last Reply Event";
	case EIGRP_FSM_EVENT_Q_FCN:
		return "Query Event Feasability not satisfied";
	case EIGRP_FSM_EVENT_LR_FCS:
		return "Last Reply Event Feasability satisfied";
	case EIGRP_FSM_EVENT_DINC:
		return "Distance Increase Event";
	case EIGRP_FSM_EVENT_QACT:
		return "Query from Successor while in active state";
	case EIGRP_FSM_EVENT_LR_FCN:
		return "Last Reply Event, Feasibility not satisfied";
	}

	return "Unknown";
}

static const char *change2str(enum metric_change change)
{
	switch (change) {
	case METRIC_DECREASE:
		return "Decrease";
	case METRIC_SAME:
		return "Same";
	case METRIC_INCREASE:
		return "Increase";
	}

	return "Unknown";
}
/*
 * Main function in which are make decisions which event occurred.
 * msg - argument of type eigrp_fsm_action_message_t contain
 * details about what happen
 *
 * Return fsm state of occurred event (arrow in diagram).
 *
 */
static enum eigrp_fsm_events
eigrp_get_fsm_event(eigrp_fsm_action_message_t *msg)
{
	// Loading base information from message
	// eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;
	eigrp_route_descriptor_t *route = msg->route;
	uint8_t actual_state = prefix->state;
	enum metric_change change;

	if (route == NULL) {
		route = eigrp_topology_route_create(msg->adv_router->ei);
		route->adv_router = msg->adv_router;
		route->prefix = prefix;
		msg->route = route;
	}

	/*
	 * Calculate resultant metrics and insert to correct position
	 * in entries list
	 */
	change = eigrp_topology_update_distance(msg);

	/* Store for display later */
	msg->change = change;

	switch (actual_state) {
	case EIGRP_FSM_STATE_PASSIVE: {
		eigrp_route_descriptor_t *head = eigrp_list_node_data(eigrp_list_head(prefix->entries));

		if (head->reported_distance < prefix->fdistance) {
			return EIGRP_FSM_KEEP_STATE;
		}
		/*
		 * if best route doesn't satisfy feasibility condition it means
		 * move to active state
		 * dependently if it was query from successor
		 */
		if (msg->packet_type == EIGRP_OPC_QUERY) {
			return EIGRP_FSM_EVENT_Q_FCN;
		} else {
			return EIGRP_FSM_EVENT_NQ_FCN;
		}

		break;
	}
	case EIGRP_FSM_STATE_ACTIVE_0: {
		if (msg->packet_type == EIGRP_OPC_REPLY) {
			eigrp_route_descriptor_t *head =
				eigrp_list_node_data(eigrp_list_head(prefix->entries));

			eigrp_list_delete_data(prefix->rij, route->adv_router);
			if (prefix->rij->count)
				return EIGRP_FSM_KEEP_STATE;

			eigrp_log_info("All reply received");
			if (head->reported_distance < prefix->fdistance) {
				return EIGRP_FSM_EVENT_LR_FCS;
			}

			return EIGRP_FSM_EVENT_LR_FCN;
		} else if (msg->packet_type == EIGRP_OPC_QUERY
			   && (route->flags
			       & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)) {
			return EIGRP_FSM_EVENT_QACT;
		}

		return EIGRP_FSM_KEEP_STATE;

		break;
	}
	case EIGRP_FSM_STATE_ACTIVE_1: {
		if (msg->packet_type == EIGRP_OPC_QUERY
		    && (route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)) {
			return EIGRP_FSM_EVENT_QACT;
		} else if (msg->packet_type == EIGRP_OPC_REPLY) {
			eigrp_list_delete_data(prefix->rij, route->adv_router);

			if (change == METRIC_INCREASE
			    && (route->flags
				& EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)) {
				return EIGRP_FSM_EVENT_DINC;
			} else if (prefix->rij->count) {
				return EIGRP_FSM_KEEP_STATE;
			} else {
				eigrp_log_info("All reply received");
				return EIGRP_FSM_EVENT_LR;
			}
		} else if (msg->packet_type == EIGRP_OPC_UPDATE
			   && change == METRIC_INCREASE
			   && (route->flags
			       & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)) {
			return EIGRP_FSM_EVENT_DINC;
		}
		return EIGRP_FSM_KEEP_STATE;

		break;
	}
	case EIGRP_FSM_STATE_ACTIVE_2: {
		if (msg->packet_type == EIGRP_OPC_REPLY) {
			eigrp_route_descriptor_t *head =
				eigrp_list_node_data(eigrp_list_head(prefix->entries));

			eigrp_list_delete_data(prefix->rij, route->adv_router);
			if (prefix->rij->count) {
				return EIGRP_FSM_KEEP_STATE;
			} else {
				eigrp_log_info("All reply received");
				if (head->reported_distance
				    < prefix->fdistance) {
					return EIGRP_FSM_EVENT_LR_FCS;
				}

				return EIGRP_FSM_EVENT_LR_FCN;
			}
		}
		return EIGRP_FSM_KEEP_STATE;

		break;
	}
	case EIGRP_FSM_STATE_ACTIVE_3: {
		if (msg->packet_type == EIGRP_OPC_REPLY) {
			eigrp_list_delete_data(prefix->rij, route->adv_router);

			if (change == METRIC_INCREASE
			    && (route->flags
				& EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)) {
				return EIGRP_FSM_EVENT_DINC;
			} else if (prefix->rij->count) {
				return EIGRP_FSM_KEEP_STATE;
			} else {
				eigrp_log_info("All reply received");
				return EIGRP_FSM_EVENT_LR;
			}
		} else if (msg->packet_type == EIGRP_OPC_UPDATE
			   && change == METRIC_INCREASE
			   && (route->flags
			       & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)) {
			return EIGRP_FSM_EVENT_DINC;
		}
		return EIGRP_FSM_KEEP_STATE;

		break;
	}
	}

	return EIGRP_FSM_KEEP_STATE;
}

/*
 * Function made to execute in separate event.
 * Load argument from event and execute proper NSM function
 */
int eigrp_fsm_event(eigrp_fsm_action_message_t *msg)
{
	enum eigrp_fsm_events event = eigrp_get_fsm_event(msg);

	if (IS_DEBUG_EIGRP(0, FSM)
	    || eigrp_debug_address_family_enabled(
		    msg->eigrp, EIGRP_DEBUG_AF_ROUTE,
		    msg->adv_router ? &msg->adv_router->src : NULL)) {
		char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";

		eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
				      &msg->prefix->destination);
		eigrp_log_debug(
			"EIGRP AS: %d State: %s Event: %s Network: %s Packet Type: %s Reply RIJ Count: %d change: %s",
			msg->eigrp->AS, prefix_state2str(msg->prefix->state),
			fsm_state2str(event), prefix_buf,
			packet_type2str(msg->packet_type),
			msg->prefix->rij->count, change2str(msg->change));
	}
	(*(NSM[msg->prefix->state][event].func))(msg);

	return 1;
}

/*
 * Function of event 0.
 *
 */
int eigrp_fsm_event_nq_fcn(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;

	/*
	 * RFC 7868 active-state invariant:
	 * entering ACTIVE must not rewrite the destination successor, FD, RD,
	 * current distance, or advertised metric. The triggering route metric
	 * update was already recorded as route/CD state by
	 * eigrp_topology_update_distance().
	 */
	prefix->state = EIGRP_FSM_STATE_ACTIVE_1;

	if (eigrp_nbr_count_get(eigrp)) {
		prefix->req_action |= EIGRP_FSM_NEED_QUERY;
		eigrp_list_add(eigrp->topology_changes, prefix);
	} else {
		eigrp_fsm_event_lr(msg); // in the case that there are no more
					 // neighbors left
	}

	return 1;
}

int eigrp_fsm_event_q_fcn(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;

	/*
	 * RFC 7868 active-state invariant: a QUERY from the current successor
	 * can move the destination into ACTIVE, but the successor, FD, RD,
	 * current distance, and advertised metric remain frozen until PASSIVE.
	 */
	prefix->state = EIGRP_FSM_STATE_ACTIVE_3;
	if (eigrp_nbr_count_get(eigrp)) {
		prefix->req_action |= EIGRP_FSM_NEED_QUERY;
		eigrp_list_add(eigrp->topology_changes, prefix);
	} else {
		eigrp_fsm_event_lr(msg); // in the case that there are no more
					 // neighbors left
	}

	return 1;
}

int eigrp_fsm_event_keep_state(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;
	eigrp_route_descriptor_t *route = eigrp_list_node_data(eigrp_list_head(prefix->entries));

	if (prefix->state == EIGRP_FSM_STATE_PASSIVE) {
		if (!eigrp_metrics_is_same(prefix->reported_metric,
					   route->total_metric)) {
			prefix->rdistance = prefix->fdistance =
				prefix->distance = route->distance;
			prefix->reported_metric = route->total_metric;
			if (msg->packet_type == EIGRP_OPC_QUERY)
				eigrp_reply_send(eigrp, msg->adv_router,
						 prefix);
			prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
			eigrp_list_add(eigrp->topology_changes, prefix);
		}
		eigrp_topology_update_node_flags(eigrp, prefix);
		eigrp_update_routing_table(eigrp, prefix);
	}

	if (msg->packet_type == EIGRP_OPC_QUERY)
		eigrp_reply_send(eigrp, msg->adv_router, prefix);

	return 1;
}

int eigrp_fsm_event_lr(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;
	eigrp_route_descriptor_t *route = eigrp_list_node_data(eigrp_list_head(prefix->entries));

	prefix->fdistance = prefix->distance = prefix->rdistance =
		route->distance;
	prefix->reported_metric = route->total_metric;

	if (prefix->state == EIGRP_FSM_STATE_ACTIVE_3) {
		eigrp_list_t *successors = eigrp_topology_get_successor(prefix);

		assert(successors); // It's like Napolean and Waterloo

		route = eigrp_list_node_data(eigrp_list_head(successors));
		eigrp_reply_send(eigrp, route->adv_router, prefix);
		eigrp_list_delete(&successors);
	}

	prefix->state = EIGRP_FSM_STATE_PASSIVE;
	prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
	eigrp_list_add(eigrp->topology_changes, prefix);
	eigrp_topology_update_node_flags(eigrp, prefix);
	eigrp_update_routing_table(eigrp, prefix);
	eigrp_update_topology_table_prefix(eigrp, eigrp->topology_table,
					   prefix);

	return 1;
}

int eigrp_fsm_event_dinc(eigrp_fsm_action_message_t *msg)
{
	/*
	 * A successor distance increase while ACTIVE only changes the DUAL
	 * origin flag. Destination-level distance/FD/RD data stays frozen
	 * until the route returns to PASSIVE.
	 */
	msg->prefix->state = msg->prefix->state == EIGRP_FSM_STATE_ACTIVE_1
				     ? EIGRP_FSM_STATE_ACTIVE_0
				     : EIGRP_FSM_STATE_ACTIVE_2;
	if (!msg->prefix->rij->count)
		(*(NSM[msg->prefix->state][eigrp_get_fsm_event(msg)].func))(
			msg);

	return 1;
}

int eigrp_fsm_event_lr_fcs(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;
	eigrp_route_descriptor_t *route = eigrp_list_node_data(eigrp_list_head(prefix->entries));

	prefix->state = EIGRP_FSM_STATE_PASSIVE;
	prefix->distance = prefix->rdistance = route->distance;
	prefix->reported_metric = route->total_metric;
	prefix->fdistance = prefix->fdistance > prefix->distance
				    ? prefix->distance
				    : prefix->fdistance;
	if (prefix->state == EIGRP_FSM_STATE_ACTIVE_2) {
		eigrp_list_t *successors = eigrp_topology_get_successor(prefix);

		assert(successors); // Having a spoon and all you need is a
		// knife
		route = eigrp_list_node_data(eigrp_list_head(successors));
		eigrp_reply_send(eigrp, route->adv_router, prefix);

		eigrp_list_delete(&successors);
	}
	prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
	eigrp_list_add(eigrp->topology_changes, prefix);
	eigrp_topology_update_node_flags(eigrp, prefix);
	eigrp_update_routing_table(eigrp, prefix);
	eigrp_update_topology_table_prefix(eigrp, eigrp->topology_table,
					   prefix);

	return 1;
}

int eigrp_fsm_event_lr_fcn(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;

	/*
	 * Last reply but FC is still not satisfied: remain ACTIVE and start
	 * another diffusing computation. Do not adopt a new successor or
	 * rewrite destination FD/RD/distance while ACTIVE.
	 */
	prefix->state = (prefix->state == EIGRP_FSM_STATE_ACTIVE_0)
				? EIGRP_FSM_STATE_ACTIVE_1
				: EIGRP_FSM_STATE_ACTIVE_3;

	if (eigrp_nbr_count_get(eigrp)) {
		prefix->req_action |= EIGRP_FSM_NEED_QUERY;
		eigrp_list_add(eigrp->topology_changes, prefix);
	} else {
		eigrp_fsm_event_lr(msg); // in the case that there are no more
					 // neighbors left
	}

	return 1;
}

int eigrp_fsm_event_qact(eigrp_fsm_action_message_t *msg)
{
	/*
	 * QUERY from the current successor while already ACTIVE only changes
	 * the DUAL origin flag. Destination-level distance/FD/RD data stays
	 * frozen until PASSIVE.
	 */
	msg->prefix->state = EIGRP_FSM_STATE_ACTIVE_2;

	return 1;
}
