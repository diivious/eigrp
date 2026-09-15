# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for EIGRP named-mode CLI direction.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
CLI = ROOT / "frr" / "eigrp_cli_named.c"
CLASSIC_CLI = ROOT / "frr" / "eigrp_cli_classic.c"
SPEC = ROOT / "specs" / "cli-spec.md"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    start = source.index(f"{name}(void)")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def test_named_router_command_is_installed():
    cli = read(CLI)
    init = function_body(cli, "eigrp_cli_named_init")

    assert "install_element(CONFIG_NODE, &router_eigrp_named_cmd);" in init
    assert "install_element(CONFIG_NODE, &no_router_eigrp_named_cmd);" in init
    assert '"router eigrp WORD"' in cli
    # vtysh has a patched combined numeric/WORD entry command.  The daemon
    # named entry must be NOSH or vtysh sees two overlapping WORD grammars
    # and reports `router eigrp savage` as ambiguous.
    assert "DEFUN_NOSH(router_eigrp_named," in cli


def test_named_address_family_commands_are_installed():
    cli = read(CLI)
    init = function_body(cli, "eigrp_cli_named_init")

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
    init = function_body(read(CLI), "eigrp_cli_named_init")

    named_commands = {
        "eigrp_exit_af_interface_cmd",
        "eigrp_af_interface_hello_interval_cmd",
        "eigrp_af_interface_hold_time_cmd",
        "eigrp_af_interface_bandwidth_percent_cmd",
        "eigrp_af_interface_bandwidth_cmd",
        "no_eigrp_af_interface_bandwidth_cmd",
        "eigrp_af_interface_delay_cmd",
        "no_eigrp_af_interface_delay_cmd",
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
        "bandwidth (1-10000000)",
        "bandwidth-percent",
        "delay (1-16777215)",
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
    assert "eigrp_cli_classic.[c|h]" in spec
    assert "eigrp_cli_named.[c|h]" in spec
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


def test_frr_driver_keeps_patch_application_out_of_normal_build_but_uut_syncs_integration():
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
    uut_end = driver.index("\t\t;;", uut_start)
    uut = driver[uut_start:uut_end]
    assert "install_eigrp" in uut
    assert "stage_eigrp" not in uut
    assert "patch_frr" not in uut


def test_frr_installer_prefers_already_applied_patch_state_and_leaves_generated_files_to_build():
    installer = read(ROOT / "tools" / "frr-install.sh")
    patch_state = installer[installer.index("patch_state() {"):installer.index("\ninstall_patch_file() {", installer.index("patch_state() {"))]

    assert patch_state.index("apply --reverse --check") < patch_state.index("apply --check")
    assert "--exclude '*_clippy.c'" in installer
    assert "invalidate_eigrp_yang_embed" in installer
    assert 'rm -f "$yang_embed"' in installer
    assert "refresh_eigrp_yang_embed" not in installer
    assert 'python3 "$embed_tool"' not in installer
    assert "repair: remove one duplicate managed EIGRP named YANG schema block" in installer


def test_named_af_children_use_semantic_core_targets_and_writeback():
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    cli = read(CLI)
    modules = {
        "eigrp_instance_router_id_update": read(ROOT / "eigrpd" / "eigrp_instance.h"),
        "eigrp_instance_router_id_delete": read(ROOT / "eigrpd" / "eigrp_instance.h"),
        "eigrp_network_create": read(ROOT / "eigrpd" / "eigrp_network.h"),
        "eigrp_network_delete": read(ROOT / "eigrpd" / "eigrp_network.h"),
        "eigrp_neighbor_static_create": read(ROOT / "eigrpd" / "eigrp_neighbor.h"),
        "eigrp_neighbor_static_delete": read(ROOT / "eigrpd" / "eigrp_neighbor.h"),
        "eigrp_instance_address_family_shutdown_update": read(
            ROOT / "eigrpd" / "eigrp_instance.h"
        ),
    }

    for xpath in (
        "/frr-eigrpd:eigrpd/named/address-family/router-id",
        "/frr-eigrpd:eigrpd/named/address-family/network",
        "/frr-eigrpd:eigrpd/named/address-family/neighbor",
        "/frr-eigrpd:eigrpd/named/address-family/shutdown",
    ):
        assert xpath in nb

    for target, header in modules.items():
        assert target in nb
        assert target in header

    assert not (ROOT / "eigrpd" / "eigrp_named.h").exists()
    assert not (ROOT / "eigrpd" / "eigrp_named.c").exists()
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


def test_named_af_interface_schema_and_semantic_targets_are_real():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-af-interface.patch")
    cli = read(CLI)
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    interface = read(ROOT / "eigrpd" / "eigrp_interface.h")
    auth = read(ROOT / "eigrpd" / "eigrp_auth.h")
    summary = read(ROOT / "eigrpd" / "eigrp_summary.h")

    assert 'list af-interface {' in patch
    assert 'key "interface";' in patch
    assert 'range "1..10000000";' in patch
    assert 'range "1..16777215";' in patch
    for leaf in (
        "bandwidth-percent",
        "bandwidth",
        "delay",
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

    targets = {
        "eigrp_interface_config_create": interface,
        "eigrp_interface_config_delete": interface,
        "eigrp_interface_bandwidth_percent_update": interface,
        "eigrp_interface_bandwidth_percent_delete": interface,
        "eigrp_interface_bandwidth_set": interface,
        "eigrp_interface_bandwidth_reset": interface,
        "eigrp_interface_delay_set": interface,
        "eigrp_interface_delay_reset": interface,
        "eigrp_interface_hello_interval_update": interface,
        "eigrp_interface_hello_interval_delete": interface,
        "eigrp_interface_hold_time_update": interface,
        "eigrp_interface_hold_time_delete": interface,
        "eigrp_interface_passive_update": interface,
        "eigrp_auth_mode_update": auth,
        "eigrp_auth_mode_delete": auth,
        "eigrp_auth_keychain_update": auth,
        "eigrp_auth_keychain_delete": auth,
        "eigrp_interface_next_hop_self_update": interface,
        "eigrp_interface_split_horizon_update": interface,
        "eigrp_summary_create": summary,
        "eigrp_summary_delete": summary,
        "eigrp_interface_shutdown_update": interface,
    }
    for target, header in targets.items():
        assert target in nb
        assert target in header

    assert 'eigrp_cli_not_configured(vty, "af-interface default")' not in cli
    assert 'eigrp_cli_not_configured(vty, "bandwidth-percent")' not in cli
    assert '"bandwidth (1-10000000)"' in cli
    assert '"no bandwidth [(1-10000000)]"' in cli
    assert '"delay (1-16777215)"' in cli
    assert '"no delay [(1-16777215)]"' in cli
    assert '"eigrp bandwidth (1-10000000)"' not in cli
    assert '"no eigrp bandwidth [(1-10000000)]"' not in cli
    assert '"no af-interface <default|IFNAME>"' in cli
    assert '"no next-hop-self"' in cli
    assert '"summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]"' in cli


def test_frr_patch_series_orders_af_interface_after_named_af_config():
    series = [
        line.strip()
        for line in read(ROOT / "frr" / "patch" / "series").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert series.index("eigrp-named-af-config.patch") < series.index(
        "eigrp-named-af-interface.patch"
    )


def test_named_mode_has_no_generic_not_implemented_dispatcher_or_core_named_api():
    cli = read(CLI)
    header = read(ROOT / "frr" / "eigrp_cli_named.h")
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    adapter = cli + nb

    assert "eigrp_cli_not_configured" not in cli
    assert "eigrp_cli_not_configured" not in header
    assert "eigrp_distance_stub" not in cli
    assert "eigrp_offset_list_stub" not in cli
    assert "eigrp_summary_metric_stub" not in cli

    targets = {
        "eigrp_instance_distance_update": ROOT / "eigrpd" / "eigrp_instance.c",
        "eigrp_offset_update": ROOT / "eigrpd" / "eigrp_filter.c",
        "eigrp_summary_metric_update": ROOT / "eigrpd" / "eigrp_summary.c",
        "eigrp_instance_parent_shutdown_update": ROOT / "eigrpd" / "eigrp_instance.c",
    }
    for target, source in targets.items():
        assert target in adapter
        assert f"{target}(" in read(source)

    assert not (ROOT / "eigrpd" / "eigrp_named.c").exists()
    assert not (ROOT / "eigrpd" / "eigrp_named.h").exists()
    portable = "\n".join(
        path.read_text() for path in (ROOT / "eigrpd").glob("*.c")
    )
    portable += "\n" + "\n".join(
        path.read_text() for path in (ROOT / "eigrpd").glob("*.h")
    )
    assert "eigrp_named_" not in portable



def test_named_topology_schema_and_stage1_commands_are_retained():
    patch = read(ROOT / "frr" / "patch" / "eigrp-named-topology.patch")
    cli = read(CLI)
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    init = function_body(cli, "eigrp_cli_named_init")
    classic_init = function_body(read(CLASSIC_CLI), "eigrp_cli_classic_init")
    all_init = init + classic_init

    assert 'container topology {' in patch
    assert 'presence "Configure the EIGRP base topology";' in patch
    for node in (
        "auto-summary",
        "default-information-in",
        "default-information-out",
        "default-metric",
        "distance",
        "maximum-prefix",
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
        assert f"install_element(EIGRP_NODE, &{cmd});" in all_init

    # Identical classic/named grammars are installed once by the classic parser
    # object and dispatch named semantics through helpers in eigrp_cli_named.c.
    for helper in (
        "eigrp_cli_named_active_time_apply",
        "eigrp_cli_named_variance_apply",
        "eigrp_cli_named_metric_weights_apply",
        "eigrp_cli_named_redistribute_apply",
    ):
        assert helper in read(CLASSIC_CLI)
        assert helper in cli

    # Named topology mode must be a real YANG context, not a second push of
    # the address-family XPath.  Relative command changes then land under it.
    assert 'xpath_len - strlen(xpath), "/topology"' in cli
    assert 'VTY_PUSH_XPATH(EIGRP_NODE, xpath);' in cli
    assert 'nb_cli_enqueue_change(vty, "./auto-summary", NB_OP_CREATE, NULL);' in cli
    auto_summary_start = nb.index("static int eigrpd_named_auto_summary_create")
    auto_summary_end = nb.index("static int eigrpd_named_auto_summary_destroy", auto_summary_start)
    auto_summary_create = nb[auto_summary_start:auto_summary_end]
    assert "eigrpd_named_topology_child_context(args->dnode" in auto_summary_create
    assert "eigrpd_named_child_context(args->dnode" not in auto_summary_create
    assert 'nb_cli_enqueue_change(vty, "./default-metric", NB_OP_CREATE, NULL);' in cli
    assert 'nb_cli_enqueue_change(vty, "./distance", NB_OP_CREATE, NULL);' in cli
    # metric weights is an address-family command.  TOS is the first
    # operand, K1-K5 are required, and RFC 7868 K6 is the optional tail.
    assert '"metric weights (0-255)$tos (0-255)$k1 (0-255)$k2 (0-255)$k3 (0-255)$k4 (0-255)$k5 [(0-255)$k6]"' in read(CLASSIC_CLI)
    metric_apply = cli[cli.index("int eigrp_cli_named_metric_weights_apply"):cli.index("int eigrp_cli_named_redistribute_apply")]
    assert 'if (!eigrp_cli_named_af_required(vty))' in metric_apply
    assert 'if (!tos || strcmp(tos, "0") != 0)' in cli
    assert '"./metric-weights/K6"' in cli
    assert '/frr-eigrpd:eigrpd/named/address-family/metric-weights/K6' in nb
    assert '/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights' not in nb

    assert 'eigrp_cli_prefix_limit_set(vty, argc, argv, "maximum-prefix",' in cli
    assert '"./maximum-prefix", true, false' in cli
    assert '"./offset-list[access-list=' in cli
    assert '"./redistribute[protocol=' in cli
    assert '"./summary-metric[address=' in cli
    assert '"./traffic-share-balanced", NB_OP_MODIFY' in cli
    assert 'nb_cli_enqueue_change(vty, "./metric-weights", NB_OP_CREATE, NULL);' in cli
    assert 'snprintf(child, sizeof(child), "%s/metrics", xpath);' in cli


def test_named_topology_uses_generic_portable_lifecycle_api():
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    topology_c = read(ROOT / "eigrpd" / "eigrp_topology.c")
    topology_h = read(ROOT / "eigrpd" / "eigrp_topology.h")
    eigrpd_c = read(ROOT / "eigrpd" / "eigrpd.c")

    assert "eigrp_topology_create(&context)" in nb
    assert "eigrp_topology_delete(&context)" in nb
    assert "context->topology_id = EIGRP_TOPOLOGY_ID_BASE;" in nb

    portable = topology_c + topology_h
    assert "eigrp_topology_base_create" not in portable
    assert "eigrp_topology_base_delete" not in portable
    assert "eigrp_topology_new" not in portable
    assert "eigrp_topology_free" not in portable

    # The legacy new/free helpers actually own route-table storage.  Keep
    # that implementation detail named as storage and reserve the generic
    # create/delete API for the EIGRP topology object/configuration target.
    assert "eigrp_topology_table_create()" in eigrpd_c
    assert "eigrp_topology_table_delete(eigrp, eigrp->topology_table)" in eigrpd_c
    assert "eigrp->networks = route_table_init();" in eigrpd_c


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

    for xpath in (
        "metric-weights/tos",
        "metric-weights/K1",
        "metric-weights/K2",
        "metric-weights/K3",
        "metric-weights/K4",
        "metric-weights/K5",
        "metric-weights/K6",
    ):
        assert f'/address-family/{xpath}"' in nb
        assert f'/topology/{xpath}"' not in nb

    k6_start = nb.index('/frr-eigrpd:eigrpd/named/address-family/metric-weights/K6"')
    k6_block = nb[k6_start : nb.index("\n\t\t},", k6_start) + 6]
    assert ".modify = eigrpd_named_metric_weights_modify" in k6_block
    assert ".destroy = eigrpd_named_metric_weights_K6_destroy" in k6_block

    # These boolean leaves have YANG defaults, so FRR permits modify but not
    # destroy callbacks for the leaf itself.
    assert "eigrpd_named_af_interface_next_hop_destroy" not in nb
    assert "eigrpd_named_af_interface_split_horizon_destroy" not in nb


def test_final_yang_patch_and_patch_detection_are_semantic():
    series = [
        line.strip()
        for line in read(ROOT / "frr" / "patch" / "series").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert series[-1] == "frr-eigrp-yang.patch"
    assert "eigrp-grammar-placement.patch" not in series

    patch = read(ROOT / "frr" / "patch" / "frr-eigrp-yang.patch")
    assert "Address-family metric weights: TOS, K1 through K5, and optional RFC 7868 K6" in patch
    assert "+    container metric-weights {" in patch
    assert "-      container metric-weights {" in patch
    assert "+      leaf K6 { type uint8; }" in patch
    assert "+        leaf dampened { type empty; }" not in patch.split("container neighbor-maximum-prefix", 1)[0]

    installer = read(ROOT / "tools" / "frr-install.sh")
    assert "patch_semantically_applied()" in installer
    assert "eigrp-named-topology-callbacks.patch)" in installer
    assert "EIGRP_STEP1_TOPOLOGY_COMPOUND_MANDATORY" in installer
    assert "frr-eigrp-yang.patch)" in installer
    assert "EIGRP_STEP1_CONFIG_COMPLETE" in installer
    assert "eigrp-grammar-placement.patch)" not in installer
    assert "eigrp_grammar_schema_current" in installer


def test_frr_eigrp_yang_patch_and_cli_cover_classic_inherited_surface():
    patch = read(ROOT / "frr" / "patch" / "frr-eigrp-yang.patch")
    cli = read(CLI)
    cli_surface = cli + "\n" + read(CLASSIC_CLI)
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    spec = read(ROOT / "specs" / "cli-spec.md")
    uut = read(ROOT / "tools" / "frr-named-uut.sh")

    assert "EIGRP_STEP1_CONFIG_COMPLETE" in patch
    assert "Named mode inherits the complete classic EIGRP configuration feature set" in spec
    assert "Every supported configuration feature must implement its applicable `no` form" in spec

    command_pairs = (
        ("eigrp event-log-size (0-4294967295)", "no eigrp event-log-size"),
        ("eigrp log-neighbor-changes", "no eigrp log-neighbor-changes"),
        ("eigrp log-neighbor-warnings", "no eigrp log-neighbor-warnings"),
        ("metric maximum-hops (1-255)", "no metric maximum-hops"),
        ("metric holddown", "no metric holddown"),
        ("maximum-paths (1-32)", "no maximum-paths"),
        ("neighbor <A.B.C.D|X:X::X:X> description LINE", "no neighbor <A.B.C.D|X:X::X:X> description"),
        ("neighbor <A.B.C.D|X:X::X:X> maximum-prefix", "no neighbor <A.B.C.D|X:X::X:X> maximum-prefix"),
        ("neighbor maximum-prefix", "no neighbor maximum-prefix"),
        ("redistribute maximum-prefix", "no redistribute maximum-prefix"),
        ("distribute-list ACCESSLIST4_NAME", "no distribute-list"),
        ("summary-metric A.B.C.D A.B.C.D distance", "no summary-metric A.B.C.D A.B.C.D"),
    )
    for positive, negative in command_pairs:
        assert positive in cli_surface
        assert negative in cli_surface

    assert "/frr-eigrpd:eigrpd/instance/event-log-size" in nb
    assert "leaf event-log-size" in patch

    for xpath in (
        "neighbor-policy/description",
        "neighbor-policy/maximum-prefix",
        "neighbor-maximum-prefix",
        "log-neighbor-changes",
        "log-neighbor-warnings",
        "metric-weights/K6",
        "af-interface/authentication-encryption-type",
        "af-interface/authentication-password",
        "af-interface/summary-address/administrative-distance",
        "af-interface/summary-address/leak-map",
        "topology/default-information-in/access-list",
        "topology/default-information-out/access-list",
        "topology/maximum-prefix/maximum",
        "topology/maximum-paths",
        "topology/metric-maximum-hops",
        "topology/metric-holddown",
        "topology/event-log-size",
        "topology/distribute-list/in/access-list",
        "topology/redistribute/route-map",
        "topology/redistribute-maximum-prefix",
        "topology/summary-metric/distance",
    ):
        assert f"/frr-eigrpd:eigrpd/named/address-family/{xpath}" in nb

    targets = (
        "eigrp_eventlog_size_update",
        "eigrp_neighbor_description_update",
        "eigrp_neighbor_maximum_prefix_update",
        "eigrp_neighbor_maximum_prefix_all_update",
        "eigrp_neighbor_log_changes_update",
        "eigrp_neighbor_log_warnings_update",
        "eigrp_metric_maximum_hops_update",
        "eigrp_metric_holddown_update",
        "eigrp_topology_maximum_paths_update",
        "eigrp_distribute_list_update",
        "eigrp_redistribute_maximum_prefix_update",
    )
    for target in targets:
        assert target in nb

    # Stage 1 is the executable acceptance matrix for the added/expanded forms.
    for line in (
        "neighbor 10.0.0.1 description STEP1-PEER",
        "neighbor 10.0.0.1 maximum-prefix 100 80",
        "no eigrp log-neighbor-changes",
        "eigrp log-neighbor-warnings 30",
        "bandwidth 100000",
        "delay 100",
        "bandwidth 200000",
        "delay 200",
        "authentication mode hmac-sha-256 0 Step1Secret",
        "summary-address 10.44.0.0 255.255.0.0 5 leak-map STEP1-LEAK",
        "default-information out STEP1-OUT",
        "maximum-paths 8",
        "metric maximum-hops 200",
        "metric holddown",
        "eigrp event-log-size 1000",
        "distribute-list STEP1-ACL-IN in",
        "offset-list EIGRP-UUT in 100 $uut_if",
        "redistribute connected metric 10000 100 255 1 1500 route-map STEP1-RM",
        "redistribute maximum-prefix 300 70 dampened",
        "summary-metric 10.44.0.0 255.255.0.0 10000 100 255 1 1500 distance 20",
    ):
        assert line in uut



def test_named_cli_grammar_matches_cisco_documented_forms():
    cli = read(CLI)
    classic = read(CLASSIC_CLI)
    patch = read(ROOT / "frr" / "patch" / "frr-eigrp-yang.patch")

    assert '"neighbor <A.B.C.D|X:X::X:X> maximum-prefix (1-4294967295) [(1-100)] [warning-only]"' in cli
    assert "neighbor-policy/maximum-prefix/dampened" not in read(ROOT / "frr" / "eigrp_northbound.c")
    assert '"authentication mode <md5|hmac-sha-256 <0|7> WORD>"' in cli
    assert '"summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]"' in cli
    assert 'when "../administrative-distance";' in patch

    for grammar in (
        '"no neighbor <A.B.C.D|X:X::X:X> maximum-prefix"',
        '"no neighbor maximum-prefix"',
        '"no eigrp log-neighbor-warnings"',
        '"no bandwidth-percent"',
        '"no bandwidth [(1-10000000)]"',
        '"no delay [(1-16777215)]"',
        '"no hello-interval"',
        '"no hold-time"',
        '"no authentication mode"',
        '"no distance eigrp"',
        '"no maximum-prefix"',
        '"no metric maximum-hops"',
        '"no eigrp event-log-size"',
        '"no redistribute maximum-prefix"',
    ):
        assert grammar in cli

    assert '"no authentication key-chain WORD"' in cli
    uut = read(ROOT / "tools" / "frr-named-uut.sh")
    assert '"no authentication key-chain EIGRP-UUT-4453"' in uut
    assert '"no authentication key-chain EIGRP-UUT6-4453"' in uut
    assert '"no default-metric (1-4294967295) (0-4294967295) (0-255) (1-255) (1-65535)"' in cli
    assert '"no metric weights"' in classic
    assert '"no timers active-time"' in classic
    assert '"no variance"' in classic

    metric_c = read(ROOT / "eigrpd" / "eigrp_metric.c")
    metric_h = read(ROOT / "eigrpd" / "eigrp_metric.h")
    assert "uint8_t k6;" in metric_h
    assert "context->runtime->k_values[5] = weights->k6;" in metric_c
    assert "context->runtime->k_values[5] = EIGRP_K6_DEFAULT;" in metric_c
    assert 'vty_out(vty, " metric weights 0 %s %s %s %s %s",' in classic

    # These documented no-forms intentionally retain identifying arguments.
    assert '"no neighbor <A.B.C.D|X:X::X:X> description [LINE]"' in cli
    assert '"no default-information <in|out> [WORD]"' in cli
    assert '"no summary-address A.B.C.D A.B.C.D [(1-255) [leak-map WORD]]"' in cli

    # Bare reset forms must also have bare-form docstrings; FRR clippy treats
    # help text for removed arguments as an excessive-docstring error.
    assert '"no timers active-time",\n\tNO_STR\n\t"Adjust routing timers\\n"\n\t"Time limit for active state\\n")' in classic
    assert '"no variance",\n\tNO_STR\n\t"Control load balancing variance\\n")' in classic
    assert '"no authentication mode",\n      NO_STR\n      "Authentication subcommands\\n"\n      "Authentication mode\\n")' in cli
    assert '"no metric maximum-hops",\n      NO_STR "Modify EIGRP metric behavior\\n" "Maximum hop count\\n")' in cli


def test_step1_optional_and_empty_yang_nodes_have_required_frr_callbacks():
    """Mirror FRR's northbound callback contract for Step-1-added nodes.

    Optional scalar leaves require modify+destroy; empty leaves require
    create+destroy; presence/list parents require create+destroy.  This catches
    the class of error that otherwise appears only when eigrpd calls
    nb_validate_callbacks() at startup.
    """
    nb = read(ROOT / "frr" / "eigrp_northbound.c")

    required_snippets = (
        # Structural list parents.
        '.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy"',
        ".create = eigrpd_named_neighbor_policy_create",
        ".destroy = eigrpd_named_neighbor_policy_destroy",
        '.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distribute-list"',
        ".create = eigrpd_named_distribute_list_entry_create",
        ".destroy = eigrpd_named_distribute_list_entry_destroy",
        # Optional authentication/detail leaves.
        ".destroy = eigrpd_named_af_interface_authentication_detail_destroy",
        ".destroy = eigrpd_named_default_information_access_list_destroy",
        ".destroy = eigrpd_named_log_neighbor_warnings_interval_destroy",
        ".destroy = eigrpd_named_redistribute_route_map_destroy",
        ".destroy = eigrpd_named_summary_metric_detail_destroy",
        # K6 is the optional metric-weights scalar; deleting it restores
        # the RFC/default coefficient while preserving TOS/K1-K5.
        ".destroy = eigrpd_named_metric_weights_K6_destroy",
        # Prefix-limit scalar and empty-leaf callback families.
        ".destroy = eigrpd_named_neighbor_prefix_limit_detail_destroy",
        ".create = eigrpd_named_neighbor_prefix_limit_empty_create",
        ".destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy",
        ".create = eigrpd_named_neighbor_prefix_limit_all_empty_create",
        ".destroy = eigrpd_named_maximum_prefix_detail_destroy",
        ".create = eigrpd_named_maximum_prefix_empty_create",
        ".destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy",
        ".create = eigrpd_named_redistribute_maximum_prefix_empty_create",
    )
    for snippet in required_snippets:
        assert snippet in nb

    # Empty YANG leaves must not be registered as MODIFY callbacks.  FRR's
    # nb_validate_callbacks() treats that as unneeded and requires CREATE.
    for xpath in (
        "neighbor-policy/maximum-prefix/warning-only",
        "neighbor-maximum-prefix/warning-only",
        "neighbor-maximum-prefix/dampened",
        "topology/maximum-prefix/warning-only",
        "topology/maximum-prefix/dampened",
        "topology/redistribute-maximum-prefix/warning-only",
        "topology/redistribute-maximum-prefix/dampened",
    ):
        start = nb.index(f'/frr-eigrpd:eigrpd/named/address-family/{xpath}"')
        block = nb[start : nb.index("\n\t\t},", start) + 6]
        assert ".create =" in block
        assert ".destroy =" in block
        assert ".modify =" not in block


def test_classic_frr_cli_and_exec_surface_is_restored_without_named_mixing():
    classic = read(CLASSIC_CLI)
    classic_header = read(ROOT / "frr" / "eigrp_cli_classic.h")
    vty = read(ROOT / "frr" / "eigrp_vty.c")
    named = read(CLI)
    spec = read(ROOT / "specs" / "cli-spec.md")

    # Every command object installed by the original FRR classic CLI remains
    # installed after the split.
    classic_commands = (
        "router_eigrp_cmd",
        "no_router_eigrp_cmd",
        "eigrp_router_id_cmd",
        "no_eigrp_router_id_cmd",
        "eigrp_passive_interface_cmd",
        "eigrp_timers_active_cmd",
        "no_eigrp_timers_active_cmd",
        "eigrp_variance_cmd",
        "no_eigrp_variance_cmd",
        "eigrp_maximum_paths_cmd",
        "no_eigrp_maximum_paths_cmd",
        "eigrp_metric_weights_cmd",
        "no_eigrp_metric_weights_cmd",
        "eigrp_network_cmd",
        "eigrp_neighbor_cmd",
        "eigrp_distribute_list_cmd",
        "eigrp_distribute_list_prefix_cmd",
        "eigrp_no_distribute_list_cmd",
        "eigrp_no_distribute_list_prefix_cmd",
        "eigrp_redistribute_source_metric_cmd",
        "eigrp_if_delay_cmd",
        "no_eigrp_if_delay_cmd",
        "eigrp_if_bandwidth_cmd",
        "no_eigrp_if_bandwidth_cmd",
        "eigrp_if_ip_hellointerval_cmd",
        "no_eigrp_if_ip_hellointerval_cmd",
        "eigrp_if_ip_holdinterval_cmd",
        "no_eigrp_if_ip_holdinterval_cmd",
        "eigrp_ip_summary_address_cmd",
        "no_eigrp_ip_summary_address_cmd",
        "eigrp_authentication_mode_cmd",
        "no_eigrp_authentication_mode_cmd",
        "eigrp_authentication_keychain_cmd",
        "no_eigrp_authentication_keychain_cmd",
    )
    classic_init = function_body(classic, "eigrp_cli_classic_init")
    for command in classic_commands:
        assert f"&{command}" in classic_init

    assert "install_element(EIGRP_NODE, &eigrp_neighbor_cmd);" in classic
    assert '"router eigrp (1-65535)$as [vrf NAME]"' in classic

    # Original classic operational surface remains in eigrp_vty.c.
    for command in (
        "show_ip_eigrp_topology_all_cmd",
        "show_ip_eigrp_topology_cmd",
        "show_ip_eigrp_interfaces_cmd",
        "show_ip_eigrp_neighbors_cmd",
        "clear_ip_eigrp_neighbors_cmd",
        "clear_ip_eigrp_neighbors_int_cmd",
        "clear_ip_eigrp_neighbors_IP_cmd",
        "clear_ip_eigrp_neighbors_soft_cmd",
        "clear_ip_eigrp_neighbors_int_soft_cmd",
        "clear_ip_eigrp_neighbors_IP_soft_cmd",
    ):
        assert command in vty

    # Named syntax and operational commands live in the named adapter, not in
    # the restored classic source.
    assert '"router eigrp WORD"' in named
    assert "DEFPY(show_eigrp_neighbor," in named
    assert "DEFPY(clear_eigrp_neighbor," in named
    assert '"router eigrp WORD"' not in classic
    assert "DEFPY(show_eigrp_neighbor," not in vty

    assert "eigrp_cli_classic_init" in classic_header
    assert "eigrp_cli_named.[c|h]" in spec
    assert "eigrp_cli_classic.[c|h]" in spec


def test_named_bandwidth_delay_share_eigrp_runtime_processor_with_classic():
    nb = read(ROOT / "frr" / "eigrp_northbound.c")
    interface_c = read(ROOT / "eigrpd" / "eigrp_interface.c")
    interface_h = read(ROOT / "eigrpd" / "eigrp_interface.h")

    # Named mode resolves only host/runtime identity in the FRR adapter, then
    # hands an EIGRP-owned runtime interface to the portable target.
    assert "context->runtime =" in nb
    assert "eigrp_intf_lookup_by_name(runtime, interface_name)" in nb

    # The named targets own the runtime value change and enter the common
    # EIGRP reset processor instead of returning NOT_IMPLEMENTED.
    assert "context->runtime->params.bandwidth = bandwidth;" in interface_c
    assert "context->runtime->params.delay = delay;" in interface_c
    assert "context->runtime->params.bandwidth = EIGRP_BANDWIDTH_DEFAULT;" in interface_c
    assert "context->runtime->params.delay = EIGRP_DELAY_DEFAULT;" in interface_c
    assert interface_c.count("eigrp_interface_runtime_reset(context->runtime);") >= 4
    assert "void eigrp_interface_runtime_reset(eigrp_interface_t *ei);" in interface_h

    # Do not rewrite the existing FRR classic callbacks.  Their legacy reset
    # wrapper now funnels into the same EIGRP-owned runtime processor.
    assert "ei->params.delay = yang_dnode_get_uint32(args->dnode, NULL);" in nb
    assert "ei->params.bandwidth = yang_dnode_get_uint32(args->dnode, NULL);" in nb
    assert "eigrp_intf_reset(ifp);" in nb
    assert "eigrp_interface_runtime_reset(ifp->info);" in interface_c
