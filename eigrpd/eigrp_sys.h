// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP host runtime/system-service contract.
 *
 * The host implements these services. Portable EIGRP calls them.
 * Do not put FRR or BIRD types in this header.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_SYS_H_
#define EIGRPD_EIGRP_SYS_H_

#include "eigrpd/eigrp.h"

typedef enum eigrp_work_queue_result {
	EIGRP_WORK_QUEUE_SUCCESS = 0,
	EIGRP_WORK_QUEUE_REQUEUE,
	EIGRP_WORK_QUEUE_BLOCKED,
} eigrp_work_queue_result_t;

typedef eigrp_work_queue_result_t (*eigrp_work_queue_func_t)(
	eigrp_work_queue_t *queue, void *data);
typedef void (*eigrp_work_queue_delete_func_t)(eigrp_work_queue_t *queue,
						void *data);
typedef void (*eigrp_event_callback_t)(void *arg);

typedef struct eigrp_interface_runtime_state {
	const char *interface_name;
	eigrp_ifindex_t ifindex;
	eigrp_prefix_t address;
	uint8_t type;
	bool secondary;
	bool operative;
	uint32_t bandwidth;
	uint32_t mtu;
} eigrp_interface_runtime_state_t;

typedef enum eigrp_interface_remove_reason {
	EIGRP_INTERFACE_REMOVE_HOST = 1,
	EIGRP_INTERFACE_REMOVE_CONFIG,
	EIGRP_INTERFACE_REMOVE_FINAL,
} eigrp_interface_remove_reason_t;

typedef struct eigrp_packet_rx_meta {
	uint16_t network_header_length;
	uint16_t eigrp_length;
	bool destination_multicast;
} eigrp_packet_rx_meta_t;

#define EIGRP_SYS_FILTER_DIRECTION_MAX 2U
typedef struct eigrp_filter_runtime_snapshot {
	const char *access_list[EIGRP_SYS_FILTER_DIRECTION_MAX];
	const char *prefix_list[EIGRP_SYS_FILTER_DIRECTION_MAX];
} eigrp_filter_runtime_snapshot_t;

typedef void (*eigrp_sys_interface_walk_cb)(
	const eigrp_interface_runtime_state_t *state, void *arg);

/* Platform lifecycle. */
void eigrp_sys_runtime_init(void);
void eigrp_sys_runtime_finish(void);

/* Scheduling/time services. */
void eigrp_sys_event_cancel(eigrp_event_t **event);
void eigrp_sys_event_add(eigrp_event_t **event,
			 eigrp_event_callback_t callback, void *arg);
void eigrp_sys_timer_add(eigrp_event_t **event,
			 eigrp_event_callback_t callback, void *arg,
			 uint32_t delay_msec);
void eigrp_sys_read_add(eigrp_event_t **event, eigrp_instance_t *eigrp,
			eigrp_event_callback_t callback, void *arg);
void eigrp_sys_write_add(eigrp_event_t **event, eigrp_instance_t *eigrp,
			 eigrp_event_callback_t callback, void *arg);
uint32_t eigrp_sys_timer_remaining_seconds(const eigrp_event_t *event);
uint64_t eigrp_sys_monotime_msec(void);
void eigrp_sys_software_version(uint8_t *major, uint8_t *minor);

/* Work scheduling. */
eigrp_work_queue_t *eigrp_sys_work_queue_new(
	eigrp_instance_t *eigrp, const char *name,
	eigrp_work_queue_func_t workfunc,
	eigrp_work_queue_delete_func_t deletefunc);
void eigrp_sys_work_queue_free(eigrp_work_queue_t *queue);
void eigrp_sys_work_queue_reset(eigrp_work_queue_t *queue);
void eigrp_sys_work_queue_enqueue(eigrp_work_queue_t *queue, void *data);
eigrp_instance_t *eigrp_sys_work_queue_instance(eigrp_work_queue_t *queue);

/* Protocol socket and interface services. */
eigrp_result_t eigrp_sys_socket_open(eigrp_instance_t *eigrp);
void eigrp_sys_socket_close(eigrp_instance_t *eigrp);
void eigrp_sys_socket_send_buffer_ensure(eigrp_instance_t *eigrp,
					 uint32_t minimum);
bool eigrp_sys_router_id_get(eigrp_instance_t *eigrp, uint32_t *router_id);
eigrp_result_t eigrp_sys_vrf_resolve(const char *vrf_name,
				      eigrp_vrf_id_t *vrf_id);
eigrp_result_t eigrp_sys_interface_walk(eigrp_instance_t *eigrp,
					eigrp_sys_interface_walk_cb callback,
					void *arg);
int eigrp_sys_multicast_interface_set(eigrp_instance_t *eigrp,
				      eigrp_interface_t *ei);
int eigrp_sys_multicast_join(eigrp_instance_t *eigrp, eigrp_interface_t *ei);
int eigrp_sys_multicast_leave(eigrp_instance_t *eigrp, eigrp_interface_t *ei);

/* IPv4 packet envelope.  The platform sees bytes and normalized addresses. */
int eigrp_sys_ipv4_packet_send(eigrp_instance_t *eigrp,
			       eigrp_interface_t *ei,
			       const eigrp_address_t *destination,
			       const uint8_t *payload, size_t length);
bool eigrp_sys_ipv4_packet_receive(eigrp_instance_t *eigrp,
				   uint8_t *buffer, size_t capacity,
				   size_t *received_length,
				   eigrp_ifindex_t *ifindex,
				   eigrp_address_t *source,
				   eigrp_address_t *destination,
				   eigrp_packet_rx_meta_t *meta);

/* IPv6 packet envelope.  Raw IPv6 sockets carry only the EIGRP payload;
 * interface and destination context are supplied through IPv6 packet info. */
int eigrp_sys_ipv6_packet_send(eigrp_instance_t *eigrp,
			       eigrp_interface_t *ei,
			       const eigrp_address_t *destination,
			       const uint8_t *payload, size_t length);
bool eigrp_sys_ipv6_packet_receive(eigrp_instance_t *eigrp,
				   uint8_t *buffer, size_t capacity,
				   size_t *received_length,
				   eigrp_ifindex_t *ifindex,
				   eigrp_address_t *source,
				   eigrp_address_t *destination,
				   eigrp_packet_rx_meta_t *meta);

/* Host policy/key services. */
void eigrp_sys_policy_init(void);
void eigrp_sys_policy_finish(void);
eigrp_result_t eigrp_sys_policy_instance_create(eigrp_instance_t *eigrp);
void eigrp_sys_policy_instance_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_sys_filter_evaluate(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, const eigrp_prefix_t *prefix,
	eigrp_filter_decision_t *decision);
bool eigrp_sys_auth_key_lookup(const char *keychain_name,
			       uint32_t *key_id, char *key_string,
			       size_t key_string_size);

/* Normalized host lifecycle notifications into portable EIGRP. */
void eigrp_sys_interface_state_apply(
	eigrp_vrf_id_t vrf_id, const eigrp_interface_runtime_state_t *state);
void eigrp_sys_interface_link_down(eigrp_vrf_id_t vrf_id,
				   eigrp_ifindex_t ifindex,
				   const char *interface_name, uint8_t type,
				   uint32_t bandwidth, uint32_t mtu);
void eigrp_sys_interface_link_remove(eigrp_vrf_id_t vrf_id,
				     eigrp_ifindex_t ifindex,
				     eigrp_interface_remove_reason_t reason);
void eigrp_sys_interface_address_remove(eigrp_vrf_id_t vrf_id,
					eigrp_ifindex_t ifindex,
					const eigrp_prefix_t *address,
					eigrp_interface_remove_reason_t reason);
void eigrp_sys_router_id_refresh(eigrp_vrf_id_t vrf_id);
void eigrp_sys_policy_runtime_refresh(void);
eigrp_result_t eigrp_sys_filter_runtime_replace(
	eigrp_instance_t *eigrp, const char *interface_name,
	const eigrp_filter_runtime_snapshot_t *snapshot);

#endif /* EIGRPD_EIGRP_SYS_H_ */
