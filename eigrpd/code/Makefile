# Standalone portable EIGRP core build. No FRR/BIRD headers or libraries are required.
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS += -DEIGRP_DISABLE_SNMP -I.
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -Wno-unused-const-variable -pthread

# eigrp_dump.c is legacy presentation/debug support and is not part of the
# standalone protocol runtime. eigrp_log.c supplies the portable fallback logger.
SRCS := $(filter-out eigrp_dump.c,$(wildcard *.c))
OBJS := $(SRCS:.c=.o)

.PHONY: all clean
all: libeigrp-core.a

libeigrp-core.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) libeigrp-core.a
