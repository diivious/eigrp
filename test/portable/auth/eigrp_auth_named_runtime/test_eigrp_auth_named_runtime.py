# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for named authentication runtime.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
AUTH_C = ROOT / "eigrpd" / "eigrp_auth.c"
AUTH_H = ROOT / "eigrpd" / "eigrp_auth.h"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"
CONVENTIONS = ROOT / "specs" / "code-conventions.md"


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


def test_named_authentication_callbacks_terminate_at_common_eigrp_targets():
    northbound = read(NORTHBOUND)

    mode_apply = function_body(
        northbound, "eigrpd_named_af_interface_authentication_apply"
    )
    mode_destroy = function_body(
        northbound, "eigrpd_named_af_interface_authentication_destroy"
    )
    keychain_modify = function_body(
        northbound, "eigrpd_named_af_interface_keychain_modify"
    )
    keychain_destroy = function_body(
        northbound, "eigrpd_named_af_interface_keychain_destroy"
    )

    assert "eigrp_auth_mode_update(&context, mode, hmac_ptr)" in mode_apply
    assert "eigrp_auth_mode_delete(&context)" in mode_destroy
    assert "eigrp_auth_keychain_update(&context" in keychain_modify
    assert "eigrp_auth_keychain_delete(&context)" in keychain_destroy


def test_common_md5_authentication_target_updates_active_runtime_mode():
    auth = read(AUTH_C)
    update = function_body(auth, "eigrp_auth_mode_update")
    delete = function_body(auth, "eigrp_auth_mode_delete")

    assert "context->runtime->params.auth_type =" in update
    assert "EIGRP_AUTH_TYPE_MD5" in update
    assert "context->runtime->params.auth_type = EIGRP_AUTH_TYPE_NONE;" in delete


def test_common_keychain_target_updates_config_and_runtime_with_independent_ownership():
    auth = read(AUTH_C)
    update = function_body(auth, "eigrp_auth_keychain_update")
    delete = function_body(auth, "eigrp_auth_keychain_delete")

    assert "char *config_copy = NULL;" in update
    assert "char *runtime_copy = NULL;" in update
    assert "config_copy = eigrp_auth_string_duplicate(keychain);" in update
    assert "runtime_copy = eigrp_auth_string_duplicate(keychain);" in update
    assert "context->config->keychain = config_copy;" in update
    assert "context->runtime->params.auth_keychain = runtime_copy;" in update
    assert "if (context->runtime)\n\t\treturn EIGRP_RESULT_NOT_IMPLEMENTED;" not in update

    assert "free(context->config->keychain);" in delete
    assert "context->config->keychain = NULL;" in delete
    assert "free(context->runtime->params.auth_keychain);" in delete
    assert "context->runtime->params.auth_keychain = NULL;" in delete


def test_keychain_replacement_is_atomic_across_retained_and_runtime_state():
    auth = read(AUTH_C)
    update = function_body(auth, "eigrp_auth_keychain_update")

    config_allocate = update.index("config_copy = eigrp_auth_string_duplicate")
    runtime_allocate = update.index("runtime_copy = eigrp_auth_string_duplicate")
    config_replace = update.index("free(context->config->keychain)")
    runtime_replace = update.index("free(context->runtime->params.auth_keychain)")

    assert config_allocate < config_replace
    assert runtime_allocate < config_replace
    assert config_allocate < runtime_replace
    assert runtime_allocate < runtime_replace
    assert "free(config_copy);" in update


def test_named_hmac_direct_password_remains_explicitly_not_implemented_at_runtime():
    auth = read(AUTH_C)
    update = function_body(auth, "eigrp_auth_mode_update")

    assert "mode == EIGRP_AUTHENTICATION_HMAC_SHA256 && context->config" in update
    assert "return EIGRP_RESULT_NOT_IMPLEMENTED;" in update
    assert "EIGRP_AUTH_TYPE_SHA256" in update


def test_classic_authentication_converges_on_common_eigrp_targets():
    northbound = read(NORTHBOUND)

    classic_mode = function_body(
        northbound, "lib_interface_eigrp_instance_authentication_modify"
    )
    classic_keychain = function_body(
        northbound, "lib_interface_eigrp_instance_keychain_modify"
    )
    classic_keychain_destroy = function_body(
        northbound, "lib_interface_eigrp_instance_keychain_destroy"
    )

    assert "eigrp_auth_mode_update(&context, mode, NULL)" in classic_mode
    assert "eigrp_auth_mode_delete(&context)" in classic_mode
    assert "intf->params.auth_type =" not in classic_mode
    assert "eigrp_auth_keychain_update(" in classic_keychain
    assert "intf->params.auth_keychain =" not in classic_keychain
    assert "eigrp_auth_keychain_delete(&context)" in classic_keychain_destroy


def test_authentication_target_contract_is_documented():
    conventions = read(CONVENTIONS)
    auth_header = read(AUTH_H)

    assert "Classic and named configuration surfaces converge" in conventions
    assert "EIGRP-owned" in conventions
    assert "eigrp_auth_keychain_update" in auth_header
    assert "eigrp_auth_keychain_delete" in auth_header
