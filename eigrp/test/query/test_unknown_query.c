// SPDX-License-Identifier: ISC
/* RFC 7868 4.3 finite/poisoned unknown QUERY storage regression. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "eigrp_query.c"

unsigned long term_debug_eigrp = 0;
void eigrp_timer_cancel(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner)
{ (void)eigrp; (void)owner; }
uint64_t eigrp_sys_wallclock_msec(void) { return 1000; }
uint64_t eigrp_sys_monotime_msec(void) { return 1000; }

/* Exercise the actual receive function while recording its observable RTP
 * reply/ACK actions.  The transport is replaced only at the output boundary. */
static unsigned replies;
static unsigned acks;
static eigrp_route_descriptor_t *incoming;

static eigrp_route_descriptor_t *decode_one(eigrp_instance_t *eigrp,
		 eigrp_nbr_t *nbr, eigrp_stream_t *pkt, uint16_t length)
{
	(void)eigrp; (void)nbr; (void)length;
	pkt->getp = pkt->endp;
	eigrp_route_descriptor_t *route = incoming;
	incoming = NULL;
	return route;
}

void eigrp_reply_send_route(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
			   eigrp_prefix_descriptor_t *prefix,
			   eigrp_route_descriptor_t *route, uint32_t flags)
{
	(void)eigrp; (void)nbr;
	assert(route && prefix);
	assert(route->metric.delay == EIGRP_MAX_METRIC);
	assert(flags == (EIGRP_PACKETIZER_WORK_F_OWN_PREFIX |
			 EIGRP_PACKETIZER_WORK_F_OWN_ROUTE));
	replies++;
	eigrp_topology_route_free(route);
	eigrp_topology_prefix_free(prefix);
}

eigrp_packetizer_work_t *eigrp_packetizer_work_create(uint8_t opcode)
{ (void)opcode; assert(!"unknown QUERY must not originate QUERY work"); return NULL; }
void eigrp_packetizer_enqueue(eigrp_instance_t *eigrp, eigrp_packetizer_work_t *work)
{ (void)eigrp; (void)work; assert(!"unknown QUERY must not originate QUERY work"); }

void eigrp_hello_send_ack(eigrp_nbr_t *nbr)
{ (void)nbr; acks++; }
void eigrp_update_send_all(eigrp_instance_t *eigrp, eigrp_intf_t *ei)
{ (void)eigrp; (void)ei; }
int eigrp_fsm_event(eigrp_fsm_action_message_t *msg)
{ (void)msg; assert(!"unknown QUERY must not enter DUAL ACTIVE"); return 0; }

bool eigrp_summary_query_route_build(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
	const eigrp_prefix_t *dest, eigrp_prefix_descriptor_t *sp,
	eigrp_route_descriptor_t *sr)
{ (void)eigrp; (void)ei; (void)dest; (void)sp; (void)sr; return false; }

static eigrp_prefix_t prefix_ipv4(uint8_t last)
{
	eigrp_prefix_t p = {0};
	p.address.afi = EIGRP_AFI_IPV4;
	p.address.bytes[0] = 10;
	p.address.bytes[1] = 99;
	p.address.bytes[2] = last;
	p.prefix_length = 24;
	return p;
}

static void receive_one(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
	eigrp_intf_t *ei, eigrp_route_descriptor_t *route)
{
	eigrp_header_t header = {.sequence = htonl(1)};
	eigrp_stream_t stream = {.getp = 0, .endp = 1, .size = 1};
	incoming = route;
	eigrp_query_receive(eigrp, nbr, &header, &stream, ei, 1);
	assert(!incoming);
}

static eigrp_route_descriptor_t *query_route(eigrp_intf_t *ei, uint8_t last,
                                            bool poisoned)
{
	eigrp_route_descriptor_t *route = eigrp_topology_route_create(ei);
	assert(route);
	route->dest = prefix_ipv4(last);
	route->type = EIGRP_TLV_IPv4_INT;
	route->metric.delay = poisoned ? EIGRP_MAX_METRIC : eigrp_delay_to_scaled(15);
	route->metric.bandwidth = eigrp_bandwidth_to_scaled(100000);
	route->metric.reliability = 255;
	route->metric.load = 1;
	return route;
}

int main(void)
{
	eigrp_instance_t eigrp = {0};
	eigrp_intf_t ei = {0};
	eigrp_nbr_t nbr = {0};
	eigrp_route_descriptor_t *finite, *poison, *stored;
	eigrp_prefix_descriptor_t *prefix;

	eigrp.topology_table = eigrp_topology_table_create();
	assert(eigrp.topology_table);
	eigrp.af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
	eigrp.max_hops = 100;
	eigrp.k_values[0] = 1;
	eigrp.k_values[2] = 1;
	eigrp.metric_version = EIGRP_TLV_32B_VERSION;
	ei.params.bandwidth = 100000;
	ei.params.delay = 10;
	nbr.ei = &ei;
	nbr.decoder = decode_one;
	finite = query_route(&ei, 1, false);
	const eigrp_metrics_t advertised = finite->metric;
	receive_one(&eigrp, &nbr, &ei, finite);
	assert(replies == 1 && acks == 1);
	prefix = eigrp_topology_table_lookup(eigrp.topology_table, &finite->dest);
	assert(prefix);
	assert(prefix->state == EIGRP_FSM_STATE_PASSIVE);
	assert(prefix->fdistance == EIGRP_MAX_METRIC && prefix->distance == EIGRP_MAX_METRIC);
	assert(prefix->reported_metric.delay == EIGRP_MAX_METRIC);
	assert(prefix->req_action == 0 && !prefix->t_active);
	assert(eigrp.topology_changes == NULL);
	assert(eigrp_prefix_descriptor_lookup(prefix, &nbr) == finite);
	assert(prefix->internal_routes->count == 1 && prefix->external_routes->count == 0);
	assert(finite->flags == 0);
	assert(eigrp_metrics_match(finite->reported_metric, advertised));
	assert(finite->reported_distance == eigrp_metric_calculate(&eigrp, advertised));
	assert(finite->distance == (uint32_t)eigrp_metric_calculate(&eigrp, finite->total_metric));
	assert(finite->distance < EIGRP_MAX_METRIC);
	assert(eigrp_topology_route_select(prefix) == finite);

	/* A second finite QUERY for the same destination must update the
	 * retained neighbor metric without promoting it to a successor. */
	{
		eigrp_route_descriptor_t *again = query_route(&ei, 1, false);
		again->metric.delay = eigrp_delay_to_scaled(25);
		receive_one(&eigrp, &nbr, &ei, again);
		assert(replies == 2 && acks == 2);
		assert(eigrp_prefix_descriptor_lookup(prefix, &nbr) == finite);
		assert(finite->reported_metric.delay == eigrp_delay_to_scaled(25));
		assert(prefix->state == EIGRP_FSM_STATE_PASSIVE);
		assert(prefix->distance == EIGRP_MAX_METRIC && prefix->fdistance == EIGRP_MAX_METRIC);
		assert(finite->flags == 0 && prefix->internal_routes->count == 1);
	}

	poison = query_route(&ei, 2, true);
	receive_one(&eigrp, &nbr, &ei, poison);
	assert(replies == 3 && acks == 3);
	assert(!eigrp_topology_table_lookup(eigrp.topology_table, &poison->dest));


	/* A newly received UPDATE with the same finite vector must be able
	 * to move the retained path through the normal DUAL update path. */
	stored = eigrp_prefix_descriptor_lookup(prefix, &nbr);
	assert(stored == finite);
	assert(stored->metric.delay == eigrp_delay_to_scaled(25));
	printf("Unknown finite QUERY retained as passive non-successor; poison skipped\n");
	return 0;
}
