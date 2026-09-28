from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def read(path):
    return (ROOT / path).read_text()


def test_feature_contract_defaults_to_real_symbol_and_disabled_to_false():
    header = read("eigrpd/code/eigrp_features.h")
    assert "#ifdef EIGRP_DISABLE_IPV4" in header
    assert "#define eigrp_ipv4_supported() false" in header
    assert "bool eigrp_ipv4_supported(void);" in header
    assert "#ifdef EIGRP_DISABLE_IPV6" in header
    assert "#define eigrp_ipv6_supported() false" in header
    assert "bool eigrp_ipv6_supported(void);" in header


def test_enabled_address_families_supply_real_support_symbols():
    assert "bool eigrp_ipv4_supported(void)" in read("eigrpd/code/eigrp_ipv4.c")
    assert "bool eigrp_ipv6_supported(void)" in read("eigrpd/code/eigrp_ipv6.c")


def test_common_af_create_rejects_disabled_family():
    source = read("eigrpd/code/eigrp_instance.c")
    assert "if (!eigrp_afi_supported(afi))" in source
    assert "return EIGRP_RESULT_UNSUPPORTED;" in source


def test_named_cli_installs_only_supported_address_families():
    source = read("frr/code/eigrp_cli_named.c")
    assert "if (eigrp_afi_supported(EIGRP_AFI_IPV4))" in source
    assert "if (eigrp_afi_supported(EIGRP_AFI_IPV6))" in source


def test_tech_support_reads_common_capability_snapshot():
    status = read("eigrpd/code/eigrp_status.c")
    cli = read("frr/code/eigrp_cli_named.c")
    assert "eigrp_status_capability_state_read" in status
    assert "eigrp_feature_supported(EIGRP_FEATURE_AFI_IPV4)" in status
    assert "eigrp_feature_supported(EIGRP_FEATURE_AFI_IPV6)" in status
    assert "eigrp_status_capability_state_read(&capabilities)" in cli
    assert '"  ipv4-af              : %s\\n"' in cli
    assert '"  ipv6-af              : %s\\n"' in cli
