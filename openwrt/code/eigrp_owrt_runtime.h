// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenWrt-private runtime integration. No OpenWrt types cross eigrp_sys.h.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_OWRT_RUNTIME_H_
#define EIGRP_OWRT_RUNTIME_H_

#include <stdbool.h>
#include "eigrp.h"

/* Packet adapter (OW-06) binds/clears the real descriptor for an instance.
 * Clearing or changing an fd invalidates outstanding readiness callbacks.
 * Descriptor ownership remains with the packet adapter. */
bool eigrp_owrt_runtime_fd_set(eigrp_instance_t *instance, int fd);
void eigrp_owrt_runtime_fd_clear(eigrp_instance_t *instance);

/* Host-side configuration/packet mutations must hold this lock if concurrent
 * with the runtime thread. Do not hold it while calling runtime_finish().
 * Lock ordering: protocol lock -> scheduler lock; never the reverse. */
void eigrp_owrt_runtime_enter(void);
void eigrp_owrt_runtime_leave(void);

#endif /* EIGRP_OWRT_RUNTIME_H_ */
