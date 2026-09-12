// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP packetizer work queue.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_PACKETIZER_H_
#define _ZEBRA_EIGRP_PACKETIZER_H_

#include "eigrpd/eigrp_types.h"

#define EIGRP_PACKETIZER_WORK_F_OWN_PREFIX 0x00000001U
#define EIGRP_PACKETIZER_WORK_F_OWN_ROUTE  0x00000002U

typedef struct eigrp_packetizer_work {
	uint8_t opcode;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_interface_t *exception;
	eigrp_neighbor_t *nbr;
	void *owner;
	uint32_t flags;
} eigrp_packetizer_work_t;

void eigrp_packetizer_init(eigrp_instance_t *eigrp);
void eigrp_packetizer_finish(eigrp_instance_t *eigrp);

eigrp_packetizer_work_t *eigrp_packetizer_work_new(uint8_t opcode);
void eigrp_packetizer_work_free(eigrp_packetizer_work_t *work);
void eigrp_packetizer_enqueue(eigrp_instance_t *eigrp,
			      eigrp_packetizer_work_t *work);

#endif /* _ZEBRA_EIGRP_PACKETIZER_H_ */
