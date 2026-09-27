# EIGRP project convenience targets.
# The real FRR daemon build still happens after eigrpd/ is staged into FRR.

.PHONY: all smoke compile-smoke portable-test platform-test test clean

all: smoke

smoke compile-smoke:
	$(MAKE) -C test/build

portable-test:
	python3 -m pytest test/common

platform-test:
	python3 -m pytest test/platform

test: smoke portable-test platform-test

clean:
	$(MAKE) -C test/build clean
