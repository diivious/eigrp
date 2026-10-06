# EIGRP standalone development pipeline.
# Platform-specific integration is kept under tools/<platform>.sh.

# Route Python execution through the project launcher. It selects the native
# macOS interpreter for local Darwin development and PATH-selected python3 on
# other Unix systems. CI or callers may override it with EIGRP_PYTHON/PYTHON.
PYTHON ?= $(CURDIR)/tools/python
export PYTHON

.PHONY: all help build uut portable-build portable-test portable-pytest \
	portable-scenarios ipv4-basic-core ipv6-basic-core rtp-core core-topology mesh-topology test clean \
	frr-smoke

all: build ## Build the standalone EIGRP code and Unix platform support.

help: ## Show available make targets and their descriptions.
	@printf 'Usage: make [target]\n\n'
	@printf 'Targets:\n'
	@awk 'BEGIN {FS = ":.*## "} /^[A-Za-z0-9_.-]+:.*## / {printf "  %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

portable-build: ## Build the platform-independent EIGRP code.
	$(MAKE) -C eigrp/code

portable-pytest: build ## Run the Python UUT test suite with pytest.
	$(PYTHON) -m pytest eigrp/test

portable-scenarios: build ## Run all portable UUT scenario tests.
	$(MAKE) -C eigrp/test/uut all-scenarios

portable-test: portable-pytest portable-scenarios ## Run all portable pytest and scenario tests.

build: portable-build ## Build portable EIGRP code and Unix platform support.
	$(MAKE) -C unix/code

uut: portable-pytest portable-scenarios ## Run the complete UUT pytest and scenario suites.

ipv4-basic-core: build ## Run the IPv4 basic-core UUT scenario.
	$(MAKE) -C eigrp/test/uut ipv4-basic-core

ipv6-basic-core: build ## Run the IPv6 basic-core UUT scenario.
	$(MAKE) -C eigrp/test/uut ipv6-basic-core

rtp-core: build ## Run the RTP core UUT scenario.
	$(MAKE) -C eigrp/test/uut rtp-core

core-topology: build ## Run the four-router square and cross-connected core redundancy scenarios.
	$(MAKE) -C eigrp/test/uut core-topology

mesh-topology: build ## Run the six-router full-mesh DUAL/query-storm qualification scenarios.
	$(MAKE) -C eigrp/test/uut mesh-topology

test: ## Run all test groups and return failure if any group fails.
	@status=0; \
	$(MAKE) portable-test || status=$$?; \
	$(MAKE) uut || status=$$?; \
	$(MAKE) ipv4-basic-core || status=$$?; \
	$(MAKE) ipv6-basic-core || status=$$?; \
	$(MAKE) rtp-core || status=$$?; \
	exit $$status

# Optional FRR-owned compile/link smoke test; not part of the portable gate.
frr-smoke: ## Run the optional FRR compile/link smoke test.
	$(MAKE) -C frr/test/build

clean: ## Remove generated build and UUT artifacts.
	$(MAKE) -C eigrp/code clean
	$(MAKE) -C unix/code clean
	$(MAKE) -C eigrp/test/uut clean
