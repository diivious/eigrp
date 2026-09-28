// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Shared control-plane segment model for the Unix EIGRP host shim.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRP_UNIX_EIGRP_UNIX_SEGMENT_H_
#define EIGRP_UNIX_EIGRP_UNIX_SEGMENT_H_

#include "eigrp_unix_interface.h"

typedef struct eigrp_unix_segment eigrp_unix_segment_t;

typedef void (*eigrp_unix_segment_endpoint_walk_cb)(
	const char *uut_name, const char *interface_name,
	const eigrp_unix_interface_t *interface, void *arg);

eigrp_result_t eigrp_unix_segment_create(const char *name,
	eigrp_unix_segment_t **segment);
eigrp_result_t eigrp_unix_segment_delete(const char *name);
eigrp_unix_segment_t *eigrp_unix_segment_find(const char *name);
const char *eigrp_unix_segment_name(const eigrp_unix_segment_t *segment);
eigrp_unix_segment_t *eigrp_unix_segment_interface_find(
	const eigrp_unix_interface_t *interface);
eigrp_result_t eigrp_unix_segment_endpoint_attach(
	eigrp_unix_segment_t *segment, const char *uut_name,
	const char *interface_name);
eigrp_result_t eigrp_unix_segment_interface_attach(
	eigrp_unix_segment_t *segment, const char *uut_name,
	eigrp_unix_interface_t *interface);
eigrp_result_t eigrp_unix_segment_interface_detach(
	eigrp_unix_segment_t *segment, const char *uut_name,
	const char *interface_name);
void eigrp_unix_segment_interface_detach_all(
	eigrp_unix_interface_t *interface);
size_t eigrp_unix_segment_endpoint_count(const eigrp_unix_segment_t *segment);
void eigrp_unix_segment_endpoint_walk(const eigrp_unix_segment_t *segment,
	eigrp_unix_segment_endpoint_walk_cb callback, void *arg);
void eigrp_unix_segment_model_reset(void);

#endif /* EIGRP_UNIX_EIGRP_UNIX_SEGMENT_H_ */
