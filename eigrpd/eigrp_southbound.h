// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_SOUTHBOUND_H_
#define _ZEBRA_EIGRP_SOUTHBOUND_H_

#include <netinet/in.h>

#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"
#include "eigrpd/eigrp_metric.h"

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

/*
 * Host-independent next-hop snapshot used only at the southbound RIB boundary.
 * Topology/DUAL state remains in its native EIGRP descriptors; host RIB objects
 * are created by the adapter after this snapshot crosses the boundary.
 */
typedef struct eigrp_southbound_nexthop {
	eigrp_ifindex_t ifindex;
	bool gateway_present;
	eigrp_address_t gateway;
} eigrp_southbound_nexthop_t;

/* Host runtime lifecycle and event services. */
void eigrp_southbound_runtime_init(void);
void eigrp_southbound_runtime_finish(void);

/* Host RIB lifecycle and route installation/removal. */
void eigrp_southbound_rib_init(void);
void eigrp_southbound_rib_finish(void);
void eigrp_southbound_rib_instance_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_southbound_route_install(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix,
	const eigrp_southbound_nexthop_t *nexthops, size_t nexthop_count,
	uint32_t distance);
eigrp_result_t eigrp_southbound_route_remove(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix);
void eigrp_southbound_event_cancel(eigrp_event_t **event);
void eigrp_southbound_event_add(eigrp_event_t **event,
                                eigrp_event_callback_t callback, void *arg);
void eigrp_southbound_timer_add(eigrp_event_t **event,
                                eigrp_event_callback_t callback, void *arg,
                                uint32_t delay_msec);
void eigrp_southbound_read_add(eigrp_event_t **event, int fd,
                               eigrp_event_callback_t callback, void *arg);
void eigrp_southbound_write_add(eigrp_event_t **event, int fd,
                                eigrp_event_callback_t callback, void *arg);
uint32_t eigrp_southbound_timer_remaining_seconds(const eigrp_event_t *event);
uint64_t eigrp_southbound_monotime_msec(void);
void eigrp_southbound_software_version(uint8_t *major, uint8_t *minor);

/* Host socket/interface services used by the portable runtime. */
eigrp_result_t eigrp_southbound_socket_open(eigrp_instance_t *eigrp);
void eigrp_southbound_socket_close(eigrp_instance_t *eigrp);
void eigrp_southbound_socket_send_buffer_ensure(eigrp_instance_t *eigrp,
                                                uint32_t minimum);
bool eigrp_southbound_router_id_get(eigrp_instance_t *eigrp,
                                    struct in_addr *router_id);
eigrp_result_t eigrp_southbound_vrf_resolve(const char *vrf_name,
                                             eigrp_vrf_id_t *vrf_id);
typedef void (*eigrp_southbound_interface_walk_cb)(
	const eigrp_interface_runtime_state_t *state, void *arg);
eigrp_result_t eigrp_southbound_interface_walk(
	eigrp_instance_t *eigrp, eigrp_southbound_interface_walk_cb callback,
	void *arg);
int eigrp_southbound_multicast_interface_set(eigrp_instance_t *eigrp,
                                             eigrp_interface_t *ei);
int eigrp_southbound_multicast_join(eigrp_instance_t *eigrp,
                                    eigrp_interface_t *ei);
int eigrp_southbound_multicast_leave(eigrp_instance_t *eigrp,
                                     eigrp_interface_t *ei);
int eigrp_southbound_ipv4_packet_send(eigrp_instance_t *eigrp,
                                        eigrp_interface_t *ei,
                                        const eigrp_addr_t *destination,
                                        const uint8_t *payload, size_t length);
bool eigrp_southbound_ipv4_packet_receive(eigrp_instance_t *eigrp, int fd,
                                           eigrp_stream_t *stream,
                                           eigrp_ifindex_t *ifindex,
                                           eigrp_addr_t *source,
                                           eigrp_addr_t *destination,
                                           eigrp_packet_rx_meta_t *meta);

eigrp_work_queue_t *eigrp_work_queue_new(eigrp_instance_t *eigrp,
						 const char *name,
						 eigrp_work_queue_func_t workfunc,
						 eigrp_work_queue_delete_func_t deletefunc);
void eigrp_work_queue_free(eigrp_work_queue_t *queue);
void eigrp_work_queue_reset(eigrp_work_queue_t *queue);
void eigrp_work_queue_enqueue(eigrp_work_queue_t *queue, void *data);
eigrp_instance_t *eigrp_work_queue_eigrp(eigrp_work_queue_t *queue);

/* Host runtime adaptation for route redistribution. */
eigrp_result_t eigrp_southbound_redistribute_update(
	eigrp_instance_t *eigrp, const char *protocol,
	const eigrp_metric_values_t *metric, const char *route_map);
eigrp_result_t eigrp_southbound_redistribute_delete(
	eigrp_instance_t *eigrp, const char *protocol);

/* Host policy/filter adaptation.  Host policy objects remain private. */
void eigrp_southbound_policy_init(void);
void eigrp_southbound_policy_finish(void);
eigrp_result_t eigrp_southbound_policy_instance_create(eigrp_instance_t *eigrp);
void eigrp_southbound_policy_instance_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_southbound_filter_evaluate(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, const eigrp_prefix_t *prefix,
	eigrp_filter_decision_t *decision);

/* Host key-chain lookup. Key material is copied into caller-owned storage. */
bool eigrp_southbound_auth_key_lookup(const char *keychain_name,
                                       uint32_t *key_id, char *key_string,
                                       size_t key_string_size);

#endif /* _ZEBRA_EIGRP_SOUTHBOUND_H_ */
