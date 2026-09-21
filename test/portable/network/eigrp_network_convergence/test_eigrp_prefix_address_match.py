# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Behavioral coverage for the common prefix/address matcher used by IPv4
# network participation and other prefix consumers.

from pathlib import Path
import os
import subprocess
import textwrap


ROOT = Path(__file__).resolve().parents[4]


def test_common_prefix_address_match(tmp_path):
    source = tmp_path / "prefix_address_match.c"
    binary = tmp_path / "prefix_address_match"
    source.write_text(
        textwrap.dedent(
            r'''
            #include <arpa/inet.h>
            #include <assert.h>
            #include <string.h>

            #include "eigrpd/eigrp_prefix.h"

            static void ipv4_prefix(eigrp_prefix_t *prefix, const char *text,
                                    unsigned int length)
            {
                memset(prefix, 0, sizeof(*prefix));
                prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
                prefix->prefix_length = length;
                assert(inet_pton(AF_INET, text, prefix->address.bytes) == 1);
            }

            static void ipv4_address(eigrp_address_t *address, const char *text)
            {
                memset(address, 0, sizeof(*address));
                address->afi = EIGRP_ADDRESS_FAMILY_IPV4;
                assert(inet_pton(AF_INET, text, address->bytes) == 1);
            }

            static void ipv6_prefix(eigrp_prefix_t *prefix, const char *text,
                                    unsigned int length)
            {
                memset(prefix, 0, sizeof(*prefix));
                prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV6;
                prefix->prefix_length = length;
                assert(inet_pton(AF_INET6, text, prefix->address.bytes) == 1);
            }

            static void ipv6_address(eigrp_address_t *address, const char *text)
            {
                memset(address, 0, sizeof(*address));
                address->afi = EIGRP_ADDRESS_FAMILY_IPV6;
                assert(inet_pton(AF_INET6, text, address->bytes) == 1);
            }

            int main(void)
            {
                eigrp_prefix_t prefix;
                eigrp_address_t address;

                ipv4_prefix(&prefix, "10.0.0.0", 8);
                ipv4_address(&address, "10.24.5.1");
                assert(eigrp_prefix_address_match(&prefix, &address));
                ipv4_address(&address, "11.24.5.1");
                assert(!eigrp_prefix_address_match(&prefix, &address));

                /* A host-specific network statement matches the interface
                 * address regardless of the interface subnet length because
                 * the matcher consumes an address, not a second prefix.
                 */
                ipv4_prefix(&prefix, "10.24.5.1", 32);
                ipv4_address(&address, "10.24.5.1");
                assert(eigrp_prefix_address_match(&prefix, &address));

                ipv4_prefix(&prefix, "192.0.2.128", 25);
                ipv4_address(&address, "192.0.2.200");
                assert(eigrp_prefix_address_match(&prefix, &address));
                ipv4_address(&address, "192.0.2.100");
                assert(!eigrp_prefix_address_match(&prefix, &address));

                ipv6_prefix(&prefix, "2001:db8:1::", 48);
                ipv6_address(&address, "2001:db8:1:2::1");
                assert(eigrp_prefix_address_match(&prefix, &address));
                ipv6_address(&address, "2001:db8:2::1");
                assert(!eigrp_prefix_address_match(&prefix, &address));

                ipv4_prefix(&prefix, "0.0.0.0", 0);
                ipv4_address(&address, "203.0.113.9");
                assert(eigrp_prefix_address_match(&prefix, &address));

                prefix.prefix_length = 33;
                assert(!eigrp_prefix_address_match(&prefix, &address));

                ipv6_address(&address, "2001:db8::1");
                assert(!eigrp_prefix_address_match(&prefix, &address));
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
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT}",
            str(source),
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
