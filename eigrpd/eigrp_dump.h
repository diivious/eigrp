// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Dump Functions and Debbuging.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *
 */

#ifndef _ZEBRA_EIGRPD_DUMP_H_
#define _ZEBRA_EIGRPD_DUMP_H_

#include "eigrpd/eigrp_result.h"

#define EIGRP_TIME_DUMP_SIZE 16

/* general debug flags */
extern unsigned long term_debug_eigrp;
extern unsigned long conf_debug_eigrp;
#define EIGRP_DEBUG_EVENT 0x01
#define EIGRP_DEBUG_DETAIL 0x02
#define EIGRP_DEBUG_TIMERS 0x04
#define EIGRP_DEBUG_FSM 0x08
#define EIGRP_DEBUG_NSF 0x10
#define EIGRP_DEBUG_FAST_REROUTE 0x20

/* neighbor debug flags */
extern unsigned long term_debug_eigrp_nei;
extern unsigned long conf_debug_eigrp_nei;
#define EIGRP_DEBUG_NEI 0x01
#define EIGRP_DEBUG_NEI_SIATIMER 0x02
#define EIGRP_DEBUG_NEI_STATIC 0x04
#define EIGRP_DEBUG_NEI_ALL \
	(EIGRP_DEBUG_NEI | EIGRP_DEBUG_NEI_SIATIMER | EIGRP_DEBUG_NEI_STATIC)

/*
 * Packet debug categories are not identical to wire opcodes.  ACK is a
 * HELLO carrying a non-zero acknowledgment number (RFC 7868), and RETRY is
 * a local reliable-transport event.
 */
typedef enum eigrp_debug_packet_category {
	EIGRP_DEBUG_PACKET_UPDATE = 0,
	EIGRP_DEBUG_PACKET_REQUEST,
	EIGRP_DEBUG_PACKET_QUERY,
	EIGRP_DEBUG_PACKET_REPLY,
	EIGRP_DEBUG_PACKET_HELLO,
	EIGRP_DEBUG_PACKET_PROBE,
	EIGRP_DEBUG_PACKET_ACK,
	EIGRP_DEBUG_PACKET_RETRY,
	EIGRP_DEBUG_PACKET_SIAQUERY,
	EIGRP_DEBUG_PACKET_SIAREPLY,
	EIGRP_DEBUG_PACKET_CATEGORY_MAX
} eigrp_debug_packet_category_t;

typedef enum eigrp_debug_scope {
	EIGRP_DEBUG_SCOPE_TERMINAL = 0,
	EIGRP_DEBUG_SCOPE_CONFIG
} eigrp_debug_scope_t;

/* packet category selection masks */
#define EIGRP_DEBUG_UPDATE (1U << EIGRP_DEBUG_PACKET_UPDATE)
#define EIGRP_DEBUG_REQUEST (1U << EIGRP_DEBUG_PACKET_REQUEST)
#define EIGRP_DEBUG_QUERY (1U << EIGRP_DEBUG_PACKET_QUERY)
#define EIGRP_DEBUG_REPLY (1U << EIGRP_DEBUG_PACKET_REPLY)
#define EIGRP_DEBUG_HELLO (1U << EIGRP_DEBUG_PACKET_HELLO)
#define EIGRP_DEBUG_PROBE (1U << EIGRP_DEBUG_PACKET_PROBE)
#define EIGRP_DEBUG_ACK (1U << EIGRP_DEBUG_PACKET_ACK)
#define EIGRP_DEBUG_RETRY (1U << EIGRP_DEBUG_PACKET_RETRY)
#define EIGRP_DEBUG_SIAQUERY (1U << EIGRP_DEBUG_PACKET_SIAQUERY)
#define EIGRP_DEBUG_SIAREPLY (1U << EIGRP_DEBUG_PACKET_SIAREPLY)

/* Cisco's all/verbose packet set does not implicitly enable retry events. */
#define EIGRP_DEBUG_PACKETS_ALL                                                \
	(EIGRP_DEBUG_UPDATE | EIGRP_DEBUG_REQUEST | EIGRP_DEBUG_QUERY           \
	 | EIGRP_DEBUG_REPLY | EIGRP_DEBUG_HELLO | EIGRP_DEBUG_PROBE            \
	 | EIGRP_DEBUG_ACK | EIGRP_DEBUG_SIAQUERY | EIGRP_DEBUG_SIAREPLY)
#define EIGRP_DEBUG_PACKETS_TERSE (EIGRP_DEBUG_PACKETS_ALL & ~EIGRP_DEBUG_HELLO)
#define EIGRP_DEBUG_PACKET_VALID_MASK                                          \
	(EIGRP_DEBUG_PACKETS_ALL | EIGRP_DEBUG_RETRY)

extern unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];
extern unsigned long conf_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];

/* packet direction/detail flags */
#define EIGRP_DEBUG_SEND 0x01
#define EIGRP_DEBUG_RECV 0x02
#define EIGRP_DEBUG_SEND_RECV 0x03
#define EIGRP_DEBUG_PACKET_DETAIL 0x04
#define EIGRP_DEBUG_PACKET_FLAG_MASK                                           \
	(EIGRP_DEBUG_SEND_RECV | EIGRP_DEBUG_PACKET_DETAIL)

/* Cisco EIGRP transmit-debug categories. */
extern unsigned long term_debug_eigrp_transmit;
extern unsigned long conf_debug_eigrp_transmit;
#define EIGRP_DEBUG_TRANSMIT_ACK 0x0001
#define EIGRP_DEBUG_TRANSMIT_BUILD 0x0002
#define EIGRP_DEBUG_TRANSMIT_DETAIL 0x0004
#define EIGRP_DEBUG_TRANSMIT_LINK 0x0008
#define EIGRP_DEBUG_TRANSMIT_PACKETIZE 0x0010
#define EIGRP_DEBUG_TRANSMIT_PEERDOWN 0x0020
#define EIGRP_DEBUG_TRANSMIT_SIA 0x0040
#define EIGRP_DEBUG_TRANSMIT_STARTUP 0x0080
#define EIGRP_DEBUG_TRANSMIT_STRANGE 0x0100
#define EIGRP_DEBUG_TRANSMIT_ALL                                               \
	(EIGRP_DEBUG_TRANSMIT_ACK | EIGRP_DEBUG_TRANSMIT_BUILD                    \
	 | EIGRP_DEBUG_TRANSMIT_DETAIL | EIGRP_DEBUG_TRANSMIT_LINK                \
	 | EIGRP_DEBUG_TRANSMIT_PACKETIZE | EIGRP_DEBUG_TRANSMIT_PEERDOWN         \
	 | EIGRP_DEBUG_TRANSMIT_SIA | EIGRP_DEBUG_TRANSMIT_STARTUP                \
	 | EIGRP_DEBUG_TRANSMIT_STRANGE)

/* zebra debug flags */
extern unsigned long term_debug_eigrp_zebra;
extern unsigned long conf_debug_eigrp_zebra;
#define EIGRP_DEBUG_ZEBRA_INTERFACE 0x01
#define EIGRP_DEBUG_ZEBRA_REDISTRIBUTE 0x02
#define EIGRP_DEBUG_ZEBRA_RIB EIGRP_DEBUG_ZEBRA_REDISTRIBUTE
#define EIGRP_DEBUG_ZEBRA 0x03

/* Macro for setting debug option. */
#define CONF_DEBUG_NEI_ON(a, b) conf_debug_eigrp_nei |= (b)
#define CONF_DEBUG_NEI_OFF(a, b) conf_debug_eigrp_nei &= ~(b)
#define TERM_DEBUG_NEI_ON(a, b) term_debug_eigrp_nei |= (b)
#define TERM_DEBUG_NEI_OFF(a, b) term_debug_eigrp_nei &= ~(b)
#define DEBUG_NEI_ON(a, b)                                                     \
	do {                                                                   \
		CONF_DEBUG_NEI_ON(a, b);                                       \
		TERM_DEBUG_NEI_ON(a, b);                                       \
	} while (0)
#define DEBUG_NEI_OFF(a, b)                                                    \
	do {                                                                   \
		CONF_DEBUG_NEI_OFF(a, b);                                      \
		TERM_DEBUG_NEI_OFF(a, b);                                      \
	} while (0)

#define CONF_DEBUG_PACKET_ON(a, b) conf_debug_eigrp_packet[a] |= (b)
#define CONF_DEBUG_PACKET_OFF(a, b) conf_debug_eigrp_packet[a] &= ~(b)
#define TERM_DEBUG_PACKET_ON(a, b) term_debug_eigrp_packet[a] |= (b)
#define TERM_DEBUG_PACKET_OFF(a, b) term_debug_eigrp_packet[a] &= ~(b)
#define DEBUG_PACKET_ON(a, b)                                                  \
	do {                                                                   \
		CONF_DEBUG_PACKET_ON(a, b);                                    \
		TERM_DEBUG_PACKET_ON(a, b);                                    \
	} while (0)
#define DEBUG_PACKET_OFF(a, b)                                                 \
	do {                                                                   \
		CONF_DEBUG_PACKET_OFF(a, b);                                   \
		TERM_DEBUG_PACKET_OFF(a, b);                                   \
	} while (0)

#define CONF_DEBUG_TRANSMIT_ON(a, b) conf_debug_eigrp_transmit |= (b)
#define CONF_DEBUG_TRANSMIT_OFF(a, b) conf_debug_eigrp_transmit &= ~(b)
#define TERM_DEBUG_TRANSMIT_ON(a, b) term_debug_eigrp_transmit |= (b)
#define TERM_DEBUG_TRANSMIT_OFF(a, b) term_debug_eigrp_transmit &= ~(b)
#define DEBUG_TRANSMIT_ON(a, b)                                                \
	do {                                                                   \
		CONF_DEBUG_TRANSMIT_ON(a, b);                                  \
		TERM_DEBUG_TRANSMIT_ON(a, b);                                  \
	} while (0)
#define DEBUG_TRANSMIT_OFF(a, b)                                               \
	do {                                                                   \
		CONF_DEBUG_TRANSMIT_OFF(a, b);                                 \
		TERM_DEBUG_TRANSMIT_OFF(a, b);                                 \
	} while (0)

#define CONF_DEBUG_ON(a, b) conf_debug_eigrp |= (EIGRP_DEBUG_##b)
#define CONF_DEBUG_OFF(a, b) conf_debug_eigrp &= ~(EIGRP_DEBUG_##b)
#define TERM_DEBUG_ON(a, b) term_debug_eigrp |= (EIGRP_DEBUG_##b)
#define TERM_DEBUG_OFF(a, b) term_debug_eigrp &= ~(EIGRP_DEBUG_##b)
#define DEBUG_ON(a, b)                                                         \
	do {                                                                   \
		CONF_DEBUG_ON(a, b);                                           \
		TERM_DEBUG_ON(a, b);                                           \
	} while (0)
#define DEBUG_OFF(a, b)                                                        \
	do {                                                                   \
		CONF_DEBUG_OFF(a, b);                                          \
		TERM_DEBUG_OFF(a, b);                                          \
	} while (0)

/* Macro for checking debug option. */
#define IS_DEBUG_EIGRP_PACKET(a, b)                                            \
	(term_debug_eigrp_packet[a] & EIGRP_DEBUG_##b)
#define IS_DEBUG_EIGRP_TRANSMIT(a, b)                                          \
	(term_debug_eigrp_transmit & EIGRP_DEBUG_TRANSMIT_##b)
#define IS_DEBUG_EIGRP_NEI(a, b) (term_debug_eigrp_nei & EIGRP_DEBUG_##b)
#define IS_DEBUG_EIGRP(a, b) (term_debug_eigrp & EIGRP_DEBUG_##b)
#define IS_DEBUG_EIGRP_EVENT IS_DEBUG_EIGRP(event, EVENT)


/* Non-packet debug targets. */
extern eigrp_result_t eigrp_debug_event_set(bool detail,
					 eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_event_reset(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_timers_set(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_timers_reset(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_fsm_set(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_fsm_reset(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_nsf_set(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_nsf_reset(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_fast_reroute_set(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_fast_reroute_reset(eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_neighbor_set(unsigned long flags,
					    eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_neighbor_reset(unsigned long flags,
					      eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_notifications_set(unsigned long flags,
						 eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_notifications_reset(unsigned long flags,
						   eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_transmit_set(unsigned long flags,
					    eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_transmit_reset(unsigned long flags,
					      eigrp_debug_scope_t scope);

typedef enum eigrp_debug_address_family_category {
	EIGRP_DEBUG_AF_ROUTE = 0,
	EIGRP_DEBUG_AF_NEIGHBOR,
	EIGRP_DEBUG_AF_NOTIFICATIONS,
	EIGRP_DEBUG_AF_SUMMARY,
	EIGRP_DEBUG_AF_CATEGORY_MAX
} eigrp_debug_address_family_category_t;

extern eigrp_result_t eigrp_debug_address_family_set(
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor, eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_address_family_reset(
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor, eigrp_debug_scope_t scope);
extern bool eigrp_debug_address_family_enabled(
	eigrp_instance_t *eigrp, eigrp_debug_address_family_category_t category,
	const eigrp_addr_t *neighbor);
extern bool eigrp_debug_address_family_config_enabled(
	const eigrp_address_family_config_t *af,
	eigrp_debug_address_family_category_t category);

/* Runtime debug hooks owned by the protocol transitions they describe. */
extern void eigrp_debug_neighbor_state(eigrp_neighbor_t *nbr, uint8_t old_state,
	uint8_t new_state);
extern void eigrp_debug_neighbor_sia(eigrp_neighbor_t *nbr, const char *event);
extern void eigrp_debug_nsf_event(const eigrp_instance_t *eigrp,
	const eigrp_neighbor_t *nbr, uint32_t flags, const char *event);
extern void eigrp_debug_transmit_event(unsigned long category,
	const eigrp_instance_t *eigrp, const eigrp_interface_t *ei,
	const eigrp_neighbor_t *nbr, const char *format, ...) PRINTFRR(5, 6);

/* Packet-debug targets and runtime hooks. */
extern eigrp_result_t eigrp_debug_packet_set(uint32_t packet_mask,
					 unsigned long flags,
					 eigrp_debug_scope_t scope);
extern eigrp_result_t eigrp_debug_packet_reset(uint32_t packet_mask,
					 unsigned long flags,
					 eigrp_debug_scope_t scope);
extern bool eigrp_debug_packet_any_enabled(unsigned long direction);
extern const char *eigrp_debug_packet_category_name(
	eigrp_debug_packet_category_t category);
extern void eigrp_debug_packet_send(eigrp_interface_t *ei,
				    const eigrp_packet_t *packet, int send_result);
extern void eigrp_debug_packet_receive(eigrp_interface_t *ei,
				       const eigrp_addr_t *source,
				       const eigrp_addr_t *destination,
				       const eigrp_header_t *header,
				       uint16_t length);
extern void eigrp_debug_packet_retry(eigrp_neighbor_t *nbr,
				     const eigrp_packet_t *packet,
				     uint8_t retry_count);

/* Prototypes. */
extern void eigrp_ip_header_dump(struct ip *);
extern void eigrp_header_dump(struct eigrp_header *);

extern void show_ip_eigrp_interface_header(struct vty *, eigrp_instance_t *);
extern void show_ip_eigrp_neighbor_header(struct vty *, eigrp_instance_t *);
extern void show_ip_eigrp_topology_header(struct vty *, eigrp_instance_t *);
extern void show_ip_eigrp_interface_detail(struct vty *, eigrp_instance_t *,
				   eigrp_interface_t *);
extern void show_ip_eigrp_interface_sub(struct vty *, eigrp_instance_t *,
				eigrp_interface_t *);
extern void show_ip_eigrp_neighbor_sub(struct vty *, eigrp_neighbor_t *, int);
extern void show_ip_eigrp_prefix_descriptor(struct vty *,
					    eigrp_prefix_descriptor_t *);
extern void show_ip_eigrp_route_descriptor(struct vty *vty, eigrp_instance_t *,
					   eigrp_route_descriptor_t *,
					   bool *first);

extern void eigrp_debug_init(void);

#endif /* _ZEBRA_EIGRPD_DUMP_H_ */
