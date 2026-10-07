from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())

def read(path):
    return (ROOT / path).read_text()

def test_ipv6_reliable_multicast_and_unicast_share_rtp():
    packet = read("eigrp/code/eigrp_packet.c")
    assert 'inet_pton(AF_INET6, "ff02::a", &packet->dst.ip.v6)' in packet
    assert "eigrp_addr_cpy(&duplicate->dst, &nbr->src);" in packet
    assert "eigrp_packet_retransmit_timer_start(nbr);" in packet
    assert "eigrp_nbr_holddown_update(nbr);" in packet

def test_ipv6_startup_eot_walk_encodes_routes_with_neighbor_codec():
    update = read("eigrp/code/eigrp_update.c")
    assert "eigrp_update_neighbor_packet_create(nbr, EIGRP_INIT_FLAG)" in update
    assert "for (rn = eigrp_table_first(eigrp->topology_table)" in update
    assert "nbr->encoder, packet->s, &wire_route" in update
    assert "eigrp_stream_set_getp(pkt, eigrp_stream_get_endp(pkt));" not in update

def test_ipv6_sequence_tlv_and_static_neighbor_identity_are_af_aware():
    hello = read("eigrp/code/eigrp_hello.c")
    neighbor = read("eigrp/code/eigrp_neighbor.c")
    assert "packet_address_encode(s, &nbr->src)" in hello
    assert "packet_address_bytes" in hello
    assert "src->afi == AF_INET6" in neighbor
    assert "dst.afi = AF_INET6;" in neighbor

def test_named_show_uses_bound_ipv6_runtime():
    cli = read("frr/code/eigrp_frr_named.c")
    runtime_lookup = cli[cli.index("eigrp_vty_named_runtime_lookup"):cli.index("typedef eigrp_result_t", cli.index("eigrp_vty_named_runtime_lookup"))]
    assert "af->afi != EIGRP_AFI_IPV4" not in runtime_lookup
    assert "return af->runtime;" in runtime_lookup
