# EIGRP standalone development pipeline.
# Platform integration remains explicit under tools/<platform>.sh.

.PHONY: all build uut portable-build portable-test portable-pytest portable-scenarios \
	ipv4-basic-core ipv6-basic-core rtp-core test clean frr-smoke

all: build

portable-build:
	$(MAKE) -C eigrpd/code

portable-pytest: build
	python3 -m pytest eigrpd/test

portable-scenarios: build
	$(MAKE) -C eigrpd/test/uut all-scenarios

portable-test: portable-pytest portable-scenarios

build: portable-build
	$(MAKE) -C unix/code

uut: portable-pytest portable-scenarios

ipv4-basic-core: build
	$(MAKE) -C eigrpd/test/uut ipv4-basic-core

ipv6-basic-core: build
	$(MAKE) -C eigrpd/test/uut ipv6-basic-core

rtp-core: build
	$(MAKE) -C eigrpd/test/uut rtp-core

test:
	@status=0; \
	$(MAKE) portable-test || status=$$?; \
	$(MAKE) uut || status=$$?; \
	$(MAKE) ipv4-basic-core || status=$$?; \
	$(MAKE) ipv6-basic-core || status=$$?; \
	$(MAKE) rtp-core || status=$$?; \
	exit $$status

# Optional FRR-owned compile/link smoke; never part of the root portable gate.
frr-smoke:
	$(MAKE) -C frr/test/build

clean:
	$(MAKE) -C eigrpd/code clean
	$(MAKE) -C unix/code clean
	$(MAKE) -C eigrpd/test/uut clean
