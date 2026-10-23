// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP VTY Interface.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 */

#ifndef _QUAGGA_EIGRP_VTY_H
#define _QUAGGA_EIGRP_VTY_H

#include "eigrp_neighbor.h"

/* The ordinary table shows active peers and retained penalty identities.
 * Use the same A/P/D eligibility as prefix-limit accounting: process-wide
 * suspension need not set the per-neighbor limit_state. */
static inline bool eigrp_vty_neighbor_visible(const eigrp_nbr_t *nbr,
						      bool detail)
{
	return nbr && (detail || eigrp_nbr_limit_status(nbr) != 0);
}

/* Prototypes. */
extern void eigrp_vty_init(void);
extern void eigrp_vty_show_init(void);

#endif /* _Quagga_EIGRP_VTY_H_ */
