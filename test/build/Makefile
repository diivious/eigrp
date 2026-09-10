# Standalone syntax/compile guard for the FRR-backed EIGRP source projection.
# This is not a replacement for the FRR build/link gate.

CC ?= gcc
PROJECT_ROOT := ../..
EIGRPD_ROOT := $(PROJECT_ROOT)/eigrpd
FRR_ADAPTER_ROOT := $(PROJECT_ROOT)/frr
FRR_SOURCE_ROOT ?= ../../../frr
BUILD := obj
STAGE := $(BUILD)/eigrpd
LOGS := logs

COMMON_CFLAGS := \
	-std=gnu11 \
	-fsyntax-only \
	-Wall -Wextra \
	-Wno-unused-parameter \
	-Wno-unused-function \
	-Wno-missing-field-initializers \
	-Werror=implicit-function-declaration \
	-Werror=incompatible-pointer-types \
	-Werror=int-conversion \
	-Werror=return-type \
	-Iinclude \
	-Iinclude/lib \
	-I$(BUILD) \
	-I$(STAGE) \
	-I$(PROJECT_ROOT) \
	-I$(FRR_SOURCE_ROOT) \
	-DEIGRP_STANDALONE_BUILD

# Match the FRR install projection: common eigrpd/ first, then top-level frr/
# daemon adapter files overlaid into the same eigrpd directory.
ALL_SOURCES = $(filter-out $(STAGE)/%_clippy.c,$(sort $(wildcard $(STAGE)/*.c)))
SMOKE_SOURCES = $(ALL_SOURCES)

all: all-sources

stage:
	@rm -rf $(STAGE)
	@mkdir -p $(STAGE)
	@cp -a $(EIGRPD_ROOT)/. $(STAGE)/
	@find $(FRR_ADAPTER_ROOT) -maxdepth 1 -type f -exec cp -p {} $(STAGE)/ \;

smoke: stage $(LOGS)
	@set -e; \
	for src in $(SMOKE_SOURCES); do \
	  echo "CC $$src"; \
	  $(CC) $(COMMON_CFLAGS) $$src >$(LOGS)/$$(basename $$src).log 2>&1 || { cat $(LOGS)/$$(basename $$src).log; exit 1; }; \
	done

all-sources: stage $(LOGS)
	@set -e; \
	for src in $(ALL_SOURCES); do \
	  echo "CC $$src"; \
	  $(CC) $(COMMON_CFLAGS) $$src >$(LOGS)/$$(basename $$src).log 2>&1 || { cat $(LOGS)/$$(basename $$src).log; exit 1; }; \
	done

cli: smoke

list: stage
	@printf '%s\n' $(SMOKE_SOURCES)

$(LOGS):
	@mkdir -p $(LOGS)

clean:
	rm -rf $(LOGS) $(BUILD)

.PHONY: all stage smoke all-sources cli list clean
