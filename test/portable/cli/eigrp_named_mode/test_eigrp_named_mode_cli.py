# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for EIGRP named-mode CLI direction.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
CLI = ROOT / "frr" / "eigrp_cli.c"
SPEC = ROOT / "specs" / "cli-spec.md"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    start = source.index(f"{name}(void)")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def test_named_router_command_is_installed():
    cli = read(CLI)
    init = function_body(cli, "eigrp_cli_init")

    assert "install_element(CONFIG_NODE, &router_eigrp_named_cmd);" in init
    assert "install_element(CONFIG_NODE, &no_router_eigrp_named_cmd);" in init
    assert '"router eigrp WORD"' in cli
    # vtysh has a patched combined numeric/WORD entry command.  The daemon
    # named entry must be NOSH or vtysh sees two overlapping WORD grammars
    # and reports `router eigrp savage` as ambiguous.
    assert "DEFUN_NOSH(router_eigrp_named," in cli


def test_named_address_family_commands_are_installed():
    cli = read(CLI)
    init = function_body(cli, "eigrp_cli_init")

    named_commands = {
        "eigrp_address_family_ipv4_cmd",
        "no_eigrp_address_family_ipv4_cmd",
        "eigrp_address_family_ipv6_cmd",
        "no_eigrp_address_family_ipv6_cmd",
        "eigrp_exit_address_family_cmd",
        "eigrp_af_interface_cmd",
        "no_eigrp_af_interface_cmd",
        "eigrp_topology_base_cmd",
        "eigrp_exit_af_topology_cmd",
    }

    for command in named_commands:
        assert f"install_element(EIGRP_NODE, &{command});" in init

    assert '"address-family ipv4 [unicast] [vrf NAME] autonomous-system (1-65535)"' in cli
    assert '"address-family ipv6 [unicast] [vrf NAME] autonomous-system (1-65535)"' in cli


def test_named_af_interface_commands_are_installed():
    init = function_body(read(CLI), "eigrp_cli_init")

    named_commands = {
        "eigrp_exit_af_interface_cmd",
        "eigrp_af_interface_hello_interval_cmd",
        "eigrp_af_interface_hold_time_cmd",
        "eigrp_af_interface_bandwidth_percent_cmd",
        "eigrp_af_interface_summary_address_cmd",
        "eigrp_af_interface_authentication_mode_cmd",
        "eigrp_af_interface_keychain_cmd",
        "eigrp_af_interface_passive_cmd",
        "eigrp_af_interface_next_hop_self_cmd",
        "no_eigrp_af_interface_next_hop_self_cmd",
        "eigrp_af_interface_split_horizon_cmd",
        "eigrp_no_shutdown_cmd",
    }

    for command in named_commands:
        assert f"install_element(EIGRP_NODE, &{command});" in init


def test_named_mode_feature_commands_are_present():
    cli = read(CLI)

    # These are named-mode command-surface guards only. Runtime implementation
    # status is tested separately and must not be encoded here as a CLI stub
    # requirement.
    for command_text in (
        "address-family ipv6",
        "bandwidth-percent",
        "summary-address",
        "split-horizon",
        "distance eigrp",
        "offset-list",
        "summary-metric",
        "redistribute",
    ):
        assert command_text in cli


def test_cli_spec_documents_named_mode_cli_direction():
    spec = read(SPEC)

    assert "Named-Mode CLI Direction" in spec
    assert "router eigrp <name>" in spec
    assert "address-family ipv4 unicast" in spec
    assert "address-family ipv6 unicast" in spec
    assert "eigrp_cli.[c|h]" in spec
    assert "eigrp_vty.[c|h]" in spec
    assert "eigrp_northbound.c" in spec


def test_named_mode_uses_real_yang_parent_and_address_family_paths():
    cli = read(CLI)
    assert "/frr-eigrpd:eigrpd/named[name='%s']" in cli
    assert "/address-family[afi='%s'][vrf='%s'][asn='%s']" in cli
    assert "eigrp-named[name=" not in cli
    assert 'eigrp_cli_not_configured(vty, "address-family ipv6")' not in cli


def test_named_mode_yang_patch_defines_parent_and_af_keys():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-yang.patch")
    assert 'list named {' in patch
    assert 'key "name";' in patch
    assert 'list address-family {' in patch
    assert 'key "afi vrf asn";' in patch


def test_named_af_config_yang_patch_defines_retained_af_children():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-af-config.patch")
    assert 'leaf router-id {' in patch
    assert 'leaf-list network {' in patch
    assert 'list neighbor {' in patch
    assert 'key "address interface";' in patch
    assert 'leaf shutdown {' in patch


def test_named_yang_patch_updates_authoritative_schema_only():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-yang.patch")

    assert "diff --git a/yang/frr-eigrpd.yang b/yang/frr-eigrpd.yang" in patch
    assert "diff --git a/yang/frr-eigrpd.yang.c b/yang/frr-eigrpd.yang.c" not in patch


def test_frr_driver_keeps_patch_application_out_of_build_and_uut():
    driver = read(ROOT / "tools" / "frr.sh")

    stage_start = driver.index("stage_eigrp() {")
    stage_end = driver.index("\n}\n", stage_start)
    stage = driver[stage_start:stage_end]
    assert "--no-patches" in stage

    assert "--patch)" in driver
    assert "set_action patch" in driver
    assert 'patch_frr() {' in driver
    assert '--no-eigrpd --no-tests' in driver

    uut_start = driver.index("\tuut)\n")
    uut_end = driver.index("\t	;;", uut_start)
    uut = driver[uut_start:uut_end]
    assert "stage_eigrp" in uut
    assert "install_eigrp" not in uut
    assert "patch_frr" not in uut


def test_frr_installer_prefers_already_applied_patch_state_and_regenerates_yang_embed():
    installer = read(ROOT / "tools" / "frr-install.sh")
    patch_state = installer[installer.index("patch_state() {"):installer.index("\ninstall_patch_file() {", installer.index("patch_state() {"))]

    assert patch_state.index("apply --reverse --check") < patch_state.index("apply --check")
    assert "refresh_eigrp_yang_embed" in installer
    assert 'python3 "$embed_tool" "$yang_source" "$yang_embed"' in installer
    assert "repair: remove one duplicate managed EIGRP named YANG schema block" in installer


def test_named_af_children_use_real_northbound_targets_and_writeback():
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    cli = read(CLI)
    named = read(ROOT / "eigrpd" / "eigrp_named.h")

    for xpath in (
        "/frr-eigrpd:eigrpd/named/address-family/router-id",
        "/frr-eigrpd:eigrpd/named/address-family/network",
        "/frr-eigrpd:eigrpd/named/address-family/neighbor",
        "/frr-eigrpd:eigrpd/named/address-family/shutdown",
    ):
        assert xpath in nb

    for target in (
        "eigrp_named_router_id_set",
        "eigrp_named_network_add",
        "eigrp_named_neighbor_add",
        "eigrp_named_address_family_shutdown_set",
    ):
        assert target in nb
        assert target in named

    assert '"neighbor A.B.C.D IFNAME"' in cli
    assert '"neighbor X:X::X:X IFNAME"' in cli
    assert 'nb_cli_enqueue_change(vty, "./shutdown", NB_OP_CREATE, NULL);' in cli


def test_frr_patch_series_orders_named_schema_before_af_children():
    series = [
        line.strip()
        for line in read(ROOT / "frr" / "patch" / "series").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert series.index("eigrp-named-yang.patch") < series.index("eigrp-named-af-config.patch")
    installer = read(ROOT / "tools" / "frr-install.sh")
    assert 'series_file="$frr_patch_src/series"' in installer


def test_named_af_interface_schema_and_targets_are_real():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-af-interface.patch")
    cli = read(CLI)
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    named = read(ROOT / "eigrpd" / "eigrp_named.h")

    assert 'list af-interface {' in patch
    assert 'key "interface";' in patch
    for leaf in (
        "bandwidth-percent",
        "hello-interval",
        "hold-time",
        "passive-interface",
        "authentication-mode",
        "authentication-key-chain",
        "next-hop-self",
        "split-horizon",
        "summary-address",
        "shutdown",
    ):
        assert leaf in patch
        assert f"/frr-eigrpd:eigrpd/named/address-family/af-interface/{leaf}" in nb

    for target in (
        "eigrp_named_af_interface_create",
        "eigrp_named_af_interface_bandwidth_percent_set",
        "eigrp_named_af_interface_hello_interval_set",
        "eigrp_named_af_interface_hold_time_set",
        "eigrp_named_af_interface_passive_set",
        "eigrp_named_af_interface_authentication_mode_set",
        "eigrp_named_af_interface_keychain_set",
        "eigrp_named_af_interface_next_hop_self_set",
        "eigrp_named_af_interface_split_horizon_set",
        "eigrp_named_af_interface_summary_add",
        "eigrp_named_af_interface_shutdown_set",
    ):
        assert target in nb
        assert target in named

    assert 'eigrp_cli_not_configured(vty, "af-interface default")' not in cli
    assert 'eigrp_cli_not_configured(vty, "bandwidth-percent")' not in cli
    assert '"no af-interface <default|IFNAME>"' in cli
    assert '"no next-hop-self"' in cli
    assert '"summary-address A.B.C.D A.B.C.D"' in cli


def test_frr_patch_series_orders_af_interface_after_named_af_config():
    series = [
        line.strip()
        for line in read(ROOT / "frr" / "patch" / "series").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert series.index("eigrp-named-af-config.patch") < series.index(
        "eigrp-named-af-interface.patch"
    )


def test_named_mode_has_no_generic_not_implemented_dispatcher():
    cli = read(CLI)
    header = read(ROOT / "frr" / "eigrp_cli.h")
    named = read(ROOT / "eigrpd" / "eigrp_named.c")
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    adapter = cli + nb

    assert "eigrp_cli_not_configured" not in cli
    assert "eigrp_cli_not_configured" not in header
    assert "eigrp_distance_stub" not in cli
    assert "eigrp_offset_list_stub" not in cli
    assert "eigrp_summary_metric_stub" not in cli

    for target in (
        "eigrp_named_distance_set",
        "eigrp_named_offset_list_set",
        "eigrp_named_summary_metric_set",
        "eigrp_named_process_shutdown_set",
    ):
        assert target in adapter
        assert f"{target}(" in named



def test_named_topology_schema_and_stage1_commands_are_retained():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-topology.patch")
    cli = read(CLI)
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    init = function_body(cli, "eigrp_cli_init")

    assert 'container topology {' in patch
    assert 'presence "Configure the EIGRP base topology";' in patch
    for node in (
        "auto-summary",
        "default-information-in",
        "default-information-out",
        "default-metric",
        "distance",
        "maximum-prefix",
        "metric-weights",
        "offset-list",
        "redistribute",
        "summary-metric",
        "active-time",
        "traffic-share-balanced",
        "variance",
    ):
        assert node in patch
        assert f"/frr-eigrpd:eigrpd/named/address-family/topology/{node}" in nb

    for cmd in (
        "eigrp_topology_base_cmd",
        "eigrp_auto_summary_cmd",
        "no_eigrp_auto_summary_cmd",
        "eigrp_default_information_cmd",
        "no_eigrp_default_information_cmd",
        "eigrp_default_metric_cmd",
        "no_eigrp_default_metric_cmd",
        "eigrp_distance_cmd",
        "no_eigrp_distance_cmd",
        "eigrp_maximum_prefix_cmd",
        "no_eigrp_maximum_prefix_cmd",
        "eigrp_metric_weights_cmd",
        "no_eigrp_metric_weights_cmd",
        "eigrp_offset_list_cmd",
        "no_eigrp_offset_list_cmd",
        "eigrp_redistribute_source_metric_cmd",
        "eigrp_summary_metric_cmd",
        "no_eigrp_summary_metric_cmd",
        "eigrp_timers_active_cmd",
        "no_eigrp_timers_active_cmd",
        "eigrp_traffic_share_balanced_cmd",
        "no_eigrp_traffic_share_balanced_cmd",
        "eigrp_variance_cmd",
        "no_eigrp_variance_cmd",
        "eigrp_exit_af_topology_cmd",
    ):
        assert f"install_element(EIGRP_NODE, &{cmd});" in init

    # Named topology mode must be a real YANG context, not a second push of
    # the address-family XPath.  Relative command changes then land under it.
    assert 'xpath_len - strlen(xpath), "/topology"' in cli
    assert 'VTY_PUSH_XPATH(EIGRP_NODE, xpath);' in cli
    assert 'nb_cli_enqueue_change(vty, "./auto-summary", NB_OP_CREATE, NULL);' in cli
    assert 'nb_cli_enqueue_change(vty, "./default-metric", NB_OP_CREATE, NULL);' in cli
    assert 'nb_cli_enqueue_change(vty, "./distance", NB_OP_CREATE, NULL);' in cli
    # The named form requires six numeric tokens: TOS plus K1-K5.  Do not
    # test the generated numeric k6 value for presence because K5=0 is valid
    # and is the normal default.  The generated *_str pointer is NULL only
    # when the optional sixth token was omitted.
    assert 'if (!k6_str || strcmp(k1_str, "0") != 0)' in cli

    assert 'nb_cli_enqueue_change(vty, "./maximum-prefix", NB_OP_MODIFY, maximum);' in cli
    assert '"./offset-list[access-list=' in cli
    assert '"./redistribute[protocol=' in cli
    assert '"./summary-metric[address=' in cli
    assert '"./traffic-share-balanced", NB_OP_MODIFY' in cli
    assert 'nb_cli_enqueue_change(vty, "./metric-weights", NB_OP_CREATE, NULL);' in cli
    assert 'snprintf(xpath_metric, sizeof(xpath_metric), "%s/metrics", xpath);' in cli


def test_frr_patch_series_orders_topology_after_named_af_interface():
    series = [
        line.strip()
        for line in read(ROOT / "frr" / "patch" / "series").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert series.index("eigrp-named-af-interface.patch") < series.index(
        "eigrp-named-topology.patch"
    )


def test_named_topology_callbacks_match_compound_yang_shape():
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-topology-callbacks.patch")

    # Compound CLI commands are represented by presence containers/lists whose
    # required value leaves are mandatory.  FRR therefore requires modify
    # callbacks for those leaves while the parent owns create/destroy.
    assert "EIGRP_STEP1_TOPOLOGY_COMPOUND_MANDATORY" in patch
    assert patch.count("mandatory true;") >= 24

    for xpath in (
        "default-metric/bandwidth",
        "default-metric/delay",
        "default-metric/reliability",
        "default-metric/load",
        "default-metric/mtu",
        "distance/internal",
        "distance/external",
        "metric-weights/tos",
        "metric-weights/K1",
        "metric-weights/K2",
        "metric-weights/K3",
        "metric-weights/K4",
        "metric-weights/K5",
        "offset-list/offset",
        "redistribute/metrics/bandwidth",
        "redistribute/metrics/delay",
        "redistribute/metrics/reliability",
        "redistribute/metrics/load",
        "redistribute/metrics/mtu",
        "summary-metric/bandwidth",
        "summary-metric/delay",
        "summary-metric/reliability",
        "summary-metric/load",
        "summary-metric/mtu",
    ):
        assert f'/topology/{xpath}"' in nb

    assert "/topology/redistribute/metrics\"" in nb
    assert ".create = eigrpd_named_redistribute_metrics_create" in nb
    assert ".destroy = eigrpd_named_redistribute_metrics_destroy" in nb

    # These boolean leaves have YANG defaults, so FRR permits modify but not
    # destroy callbacks for the leaf itself.
    assert "eigrpd_named_af_interface_next_hop_destroy" not in nb
    assert "eigrpd_named_af_interface_split_horizon_destroy" not in nb


def test_topology_callback_schema_patch_is_last_and_patch_detection_is_semantic():
    series = [
        line.strip()
        for line in read(ROOT / "frr" / "patch" / "series").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert series[-1] == "eigrp-named-topology-callbacks.patch"

    installer = read(ROOT / "tools" / "frr-install.sh")
    assert "patch_semantically_applied()" in installer
    assert "eigrp-named-topology-callbacks.patch)" in installer
    assert "EIGRP_STEP1_TOPOLOGY_COMPOUND_MANDATORY" in installer
