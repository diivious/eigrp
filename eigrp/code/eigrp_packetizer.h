// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP packetizer work queue.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_PACKETIZER_H_
#define _ZEBRA_EIGRP_PACKETIZER_H_

#include "eigrp_types.h"

#define EIGRP_PACKETIZER_WORK_F_OWN_PREFIX 0x00000001U
#define EIGRP_PACKETIZER_WORK_F_OWN_ROUTE  0x00000002U
#define EIGRP_PACKETIZER_WORK_F_DEFER_FREE 0x00000004U
#define EIGRP_PACKETIZER_WORK_F_ROUTE_ACTIVE 0x00000008U
#define EIGRP_PACKETIZER_WORK_F_POISON       0x00000010U

struct eigrp_packetizer_work {
	uint8_t opcode;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_intf_t *exception;
	eigrp_nbr_t *nbr;
	void *owner;
	uint32_t flags;
	eigrp_packetizer_work_t *next;
};

void eigrp_packetizer_init(eigrp_instance_t *eigrp);
void eigrp_packetizer_delete(eigrp_instance_t *eigrp);

eigrp_packetizer_work_t *eigrp_packetizer_work_create(uint8_t opcode);
void eigrp_packetizer_work_free(eigrp_packetizer_work_t *work);
void eigrp_packetizer_work_process(eigrp_instance_t *eigrp,
				   eigrp_packetizer_work_t *work);
void eigrp_packetizer_enqueue(eigrp_instance_t *eigrp,
			      eigrp_packetizer_work_t *work);
void eigrp_packetizer_prefix_defer_free(eigrp_instance_t *eigrp,
				       eigrp_prefix_descriptor_t *prefix);

#endif /* _ZEBRA_EIGRP_PACKETIZER_H_ */
