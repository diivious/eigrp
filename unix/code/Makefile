# Standalone Unix runtime build. No FRR/BIRD headers or libraries are required.
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS += -I../../eigrpd/code -I.
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread

OBJS = eigrp_unix_sys.o eigrp_unix_config.o eigrp_unix_rib.o eigrp_unix_interface.o eigrp_unix_segment.o eigrp_unix_wire.o eigrp_log.o

.PHONY: all clean
all: libeigrp-unix-runtime.a eigrp-wire

libeigrp-unix-runtime.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

eigrp_unix_sys.o: eigrp_unix_sys.c eigrp_unix.h ../../eigrpd/code/eigrp_sys.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ eigrp_unix_sys.c

eigrp_unix_config.o: eigrp_unix_config.c eigrp_unix_config.h eigrp_unix.h ../../eigrpd/code/eigrp_cli.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ eigrp_unix_config.c

eigrp_unix_rib.o: eigrp_unix_rib.c eigrp_unix_rib.h ../../eigrpd/code/eigrp_rib.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ eigrp_unix_rib.c

eigrp_unix_interface.o: eigrp_unix_interface.c eigrp_unix_interface.h ../../eigrpd/code/eigrp_sys.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ eigrp_unix_interface.c

eigrp_unix_segment.o: eigrp_unix_segment.c eigrp_unix_segment.h eigrp_unix_interface.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ eigrp_unix_segment.c

eigrp_unix_wire.o: eigrp_unix_wire.c eigrp_unix_wire.h eigrp_unix.h eigrp_unix_interface.h eigrp_unix_segment.h ../../eigrpd/code/eigrp_sys.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ eigrp_unix_wire.c

eigrp-wire: eigrp_unix_wire_broker.c eigrp_unix_wire.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ eigrp_unix_wire_broker.c

eigrp_log.o: ../../eigrpd/code/eigrp_log.c ../../eigrpd/code/eigrp_log.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ ../../eigrpd/code/eigrp_log.c

clean:
	rm -f $(OBJS) libeigrp-unix-runtime.a eigrp-wire
