// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Neighbor Handling.
 * Copyright (C) 2013-2016, 2026
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
 *
 */

#ifndef _ZEBRA_EIGRP_NEIGHBOR_H
#define _ZEBRA_EIGRP_NEIGHBOR_H

#include <stddef.h>

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_instance.h"
#include "eigrp_types.h"

/* Protective defaults for an AF runtime, not protocol-defined limits.  Leave
 * room for large hub/spoke designs with over 1,000 live peers per interface. */
#define EIGRP_NEIGHBOR_MAX_PER_INTERFACE 2048U
#define EIGRP_NEIGHBOR_MAX_PER_INSTANCE 8192U

/* Caller must hold instance->peer_lock (ingress authentication already does). */
bool eigrp_nbr_admit_locked(const eigrp_intf_t *ei);

/* Prefix-limit admission is orthogonal to the normal EIGRP DOWN/PENDING/UP
 * handshake. A suspended neighbor is DOWN operationally, but remains published
 * so its penalty identity survives until explicit deletion. */
typedef enum eigrp_nbr_limit_state {
	EIGRP_NBR_LIMIT_NONE = 0,
	EIGRP_NBR_LIMIT_SUSPENDED,
	EIGRP_NBR_LIMIT_LOCKED,
} eigrp_nbr_limit_state_t;

/* Neighbor Data Structure */
typedef struct eigrp_neighbor {

	uint8_t os_rel_major; // system version - just for show
	uint8_t os_rel_minor; // system version - just for show

	/* TLV version and packet vectors for this neighbor. */
	uint8_t tlv_rel_major; // eigrp version - tells us what TLV format to use
	uint8_t tlv_rel_minor; // eigrp version - tells us what TLV format to use
	uint8_t tlv_version;

	eigrp_packet_decoder_t decoder;
	eigrp_packet_encoder_t encoder;

	uint8_t K1;
	uint8_t K2;
	uint8_t K3;
	uint8_t K4;
	uint8_t K5;
	uint8_t K6;

	/* This neighbor's parent eigrp interface. */
	eigrp_intf_t *ei;
	bool published; /* Protected by eigrp_instance::peer_lock. */
	uint64_t generation; /* Prevent queued packets crossing neighbor lifetimes. */
	uint64_t multicast_epoch; /* Changes on every UP transition. */

	/* EIGRP neighbor Information */
	uint8_t state; /* normal adjacency FSM status */
	eigrp_nbr_limit_state_t limit_state; /* prefix-limit admission status */
	/* The receive path marks DOWN immediately, but drains DUAL/RTP only
	 * after the packet TLV walker returns. Owned by the AF thread. */
	bool limit_cleanup_pending;
	uint8_t limit_previous_state;
	eigrp_prefix_limit_t limit_pending_policy;
	/* Maintained on actual RDB insert/delete, so admission stays O(1). */
	uint32_t limit_prefix_count;
	bool limit_prefix_count_valid;

	/* Prefix-limit penalty lifetime is owned by this peer.  Protocol timers
	 * are cancelled on DOWN; these survive until restart/reset or DELETE. */
	uint16_t limit_restarts;
	uint32_t limit_penalty_q16;
	uint64_t limit_penalty_updated_msec;
	uint64_t limit_restart_due_msec;
	uint64_t limit_reset_due_msec;
	eigrp_timer_event_t *t_limit_restart;
	eigrp_timer_event_t *t_limit_reset;

	uint32_t recv_sequence_number; /* Last received sequence Number. */
	uint32_t peer_init_sequence_number; /* Accepted peer INIT, for duplicate detection. */
	/* Only an uncompleted peer INIT exchange may retransmit this sequence.
	 * Do not reserve a real RTP sequence number as a completion sentinel. */
	bool peer_init_retransmit_eligible;
	/* A repeated INIT before EOT is ambiguous: it may be an RTP retry or
	 * a restarted peer with the same INIT sequence.  Reconcile and re-send
	 * topology once its EOT arrives instead of flapping the adjacency. */
	bool peer_init_resync_pending;
	eigrp_list_t *peer_init_resync_prefixes;
	uint32_t init_sequence_number;

	/* If a packet is unacknowledged, retry it up to the transport limit. */
	uint8_t retrans_counter;
	uint64_t retransmissions;
	uint64_t up_since_msec;

	/* Per-neighbor Reliable Transport Protocol RTT/RTO state. */
	bool srtt_valid;
	uint32_t srtt_msec;
	uint32_t rttvar_msec;
	uint32_t rto_msec;

	eigrp_addr_t src;		/* Neighbor Src address. */

	/* Timer values. */
	uint16_t v_holddown;

	/* Events. */
	eigrp_timer_event_t *t_holddown;
	eigrp_timer_event_t *t_nbr_send_gr; /* event for sending multiple GR packet
					 chunks */

	eigrp_packet_queue_t *retrans_queue;

	/* Initial table synchronization is a per-peer, ACK-driven producer.
	 * Topology table nodes are append-only until instance shutdown; the
	 * cursor is the last fully processed node, never a route descriptor. */
	bool sync_active;
	eigrp_table_node_t *sync_cursor;
	eigrp_packet_t *sync_packet; /* partially encoded, not yet enqueued */
	uint32_t sync_route_count;
	eigrp_prefix_t *sync_summary_seen;
	size_t sync_summary_count;
	size_t sync_summary_capacity;
	eigrp_timer_event_t *t_sync;
	uint64_t sync_backpressure_count;
	uint64_t sync_resume_count;


	/* Reliable multicast conditional-receive state. */
	bool cr_mode;
	uint32_t cr_sequence;

	uint32_t crypt_seqnum; /* Cryptographic Sequence Number. */

	/* prefixes not received from neighbor during Graceful restart */
	eigrp_list_t *nbr_gr_prefixes;
	/* prefixes not yet send to neighbor during Graceful restart */
	eigrp_list_t *nbr_gr_prefixes_send;
	/* if packet is first or last during Graceful restart */
	enum Packet_part_type nbr_gr_packet_type;

} eigrp_nbr_t;


/* Prototypes */
extern eigrp_nbr_t *eigrp_nbr_lookup(eigrp_intf_t *, struct eigrp_header *,
					  eigrp_addr_t *);
extern eigrp_nbr_t *eigrp_nbr_create(eigrp_intf_t *, eigrp_addr_t *);
/* DOWN clears operational ownership but retains the neighbor identity.
 * DELETE additionally unpublishes the peer and frees all remaining state. */
void eigrp_nbr_down(eigrp_nbr_t *nbr);
extern void eigrp_nbr_delete(eigrp_nbr_t *neigh);
/* Task 1 lifecycle API: scheduling/violation policy belongs to later tasks. */
bool eigrp_nbr_suspend(eigrp_nbr_t *nbr, eigrp_nbr_limit_state_t state);
void eigrp_nbr_release(eigrp_nbr_t *nbr);
/* Task 2 timer engine.  Task 3 invokes this when a hard limit is crossed.
 * Per-peer timer options inherit from neighbor-all, then process settings. */
/* Mark prefix-limit DOWN without retiring descriptors used by this packet. */
bool eigrp_nbr_limit_mark(eigrp_nbr_t *nbr,
	const eigrp_prefix_limit_t *limit);
/* Complete DOWN and start the penalty only at a safe receive boundary. */
void eigrp_nbr_limit_cleanup(eigrp_nbr_t *nbr);
bool eigrp_nbr_limit_exceeded(eigrp_nbr_t *nbr,
	const eigrp_prefix_limit_t *limit);
void eigrp_nbr_limit_restart_expired(void *arg);
void eigrp_nbr_limit_reset_expired(void *arg);
void eigrp_nbr_limit_timers_cancel(eigrp_nbr_t *nbr);

bool eigrp_nbr_eligible(const eigrp_nbr_t *nbr);
/* Read-only prefix-limit status for operational state snapshots. */
char eigrp_nbr_limit_status(const eigrp_nbr_t *nbr);
uint32_t eigrp_nbr_limit_seconds(uint64_t deadline_msec, uint64_t now_msec);

extern void eigrp_nbr_holddown_expired(void *arg);

extern void eigrp_nbr_holddown_update(eigrp_nbr_t *);
extern void eigrp_nbr_state_update(eigrp_operation_t, eigrp_nbr_t *, uint8_t state);
extern void eigrp_nbr_state_update_reason(eigrp_operation_t, eigrp_nbr_t *,
	uint8_t state, uint16_t reason);
extern void eigrp_nbr_codec_select(eigrp_nbr_t *, uint8_t tlv_version);
extern void eigrp_nbr_codec_update(eigrp_instance_t *);
extern uint8_t eigrp_nbr_state(eigrp_nbr_t *);
extern int eigrp_nbr_count(eigrp_instance_t *);
extern const char *eigrp_nbr_state_str(eigrp_nbr_t *);

/* Per-neighbor Reliable Transport Protocol RTT/RTO state. */
void eigrp_nbr_rtt_clear(eigrp_nbr_t *nbr);
void eigrp_nbr_srtt_update(eigrp_nbr_t *nbr,
				const eigrp_packet_t *packet);
void eigrp_nbr_rto_backoff(eigrp_nbr_t *nbr);
uint32_t eigrp_nbr_rto(const eigrp_nbr_t *nbr);
extern eigrp_nbr_t *eigrp_nbr_lookup_by_addr(eigrp_intf_t *,
						  struct in_addr *);
extern eigrp_nbr_t *eigrp_nbr_lookup_by_addr_process(eigrp_instance_t *,
							  struct in_addr addr);

extern int eigrp_nbr_split_horizon(eigrp_route_descriptor_t *,
					 eigrp_intf_t *);

eigrp_result_t eigrp_nbr_static_create(eigrp_af_config_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
eigrp_result_t eigrp_nbr_static_delete(eigrp_af_config_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
void eigrp_nbr_static_delete_all(eigrp_af_config_t *af);
bool eigrp_nbr_static_hello_send(eigrp_intf_t *ei);
bool eigrp_nbr_static_source_allowed(eigrp_intf_t *ei,
                                          const eigrp_addr_t *src);
eigrp_result_t eigrp_nbr_state_iterate(
	eigrp_af_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, bool static_only,
	eigrp_nbr_state_iterate_cb callback, void *arg);
eigrp_result_t eigrp_nbr_clear(
	eigrp_instance_t *runtime, const eigrp_nbr_clear_request_t *request,
	eigrp_nbr_clear_cb callback, void *arg, size_t *affected_count);

eigrp_result_t eigrp_nbr_description_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_address_t *address,
	const char *description);
eigrp_result_t eigrp_nbr_max_prefix_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_address_t *address,
	const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_nbr_max_prefix_all_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_nbr_log_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	eigrp_nbr_log_type_t type,
	bool enabled,
	uint16_t seconds);
bool eigrp_nbr_prefix_admit(eigrp_instance_t *runtime, eigrp_nbr_t *neighbor,
	const eigrp_prefix_t *prefix);
bool eigrp_nbr_warning_should_emit(eigrp_instance_t *runtime,
	const eigrp_nbr_t *neighbor, const char *reason);
void eigrp_nbr_warning_state_clear(eigrp_instance_t *runtime);
#ifdef EIGRP_TESTING
bool eigrp_nbr_warning_should_emit_test(eigrp_instance_t *runtime,
	const eigrp_nbr_t *neighbor, const char *reason, uint64_t now_msec);
#endif
void eigrp_nbr_policy_delete_all(eigrp_af_config_t *af);

#endif /* _ZEBRA_EIGRP_NEIGHBOR_H */
