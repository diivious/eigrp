// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable memory helpers.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_MEMORY_H_
#define _EIGRP_MEMORY_H_

#include <stddef.h>

void eigrp_memory_wipe(void *buffer, size_t length);

#endif /* _EIGRP_MEMORY_H_ */
