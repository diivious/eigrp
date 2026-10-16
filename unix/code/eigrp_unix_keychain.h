// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix-owned keychain provisioning for the standalone EIGRP adapter.
 *
 * Portable EIGRP only calls eigrp_sys_auth_key_lookup() from eigrp_sys.h;
 * this interface is private to Unix configuration and test orchestration.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_UNIX_KEYCHAIN_H_
#define EIGRP_UNIX_KEYCHAIN_H_

#include "eigrp.h"

/* Add a keychain. An identical registration is idempotent; an attempt to
 * change an existing entry is rejected until it is explicitly reset. */
eigrp_result_t eigrp_unix_keychain_add(const char *name, uint32_t key_id,
                                       const char *secret);

/* Release all Unix keychains and scrub their stored secrets. */
void eigrp_unix_keychain_reset(void);

#endif /* EIGRP_UNIX_KEYCHAIN_H_ */
