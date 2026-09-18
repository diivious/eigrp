// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable prefix helpers.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_PREFIX_H_
#define EIGRPD_EIGRP_PREFIX_H_

#include <stdbool.h>
#include <stddef.h>

#include "eigrpd/eigrp_types.h"

#define EIGRP_PREFIX_STRLEN 52U

bool eigrp_prefix_valid(const eigrp_prefix_t *prefix);
void eigrp_prefix_normalize(eigrp_prefix_t *prefix);
int eigrp_prefix_snprintf(char *buf, size_t len, const eigrp_prefix_t *prefix);

#endif /* EIGRPD_EIGRP_PREFIX_H_ */
