# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Behavioral coverage for RFC 7868 multiprotocol/v2 IPv6 wide route TLVs.

from pathlib import Path
import os
import subprocess
import sys
import textwrap


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


def test_ipv6_wide_tlv2_codec_round_trip_and_rejection(tmp_path):
    source = tmp_path / "tlv2_ipv6.c"
    binary = tmp_path / "tlv2_ipv6"
    source.write_text(
        textwrap.dedent(
            r'''
            #include <arpa/inet.h>
            #include <assert.h>
            #include <stdbool.h>
            #include <stdarg.h>
            #include <stdint.h>
            #include <string.h>

            #include "eigrp/code/eigrp.h"
            #include "eigrp/code/eigrp_log.h"
            #include "eigrp/code/eigrp_filter.h"
            #include "eigrp/code/eigrp_fsm.h"
            #include "eigrp/code/eigrp_metric.h"
            #include "eigrp/code/eigrp_prefix.h"
            #include "eigrp/code/eigrp_stream.h"
            #include "eigrp/code/eigrp_structs.h"
            #include "eigrp/code/eigrp_packet.h"
            #include "eigrp/code/eigrp_neighbor.h"
            #include "eigrp/code/eigrp_tlv1.h"
            #include "eigrp/code/eigrp_tlv2.h"

            static eigrp_route_descriptor_t decoded_route;
            static unsigned int route_free_count;

            eigrp_route_descriptor_t *eigrp_topology_route_create(eigrp_intf_t *ei)
            {
                memset(&decoded_route, 0, sizeof(decoded_route));
                decoded_route.ei = ei;
                return &decoded_route;
            }

            void eigrp_topology_route_free(eigrp_route_descriptor_t *route)
            {
                assert(route == &decoded_route);
                route_free_count++;
            }

            bool eigrp_filter_prefix_update(eigrp_instance_t *eigrp,
                                           eigrp_intf_t *ei, int direction,
                                           const eigrp_prefix_t *prefix)
            {
                (void)eigrp;
                (void)ei;
                (void)direction;
                (void)prefix;
                return false;
            }

            bool eigrp_debug_packet_any_enabled(uint8_t direction)
            {
                (void)direction;
                return false;
            }

            void eigrp_log(eigrp_log_level_t level, const char *format, ...)
            {
                (void)level;
                (void)format;
            }

            const char *eigrp_print_addr(const eigrp_addr_t *address)
            {
                (void)address;
                return "test";
            }

            eigrp_scaled_t eigrp_delay_to_scaled(eigrp_delay_t delay)
            {
                delay = delay ? delay : EIGRP_DELAY_MIN;
                return delay * EIGRP_CLASSIC_SCALER;
            }

            eigrp_delay_t eigrp_scaled_to_delay(eigrp_scaled_t scaled)
            {
                eigrp_delay_t delay = scaled / EIGRP_CLASSIC_SCALER;
                return delay ? delay : EIGRP_DELAY_MIN;
            }

            int eigrp_sys_packet_send(eigrp_instance_t *eigrp,
                                      eigrp_intf_t *ei,
                                      const eigrp_address_t *destination,
                                      const uint8_t *payload, size_t length)
            {
                (void)eigrp; (void)ei; (void)destination; (void)payload; (void)length;
                return -1;
            }

            bool eigrp_sys_packet_receive(eigrp_instance_t *eigrp,
                                          uint8_t *buffer, size_t capacity,
                                          size_t *received_length,
                                          eigrp_ifindex_t *ifindex,
                                          eigrp_address_t *source,
                                          eigrp_address_t *destination,
                                          eigrp_packet_rx_meta_t *meta)
            {
                (void)eigrp; (void)buffer; (void)capacity; (void)received_length;
                (void)ifindex; (void)source; (void)destination; (void)meta;
                return false;
            }

            eigrp_intf_t *eigrp_intf_lookup_by_ifindex(eigrp_instance_t *eigrp,
                                                            eigrp_ifindex_t ifindex)
            {
                (void)eigrp;
                (void)ifindex;
                return NULL;
            }

            static unsigned int ipv6_prefix_octets(uint8_t prefix_length)
            {
                if (prefix_length == 0)
                    return 0;
                if (prefix_length == 128)
                    return 16;
                return (prefix_length / 8U) + 1U;
            }

            static void metric_set(eigrp_metrics_t *metric)
            {
                memset(metric, 0, sizeof(*metric));
                metric->delay = 10;
                metric->bandwidth = 0x000a0000;
                metric->mtu[0] = 0xdc;
                metric->mtu[1] = 0x05;
                metric->mtu[2] = 0x00;
                metric->hop_count = 3;
                metric->reliability = 255;
                metric->load = 7;
                metric->tag = 9;
                metric->flags = 1;
            }

            static void prefix_set(eigrp_prefix_t *prefix, uint8_t prefix_length)
            {
                memset(prefix, 0, sizeof(*prefix));
                prefix->address.afi = EIGRP_AFI_IPV6;
                prefix->prefix_length = prefix_length;
                assert(inet_pton(AF_INET6, "2001:db8:1234:5678:9abc:def0:1234:5678",
                                 prefix->address.bytes) == 1);
                eigrp_prefix_normalize(prefix);
            }

            static void route_set(eigrp_route_descriptor_t *route, bool external,
                                  uint8_t prefix_length)
            {
                memset(route, 0, sizeof(*route));
                route->type = external ? EIGRP_EXT : EIGRP_INT;
                route->nexthop.afi = AF_INET6;
                assert(inet_pton(AF_INET6, "fe80::1234", &route->nexthop.ip.v6) == 1);
                prefix_set(&route->dest, prefix_length);
                metric_set(&route->metric);
                if (external) {
                    route->extdata.orig = 0x0a000001;
                    route->extdata.as = 4453;
                    route->extdata.tag = 0x12345678;
                    route->extdata.metric = 20;
                    route->extdata.reserved = 0;
                    route->extdata.protocol = 6;
                    route->extdata.flags = 1;
                }
            }

            static void assert_metric(const eigrp_metrics_t *metric)
            {
                assert(metric->delay == 10);
                assert(metric->bandwidth == 0x000a0000);
                assert(metric->mtu[0] == 0xdc);
                assert(metric->mtu[1] == 0x05);
                assert(metric->mtu[2] == 0x00);
                assert(metric->hop_count == 3);
                assert(metric->reliability == 255);
                assert(metric->load == 7);
                assert(metric->tag == 9);
                assert(metric->flags == 1);
            }

            static void round_trip(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
                                   eigrp_nbr_t *nbr, eigrp_tlv_codec_t *codec,
                                   bool external, uint8_t prefix_length)
            {
                eigrp_route_descriptor_t route;
                eigrp_route_descriptor_t *decoded;
                eigrp_stream_t *stream = eigrp_stream_create(128);
                unsigned int prefix_octets = ipv6_prefix_octets(prefix_length);
                uint16_t expected_type = external ? EIGRP_TLV_MP_EXT : EIGRP_TLV_MP_INT;
                uint16_t addpath_len = external ? 28 : 26;
                uint16_t expected_length = 4 + 8 + 24 + addpath_len + (external ? 16 : 0) + 1 + prefix_octets;

                assert(stream != NULL);
                route_set(&route, external, prefix_length);
                assert(codec->encoder(eigrp, ei, nbr, stream, &route) == expected_length);
                assert(stream->endp == expected_length);
                assert(stream->data[0] == (expected_type >> 8));
                assert(stream->data[1] == (expected_type & 0xff));
                assert(stream->data[2] == (expected_length >> 8));
                assert(stream->data[3] == (expected_length & 0xff));
                assert(stream->data[4] == 0x00 && stream->data[5] == EIGRP_AF_IPv6);
                assert(stream->data[6] == 0x00 && stream->data[7] == EIGRP_TOPOLOGY_ID_BASE);
                assert(stream->data[12] == (uint8_t)(addpath_len / 2));
                assert(stream->data[36] == 0x07);
                assert(stream->data[37] == (external ? 13 : 12));
                assert(memcmp(stream->data + 38, &route.nexthop.ip.v6, 16) == 0);
                assert(stream->data[expected_length - (1 + prefix_octets)] == prefix_length);

                decoded = codec->decoder(eigrp, nbr, stream, (uint16_t)stream->endp);
                assert(decoded != NULL);
                assert(decoded->type == (external ? EIGRP_TLV_IPv6_EXT : EIGRP_TLV_IPv6_INT));
                assert(decoded->topology_id == EIGRP_TOPOLOGY_ID_BASE);
                assert(decoded->nexthop.afi == AF_INET6);
                assert(memcmp(&decoded->nexthop.ip.v6, &route.nexthop.ip.v6, 16) == 0);
                assert(decoded->dest.address.afi == EIGRP_AFI_IPV6);
                assert(decoded->dest.prefix_length == prefix_length);
                assert(memcmp(decoded->dest.address.bytes, route.dest.address.bytes, 16) == 0);
                assert_metric(&decoded->metric);
                if (external)
                    assert(memcmp(&decoded->extdata, &route.extdata,
                                  sizeof(route.extdata)) == 0);
                assert(stream->getp == stream->endp);
                eigrp_stream_free(stream);
            }

            static void reject_bytes(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
                                     eigrp_tlv_codec_t *codec,
                                     const uint8_t *bytes, size_t length)
            {
                eigrp_stream_t *stream = eigrp_stream_create(length ? length : 1);
                assert(stream != NULL);
                assert(eigrp_stream_put(stream, bytes, length) == length);
                assert(codec->decoder(eigrp, nbr, stream, (uint16_t)length) == NULL);
                assert(stream->getp == stream->endp);
                eigrp_stream_free(stream);
            }

            int main(void)
            {
                static const uint8_t prefix_lengths[] = {0, 1, 7, 8, 9, 63, 64, 65, 127, 128};
                eigrp_instance_t eigrp = {0};
                eigrp_intf_t interface = {0};
                eigrp_nbr_t neighbor = {0};
                eigrp_tlv_codec_t codec = {0};
                eigrp_tlv_codec_t tlv1 = {0};
                eigrp_route_descriptor_t route;
                eigrp_route_descriptor_t *decoded;
                eigrp_stream_t *valid;
                eigrp_stream_t *mixed;
                uint8_t short_header[] = {0x04, 0x02, 0x00};
                uint8_t too_long[] = {0x04, 0x02, 0x00, 0x40};
                size_t prefix_offset;

                eigrp_ipv6_init(&eigrp.af_vectors);
                eigrp_tlv1_init(&tlv1);
                eigrp_tlv2_init(&codec);
                interface.eigrp = &eigrp;
                neighbor.ei = &interface;

                /* Forced `metric version 32bit` packet policy selects TLV1. */
                eigrp.metric_version = EIGRP_TLV_64B_VERSION;
                eigrp.tlv1_codec = tlv1;
                eigrp.tlv2_codec = codec;
                neighbor.tlv_version = EIGRP_TLV_64B_VERSION;
                neighbor.encoder = eigrp.tlv2_codec.encoder;
                neighbor.decoder = eigrp.tlv2_codec.decoder;
                assert(neighbor.encoder == codec.encoder);
                assert(neighbor.decoder == codec.decoder);

                for (size_t i = 0; i < sizeof(prefix_lengths); i++) {
                    round_trip(&eigrp, &interface, &neighbor, &codec, false,
                               prefix_lengths[i]);
                    round_trip(&eigrp, &interface, &neighbor, &codec, true,
                               prefix_lengths[i]);
                }

                reject_bytes(&eigrp, &neighbor, &codec, short_header, sizeof(short_header));
                reject_bytes(&eigrp, &neighbor, &codec, too_long, sizeof(too_long));


                /* A mixed-capability interface emits one classic and one MP TLV. */
                mixed = eigrp_stream_create(256);
                assert(mixed != NULL);
                route_set(&route, false, 64);
                assert(eigrp_packet_encoder_both(&eigrp, &interface, NULL, mixed, &route)
                       == 46 + 72);
                assert(mixed->data[0] == 0x04 && mixed->data[1] == 0x02);
                assert(mixed->data[46] == 0x06 && mixed->data[47] == 0x02);
                eigrp_stream_free(mixed);

                /* Corrupt AddPath's data-word count: decoder must terminate safely. */
                valid = eigrp_stream_create(128);
                assert(valid != NULL);
                route_set(&route, false, 64);
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 72);
                valid->data[37] = 13;
                assert(codec.decoder(&eigrp, &neighbor, valid, (uint16_t)valid->endp) == NULL);
                assert(valid->getp == valid->endp);
                eigrp_stream_free(valid);

                /* Multiprotocol TIDs are portable protocol identities, not CLI capability gates. */
                valid = eigrp_stream_create(128);
                assert(valid != NULL);
                route_set(&route, false, 64);
                route.topology_id = 1;
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 72);
                assert(valid->data[6] == 0x00 && valid->data[7] == 0x01);
                valid->getp = 0;
                decoded = codec.decoder(&eigrp, &neighbor, valid, (uint16_t)valid->endp);
                assert(decoded != NULL);
                assert(decoded->topology_id == 1);
                assert(valid->getp == valid->endp);
                eigrp_stream_free(valid);

                /* IPv4 AFI in an IPv6 runtime is skipped as a non-matching MP TLV. */
                valid = eigrp_stream_create(128);
                assert(valid != NULL);
                route_set(&route, false, 64);
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 72);
                valid->data[5] = EIGRP_AF_IPv4;
                assert(codec.decoder(&eigrp, &neighbor, valid, (uint16_t)valid->endp) == NULL);
                assert(valid->getp == valid->endp);
                eigrp_stream_free(valid);

                /* Start from a valid /128 internal TLV, then truncate its destination. */
                valid = eigrp_stream_create(128);
                assert(valid != NULL);
                route_set(&route, false, 128);
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 79);
                valid->endp--;
                valid->data[2] = (uint8_t)(valid->endp >> 8);
                valid->data[3] = (uint8_t)(valid->endp & 0xff);
                assert(codec.decoder(&eigrp, &neighbor, valid, (uint16_t)valid->endp) == NULL);
                assert(valid->getp == valid->endp);
                assert(route_free_count > 0);
                eigrp_stream_free(valid);

                /* Invalid IPv6 prefix lengths are rejected without partial decode. */
                valid = eigrp_stream_create(128);
                assert(valid != NULL);
                route_set(&route, false, 64);
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 72);
                prefix_offset = 4 + 8 + 24 + 26;
                valid->data[prefix_offset] = 129;
                assert(codec.decoder(&eigrp, &neighbor, valid, (uint16_t)valid->endp) == NULL);
                assert(valid->getp == valid->endp);
                eigrp_stream_free(valid);

                return 0;
            }
            '''
        )
    )

    compiler = os.environ.get("CC", "cc")
    result = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-D_POSIX_C_SOURCE=200809L",
            "-ffunction-sections",
            "-fdata-sections",
            f"-I{ROOT}",
            str(source),
            str(ROOT / "eigrp" / "code" / "eigrp_tlv1.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_tlv2.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_packet.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_ipv6.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_stream.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_prefix.c"),
            *( ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"] ),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr

    result = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr



def test_v2_is_default_and_ipv6_binds_multiprotocol_afi():
    instance = (ROOT / "eigrp" / "code" / "eigrp_instance.c").read_text()
    metric = (ROOT / "eigrp" / "code" / "eigrp_metric.c").read_text()
    ipv6 = (ROOT / "eigrp" / "code" / "eigrp_ipv6.c").read_text()

    assert "eigrp->metric_version = EIGRP_MAJOR_VERSION;" in instance
    assert "#define EIGRP_MAJOR_VERSION 2" in (ROOT / "eigrp" / "code" / "eigrp_metric.h").read_text()
    assert "context->runtime->metric_version = EIGRP_MAJOR_VERSION;" in metric
    assert "vectors->multiprotocol_afi = EIGRP_AF_IPv6;" in ipv6
