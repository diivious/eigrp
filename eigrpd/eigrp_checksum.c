// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP Internet checksum. Copyright (C) 2026 Donnie V. Savage */
#include <stdint.h>
#include "eigrpd/eigrp_checksum.h"
uint16_t eigrp_checksum(const void *data, size_t length)
{
	const uint8_t *p = data;
	uint32_t sum = 0;
	while (length > 1) {
		sum += ((uint16_t)p[0] << 8) | p[1];
		p += 2;
		length -= 2;
	}
	if (length)
		sum += (uint16_t)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffffU) + (sum >> 16);
	return (uint16_t)~sum;
}
