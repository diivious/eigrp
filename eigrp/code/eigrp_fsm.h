// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Finite State Machine (DUAL).
 * Copyright (C) 2013-2014, 2026
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 */

#ifndef _ZEBRA_EIGRP_FSM_H
#define _ZEBRA_EIGRP_FSM_H

/* EIGRP Finite State Machine */
typedef enum {
	EIGRP_CONNECTED,
	EIGRP_INT,
	EIGRP_EXT,
} msg_data_t;

typedef struct eigrp_fsm_action_message {
	uint8_t packet_type;			// UPDATE, QUERY, SIAQUERY, SIAREPLY
	eigrp_instance_t *eigrp;		// which event sent mesg
	eigrp_nbr_t *adv_router;		// advertising neighbor
	eigrp_route_descriptor_t *route;	//
	eigrp_prefix_descriptor_t *prefix;	//
	msg_data_t data_type;			// internal or external tlv type
	eigrp_metrics_t metrics;		//
	enum metric_change change;		//
} eigrp_fsm_action_message_t;

extern int eigrp_fsm_event(eigrp_fsm_action_message_t *msg);
void eigrp_fsm_reply_status_add(eigrp_prefix_descriptor_t *prefix,
				eigrp_nbr_t *nbr);
bool eigrp_fsm_reply_status_remove(eigrp_prefix_descriptor_t *prefix,
				   eigrp_nbr_t *nbr);
bool eigrp_fsm_reply_status_pending(const eigrp_prefix_descriptor_t *prefix,
				    const eigrp_nbr_t *nbr);
void eigrp_fsm_sia_reply_received(eigrp_prefix_descriptor_t *prefix,
				  eigrp_nbr_t *nbr);
void eigrp_fsm_active_timer_start(eigrp_instance_t *eigrp,
				  eigrp_prefix_descriptor_t *prefix);
void eigrp_fsm_query_sent(eigrp_instance_t *eigrp,
			  eigrp_prefix_descriptor_t *prefix);
void eigrp_fsm_active_timer_stop(eigrp_prefix_descriptor_t *prefix);


#endif /* _ZEBRA_EIGRP_DUAL_H */
