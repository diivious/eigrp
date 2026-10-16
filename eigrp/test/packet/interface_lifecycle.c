// SPDX-License-Identifier: ISC
/* Executable TRANSMIT lifecycle regression: immediate goodbye and stale queue purge. */
#include <assert.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise production helpers, not a shadow copy of their algorithms. */
#include "eigrp_packet.c"

static unsigned sends;
static unsigned finalized;
static uint8_t sent_opcode;
static uint8_t sent_k1;
static uint32_t sent_seq;

static int record_send(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
                       eigrp_packet_t *packet)
{
    const struct eigrp_header *h = (const void *)eigrp_stream_data(packet->s);
    (void)eigrp;
    (void)ei;
    sends++;
    sent_opcode = h->opcode;
    sent_seq = ntohl(h->sequence);
    sent_k1 = packet->s->data[EIGRP_HEADER_LEN + 4];
    return packet->length;
}

/* Minimal instance test fixture for send path. */
void eigrp_timer_cancel(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner)
{ (void)eigrp; (void)owner; }
void eigrp_debug_packet_send(eigrp_intf_t *ei, const eigrp_packet_t *pkt, int result)
{ (void)ei; (void)pkt; (void)result; }
void eigrp_eventlog_addr_from_legacy(eigrp_prefix_t *prefix, const eigrp_addr_t *addr)
{ (void)addr; memset(prefix, 0, sizeof(*prefix)); }
eigrp_result_t eigrp_eventlog_msg_add(eigrp_instance_t *eigrp, uint16_t opcode,
    const eigrp_prefix_t *prefix, eventmsg_arg_t a, eventmsg_arg_t b,
    eventmsg_arg_t c, eventmsg_arg_t d)
{ (void)eigrp; (void)opcode; (void)prefix; (void)a; (void)b; (void)c; (void)d; return EIGRP_RESULT_SUCCESS; }
bool eigrp_auth_transmit_finalize(eigrp_intf_t *ei, eigrp_stream_t *s, uint16_t n)
{ (void)ei; (void)s; (void)n; finalized++; return true; }
void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{ (void)level; (void)format; }

unsigned long term_debug_eigrp;
unsigned long term_debug_eigrp_transmit;
void eigrp_nbr_state_update_reason(eigrp_operation_t op, eigrp_nbr_t *nbr,
                                   uint8_t state, uint16_t reason)
{ (void)op; (void)nbr; (void)state; (void)reason; }
void eigrp_nbr_delete(eigrp_nbr_t *nbr) { (void)nbr; }
uint64_t eigrp_sys_monotime_msec(void) { return 1000; }
void eigrp_sys_write_add(eigrp_event_t **event, eigrp_instance_t *eigrp,
                         eigrp_event_callback_t callback, void *arg)
{ (void)event; (void)eigrp; (void)callback; (void)arg; }
const char *eigrp_intf_name_string(eigrp_intf_t *ei)
{ (void)ei; return "test"; }
const char *eigrp_message_lookup(const eigrp_message_t *messages, int value,
                                 const char *fallback)
{ (void)messages; (void)value; (void)fallback; return "Hello"; }
uint32_t eigrp_nbr_rto(const eigrp_nbr_t *nbr) { (void)nbr; return 250; }
void eigrp_nbr_rto_backoff(eigrp_nbr_t *nbr) { (void)nbr; }
void eigrp_timer_add(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner,
                     eigrp_event_callback_t cb, void *arg, uint32_t delay)
{ (void)eigrp; (void)owner; (void)cb; (void)arg; (void)delay; }
void eigrp_debug_packet_retry(eigrp_nbr_t *nbr,
                              const eigrp_packet_t *pkt, uint8_t count)
{ (void)nbr; (void)pkt; (void)count; }

static eigrp_packet_t *new_packet(uint8_t opcode)
{
    eigrp_packet_t *p = eigrp_packet_create(64, NULL);
    struct eigrp_header header = {0};
    assert(p);
    header.opcode = opcode;
    assert(eigrp_stream_put(p->s, &header, sizeof(header)) == sizeof(header));
    /* A parameter TLV with graceful-shutdown K1 marker. */
    uint8_t tlv[5] = {0, 1, 0, 5, 0xff};
    assert(eigrp_stream_put(p->s, tlv, sizeof(tlv)) == sizeof(tlv));
    p->length = EIGRP_HEADER_LEN + sizeof(tlv);
    return p;
}

int main(void)
{
    eigrp_instance_t eigrp = {0};
    eigrp_intf_t ei = {0};
    eigrp_packet_queue_t obuf = {0};
    eigrp_list_t writeq = {0};
    eigrp_packet_t *stale_hello = new_packet(EIGRP_OPC_HELLO);
    eigrp_packet_t *stale_cr = new_packet(EIGRP_OPC_HELLO);
    eigrp_packet_t *stale_data = new_packet(EIGRP_OPC_UPDATE);

    eigrp.af_vectors.packet_send = record_send;
    eigrp.oi_write_q = &writeq;
    ei.eigrp = &eigrp;
    ei.obuf = &obuf;

    eigrp_packet_enqueue(&obuf, stale_data);
    eigrp_packet_enqueue(&obuf, stale_cr);
    eigrp_packet_enqueue(&obuf, stale_hello);
    assert(eigrp_list_add(&writeq, &ei));
    ei.on_write_q = 1;
    assert(obuf.count == 3 && writeq.count == 1);

    /* Existing queued output cannot prevent the final Hello reaching host. */
    eigrp_packet_goodbye_send(&ei, new_packet(EIGRP_OPC_HELLO));
    assert(sends == 1 && finalized == 1);
    assert(sent_opcode == EIGRP_OPC_HELLO && sent_seq == 0);
    assert(sent_k1 == 0xff);
    assert(obuf.count == 3);
    assert(ei.stats.sent.hello == 1);
    assert(ei.pacing_tx_bytes == EIGRP_HEADER_LEN + 5);

    /* Interface DOWN purges stale periodic/CR Hellos and data atomically. */
    eigrp_packet_interface_output_clear(&ei);
    assert(obuf.count == 0 && !obuf.head && !obuf.tail);
    assert(writeq.count == 0 && !writeq.head && !writeq.tail);
    assert(ei.on_write_q == 0);

    /* Fresh adjacency lifetime must not inherit old output. */
    eigrp_packet_enqueue(&obuf, new_packet(EIGRP_OPC_HELLO));
    assert(obuf.count == 1 && writeq.count == 0);
    eigrp_packet_interface_output_clear(&ei);
    assert(obuf.count == 0);
    puts("EIGRP interface lifecycle transmit passed");
    return 0;
}
