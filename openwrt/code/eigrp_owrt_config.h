// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt-owned UCI configuration model, normalized before portable API calls.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_OWRT_CONFIG_H
#define EIGRP_OWRT_CONFIG_H

#include <stddef.h>
#include "eigrp.h"

#define EIGRP_OWRT_CONFIG_DIAGNOSTIC_MAX 256

typedef struct eigrp_owrt_config eigrp_owrt_config_t;
typedef struct eigrp_owrt_config_section eigrp_owrt_config_section_t;

typedef struct eigrp_owrt_config_diagnostic {
	char section[96];
	char message[EIGRP_OWRT_CONFIG_DIAGNOSTIC_MAX];
	eigrp_result_t result;
} eigrp_owrt_config_diagnostic_t;

/* Host-only builder. The libuci adapter uses these same operations; tests can
 * build a snapshot without linking libuci or using OpenWrt's filesystem. */
eigrp_owrt_config_t *eigrp_owrt_config_create(void);
void eigrp_owrt_config_free(eigrp_owrt_config_t *config);
eigrp_owrt_config_section_t *eigrp_owrt_config_section_add(
	eigrp_owrt_config_t *config, const char *type, const char *id);
eigrp_result_t eigrp_owrt_config_option_set(eigrp_owrt_config_section_t *section,
	const char *key, const char *value);
eigrp_result_t eigrp_owrt_config_validate(const eigrp_owrt_config_t *config,
	eigrp_owrt_config_diagnostic_t *diagnostic);
/* Apply validated config through real portable semantic targets. The caller
 * owns the process lifecycle; no runtime is started by config parsing. */
eigrp_result_t eigrp_owrt_config_apply(const eigrp_owrt_config_t *config,
	eigrp_owrt_config_diagnostic_t *diagnostic);
/* Replaces an applied snapshot. Validation occurs before mutation. On a
 * semantic failure the previous snapshot is replayed on a best-effort basis. */
eigrp_result_t eigrp_owrt_config_replace(eigrp_owrt_config_t **active,
	eigrp_owrt_config_t *candidate, eigrp_owrt_config_diagnostic_t *diagnostic);
void eigrp_owrt_config_remove(const eigrp_owrt_config_t *config);

/* Host policy lookups use the active, borrowed configuration. */
void eigrp_owrt_config_policy_bind(const eigrp_owrt_config_t *config);
const eigrp_owrt_config_section_t *eigrp_owrt_config_first(const eigrp_owrt_config_t *config);
const eigrp_owrt_config_section_t *eigrp_owrt_config_next(const eigrp_owrt_config_section_t *section);
const char *eigrp_owrt_config_section_type(const eigrp_owrt_config_section_t *section);
const char *eigrp_owrt_config_section_id(const eigrp_owrt_config_section_t *section);
const char *eigrp_owrt_config_option(const eigrp_owrt_config_section_t *section, const char *key);

/* libuci-specific loading and UCI commit. Implemented in eigrp_owrt_uci.c. */
eigrp_result_t eigrp_owrt_uci_load(const char *directory,
	eigrp_owrt_config_t **config, eigrp_owrt_config_diagnostic_t *diagnostic);
eigrp_result_t eigrp_owrt_uci_reload(const char *directory,
	eigrp_owrt_config_t **active, eigrp_owrt_config_diagnostic_t *diagnostic);
/* Documented no/reset forms are UCI option deletion, or section deletion;
 * callers use the standard UCI configuration commit, never edit a flat file. */
eigrp_result_t eigrp_owrt_uci_option_write(const char *directory,
	const char *section, const char *option, const char *value,
	eigrp_operation_t operation, eigrp_owrt_config_diagnostic_t *diagnostic);

#endif
