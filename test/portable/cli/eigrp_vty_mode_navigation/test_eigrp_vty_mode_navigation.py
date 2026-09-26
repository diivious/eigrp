# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for EIGRP CLI hierarchy navigation and FRR integration.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
CLASSIC = ROOT / "frr" / "eigrp_cli_classic.c"
NAMED = ROOT / "frr" / "eigrp_cli_named.c"
VTYSH_PATCH = ROOT / "frr" / "patch" / "vtysh-named-eigrp.patch"
YANG_PATCH = ROOT / "frr" / "patch" / "frr-eigrp-yang.patch"
PATCH_SERIES = ROOT / "frr" / "patch" / "series"
INSTALLER = ROOT / "tools" / "frr-install.sh"
ZEBRA = ROOT / "frr" / "eigrp_zebra.c"


def read(path: Path) -> str:
    return path.read_text()


def test_router_entry_is_available_from_config_and_eigrp_modes():
    classic = read(CLASSIC)
    named = read(NAMED)
    vtysh = read(VTYSH_PATCH)

    assert "install_element(CONFIG_NODE, &router_eigrp_cmd);" in classic
    assert "install_element(EIGRP_NODE, &router_eigrp_cmd);" in classic
    assert "install_element(CONFIG_NODE, &router_eigrp_named_cmd);" in named
    assert "install_element(EIGRP_NODE, &router_eigrp_named_cmd);" in named
    assert "+\tinstall_element(EIGRP_NODE, &router_eigrp_cmd);" in vtysh


def test_router_callbacks_rewind_to_top_level_before_selecting_process():
    classic = read(CLASSIC)
    named = read(NAMED)

    assert "static void eigrp_cli_classic_config_rewind" in classic
    assert "eigrp_cli_classic_config_rewind(vty);" in classic
    assert "static void eigrp_cli_config_rewind" in named
    assert named.count("eigrp_cli_config_rewind(vty);") >= 2


def test_named_mode_entry_rewinds_only_to_existing_parent_context():
    named = read(NAMED)

    assert "static bool eigrp_cli_named_xpath_rewind" in named
    assert "static bool eigrp_cli_named_root_rewind" in named
    assert "static bool eigrp_cli_named_af_rewind" in named
    assert 'vty_out(vty, "%% Enter named EIGRP router mode first\\n");' in named

    # address-family moves to the existing named parent before entering a
    # sibling AF.  af-interface and topology move to the existing AF parent.
    assert named.count("eigrp_cli_named_root_rewind(vty, name, sizeof(name))") >= 5
    assert named.count("eigrp_cli_named_af_rewind(vty, name, sizeof(name), afi,") >= 5


def test_named_mode_checks_do_not_dereference_empty_xpath_stack():
    named = read(NAMED)

    assert "return vty && vty->xpath_index > 0" in named
    assert "if (!vty || vty->xpath_index <= 0" in named


def test_classic_multi_instance_constraint_is_removed_by_flattened_yang_patch():
    patch = read(YANG_PATCH)
    series = [
        line.strip()
        for line in read(PATCH_SERIES).splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    installer = read(INSTALLER)

    constraint = 'must "count(../instance[vrf =current()/vrf]) = 1";'
    assert f"-      {constraint}" in patch
    assert series == ["vtysh-named-eigrp.patch", "frr-eigrp-yang.patch"]
    assert constraint in installer
    assert "eigrp-multi-instance.patch" not in installer


def test_zebra_router_id_refresh_covers_all_instances_in_the_vrf():
    zebra = read(ZEBRA)
    start = zebra.index("static int eigrp_zebra_router_id_update")
    end = zebra.index("\n}\n", start) + 3
    body = zebra[start:end]

    assert "eigrp_sys_router_id_refresh((eigrp_vrf_id_t)vrf_id)" in body
    assert "ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp" not in body
    assert "eigrp_router_id_update(" not in body
    instance = read(ROOT / "eigrpd" / "eigrp_instance.c")
    refresh = instance[
        instance.index("void eigrp_sys_router_id_refresh"):
        instance.index("eigrp_result_t eigrp_instance_address_family_stop")
    ]
    assert "EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, runtime)" in refresh
    assert "runtime->vrf_id == vrf_id" in refresh
