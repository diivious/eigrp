from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_portable_public_headers_do_not_expose_frr_runtime_objects():
    headers = sorted((ROOT / "eigrpd").glob("*.h"))
    forbidden = [
        r"\bstruct\s+interface\b",
        r"\bstruct\s+vrf\b",
        r"\bstruct\s+event\b",
        r"\bstruct\s+event_loop\b",
        r"\bvrf_id_t\b",
    ]

    for header in headers:
        text = header.read_text()
        for pattern in forbidden:
            assert not re.search(pattern, text), f"{header.relative_to(ROOT)} exposes {pattern}"


def test_runtime_interface_state_is_eigrp_owned():
    types = read("eigrpd/eigrp_types.h")
    structs = read("eigrpd/eigrp_structs.h")
    interface_h = read("eigrpd/eigrp_interface.h")

    assert "typedef uint32_t eigrp_vrf_id_t;" in types
    assert "typedef uint32_t eigrp_ifindex_t;" in types
    assert "typedef struct eigrp_event eigrp_event_t;" in types
    assert "eigrp_vrf_id_t vrf_id;" in structs
    assert "eigrp_event_t *t_write;" in structs
    assert "eigrp_event_t *t_hello;" in structs
    assert "char *name;" in structs
    assert "eigrp_ifindex_t ifindex;" in structs
    assert "eigrp_prefix_t address;" in structs
    assert "struct interface *ifp" not in structs
    assert "struct list *iflist;" not in read("eigrpd/eigrpd.h")

    assert "typedef struct eigrp_interface_runtime_state" in types
    assert "bool secondary;" in types
    assert "eigrp_interface_runtime_create(" in interface_h
    assert "eigrp_interface_runtime_update(" in interface_h
    assert "eigrp_interface_runtime_delete(" in interface_h


def test_protocol_runtime_uses_eigrp_southbound_event_contracts():
    southbound_h = read("eigrpd/eigrp_southbound.h")
    protocol_files = [
        "eigrpd/eigrpd.c",
        "eigrpd/eigrp_interface.c",
        "eigrpd/eigrp_packet.c",
        "eigrpd/eigrp_hello.c",
        "eigrpd/eigrp_neighbor.c",
        "eigrpd/eigrp_update.c",
        "eigrpd/eigrp_filter.c",
        "eigrpd/eigrp_debug.c",
    ]

    assert "eigrp_southbound_event_add" in southbound_h
    assert "eigrp_southbound_timer_add" in southbound_h
    assert "eigrp_southbound_timer_msec_add" in southbound_h
    assert "eigrp_southbound_read_add" in southbound_h
    assert "eigrp_southbound_write_add" in southbound_h
    assert "eigrp_southbound_event_cancel" in southbound_h

    host_event_calls = [
        r"\bevent_add_(?:event|timer|timer_msec|read|write)\s*\(",
        r"\bevent_cancel\s*\(",
        r"\bevent_execute\s*\(",
        r"\bEVENT_ARG\s*\(",
    ]
    for source in protocol_files:
        text = read(source)
        for pattern in host_event_calls:
            assert not re.search(pattern, text), f"{source} calls FRR event API"


def test_interface_discovery_and_socket_host_objects_are_owned_by_frr_adapter():
    southbound = read("frr/eigrp_southbound.c")
    interface_c = read("eigrpd/eigrp_interface.c")
    ipv4 = read("eigrpd/eigrp_ipv4.c")
    network = read("eigrpd/eigrp_network.c")

    assert "struct eigrp_event" in southbound
    assert "struct interface" in southbound
    assert "struct vrf" in southbound
    assert "FOR_ALL_INTERFACES" in southbound
    assert "vrf_lookup_by_id" in southbound
    assert "hook_register_prio(if_real" in southbound
    assert "eigrp_southbound_socket_open" in southbound
    assert "eigrp_southbound_socket_send_buffer_ensure" in southbound
    assert "setsockopt_so_sendbuf" in southbound
    assert "eigrp_southbound_multicast_join" in southbound

    for text in (interface_c, ipv4, network):
        assert "struct interface" not in text
        assert "if_lookup_by_" not in text
        assert "ifindex2ifname" not in text
        assert "FOR_ALL_INTERFACES" not in text

    # FRR's distribute context may remain until the policy/filter boundary,
    # but portable code must not dereference its host VRF object.
    assert "ctx->vrf" not in read("eigrpd/eigrp_filter.c")

    assert "eigrp_sock_init(struct vrf" not in network
    assert "eigrp_intf_ipmulticast" not in network
    assert "eigrp_intf_add_allspfrouters" not in network
    assert "eigrp_intf_drop_allspfrouters" not in network
    assert "eigrp_adjust_sndbuflen" not in network
    assert "setsockopt_so_sendbuf" not in network
    assert "getsockopt_so_sendbuf" not in network


def test_frr_management_and_zebra_no_longer_store_runtime_in_ifp_info():
    northbound = read("frr/eigrp_northbound.c")
    zebra = read("frr/eigrp_zebra.c")
    vty = read("frr/eigrp_vty.c")

    assert "ifp->info" not in northbound
    assert "ifp->info" not in zebra
    assert "->ifp" not in vty
    assert "eigrp_interface_lookup_host(ifp)" in northbound
    assert "eigrp_network_interface_refresh((eigrp_vrf_id_t)vrf_id, &state)" in zebra
    assert "ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp" not in zebra


def test_frr_southbound_has_explicit_host_header_dependencies():
    southbound = read("frr/eigrp_southbound.c")
    southbound_h = read("eigrpd/eigrp_southbound.h")

    # FRR route-table objects are intentionally confined to the FRR adapter,
    # but the adapter must include the FRR header that defines route_node and
    # route_* traversal/lookup APIs instead of relying on transitive includes.
    if "struct route_node" in southbound or "route_node_" in southbound or "route_top(" in southbound:
        assert '#include "table.h"' in southbound

    # The portable contract currently uses the system IPv4 scalar wrapper for
    # router-id handoff.  Declare that dependency directly so callers that do
    # not include zebra.h first still see a complete declaration.
    if "struct in_addr" in southbound_h:
        assert "#include <netinet/in.h>" in southbound_h


def test_route_table_users_include_the_host_storage_header_directly():
    """Keep the compile-smoke harness honest about FRR route-table ownership.

    Route-table storage is intentionally outside this boundary increment, but
    source files that still use it must include its defining header directly.
    FRR's zebra.h does not provide route_node/route_* definitions.
    """
    portable_sources = sorted((ROOT / "eigrpd").glob("*.c"))
    route_table_tokens = (
        "struct route_node",
        "route_table_init(",
        "route_table_finish(",
        "route_node_get(",
        "route_node_lookup(",
        "route_node_match(",
        "route_top(",
        "route_next(",
        "route_unlock_node(",
    )

    for source in portable_sources:
        text = source.read_text()
        if any(token in text for token in route_table_tokens):
            assert ('#include "table.h"' in text
                    or '#include "lib/table.h"' in text), (
                f"{source.relative_to(ROOT)} uses FRR route-table storage "
                "without including table.h directly"
            )


def test_portable_runtime_does_not_depend_on_frr_qobj_registration():
    """QOBJ is FRR object-registration machinery, not EIGRP runtime state."""
    forbidden = (
        "QOBJ_FIELDS",
        "DECLARE_QOBJ_TYPE",
        "DEFINE_QOBJ_TYPE",
        "QOBJ_REG",
        "QOBJ_UNREG",
    )

    for path in sorted((ROOT / "eigrpd").glob("*.[ch]")):
        text = path.read_text()
        for token in forbidden:
            assert token not in text, (
                f"{path.relative_to(ROOT)} depends on FRR qobj runtime state: {token}"
            )

    # Do not let the standalone harness hide this dependency again.
    zebra_stub = read("test/build/include/zebra.h")
    for token in forbidden:
        assert token not in zebra_stub
