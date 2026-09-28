PYTHON ?= ../../../tools/python
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS += -DEIGRP_DISABLE_SNMP  -I../../../eigrpd/code -I../../../unix/code
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -Wno-unused-const-variable -pthread

CORE := $(filter-out ../../../eigrpd/code/eigrp_log.c ../../../eigrpd/code/eigrp_dump.c,$(wildcard ../../../eigrpd/code/*.c))
UNIX := ../../../unix/code/eigrp_unix_sys.c ../../../unix/code/eigrp_unix_rib.c \
        ../../../unix/code/eigrp_unix_interface.c ../../../unix/code/eigrp_unix_segment.c \
        ../../../unix/code/eigrp_unix_wire.c ../../../eigrpd/code/eigrp_log.c

.PHONY: all clean
all: eigrp-uut-node

eigrp-uut-node: node.c $(CORE) $(UNIX)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ node.c $(CORE) $(UNIX)

clean:
	rm -f eigrp-uut-node

.PHONY: ipv4-basic-core ipv6-basic-core basic-core rtp-core all-scenarios
ipv4-basic-core: eigrp-uut-node
	$(PYTHON) run.py examples/ipv4-basic-core.yaml examples/ipv4-basic-core-gate.yaml


ipv6-basic-core: eigrp-uut-node
	$(PYTHON) run.py examples/ipv6-basic-core.yaml examples/ipv6-basic-core-gate.yaml

basic-core: eigrp-uut-node
	@status=0; \
	$(PYTHON) run.py examples/ipv4-basic-core.yaml examples/ipv4-basic-core-gate.yaml || status=$$?; \
	$(PYTHON) run.py examples/ipv6-basic-core.yaml examples/ipv6-basic-core-gate.yaml || status=$$?; \
	exit $$status

rtp-core: eigrp-uut-node
	@status=0; \
	$(PYTHON) run.py ../rtp/two-router.yaml ../rtp/srtt-rto.yaml || status=$$?; \
	$(PYTHON) run.py ../rtp/two-router.yaml ../rtp/lost-reliable-packet.yaml || status=$$?; \
	$(PYTHON) run.py ../rtp/two-router.yaml ../rtp/lost-ack.yaml || status=$$?; \
	$(PYTHON) run.py ../rtp/three-router-shared.yaml ../rtp/reliable-multicast-peer-retry.yaml || status=$$?; \
	exit $$status

all-scenarios: eigrp-uut-node
	$(PYTHON) run_all.py
