// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Portable EIGRP operation results.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_RESULT_H_
#define _EIGRP_RESULT_H_

typedef enum eigrp_result {
	EIGRP_RESULT_SUCCESS = 0,
	EIGRP_RESULT_NOT_IMPLEMENTED,
	EIGRP_RESULT_INVALID_ARGUMENT,
	EIGRP_RESULT_NOT_FOUND,
	EIGRP_RESULT_CONFLICT,
	EIGRP_RESULT_UNSUPPORTED,
	EIGRP_RESULT_INTERNAL_FAILURE,
} eigrp_result_t;

#endif /* _EIGRP_RESULT_H_ */
