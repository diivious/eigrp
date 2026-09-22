// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP runtime-state and instrumentation contract.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_MGNT_H_
#define EIGRPD_EIGRP_MGNT_H_

#include "eigrpd/eigrp.h"

/* Interface state snapshot. */
typedef struct eigrp_interface_state {
	const char *interface_name;
	bool config_present;
	bool runtime_present;
	bool passive;
	bool shutdown;
	bool multicast_enabled;
	bool authentication_configured;
	uint8_t authentication_mode;
	uint32_t bandwidth;
	uint32_t bandwidth_percent;
	uint32_t delay;
	uint32_t mtu;
	uint32_t hello_interval;
	uint16_t hold_time;
	uint32_t peer_count;
	unsigned long output_queue_count;
	unsigned long reliable_queue_count;
	uint8_t reliability;
	uint8_t load;
	uint16_t tlv1_peer_count;
	uint16_t tlv2_peer_count;
	bool split_horizon;
	bool next_hop_self;
	bool hello_timer_running;
	uint32_t hello_timer_remaining;
	uint64_t unreliable_multicast_sent;
	uint64_t reliable_multicast_sent;
	uint64_t unreliable_unicast_sent;
	uint64_t reliable_unicast_sent;
	uint64_t multicast_exceptions;
	uint64_t cr_packets_sent;
	uint64_t retransmissions_sent;
	bool bandwidth_percent_configured;
	bool hello_interval_configured;
	bool hold_time_configured;
} eigrp_interface_state_t;

typedef eigrp_result_t (*eigrp_interface_state_walk_cb)(
	const eigrp_interface_state_t *state, void *arg);

eigrp_result_t eigrp_interface_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, eigrp_interface_state_walk_cb callback,
	void *arg);

/* Neighbor state snapshot. */
typedef struct eigrp_neighbor_state {
	eigrp_address_t address;
	const char *interface_name;
	const char *state_name;
	bool static_configured;
	bool runtime_present;
	uint16_t hold_time;
	uint64_t uptime_seconds;
	unsigned long reliable_queue_count;
	uint32_t sequence_number;
	uint32_t prefix_count;
	uint64_t retransmit_count;
	uint8_t retry_count;
	bool srtt_valid;
	uint32_t srtt_msec;
	uint32_t rto_msec;
	uint8_t os_major;
	uint8_t os_minor;
	uint8_t tlv_major;
	uint8_t tlv_minor;
	uint8_t tlv_version;
} eigrp_neighbor_state_t;

typedef eigrp_result_t (*eigrp_neighbor_state_walk_cb)(
	const eigrp_neighbor_state_t *state, void *arg);

eigrp_result_t eigrp_neighbor_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, bool static_only,
	eigrp_neighbor_state_walk_cb callback, void *arg);

/* Topology snapshots. */
typedef struct eigrp_topology_prefix_state {
	eigrp_prefix_t destination;
	bool active;
	uint32_t feasible_distance;
	uint32_t successor_count;
	uint64_t serial_number;
} eigrp_topology_prefix_state_t;

typedef struct eigrp_topology_route_state {
	eigrp_address_t next_hop;
	const char *interface_name;
	bool connected;
	bool successor;
	bool feasible_successor;
	uint32_t distance;
	uint32_t reported_distance;
} eigrp_topology_route_state_t;

typedef eigrp_result_t (*eigrp_topology_prefix_state_cb)(
	const eigrp_topology_prefix_state_t *state, void *arg);
typedef eigrp_result_t (*eigrp_topology_route_state_cb)(
	const eigrp_topology_route_state_t *state, void *arg);
typedef eigrp_result_t (*eigrp_topology_instance_walk_cb)(
	eigrp_instance_t *runtime, void *arg);

eigrp_result_t eigrp_topology_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const eigrp_prefix_t *destination, bool all_links,
	eigrp_topology_prefix_state_cb prefix_callback,
	eigrp_topology_route_state_cb route_callback, void *arg);
eigrp_result_t eigrp_topology_instance_walk(
	eigrp_address_family_t afi, eigrp_vrf_id_t vrf_id, uint16_t asn,
	eigrp_topology_instance_walk_cb callback, void *arg);

/* Timer state. */
typedef enum eigrp_timer_state_type {
	EIGRP_TIMER_STATE_HELLO = 0,
	EIGRP_TIMER_STATE_PEER_HOLD,
} eigrp_timer_state_type_t;

typedef struct eigrp_timer_state {
	eigrp_timer_state_type_t type;
	const char *interface_name;
	bool neighbor_present;
	eigrp_address_t neighbor_address;
	uint32_t expiration_seconds;
} eigrp_timer_state_t;

typedef eigrp_result_t (*eigrp_timer_state_cb)(
	const eigrp_timer_state_t *state, void *arg);
eigrp_result_t eigrp_timer_show(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg);

/* Event log. */
#define EIGRP_EVENTLOG_DEFAULT_SIZE 500U
typedef struct eigrp_eventlog_entry {
	unsigned long opcode;
	unsigned long arg1;
	unsigned long arg2;
} eigrp_eventlog_entry_t;

typedef struct eigrp_eventlog_state {
	uint32_t capacity;
	uint32_t count;
} eigrp_eventlog_state_t;

typedef eigrp_result_t (*eigrp_eventlog_show_cb)(
	uint32_t event_number, const eigrp_eventlog_entry_t *entry,
	const char *format, void *arg);

/* The context type is defined by eigrp_cli.h; use the tagged name here. */
eigrp_result_t eigrp_eventlog_state_read(
	const eigrp_instance_context_t *context,
	eigrp_eventlog_state_t *state);
eigrp_result_t eigrp_eventlog_show(
	const eigrp_instance_context_t *context,
	eigrp_eventlog_show_cb callback, void *arg);
const char *eigrp_eventlog_format_read(unsigned long opcode);
eigrp_result_t eigrp_eventlog_entry_format(
	const eigrp_eventlog_entry_t *entry, char *buffer, size_t buffer_size);

/* Traffic/accounting state. */
#define EIGRP_STATISTICS_TRAFFIC_ACK (1U << 0)
#define EIGRP_STATISTICS_TRAFFIC_HELLO (1U << 1)
#define EIGRP_STATISTICS_TRAFFIC_QUERY (1U << 2)
#define EIGRP_STATISTICS_TRAFFIC_REPLY (1U << 3)
#define EIGRP_STATISTICS_TRAFFIC_UPDATE (1U << 4)
#define EIGRP_STATISTICS_TRAFFIC_SIA_QUERY (1U << 5)
#define EIGRP_STATISTICS_TRAFFIC_SIA_REPLY (1U << 6)

typedef struct eigrp_statistics_traffic_state {
	uint16_t sent_valid;
	uint16_t received_valid;
	uint64_t sent_ack;
	uint64_t sent_hello;
	uint64_t sent_query;
	uint64_t sent_reply;
	uint64_t sent_update;
	uint64_t sent_sia_query;
	uint64_t sent_sia_reply;
	uint64_t received_ack;
	uint64_t received_hello;
	uint64_t received_query;
	uint64_t received_reply;
	uint64_t received_update;
	uint64_t received_sia_query;
	uint64_t received_sia_reply;
} eigrp_statistics_traffic_state_t;

typedef struct eigrp_statistics_accounting_state {
	eigrp_address_t neighbor_address;
	const char *interface_name;
	const char *neighbor_state;
	uint32_t prefix_count;
} eigrp_statistics_accounting_state_t;

typedef eigrp_result_t (*eigrp_statistics_accounting_cb)(
	const eigrp_statistics_accounting_state_t *state, void *arg);
eigrp_result_t eigrp_statistics_accounting_show(
	const eigrp_instance_context_t *context,
	uint32_t *total_prefix_count,
	eigrp_statistics_accounting_cb callback, void *arg);
eigrp_result_t eigrp_statistics_traffic_show(
	const eigrp_instance_context_t *context,
	eigrp_statistics_traffic_state_t *state);

/* Protocol summary/tech-support snapshot. */
typedef struct eigrp_status_protocol_state {
	const char *instance_name;
	/* Opaque identity used only to feed other public management walkers. */
	eigrp_address_family_config_t *config;
	eigrp_address_family_t afi;
	const char *vrf_name;
	uint16_t asn;
	bool shutdown;
	bool router_id_configured;
	uint32_t router_id;
	bool runtime_present;
	bool data_path_ready;
} eigrp_status_protocol_state_t;

typedef eigrp_result_t (*eigrp_status_protocol_cb)(
	const eigrp_status_protocol_state_t *state, void *arg);
eigrp_result_t eigrp_status_protocol_show(
	eigrp_status_protocol_cb callback, void *arg);
eigrp_result_t eigrp_status_tech_support_show(
	eigrp_status_protocol_cb callback, void *arg);

/* Debug state snapshots consumed by host presentation. */
typedef struct eigrp_debug_address_family_state {
	bool used;
	eigrp_address_family_t afi;
	uint16_t asn;
	bool all_vrfs;
	char vrf_name[64];
	eigrp_debug_address_family_category_t category;
	bool neighbor_set;
	eigrp_address_t neighbor;
} eigrp_debug_address_family_state_t;

size_t eigrp_debug_address_family_state_count(void);
bool eigrp_debug_address_family_state_get(
	eigrp_debug_scope_t scope, size_t index,
	eigrp_debug_address_family_state_t *state);

#endif /* EIGRPD_EIGRP_MGNT_H_ */
