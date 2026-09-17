// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR datatype adaptation.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _ZEBRA_EIGRP_FRR_H_
#define _ZEBRA_EIGRP_FRR_H_

#include "prefix.h"
#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"

/* FRR prefix objects stop at this adapter boundary. */
eigrp_result_t eigrp_frr_prefix_import(const struct prefix *host,
				       eigrp_prefix_t *prefix);
eigrp_result_t eigrp_frr_prefix_export(const eigrp_prefix_t *prefix,
				       struct prefix *host);

#endif /* _ZEBRA_EIGRP_FRR_H_ */
