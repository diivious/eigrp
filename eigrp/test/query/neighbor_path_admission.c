// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
/* QUERY and REPLY admission through production receive, DUAL, and topology. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "eigrp/code/eigrp.h"
#include "eigrp/code/eigrp_structs.h"
#include "eigrp/code/eigrp_log.h"
#include "eigrp/code/eigrp_debug.h"
#include "eigrp/code/eigrp_neighbor.h"
#include "eigrp/code/eigrp_metric.h"
#include "eigrp/code/eigrp_topology.h"
#include "eigrp/code/eigrp_instance.h"
#include "eigrp/code/eigrp_rib.h"
#include "eigrp/code/eigrp_packetizer.h"
#include "eigrp/code/eigrp_fsm.h"
#include "eigrp/code/eigrp_eventlog.h"
#include "eigrp/code/eigrp_summary.h"
#include "eigrp/code/eigrp_stream.h"

void eigrp_query_receive(eigrp_instance_t *, eigrp_nbr_t *,
    struct eigrp_header *, eigrp_stream_t *, eigrp_intf_t *, int);
void eigrp_reply_receive(eigrp_instance_t *, eigrp_nbr_t *,
    struct eigrp_header *, eigrp_stream_t *, eigrp_intf_t *, int);
static eigrp_af_config_t policy;
static eigrp_route_descriptor_t *incoming;
static unsigned replies, acks, updates;
static eigrp_instance_t runtime;
static eigrp_nbr_t peer, self;
static eigrp_intf_t link;

unsigned long term_debug_eigrp, term_debug_eigrp_nei;
unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];
unsigned long term_debug_eigrp_transmit, term_debug_eigrp_notifications;
void eigrp_log(eigrp_log_level_t level, const char *fmt, ...) {(void)level; (void)fmt;}
bool eigrp_debug_af_enabled(eigrp_instance_t *e, eigrp_debug_af_category_t c,
    const eigrp_addr_t *n) {(void)e; (void)c; (void)n; return false;}
eigrp_result_t eigrp_eventlog_msg_add(eigrp_instance_t *e, uint16_t code,
    const eigrp_prefix_t *p, eventmsg_arg_t a, eventmsg_arg_t b,
    eventmsg_arg_t c, eventmsg_arg_t d)
{ (void)e; (void)code; (void)p; (void)a; (void)b; (void)c; (void)d;
  return EIGRP_RESULT_SUCCESS; }
eigrp_af_config_t *eigrp_af_config_runtime_read(eigrp_instance_t *e)
{ (void)e; return &policy; }
void eigrp_pfxlimits_prefix_exceeded(eigrp_instance_t *e, const eigrp_prefix_limit_t *l)
{ (void)e; (void)l; }
void eigrp_eventlog_addr_from_legacy(eigrp_prefix_t *p, const eigrp_addr_t *a)
{ (void)a; memset(p, 0, sizeof(*p)); }
uint64_t eigrp_sys_monotime_msec(void) { return 123456; }
void eigrp_hello_send_unicast(eigrp_intf_t *ei, const eigrp_addr_t *dst)
{ (void)ei; (void)dst; }
eigrp_intf_t *eigrp_intf_lookup_by_name(eigrp_instance_t *e, const char *name)
{ (void)e; (void)name; return NULL; }
const char *eigrp_intf_name_string(eigrp_intf_t *ei)
{ return ei ? ei->name : "none"; }
eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *e, const eigrp_rib_route_t *r)
{ (void)e; (void)r; return EIGRP_RESULT_SUCCESS; }
eigrp_result_t eigrp_rib_route_del(eigrp_instance_t *e, const eigrp_prefix_t *p)
{ (void)e; (void)p; return EIGRP_RESULT_SUCCESS; }
void eigrp_update_send_all(eigrp_instance_t *e, eigrp_intf_t *ei)
{ (void)e; (void)ei; updates++; }
void eigrp_hello_send_ack(eigrp_nbr_t *nbr) { (void)nbr; acks++; }
bool eigrp_summary_query_route_build(eigrp_instance_t *e, eigrp_intf_t *ei,
 const eigrp_prefix_t *dest, eigrp_prefix_descriptor_t *sp, eigrp_route_descriptor_t *sr)
{ (void)e; (void)ei; (void)dest; (void)sp; (void)sr; return false; }
void eigrp_packetizer_prefix_defer_free(eigrp_instance_t *e, eigrp_prefix_descriptor_t *p)
{ (void)e; (void)p; }
void eigrp_sys_event_cancel(eigrp_event_t **owner) { if (owner) *owner = NULL; }
void eigrp_timer_cancel(eigrp_instance_t *e, eigrp_timer_event_t **owner)
{ (void)e; if (owner) *owner = NULL; }
/* Free queued output at the transport boundary, as with delivered work. */
eigrp_packetizer_work_t *eigrp_packetizer_work_create(uint8_t opcode)
{ eigrp_packetizer_work_t *work = calloc(1, sizeof(*work)); assert(work);
  work->opcode = opcode; return work; }
void eigrp_packetizer_enqueue(eigrp_instance_t *e, eigrp_packetizer_work_t *work)
{ (void)e; if (work->opcode == EIGRP_OPC_REPLY) replies++;
  if (work->flags & EIGRP_PACKETIZER_WORK_F_OWN_ROUTE)
      eigrp_topology_route_free(work->route);
  if (work->flags & EIGRP_PACKETIZER_WORK_F_OWN_PREFIX)
      eigrp_topology_prefix_free(work->prefix);
  free(work); }

static eigrp_route_descriptor_t *decode(eigrp_instance_t *e, eigrp_nbr_t *n,
 eigrp_stream_t *s, uint16_t len)
{ (void)e; (void)n; (void)len; s->getp = s->endp;
  eigrp_route_descriptor_t *r = incoming; incoming = NULL; return r; }

static eigrp_prefix_t dest(unsigned id)
{
 eigrp_prefix_t p = {0};
 p.address.afi = EIGRP_AFI_IPV4;
 p.address.bytes[0] = 198; p.address.bytes[1] = 51;
 p.address.bytes[2] = 100; p.address.bytes[3] = id;
 p.prefix_length = 32;
 return p;
}
static eigrp_metrics_t metric(void)
{
 eigrp_metrics_t m = {0};
 m.bandwidth = eigrp_bandwidth_to_scaled(100000);
 m.delay = eigrp_delay_to_scaled(200);
 m.reliability = 255; m.load = 1;
 return m;
}
static eigrp_route_descriptor_t *wire(unsigned id, bool poison)
{
 eigrp_route_descriptor_t *r = eigrp_topology_route_create(&link);
 assert(r);
 r->type = EIGRP_TLV_IPv4_INT; r->dest = dest(id);
 r->metric = metric();
 if (poison) r->metric.delay = EIGRP_MAX_METRIC;
 return r;
}
static eigrp_prefix_descriptor_t *local(unsigned id)
{
 eigrp_prefix_descriptor_t *p = eigrp_topology_prefix_create();
 eigrp_route_descriptor_t *r = eigrp_topology_route_create(&link);
 assert(p && r);
 p->destination = dest(id);
 p->nt = EIGRP_TOPOLOGY_TYPE_CONNECTED;
 p->state = EIGRP_FSM_STATE_PASSIVE;
 r->type = EIGRP_TLV_IPv4_INT;
 r->adv_router = &self; r->dest = p->destination;
 r->metric = r->reported_metric = r->total_metric = metric();
 r->distance = r->reported_distance = eigrp_metric_calculate(&runtime, r->metric);
 r->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
 p->fdistance = p->distance = p->rdistance = r->distance;
 p->reported_metric = r->metric;
 eigrp_prefix_descriptor_add(runtime.topology_table, p);
 eigrp_route_descriptor_add(&runtime, p, r);
 return p;
}
static void receive_one(unsigned opcode, unsigned id, bool poison)
{
 eigrp_header_t h = {.sequence = htonl(42)};
 eigrp_stream_t s = {.getp = 0, .endp = 1, .size = 1};
 incoming = wire(id, poison);
 if (opcode == EIGRP_OPC_QUERY)
  eigrp_query_receive(&runtime, &peer, &h, &s, &link, 1);
 else
  eigrp_reply_receive(&runtime, &peer, &h, &s, &link, 1);
 assert(incoming == NULL);
}
static void start(bool warning_only)
{
 memset(&runtime, 0, sizeof(runtime));
 memset(&policy, 0, sizeof(policy));
 memset(&link, 0, sizeof(link));
 memset(&peer, 0, sizeof(peer));
 memset(&self, 0, sizeof(self));
 runtime.neighbor_self = &self;
 runtime.AS = 4453; runtime.k_values[0] = 1;
 runtime.k_values[2] = 1; runtime.variance = 1;
 runtime.max_paths = 4; runtime.max_hops = 100;
 runtime.metric_version = EIGRP_TLV_32B_VERSION;
 runtime.af_vectors.afi = EIGRP_AFI_IPV4;
 runtime.af_vectors.classic_internal_tlv_type = EIGRP_TLV_IPv4_INT;
 runtime.af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
 runtime.topology_table = eigrp_topology_table_create();
 runtime.topology_changes = eigrp_list_create();
 assert(runtime.topology_table && runtime.topology_changes);
 link.params.bandwidth = 100000; link.params.delay = 100;
 link.params.reliability = 255; link.params.load = 1;
 link.eigrp = &runtime;
 peer.ei = &link; peer.decoder = decode;
 peer.src.afi = AF_INET; peer.src.ip.v4.s_addr = htonl(0x0a000002);
 peer.limit_prefix_count_valid = true;
 eigrp_instance_context_t ctx = {.config = &policy};
 eigrp_prefix_limit_t limit = {.maximum = 1, .warning_only = warning_only};
 assert(eigrp_nbr_max_prefix_all_update(EIGRP_SET, &ctx, &limit)
        == EIGRP_RESULT_SUCCESS);
 replies = acks = updates = 0;
}
static void finish(void)
{
 eigrp_topology_table_delete(&runtime, runtime.topology_table);
 eigrp_list_delete(&runtime.topology_changes);
}
static void active(eigrp_prefix_descriptor_t *p)
{
 p->state = EIGRP_FSM_STATE_ACTIVE_1;
 eigrp_fsm_reply_status_add(p, &peer);
 assert(eigrp_fsm_reply_status_pending(p, &peer));
}
static void query_cases(bool warning_only)
{
 start(warning_only);
 eigrp_prefix_descriptor_t *p1 = local(1), *p2 = local(2), *p3 = local(3);
 receive_one(EIGRP_OPC_QUERY, 1, false);
 assert(eigrp_prefix_descriptor_lookup(p1, &peer));
 assert(peer.limit_prefix_count == 1);
 receive_one(EIGRP_OPC_QUERY, 1, false);
 assert(peer.limit_prefix_count == 1);
 /* A poisoned QUERY introduces no path, even when at capacity. */
 receive_one(EIGRP_OPC_QUERY, 3, true);
 assert(!eigrp_prefix_descriptor_lookup(p3, &peer));
 assert(peer.limit_prefix_count == 1);
 assert(peer.limit_state == EIGRP_NBR_LIMIT_NONE);
 receive_one(EIGRP_OPC_QUERY, 2, false);
 assert((eigrp_prefix_descriptor_lookup(p2, &peer) != NULL) == warning_only);
 assert(peer.limit_prefix_count == (warning_only ? 2U : 1U));
 assert(replies == 4 && acks == 4);
 assert(p2->state == EIGRP_FSM_STATE_PASSIVE);
 if (warning_only) {
  eigrp_route_descriptor_delete(&runtime, p2,
   eigrp_prefix_descriptor_lookup(p2, &peer));
  assert(peer.limit_prefix_count == 1);
 }
 eigrp_route_descriptor_delete(&runtime, p1,
  eigrp_prefix_descriptor_lookup(p1, &peer));
 assert(peer.limit_prefix_count == 0);
 /* Once the rejected peer's suspension has cleared, a new path may enter. */
 peer.limit_state = EIGRP_NBR_LIMIT_NONE;
 peer.limit_cleanup_pending = false;
 receive_one(EIGRP_OPC_QUERY, 2, false);
 assert(peer.limit_prefix_count == 1);
 assert(eigrp_prefix_descriptor_lookup(p2, &peer));
 finish();
}
static void reply_cases(bool warning_only)
{
 start(warning_only);
 eigrp_prefix_descriptor_t *p1 = local(1), *p2 = local(2), *p3 = local(3);
 active(p1);
 receive_one(EIGRP_OPC_REPLY, 1, false);
 assert(p1->state == EIGRP_FSM_STATE_PASSIVE);
 assert(!eigrp_fsm_reply_status_pending(p1, &peer));
 assert(peer.limit_prefix_count == 1);
 assert(eigrp_prefix_descriptor_lookup(p1, &peer));
 /* PASSIVE REPLY must not duplicate or revive any descriptor. */
 receive_one(EIGRP_OPC_REPLY, 1, false);
 assert(peer.limit_prefix_count == 1);
 /* Infinite REPLY clears RIJ without wasting a new neighbor slot. */
 active(p3);
 receive_one(EIGRP_OPC_REPLY, 3, true);
 assert(!eigrp_fsm_reply_status_pending(p3, &peer));
 assert(p3->state == EIGRP_FSM_STATE_PASSIVE);
 assert(!eigrp_prefix_descriptor_lookup(p3, &peer));
 assert(peer.limit_prefix_count == 1);
 assert(peer.limit_state == EIGRP_NBR_LIMIT_NONE);
 active(p2);
 receive_one(EIGRP_OPC_REPLY, 2, false);
 assert(!eigrp_fsm_reply_status_pending(p2, &peer));
 assert(p2->state == EIGRP_FSM_STATE_PASSIVE);
 assert((eigrp_prefix_descriptor_lookup(p2, &peer) != NULL) == warning_only);
 assert(peer.limit_prefix_count == (warning_only ? 2U : 1U));
 if (warning_only) {
  eigrp_route_descriptor_delete(&runtime, p2,
   eigrp_prefix_descriptor_lookup(p2, &peer));
  assert(peer.limit_prefix_count == 1);
 }
 eigrp_route_descriptor_delete(&runtime, p1,
  eigrp_prefix_descriptor_lookup(p1, &peer));
 assert(peer.limit_prefix_count == 0);
 peer.limit_state = EIGRP_NBR_LIMIT_NONE;
 peer.limit_cleanup_pending = false;
 active(p2);
 receive_one(EIGRP_OPC_REPLY, 2, false);
 assert(p2->state == EIGRP_FSM_STATE_PASSIVE);
 assert(peer.limit_prefix_count == 1);
 assert(eigrp_prefix_descriptor_lookup(p2, &peer));
 finish();
}
int main(int argc, char **argv)
{
 if (argc != 2) return 2;
 if (strcmp(argv[1], "query") == 0) {
  query_cases(false); query_cases(true);
 } else if (strcmp(argv[1], "reply") == 0) {
  reply_cases(false); reply_cases(true);
 } else return 2;
 puts("Neighbor admission, DUAL processing, and retirement: passed");
 return 0;
}
