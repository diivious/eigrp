// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix host-shim declarations for standalone EIGRP development.
 *
 * This header is private to the Unix adapter. Portable EIGRP must not include
 * it; the adapter consumes only the public integration contract.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_UNIX_EIGRP_UNIX_H_
#define EIGRP_UNIX_EIGRP_UNIX_H_

#include "eigrp.h"

#define EIGRP_UNIX_DEFAULT_VRF_NAME "default"

/* Private host binding used by Unix socket/runtime code. The descriptor never
 * crosses the public EIGRP integration boundary. */
void eigrp_unix_runtime_fd_set(eigrp_instance_t *eigrp, int fd);
void eigrp_unix_runtime_fd_clear(eigrp_instance_t *eigrp);

/* Serialize host-side protocol mutations with runtime callbacks. */
void eigrp_unix_runtime_enter(void);
void eigrp_unix_runtime_leave(void);

#endif /* EIGRP_UNIX_EIGRP_UNIX_H_ */
