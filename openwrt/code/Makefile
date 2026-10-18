# Copyright (C) 2026 Donnie V. Savage
# Host-neutral test compilation and OpenWrt-target archive use this Makefile.
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
EIGRP_CORE_INCLUDE ?= ../../eigrp/code
CPPFLAGS += -I. -I$(EIGRP_CORE_INCLUDE)
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -pthread

# Linux rtnetlink integration is compiled only for the OpenWrt target (or a
# native Linux UUT). Runtime alone remains host-neutral for macOS testing.
OWRT_LINUX ?= $(if $(filter Linux,$(shell uname -s)),1,0)
OBJS := eigrp_owrt_runtime.o eigrp_owrt_interface_state.o
ifeq ($(OWRT_LINUX),1)
OBJS += eigrp_owrt_interface.o eigrp_owrt_packet.o
endif

.PHONY: all clean
all: libeigrp-owrt-runtime.a

libeigrp-owrt-runtime.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

eigrp_owrt_runtime.o: eigrp_owrt_runtime.c eigrp_owrt_runtime.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

eigrp_owrt_interface_state.o: eigrp_owrt_interface_state.c eigrp_owrt_interface.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

eigrp_owrt_interface.o: eigrp_owrt_interface.c eigrp_owrt_interface.h eigrp_owrt_runtime.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

eigrp_owrt_packet.o: eigrp_owrt_packet.c eigrp_owrt_packet.h eigrp_owrt_runtime.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) libeigrp-owrt-runtime.a
