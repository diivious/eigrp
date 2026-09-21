// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable memory helpers.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrpd/eigrp_memory.h"

void eigrp_memory_wipe(void *buffer, size_t length)
{
	volatile unsigned char *cursor = buffer;

	if (!cursor)
		return;

	while (length-- != 0)
		*cursor++ = 0;
}
