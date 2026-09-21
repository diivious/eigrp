// SPDX-License-Identifier: GPL-2.0-or-later
/* EIGRP Internet checksum. Copyright (C) 2026 Donnie V. Savage */
#ifndef _EIGRP_CHECKSUM_H_
#define _EIGRP_CHECKSUM_H_
#include <stddef.h>
#include <stdint.h>
uint16_t eigrp_checksum(const void *data, size_t length);
#endif
