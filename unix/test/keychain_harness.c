// SPDX-License-Identifier: GPL-2.0-or-later
/* Unix keychain provider regression test.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eigrp_sys.h"
#include "eigrp_unix_keychain.h"

int main(void)
{
    char key[82] = "unchanged";
    char small[3] = "zz";
    uint32_t id = 17;

    /* An unconfigured Unix host cannot authenticate by default. */
    assert(!eigrp_sys_auth_key_lookup("md5-lan", &id, key, sizeof(key)));
    assert(id == 17 && strcmp(key, "unchanged") == 0);
    assert(eigrp_unix_keychain_add("md5-lan", 1, "eigrprocks")
           == EIGRP_RESULT_SUCCESS);
    assert(eigrp_sys_auth_key_lookup("md5-lan", &id, key, sizeof(key)));
    assert(id == 1 && strcmp(key, "eigrprocks") == 0);

    /* Idempotency does not silently replace an existing secret or ID. */
    assert(eigrp_unix_keychain_add("md5-lan", 1, "eigrprocks")
           == EIGRP_RESULT_SUCCESS);
    assert(eigrp_unix_keychain_add("md5-lan", 1, "wrong-secret")
           == EIGRP_RESULT_INVALID_ARGUMENT);
    assert(eigrp_unix_keychain_add("md5-lan", 2, "eigrprocks")
           == EIGRP_RESULT_INVALID_ARGUMENT);
    assert(eigrp_unix_keychain_add("no-id", 0, "eigrprocks")
           == EIGRP_RESULT_INVALID_ARGUMENT);
    assert(eigrp_unix_keychain_add("empty", 1, "")
           == EIGRP_RESULT_INVALID_ARGUMENT);
    assert(eigrp_unix_keychain_add("", 1, "eigrprocks")
           == EIGRP_RESULT_INVALID_ARGUMENT);

    /* The destination must fit the complete nul-terminated secret. */
    assert(!eigrp_sys_auth_key_lookup("md5-lan", &id, small, sizeof(small)));
    assert(id == 1 && strcmp(small, "zz") == 0);
    assert(!eigrp_sys_auth_key_lookup("missing", &id, key, sizeof(key)));
    assert(eigrp_unix_keychain_add("second", 17, "second-secret")
           == EIGRP_RESULT_SUCCESS);
    assert(eigrp_sys_auth_key_lookup("second", &id, key, sizeof(key)));
    assert(id == 17 && strcmp(key, "second-secret") == 0);

    eigrp_unix_keychain_reset();
    assert(!eigrp_sys_auth_key_lookup("md5-lan", &id, key, sizeof(key)));
    assert(!eigrp_sys_auth_key_lookup("second", &id, key, sizeof(key)));
    assert(eigrp_unix_keychain_add("md5-lan", 8, "new-secret")
           == EIGRP_RESULT_SUCCESS);
    assert(eigrp_sys_auth_key_lookup("md5-lan", &id, key, sizeof(key)));
    assert(id == 8 && strcmp(key, "new-secret") == 0);
    eigrp_unix_keychain_reset();
    puts("unix keychain tests: PASS");
    return 0;
}
