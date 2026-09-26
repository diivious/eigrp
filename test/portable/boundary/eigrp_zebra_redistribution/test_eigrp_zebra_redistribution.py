# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Focused FRR/Zebra redistribution subscription and normalization tests.

from pathlib import Path
import os
import re
import subprocess
import textwrap


ROOT = Path(__file__).resolve().parents[4]
ZEBRA_C = ROOT / "frr" / "eigrp_zebra.c"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    pattern = rf"(?:^|\n)(?:static\s+)?[^\n;{{]*(?:\n[ \t]*)?\b{name}\("
    for match in re.finditer(pattern, source):
        start = match.start()
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
    raise AssertionError(f"missing function definition {name}")


def test_zebra_subscription_uses_exact_afi_type_instance_and_vrf():
    zebra = read(ZEBRA_C)
    update = function_body(zebra, "eigrp_zebra_redistribute_update")
    delete = function_body(zebra, "eigrp_zebra_redistribute_delete")
    params = function_body(zebra, "eigrp_zebra_subscription_params_build")
    valid = function_body(zebra, "eigrp_zebra_source_instance_valid")

    assert "params->afi = eigrp_zebra_instance_afi(eigrp);" in params
    assert "params->type = eigrp_zebra_redistribute_type(source->protocol);" in params
    assert "params->instance = (unsigned short)source->route_instance;" in params
    assert "params->vrf_id = (vrf_id_t)eigrp_instance_vrf_id(eigrp);" in params
    assert "params.afi, params.type, params.instance" in update
    assert "params.vrf_id" in update
    assert "params.type, params.instance" in delete
    assert "params.vrf_id" in delete
    assert "AFI_IP," not in update
    assert "AFI_IP," not in delete

    # FRR carries OSPF and EIGRP route instances, but a BGP ASN is not a
    # Zebra route-instance value.
    assert "EIGRP_REDISTRIBUTE_PROTOCOL_OSPF" in valid
    assert "EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP" in valid
    assert "EIGRP_REDISTRIBUTE_PROTOCOL_BGP" in valid
    assert "return route_instance == 0;" in valid
    assert "BGP ASN is not a Zebra route-instance value" in valid


def test_zebra_receive_filters_exact_runtime_and_preserves_lifecycle():
    zebra = read(ZEBRA_C)
    receive = function_body(zebra, "eigrp_zebra_redistribute_route")
    accepts = function_body(zebra, "eigrp_zebra_redistribute_accepts")

    assert "eigrp_instance_vrf_id(state->eigrp)" in receive
    assert "eigrp_instance_address_family(state->eigrp)" in receive
    assert "source.source.route_instance" in receive
    assert "eigrp_instance_asn(state->eigrp)" in receive
    assert "eigrp_rib_source_route_add" in receive
    assert "eigrp_rib_source_route_remove" in receive
    assert "cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD" in receive
    assert "redist->route_instance == route_instance" in accepts


def test_subscription_parameters_and_route_normalization_execute(tmp_path):
    zebra = read(ZEBRA_C)
    functions = "\n\n".join(
        function_body(zebra, name)
        for name in (
            "eigrp_zebra_instance_afi",
            "eigrp_zebra_redistribute_protocol",
            "eigrp_zebra_redistribute_type",
            "eigrp_zebra_source_instance_valid",
            "eigrp_zebra_subscription_params_build",
            "eigrp_zebra_source_nexthop_import",
            "eigrp_zebra_source_route_import",
        )
    )

    source = tmp_path / "zebra_redistribution_focus.c"
    binary = tmp_path / "zebra_redistribution_focus"
    source.write_text(
        textwrap.dedent(
            r"""
            #include <arpa/inet.h>
            #include <assert.h>
            #include <stdbool.h>
            #include <stdint.h>
            #include <string.h>

            typedef enum eigrp_result {
                EIGRP_RESULT_SUCCESS = 0,
                EIGRP_RESULT_NOT_IMPLEMENTED,
                EIGRP_RESULT_INVALID_ARGUMENT,
                EIGRP_RESULT_NOT_FOUND,
                EIGRP_RESULT_CONFLICT,
                EIGRP_RESULT_UNSUPPORTED,
                EIGRP_RESULT_INTERNAL_FAILURE,
            } eigrp_result_t;

            typedef enum eigrp_address_family {
                EIGRP_ADDRESS_FAMILY_IPV4 = 4,
                EIGRP_ADDRESS_FAMILY_IPV6 = 6,
            } eigrp_address_family_t;

            typedef uint32_t eigrp_vrf_id_t;
            typedef uint32_t eigrp_ifindex_t;
            typedef uint32_t eigrp_route_instance_t;
            typedef uint32_t vrf_id_t;
            typedef int afi_t;

            enum {
                AFI_UNSPEC = 0,
                AFI_IP = 1,
                AFI_IP6 = 2,
                SAFI_UNICAST = 1,
            };

            enum {
                ZEBRA_ROUTE_CONNECT = 1,
                ZEBRA_ROUTE_STATIC = 2,
                ZEBRA_ROUTE_RIP = 3,
                ZEBRA_ROUTE_OSPF = 4,
                ZEBRA_ROUTE_ISIS = 5,
                ZEBRA_ROUTE_BGP = 6,
                ZEBRA_ROUTE_EIGRP = 7,
                ZEBRA_ROUTE_MAX = 8,
            };

            enum {
                NEXTHOP_TYPE_IFINDEX = 1,
                NEXTHOP_TYPE_IPV4 = 2,
                NEXTHOP_TYPE_IPV4_IFINDEX = 3,
                NEXTHOP_TYPE_IPV6 = 4,
                NEXTHOP_TYPE_IPV6_IFINDEX = 5,
                NEXTHOP_TYPE_BLACKHOLE = 6,
            };

            typedef enum eigrp_redistribute_protocol {
                EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED = 0,
                EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED,
                EIGRP_REDISTRIBUTE_PROTOCOL_STATIC,
                EIGRP_REDISTRIBUTE_PROTOCOL_RIP,
                EIGRP_REDISTRIBUTE_PROTOCOL_OSPF,
                EIGRP_REDISTRIBUTE_PROTOCOL_ISIS,
                EIGRP_REDISTRIBUTE_PROTOCOL_BGP,
                EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP,
            } eigrp_redistribute_protocol_t;

            typedef struct eigrp_address {
                eigrp_address_family_t afi;
                uint8_t bytes[16];
            } eigrp_address_t;

            typedef struct eigrp_prefix {
                eigrp_address_t address;
                uint8_t prefix_length;
            } eigrp_prefix_t;

            typedef struct eigrp_redistribute_source {
                eigrp_redistribute_protocol_t protocol;
                eigrp_route_instance_t route_instance;
            } eigrp_redistribute_source_t;

            typedef struct eigrp_rib_source_route {
                eigrp_prefix_t prefix;
                eigrp_address_t gateway;
                bool gateway_present;
                eigrp_ifindex_t ifindex;
                uint64_t metric;
                uint32_t tag;
                eigrp_redistribute_source_t source;
            } eigrp_rib_source_route_t;

            typedef struct eigrp_instance {
                eigrp_vrf_id_t vrf_id;
                eigrp_address_family_t afi;
                uint16_t asn;
            } eigrp_instance_t;

            struct eigrp_zebra_subscription_params {
                afi_t afi;
                int type;
                unsigned short instance;
                vrf_id_t vrf_id;
            };

            union g_addr {
                struct in_addr ipv4;
                struct in6_addr ipv6;
            };

            struct zapi_nexthop {
                int type;
                vrf_id_t vrf_id;
                eigrp_ifindex_t ifindex;
                union g_addr gate;
            };

            struct prefix {
                int family;
                uint8_t prefixlen;
                union {
                    struct in_addr prefix4;
                    struct in6_addr prefix6;
                } u;
            };

            struct zapi_route {
                int type;
                unsigned short instance;
                int safi;
                struct prefix prefix;
                uint32_t metric;
                uint32_t tag;
                struct zapi_nexthop nexthops[8];
                int nexthop_num;
            };

            static eigrp_vrf_id_t
            eigrp_instance_vrf_id(const eigrp_instance_t *eigrp)
            {
                return eigrp ? eigrp->vrf_id : 0;
            }

            static eigrp_address_family_t
            eigrp_instance_address_family(const eigrp_instance_t *eigrp)
            {
                return eigrp ? eigrp->afi : 0;
            }

            static eigrp_result_t
            eigrp_frr_prefix_import(const struct prefix *host,
                                    eigrp_prefix_t *prefix)
            {
                if (!host || !prefix)
                    return EIGRP_RESULT_INVALID_ARGUMENT;
                memset(prefix, 0, sizeof(*prefix));
                prefix->prefix_length = host->prefixlen;
                if (host->family == AF_INET) {
                    prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
                    memcpy(prefix->address.bytes, &host->u.prefix4,
                           sizeof(host->u.prefix4));
                    return EIGRP_RESULT_SUCCESS;
                }
                if (host->family == AF_INET6) {
                    prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV6;
                    memcpy(prefix->address.bytes, &host->u.prefix6,
                           sizeof(host->u.prefix6));
                    return EIGRP_RESULT_SUCCESS;
                }
                return EIGRP_RESULT_UNSUPPORTED;
            }
            """
        )
        + "\n"
        + functions
        + textwrap.dedent(
            r"""

            int main(void)
            {
                eigrp_instance_t eigrp = {
                    .vrf_id = 42,
                    .afi = EIGRP_ADDRESS_FAMILY_IPV4,
                    .asn = 4453,
                };
                eigrp_redistribute_source_t source = {0};
                struct eigrp_zebra_subscription_params params = {0};
                struct zapi_route api = {0};
                eigrp_rib_source_route_t route = {0};
                struct in_addr v4;
                struct in6_addr v6;

                source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_OSPF;
                source.route_instance = 100;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params) == EIGRP_RESULT_SUCCESS);
                assert(params.afi == AFI_IP);
                assert(params.type == ZEBRA_ROUTE_OSPF);
                assert(params.instance == 100);
                assert(params.vrf_id == 42);

                source.route_instance = 200;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params) == EIGRP_RESULT_SUCCESS);
                assert(params.instance == 200);

                eigrp.afi = EIGRP_ADDRESS_FAMILY_IPV6;
                source.route_instance = 100;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params) == EIGRP_RESULT_SUCCESS);
                assert(params.afi == AFI_IP6);

                source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
                source.route_instance = 4454;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params) == EIGRP_RESULT_SUCCESS);
                assert(params.type == ZEBRA_ROUTE_EIGRP);
                assert(params.instance == 4454);
                source.route_instance = 0;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params)
                       == EIGRP_RESULT_INVALID_ARGUMENT);

                source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_BGP;
                source.route_instance = 0;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params) == EIGRP_RESULT_SUCCESS);
                assert(params.type == ZEBRA_ROUTE_BGP);
                assert(params.instance == 0);
                source.route_instance = 65000;
                assert(eigrp_zebra_subscription_params_build(
                           &eigrp, &source, &params)
                       == EIGRP_RESULT_INVALID_ARGUMENT);

                memset(&api, 0, sizeof(api));
                api.safi = SAFI_UNICAST;
                api.type = ZEBRA_ROUTE_OSPF;
                api.instance = 123;
                api.metric = 777;
                api.tag = 99;
                api.prefix.family = AF_INET;
                api.prefix.prefixlen = 24;
                assert(inet_pton(AF_INET, "192.0.2.0",
                                 &api.prefix.u.prefix4) == 1);
                api.nexthop_num = 2;
                api.nexthops[0].type = NEXTHOP_TYPE_BLACKHOLE;
                api.nexthops[1].type = NEXTHOP_TYPE_IPV4_IFINDEX;
                api.nexthops[1].ifindex = 17;
                assert(inet_pton(AF_INET, "192.0.2.1",
                                 &api.nexthops[1].gate.ipv4) == 1);
                assert(eigrp_zebra_source_route_import(&api, &route)
                       == EIGRP_RESULT_SUCCESS);
                assert(route.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_OSPF);
                assert(route.source.route_instance == 123);
                assert(route.prefix.address.afi == EIGRP_ADDRESS_FAMILY_IPV4);
                assert(route.prefix.prefix_length == 24);
                assert(route.metric == 777);
                assert(route.tag == 99);
                assert(route.ifindex == 17);
                assert(route.gateway_present);
                assert(inet_pton(AF_INET, "192.0.2.1", &v4) == 1);
                assert(memcmp(route.gateway.bytes, &v4, sizeof(v4)) == 0);

                memset(&api, 0, sizeof(api));
                api.safi = SAFI_UNICAST;
                api.type = ZEBRA_ROUTE_OSPF;
                api.instance = 124;
                api.prefix.family = AF_INET6;
                api.prefix.prefixlen = 64;
                assert(inet_pton(AF_INET6, "2001:db8::",
                                 &api.prefix.u.prefix6) == 1);
                api.nexthop_num = 1;
                api.nexthops[0].type = NEXTHOP_TYPE_IPV6_IFINDEX;
                api.nexthops[0].ifindex = 18;
                assert(inet_pton(AF_INET6, "fe80::1",
                                 &api.nexthops[0].gate.ipv6) == 1);
                assert(eigrp_zebra_source_route_import(&api, &route)
                       == EIGRP_RESULT_SUCCESS);
                assert(route.prefix.address.afi == EIGRP_ADDRESS_FAMILY_IPV6);
                assert(route.source.route_instance == 124);
                assert(route.ifindex == 18);
                assert(route.gateway_present);
                assert(route.gateway.afi == EIGRP_ADDRESS_FAMILY_IPV6);
                assert(inet_pton(AF_INET6, "fe80::1", &v6) == 1);
                assert(memcmp(route.gateway.bytes, &v6, sizeof(v6)) == 0);

                api.safi = 99;
                assert(eigrp_zebra_source_route_import(&api, &route)
                       == EIGRP_RESULT_INVALID_ARGUMENT);
                api.safi = SAFI_UNICAST;
                api.type = ZEBRA_ROUTE_MAX;
                assert(eigrp_zebra_source_route_import(&api, &route)
                       == EIGRP_RESULT_UNSUPPORTED);
                return 0;
            }
            """
        )
    )

    compiler = os.environ.get("CC", "cc")
    subprocess.run(
        [compiler, "-std=c11", "-O0", str(source), "-o", str(binary)],
        check=True,
        cwd=ROOT,
    )
    subprocess.run([str(binary)], check=True, cwd=ROOT)
