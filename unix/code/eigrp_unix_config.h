// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Standalone Unix EIGRP configuration parser.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_UNIX_EIGRP_UNIX_CONFIG_H_
#define EIGRP_UNIX_EIGRP_UNIX_CONFIG_H_

#include <stdio.h>

#include "eigrp.h"

typedef struct eigrp_unix_config eigrp_unix_config_t;

eigrp_unix_config_t *eigrp_unix_config_create(void);
void eigrp_unix_config_delete(eigrp_unix_config_t *config);
eigrp_result_t eigrp_unix_config_apply_file(eigrp_unix_config_t *config,
	FILE *input, unsigned *error_line);
eigrp_result_t eigrp_unix_config_apply_text(eigrp_unix_config_t *config,
	const char *text, unsigned *error_line);

#endif /* EIGRP_UNIX_EIGRP_UNIX_CONFIG_H_ */
