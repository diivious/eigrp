# EIGRP standalone development pipeline.
# Platform-specific integration is kept under tools/<platform>.sh.

# Route Python execution through the project launcher. It selects the native
# macOS interpreter for local Darwin development and PATH-selected python3 on
# other Unix systems. CI or callers may override it with EIGRP_PYTHON/PYTHON.
PYTHON ?= $(CURDIR)/tools/python
export PYTHON

.PHONY: all help build uut portable-build portable-test portable-pytest \
	portable-scenarios ipv4-basic-core ipv6-basic-core rtp-core core-topology mesh-topology test \
	scale scale-peers scale-internal scale-external scale-line scale-kite scale-peer-routes \
	scale-100k scale-withdraw scale-mcast scale-linkdown scale-mixed-tlv scale-churn \
	scale-stress scale-auth-md5 scale-auth scale-all clean frr-smoke \
	merge-check merge-check-host merge-check-test

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

test: uut ## Run the complete portable UUT gate once (pytest + all YAML scenarios).

# The quick scale gate covers the three TRANSMIT guards. It intentionally does
# not run all UUT cases, the 2048-peer startup test, or any 100K-route scenario.
# The small multicast case is the real synthetic-wire backpressure test with
# a reduced peer count; it must still withhold/release ACKs and drain RTP.
scale: build ## Quick packet, unicast and multicast pacing/backpressure qualification.
	@printf '\n%s\n\n' '======================== quick pacing/backpressure scale gate ========================'
	$(MAKE) -C eigrp/test/uut eigrp-uut-node
	@printf '\n%s\n' '--- Packet pacing: RFC 7868 interface bandwidth budget ---'
	$(PYTHON) -m pytest -q eigrp/test/packet/pacing.py
	@printf '\n%s\n' '--- Unicast pacing: lost ACK and RTP retry progress ---'
	$(PYTHON) eigrp/test/uut/run.py eigrp/test/rtp/two-router.yaml eigrp/test/rtp/lost-ack.yaml
	@printf '\n%s\n' '--- Multicast pacing: CR/backpressure, held ACKs and recovery ---'
	EIGRP_SCALE_PEERS=32 EIGRP_SCALE_MCAST_SLOW_PEERS=8 EIGRP_SCALE_MCAST_ROUTES=64 \
	EIGRP_SCALE_WATCHDOG_SEC=120 \
		$(PYTHON) eigrp/test/scale/stress_extended.py mcast

# The peer test uses one real R1 UUT and 2048 synthetic RFC-format EIGRP peers;
# do not allocate 2048 host processes.
scale-peers: build ## Establish 2,048 EIGRP neighbors and verify RTP quiescence.
	@printf '\n%s\n\n' '======================== 2048-peer scale gate ========================'
	$(MAKE) -C eigrp/test/uut eigrp-uut-node
	$(PYTHON) eigrp/test/scale/neighbor_2048.py

# Individually addressable heavy tests; no numbered names or compatibility aliases.
scale-internal: ## Inject 100K EIGRP internal routes into R1.
	$(MAKE) -C eigrp/test/scale internal

scale-external: ## Inject 100K EIGRP external routes into R1.
	$(MAKE) -C eigrp/test/scale external

scale-line: ## Flood 100K mixed routes along R1-R2-R3.
	$(MAKE) -C eigrp/test/scale line

scale-kite: ## Flood 100K mixed routes from the kite tail at R1.
	$(MAKE) -C eigrp/test/scale kite

scale-peer-routes: ## Receive 50 routes from each of 2,048 EIGRP neighbors.
	$(MAKE) -C eigrp/test/scale peer-routes

scale-withdraw: ## Install, withdraw and reinject 100K routes; verify cleanup.
	$(MAKE) -C eigrp/test/scale withdraw

scale-mcast: ## Withhold multicast peer ACKs, then verify CR MTU and RTP recovery.
	$(MAKE) -C eigrp/test/scale mcast

scale-linkdown: ## Drop the R1-R2 link during a 100K-route flood and recover.
	$(MAKE) -C eigrp/test/scale linkdown

scale-mixed-tlv: ## Test TLV1 and TLV2 peers sharing one interface.
	$(MAKE) -C eigrp/test/scale mixed-tlv

scale-churn: ## Change, withdraw and restore routes under sustained load.
	$(MAKE) -C eigrp/test/scale churn

scale-auth-md5: ## Flood 100K routes across an MD5-authenticated adjacency.
	$(MAKE) -C eigrp/test/scale auth-md5

# Each suite runs every child test, even after a failure. Print the identity of
# each failure and return a nonzero aggregate exit status at the end. These
# deliberately run sequentially to avoid cross-test memory pressure.
define run_scale_suite
	@failed=0; \
	for target in $(1); do \
		printf '\n%s\n' "======================== $$target ========================"; \
		if $(MAKE) $$target; then \
			printf 'PASS %s\n' "$$target"; \
		else \
			printf 'FAIL %s\n' "$$target"; \
			failed=1; \
		fi; \
	done; \
	exit $$failed
endef

scale-100k: ## Run the 2,048-peer gate and all five heavy route-volume tests.
	$(call run_scale_suite,scale-peers scale-internal scale-external scale-line scale-kite scale-peer-routes)

scale-stress: ## Run withdraw, multicast, link-down, mixed-TLV and churn tests.
	$(call run_scale_suite,scale-withdraw scale-mcast scale-linkdown scale-mixed-tlv scale-churn)

# scale-auth-sha2 is intentionally not a target yet. Add it to the suite only
# when an executable SHA-256 authentication stress test has been implemented.
scale-auth: ## Run implemented authenticated scale tests (MD5; SHA-256 later).
	$(call run_scale_suite,scale-auth-md5)

scale-all: ## Run all 100K, stress and authentication suites; report all failures.
	$(call run_scale_suite,scale-100k scale-stress scale-auth)

# Optional FRR-owned compile/link smoke test; not part of the portable gate.
frr-smoke: ## Run the optional FRR compile/link smoke test.
	$(MAKE) -C frr/test/build

clean: ## Remove generated build and UUT artifacts.
	$(MAKE) -C eigrp/code clean
	$(MAKE) -C unix/code clean
	$(MAKE) -C eigrp/test/uut clean

# Preserve make all as the fast portable/Unix build. The strict pre-merge gate
# explicitly exercises every present integration, including target build/UUT.
merge-check: ## Full pre-merge gate (FRR + OpenWrt cross-build, QEMU, live UUT).
	bash tools/merge-check.sh --full $(MERGE_CHECK_ARGS)

merge-check-host: ## Host-only shim regression; not full merge qualification.
	bash tools/merge-check.sh --host $(MERGE_CHECK_ARGS)

merge-check-test: ## Test merge-gate orchestration without FRR, root or QEMU.
	bash test/merge/merge-check-test.sh
