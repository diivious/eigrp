#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source this in a Bash shell to expose Homebrew GNU tools for experimental
# native macOS OpenWrt development. It has no effect on Linux.

if [[ "$(uname -s)" == Darwin ]] && command -v brew >/dev/null 2>&1; then
	for _openwrt_formula in coreutils findutils gnu-getopt gnu-sed gnu-tar grep make; do
		_openwrt_prefix="$(brew --prefix "$_openwrt_formula" 2>/dev/null)" || continue
		_openwrt_bindir="$_openwrt_prefix/libexec/gnubin"
		[[ -d "$_openwrt_bindir" ]] || continue
		case ":$PATH:" in
			*":$_openwrt_bindir:"*) ;;
			*) PATH="$_openwrt_bindir:$PATH" ;;
		esac
	done
	for _openwrt_formula in bison flex; do
		_openwrt_prefix="$(brew --prefix "$_openwrt_formula" 2>/dev/null)" || continue
		_openwrt_bindir="$_openwrt_prefix/bin"
		[[ -d "$_openwrt_bindir" ]] || continue
		case ":$PATH:" in
			*":$_openwrt_bindir:"*) ;;
			*) PATH="$_openwrt_bindir:$PATH" ;;
		esac
	done
	export PATH
	unset _openwrt_formula _openwrt_prefix _openwrt_bindir
fi
