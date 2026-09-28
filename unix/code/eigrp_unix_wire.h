// SPDX-License-Identifier: GPL-2.0-or-later
/* Unix virtual control-plane wire protocol. Copyright (C) 2026 Donnie V. Savage */
#ifndef EIGRP_UNIX_EIGRP_UNIX_WIRE_H_
#define EIGRP_UNIX_EIGRP_UNIX_WIRE_H_
#include <stdint.h>
#include "eigrp.h"
#define EIGRP_UNIX_WIRE_VERSION 1U
#define EIGRP_UNIX_WIRE_NAME_MAX 64U
#define EIGRP_UNIX_WIRE_PACKET_MAX 65535U
#define EIGRP_UNIX_WIRE_DEFAULT_SOCKET "/tmp/eigrp-wire.sock"
typedef enum eigrp_unix_wire_type { EIGRP_UNIX_WIRE_REGISTER=1, EIGRP_UNIX_WIRE_PACKET=2 } eigrp_unix_wire_type_t;
typedef struct eigrp_unix_wire_header {
 uint8_t version, type, afi, flags;
 uint32_t ifindex;
 uint16_t segment_length, interface_length, payload_length, reserved;
 uint8_t source[16], destination[16];
} eigrp_unix_wire_header_t;
#define EIGRP_UNIX_WIRE_FLAG_MULTICAST 0x01U
#endif
