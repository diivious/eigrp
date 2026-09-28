# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Behavioral coverage for RFC 7868 classic/v1 IPv6 route TLVs.

from pathlib import Path
import os
import subprocess
import textwrap


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def test_ipv6_classic_tlv1_codec_round_trip_and_rejection(tmp_path):
    source = tmp_path / "tlv1_ipv6.c"
    binary = tmp_path / "tlv1_ipv6"
    source.write_text(
        textwrap.dedent(
            r'''
            #include <arpa/inet.h>
            #include <assert.h>
            #include <stdbool.h>
            #include <stdarg.h>
            #include <stdint.h>
            #include <string.h>

            #include "eigrpd/code/eigrpd.h"
            #include "eigrpd/code/eigrp_filter.h"
            #include "eigrpd/code/eigrp_fsm.h"
            #include "eigrpd/code/eigrp_metric.h"
            #include "eigrpd/code/eigrp_prefix.h"
            #include "eigrpd/code/eigrp_stream.h"
            #include "eigrpd/code/eigrp_structs.h"
            #include "eigrpd/code/eigrp_packet.h"
            #include "eigrpd/code/eigrp_neighbor.h"
            #include "eigrpd/code/eigrp_tlv1.h"

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
                uint16_t expected_type = external ? EIGRP_TLV_IPv6_EXT : EIGRP_TLV_IPv6_INT;
                uint16_t expected_length = 4 + 16 + (external ? 20 : 0) + 16 + 1 + prefix_octets;

                assert(stream != NULL);
                route_set(&route, external, prefix_length);
                assert(codec->encoder(eigrp, ei, nbr, stream, &route) == expected_length);
                assert(stream->endp == expected_length);
                assert(stream->data[0] == (expected_type >> 8));
                assert(stream->data[1] == (expected_type & 0xff));
                assert(stream->data[2] == (expected_length >> 8));
                assert(stream->data[3] == (expected_length & 0xff));
                assert(memcmp(stream->data + 4, &route.nexthop.ip.v6, 16) == 0);
                assert(stream->data[4 + 16 + (external ? 20 : 0) + 16] == prefix_length);

                decoded = codec->decoder(eigrp, nbr, stream, (uint16_t)stream->endp);
                assert(decoded != NULL);
                assert(decoded->type == expected_type);
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
                eigrp_route_descriptor_t route;
                eigrp_stream_t *valid;
                uint8_t short_header[] = {0x04, 0x02, 0x00};
                uint8_t too_long[] = {0x04, 0x02, 0x00, 0x40};
                size_t prefix_offset;

                eigrp_ipv6_init(&eigrp.af_vectors);
                eigrp_tlv1_init(&codec);
                interface.eigrp = &eigrp;
                neighbor.ei = &interface;

                /* Forced `metric version 32bit` packet policy selects TLV1. */
                eigrp.metric_version = EIGRP_TLV_32B_VERSION;
                eigrp.tlv1_codec = codec;
                neighbor.tlv_version = EIGRP_TLV_32B_VERSION;
                neighbor.encoder = eigrp.tlv1_codec.encoder;
                neighbor.decoder = eigrp.tlv1_codec.decoder;
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

                /* Start from a valid /128 internal TLV, then truncate its destination. */
                valid = eigrp_stream_create(128);
                assert(valid != NULL);
                route_set(&route, false, 128);
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 53);
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
                assert(codec.encoder(&eigrp, &interface, &neighbor, valid, &route) == 46);
                prefix_offset = 4 + 16 + 16;
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
            str(ROOT / "eigrpd" / "code" / "eigrp_tlv1.c"),
            str(ROOT / "eigrpd" / "code" / "eigrp_ipv6.c"),
            str(ROOT / "eigrpd" / "code" / "eigrp_stream.c"),
            str(ROOT / "eigrpd" / "code" / "eigrp_prefix.c"),
            "-Wl,--gc-sections",
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


def test_forced_32bit_policy_refreshes_neighbor_codec_and_ipv6_uses_classic_types():
    metric = (ROOT / "eigrpd" / "code" / "eigrp_metric.c").read_text()
    neighbor = (ROOT / "eigrpd" / "code" / "eigrp_neighbor.c").read_text()
    ipv6 = (ROOT / "eigrpd" / "code" / "eigrp_ipv6.c").read_text()

    assert "context->runtime->metric_version = EIGRP_TLV_32B_VERSION;" in metric
    assert "eigrp_nbr_codec_update(context->runtime);" in metric
    assert "case EIGRP_TLV_32B_VERSION:" in neighbor
    assert "codec = &nbr->ei->eigrp->tlv1_codec;" in neighbor
    assert "vectors->classic_internal_tlv_type = EIGRP_TLV_IPv6_INT;" in ipv6
    assert "vectors->classic_external_tlv_type = EIGRP_TLV_IPv6_EXT;" in ipv6
