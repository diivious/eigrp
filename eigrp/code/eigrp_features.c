// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP image feature dispatch.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrp_features.h"

bool eigrp_feature_supported(eigrp_feature_t feature)
{
	switch (feature) {
	case EIGRP_FEATURE_AFI_IPV4:
		return eigrp_ipv4_supported();
	case EIGRP_FEATURE_AFI_IPV6:
		return eigrp_ipv6_supported();
	case EIGRP_FEATURE_BFD:
		return eigrp_bfd_supported();
	case EIGRP_FEATURE_SNMP:
		return eigrp_snmp_supported();
	default:
		return false;
	}
}

bool eigrp_afi_supported(eigrp_afi_t afi)
{
	switch (afi) {
	case EIGRP_AFI_IPV4:
		return eigrp_feature_supported(EIGRP_FEATURE_AFI_IPV4);
	case EIGRP_AFI_IPV6:
		return eigrp_feature_supported(EIGRP_FEATURE_AFI_IPV6);
	default:
		return false;
	}
}
