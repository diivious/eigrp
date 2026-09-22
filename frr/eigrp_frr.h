// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR datatype adaptation.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_FRR_H_
#define _ZEBRA_EIGRP_FRR_H_

#include "prefix.h"
#include "if.h"
#include "eigrpd/eigrp.h"
#include "eigrpd/eigrp_types.h"

/* FRR prefix objects stop at this adapter boundary. */
eigrp_result_t eigrp_frr_prefix_import(const struct prefix *host,
				       eigrp_prefix_t *prefix);
eigrp_result_t eigrp_frr_prefix_export(const eigrp_prefix_t *prefix,
				       struct prefix *host);
uint8_t eigrp_frr_interface_type(const struct interface *ifp);
eigrp_result_t eigrp_frr_interface_state_import(
	const struct interface *ifp, const struct prefix *address, bool secondary,
	eigrp_interface_runtime_state_t *state);

#endif /* _ZEBRA_EIGRP_FRR_H_ */
