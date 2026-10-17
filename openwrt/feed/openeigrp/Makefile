# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Donnie V. Savage
#
# OW-02: Build the real portable EIGRP core with the OpenWrt target compiler.
# This is a static-library development package. The OpenWrt daemon, sockets,
# kernel RIB, UCI and procd integration belong to subsequent shim tasks.
#
# Resolve the *recipe* location, not OpenWrt's symlinked package directory.
# Our source tree remains owned by the OpenEIGRP checkout.
OPENEIGRP_RECIPE_DIR:=$(realpath $(dir $(lastword $(MAKEFILE_LIST))))
OPENEIGRP_CORE_SOURCE:=$(OPENEIGRP_RECIPE_DIR)/../../../eigrp/code

include $(TOPDIR)/rules.mk

PKG_NAME:=openeigrp
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later BSD-2-Clause
PKG_MAINTAINER:=Donnie V. Savage
PKG_BUILD_PARALLEL:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/openeigrp
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=OpenEIGRP portable RFC 7868 core (static development library)
  URL:=https://github.com/diivious/eigrp
endef

define Package/openeigrp/description
 A target-built static archive of the portable OpenEIGRP RFC 7868 protocol
 engine. This first-stage package does not provide a daemon or operate on
 network interfaces. Native OpenWrt host integration follows in later tasks.
endef

# Fail clearly if a detached/copied feed cannot locate its source checkout.
# Do not substitute another EIGRP implementation or fake a daemon executable.
define Build/Prepare
	@test -f "$(OPENEIGRP_CORE_SOURCE)/Makefile" || { \
		echo 'OpenEIGRP portable sources unavailable: $(OPENEIGRP_CORE_SOURCE)' >&2; \
		exit 1; \
	}
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(OPENEIGRP_CORE_SOURCE)/*.c $(OPENEIGRP_CORE_SOURCE)/*.h \
		$(OPENEIGRP_CORE_SOURCE)/Makefile $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

# The standalone core Makefile explicitly passes -pthread for its receive
# worker. TARGET_CC/TARGET_AR ensure the resulting archive is for OpenWrt,
# not the Debian/macOS build machine.
define Build/Compile
	+$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" AR="$(TARGET_AR)" \
		CFLAGS="$(TARGET_CFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -pthread" \
		CPPFLAGS="$(TARGET_CPPFLAGS) -DEIGRP_DISABLE_SNMP -I." \
		libeigrp-core.a
	@test -s $(PKG_BUILD_DIR)/libeigrp-core.a
endef

# Only the public host boundary is staged for future OpenWrt adapter builds.
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/openeigrp $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/eigrp.h \
		$(PKG_BUILD_DIR)/eigrp_cli.h $(PKG_BUILD_DIR)/eigrp_mgnt.h \
		$(PKG_BUILD_DIR)/eigrp_rib.h $(PKG_BUILD_DIR)/eigrp_sys.h \
		$(1)/usr/include/openeigrp/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/libeigrp-core.a $(1)/usr/lib/
endef

# OW-02 installs the actual compiled archive, not a placeholder daemon.
define Package/openeigrp/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/libeigrp-core.a $(1)/usr/lib/
endef

$(eval $(call BuildPackage,openeigrp))
