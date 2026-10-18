# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Donnie V. Savage
#
# OW-06: Build portable EIGRP and native OpenWrt runtime, interface and
# protocol-88 socket adapters. These are real target libraries. A runnable
# daemon still requires kernel RIB, configuration and service lifecycle.
#
# Resolve the *recipe* location, not OpenWrt's symlinked package directory.
# Our source tree remains owned by the OpenEIGRP checkout.
OPENEIGRP_RECIPE_DIR:=$(realpath $(dir $(lastword $(MAKEFILE_LIST))))
OPENEIGRP_CORE_SOURCE:=$(OPENEIGRP_RECIPE_DIR)/../../../eigrp/code
OPENEIGRP_RUNTIME_SOURCE:=$(OPENEIGRP_RECIPE_DIR)/../../code

include $(TOPDIR)/rules.mk

PKG_NAME:=openeigrp
PKG_VERSION:=0.1.0
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-or-later BSD-2-Clause
PKG_MAINTAINER:=Donnie V. Savage
PKG_BUILD_PARALLEL:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
# Autorebuild a locally linked feed when portable or runtime sources change.
PKG_FILE_DEPENDS:=$(OPENEIGRP_CORE_SOURCE) $(OPENEIGRP_RUNTIME_SOURCE)

include $(INCLUDE_DIR)/package.mk

define Package/openeigrp
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Routing and Redirection
  TITLE:=OpenEIGRP portable core and native OpenWrt host/packet libraries
  URL:=https://github.com/diivious/eigrp
endef

define Package/openeigrp/description
 Target-built static archives for the portable OpenEIGRP RFC 7868 engine
 and its native OpenWrt host support (events, timers, FD readiness, work
 queues, Linux netlink interface/address inventory, IPv4/IPv6 protocol 88
 sockets, packet I/O and multicast memberships).
 This package does not yet provide a deployable daemon.
endef

# Fail clearly if a detached/copied feed cannot locate its source checkout.
# Do not substitute another EIGRP implementation or fake a daemon executable.
define Build/Prepare
	@test -f "$(OPENEIGRP_CORE_SOURCE)/Makefile" || { \
		echo 'OpenEIGRP portable sources unavailable: $(OPENEIGRP_CORE_SOURCE)' >&2; \
		exit 1; \
	}
	@test -f "$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_runtime.c" || { \
		echo 'OpenEIGRP OpenWrt runtime source unavailable: $(OPENEIGRP_RUNTIME_SOURCE)' >&2; \
		exit 1; \
	}
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/openwrt-runtime
	$(CP) $(OPENEIGRP_CORE_SOURCE)/*.c $(OPENEIGRP_CORE_SOURCE)/*.h \
		$(OPENEIGRP_CORE_SOURCE)/Makefile $(PKG_BUILD_DIR)/
	$(CP) $(OPENEIGRP_RUNTIME_SOURCE)/Makefile \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_runtime.c \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_runtime.h \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_interface.c \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_interface.h \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_interface_state.c \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_packet.c \
		$(OPENEIGRP_RUNTIME_SOURCE)/eigrp_owrt_packet.h \
		$(PKG_BUILD_DIR)/openwrt-runtime/
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
	+$(MAKE) -C $(PKG_BUILD_DIR)/openwrt-runtime \
		CC="$(TARGET_CC)" AR="$(TARGET_AR)" \
		CFLAGS="$(TARGET_CFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L -pthread" \
		CPPFLAGS="$(TARGET_CPPFLAGS) -I. -I$(PKG_BUILD_DIR)" \
		EIGRP_CORE_INCLUDE="$(PKG_BUILD_DIR)" OWRT_LINUX=1 \
		libeigrp-owrt-runtime.a
	@test -s $(PKG_BUILD_DIR)/openwrt-runtime/libeigrp-owrt-runtime.a
endef

# Only the public host boundary is staged for future OpenWrt adapter builds.
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/openeigrp $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/eigrp.h \
		$(PKG_BUILD_DIR)/eigrp_cli.h $(PKG_BUILD_DIR)/eigrp_mgnt.h \
		$(PKG_BUILD_DIR)/eigrp_rib.h $(PKG_BUILD_DIR)/eigrp_sys.h \
		$(1)/usr/include/openeigrp/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/libeigrp-core.a $(1)/usr/lib/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/openwrt-runtime/libeigrp-owrt-runtime.a $(1)/usr/lib/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/openwrt-runtime/eigrp_owrt_runtime.h \
		$(PKG_BUILD_DIR)/openwrt-runtime/eigrp_owrt_interface.h \
		$(PKG_BUILD_DIR)/openwrt-runtime/eigrp_owrt_packet.h \
		$(1)/usr/include/openeigrp/
endef

# Install real target archives; no runnable daemon is claimed at OW-06.
define Package/openeigrp/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/libeigrp-core.a $(1)/usr/lib/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/openwrt-runtime/libeigrp-owrt-runtime.a $(1)/usr/lib/
endef

$(eval $(call BuildPackage,openeigrp))
