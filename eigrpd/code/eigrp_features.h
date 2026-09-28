// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP image feature contract.
 *
 * Features are enabled by default.  An integrator may disable a feature with
 * an EIGRP_DISABLE_* compile definition or by defining it below.  An enabled
 * feature deliberately requires a real eigrp_*_supported() implementation;
 * a missing integration therefore fails at link time instead of silently
 * advertising support that is not present in the image.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_FEATURES_H_
#define EIGRPD_EIGRP_FEATURES_H_

#include <stdbool.h>
#include "eigrp.h"

#ifndef EIGRP_RELEASE
#define EIGRP_RELEASE "development"
#endif

/* Features not implemented by the current common image. */
#ifndef EIGRP_ENABLE_BFD
#define EIGRP_DISABLE_BFD
#endif
#ifndef EIGRP_ENABLE_MANET
#define EIGRP_DISABLE_MANET
#endif
#ifndef EIGRP_ENABLE_MTR
#define EIGRP_DISABLE_MTR
#endif
#ifndef EIGRP_ENABLE_EVN
#define EIGRP_DISABLE_EVN
#endif

typedef enum eigrp_feature {
	EIGRP_FEATURE_AFI_IPV4 = 1,
	EIGRP_FEATURE_AFI_IPV6,
	EIGRP_FEATURE_BFD,
	EIGRP_FEATURE_SNMP,
} eigrp_feature_t;

#ifdef EIGRP_DISABLE_IPV4
#define eigrp_ipv4_supported() false
#else
bool eigrp_ipv4_supported(void);
#endif

#ifdef EIGRP_DISABLE_IPV6
#define eigrp_ipv6_supported() false
#else
bool eigrp_ipv6_supported(void);
#endif

#ifdef EIGRP_DISABLE_BFD
#define eigrp_bfd_supported() false
#else
bool eigrp_bfd_supported(void);
#endif

#ifdef EIGRP_DISABLE_MANET
#define eigrp_manet_supported() false
#else
bool eigrp_manet_supported(void);
#endif

#ifdef EIGRP_DISABLE_MTR
#define eigrp_mtr_supported() false
#else
bool eigrp_mtr_supported(void);
#endif

#ifdef EIGRP_DISABLE_EVN
#define eigrp_evn_supported() false
#else
bool eigrp_evn_supported(void);
#endif

#ifdef EIGRP_DISABLE_SNMP
#define eigrp_snmp_supported() false
#else
bool eigrp_snmp_supported(void);
#endif

bool eigrp_feature_supported(eigrp_feature_t feature);
bool eigrp_afi_supported(eigrp_afi_t afi);

#endif /* EIGRPD_EIGRP_FEATURES_H_ */
