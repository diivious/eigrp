# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Behavioral coverage for the IPv6 address-family vectors that do not depend
# on the host IPv6 socket/data-path integration.

from pathlib import Path
import os
import subprocess
import textwrap


ROOT = Path(__file__).resolve().parents[4]


def test_ipv6_af_local_vectors(tmp_path):
    source = tmp_path / "ipv6_vectors.c"
    binary = tmp_path / "ipv6_vectors"
    source.write_text(
        textwrap.dedent(
            r'''
            #include <arpa/inet.h>
            #include <assert.h>
            #include <string.h>

            #include "eigrpd/eigrp_structs.h"
            #include "eigrpd/eigrp_prefix.h"
            #include "eigrpd/eigrp_types.h"

            int main(void)
            {
                eigrp_af_vectors_t vectors;
                eigrp_stream_t *stream;
                eigrp_addr_t address = {0};
                eigrp_addr_t decoded_address = {0};
                eigrp_prefix_t prefix = {0};
                eigrp_prefix_t decoded_prefix = {0};
                eigrp_interface_t interface = {0};
                char text[128];

                eigrp_ipv6_init(&vectors);
                assert(vectors.afi == EIGRP_ADDRESS_FAMILY_IPV6);
                assert(vectors.packet_send == NULL);
                assert(vectors.packet_receive == NULL);
                assert(vectors.packet_source_on_link != NULL);
                assert(vectors.packet_address_bytes == 16);
                assert(vectors.packet_address_encode != NULL);
                assert(vectors.packet_address_decode != NULL);
                assert(vectors.packet_prefix_encode != NULL);
                assert(vectors.packet_prefix_decode != NULL);
                assert(vectors.classic_internal_tlv_type == EIGRP_TLV_IPv6_INT);
                assert(vectors.classic_external_tlv_type == EIGRP_TLV_IPv6_EXT);
                assert(vectors.multiprotocol_afi == EIGRP_AF_IPv6);
                assert(vectors.summary_auto_prefix != NULL);

                stream = eigrp_stream_new(64);
                assert(stream != NULL);

                address.afi = AF_INET6;
                assert(inet_pton(AF_INET6, "fe80::1234", &address.ip.v6) == 1);
                assert(vectors.packet_address_encode(stream, &address) == 16);
                assert(vectors.packet_address_decode(stream, &decoded_address) == 16);
                assert(decoded_address.afi == AF_INET6);
                assert(memcmp(&address.ip.v6, &decoded_address.ip.v6, 16) == 0);
                eigrp_stream_reset(stream);

                prefix.address.afi = EIGRP_ADDRESS_FAMILY_IPV6;
                prefix.prefix_length = 64;
                assert(inet_pton(AF_INET6, "2001:db8:1:2:abcd::",
                                 prefix.address.bytes) == 1);
                assert(vectors.packet_prefix_encode(stream, &prefix) == 10);
                assert(stream->endp == 10);
                assert(stream->data[0] == 64);
                assert(stream->data[9] == 0);
                assert(vectors.packet_prefix_decode(stream, &decoded_prefix) == 10);
                assert(decoded_prefix.address.afi == EIGRP_ADDRESS_FAMILY_IPV6);
                assert(decoded_prefix.prefix_length == 64);
                assert(decoded_prefix.address.bytes[8] == 0);
                assert(eigrp_prefix_snprintf(text, sizeof(text),
                                             &decoded_prefix) > 0);
                assert(strcmp(text, "2001:db8:1:2::/64") == 0);
                eigrp_stream_reset(stream);

                prefix.prefix_length = 0;
                memset(prefix.address.bytes, 0xff, sizeof(prefix.address.bytes));
                assert(vectors.packet_prefix_encode(stream, &prefix) == 1);
                assert(stream->endp == 1);
                eigrp_stream_reset(stream);

                prefix.prefix_length = 128;
                assert(inet_pton(AF_INET6, "2001:db8::1",
                                 prefix.address.bytes) == 1);
                assert(vectors.packet_prefix_encode(stream, &prefix) == 17);
                assert(stream->endp == 17);
                eigrp_stream_reset(stream);

                eigrp_stream_putc(stream, 129);
                assert(vectors.packet_prefix_decode(stream, &decoded_prefix) == 0);
                assert(stream->getp == 0);
                eigrp_stream_reset(stream);

                address.afi = AF_INET6;
                assert(inet_pton(AF_INET6, "fe80::1", &address.ip.v6) == 1);
                assert(vectors.packet_source_on_link(&interface, &address));
                assert(inet_pton(AF_INET6, "2001:db8::1", &address.ip.v6) == 1);
                assert(!vectors.packet_source_on_link(&interface, &address));

                assert(vectors.summary_auto_prefix(&prefix, &decoded_prefix)
                       == EIGRP_RESULT_UNSUPPORTED);

                eigrp_stream_free(stream);
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
            f"-I{ROOT}",
            str(source),
            str(ROOT / "eigrpd" / "eigrp_ipv6.c"),
            str(ROOT / "eigrpd" / "eigrp_stream.c"),
            str(ROOT / "eigrpd" / "eigrp_prefix.c"),
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
