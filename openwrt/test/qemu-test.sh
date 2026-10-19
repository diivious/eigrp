#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Offline QEMU harness contract tests. Real guests run via --qemu.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
python3 "$root/openwrt/test/qemu-uut.py" --self-test
