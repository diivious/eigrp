// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenWrt-private protocol 88 socket and packet framing helpers.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_OWRT_PACKET_H_
#define EIGRP_OWRT_PACKET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "eigrp_sys.h"

#define EIGRP_OWRT_IP_PROTOCOL 88
#define EIGRP_OWRT_IPV4_ALL_ROUTERS 0xe000000aU /* 224.0.0.10, host order */

/* Strict, side-effect-free framing validation; also exercised by UUTs.
 * The IPv4 buffer includes its network header. IPv6 contains EIGRP only.
 * Lengths and address values are copied, not borrowed from the caller. */
bool eigrp_owrt_packet_ipv4_decode(const uint8_t *data, size_t size,
	uint32_t ingress_ifindex, eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta);
bool eigrp_owrt_packet_ipv6_decode(const uint8_t *data, size_t size,
	uint32_t ingress_ifindex, const uint8_t source_bytes[16],
	const uint8_t destination_bytes[16], eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta);

#endif /* EIGRP_OWRT_PACKET_H_ */
