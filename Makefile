# EIGRP project convenience targets.
# The real FRR daemon build still happens after eigrpd/ is staged into FRR.

.PHONY: all smoke compile-smoke portable-test test clean

all: smoke

smoke compile-smoke:
	$(MAKE) -C test/build

portable-test:
	python3 -m pytest test/portable

test: smoke portable-test

clean:
	$(MAKE) -C test/build clean
