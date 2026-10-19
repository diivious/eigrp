// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt procd-owned EIGRP service entry point. No private core access.
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "eigrp.h"
#include "eigrp_rib.h"
#include "eigrp_sys.h"
#include "eigrp_owrt_config.h"
#include "eigrp_owrt_interface.h"
#include "eigrp_owrt_mgnt.h"
#include "eigrp_owrt_rib.h"
#include "eigrp_owrt_runtime.h"

#define EIGRP_OWRT_SOCKET_DEFAULT "/var/run/openeigrpd.sock"

static void eigrp_owrt_daemon_usage(FILE *stream)
{
	fprintf(stream,
		"Usage: openeigrpd [--foreground] [--check] [--config-dir DIR] [--socket PATH]\n"
		"       openeigrpd --help | --version\n"
		"  --check validates /etc/config/openeigrp without starting sockets or routing\n"
		"  HUP reloads validated UCI configuration; TERM/INT stop gracefully\n");
}

static void eigrp_owrt_daemon_diagnostic(const char *action,
	eigrp_result_t result, const eigrp_owrt_config_diagnostic_t *diagnostic)
{
	fprintf(stderr, "openeigrpd: %s failed: result=%d section=%s %s\n", action,
		(int)result, diagnostic ? diagnostic->section : "",
		diagnostic ? diagnostic->message : "");
}

static int eigrp_owrt_daemon_run(const char *config_dir, const char *socket_path,
	eigrp_owrt_config_t *candidate)
{
	eigrp_owrt_config_t *active = NULL;
	eigrp_owrt_config_diagnostic_t diagnostic = {0};
	sigset_t signals;
	int signal_fd = -1, listener = -1, rc = 1;
	bool core_started = false, rib_started = false, interfaces_started = false;
	eigrp_result_t result;

	/* Block handled signals before creating any runtime or protocol threads. */
	sigemptyset(&signals);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGHUP);
	sigaddset(&signals, SIGPIPE);
	if (pthread_sigmask(SIG_BLOCK, &signals, NULL) != 0) {
		fprintf(stderr, "openeigrpd: could not block service signals\n");
		goto finish;
	}
	signal_fd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
	if (signal_fd < 0) {
		perror("openeigrpd: signalfd");
		goto finish;
	}
	/* The portable core owns its runtime instances. The host owns services. */
	eigrp_sys_runtime_init();
	eigrp_sys_policy_init();
	eigrp_rib_init();
	eigrp_init();
	core_started = true;
	if (!eigrp_process_receive_start()) {
		fprintf(stderr, "openeigrpd: failed to start protocol receive worker\n");
		goto finish;
	}
	result = eigrp_owrt_config_replace(&active, candidate, &diagnostic);
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_owrt_daemon_diagnostic("startup configuration", result, &diagnostic);
		goto finish;
	}
	candidate = NULL;
	if (!eigrp_owrt_rib_start()) {
		fprintf(stderr, "openeigrpd: failed to start kernel route watcher\n");
		goto finish;
	}
	rib_started = true;
	if (!eigrp_owrt_interface_start()) {
		fprintf(stderr, "openeigrpd: failed to start interface watcher\n");
		goto finish;
	}
	interfaces_started = true;
	listener = eigrp_owrt_mgnt_listen(socket_path);
	if (listener < 0) {
		fprintf(stderr, "openeigrpd: cannot open management socket: %s\n", socket_path);
		goto finish;
	}
	fprintf(stderr, "openeigrpd: started (read-only management: %s)\n", socket_path);
	for (;;) {
		struct pollfd fds[2] = {
			{.fd = signal_fd, .events = POLLIN},
			{.fd = listener, .events = POLLIN}
		};
		int ready = poll(fds, 2, -1);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("openeigrpd: poll");
			goto finish;
		}
		if (fds[0].revents & POLLIN) {
			struct signalfd_siginfo signal_event;
			if (read(signal_fd, &signal_event, sizeof(signal_event)) != sizeof(signal_event))
				continue;
			if (signal_event.ssi_signo == SIGTERM || signal_event.ssi_signo == SIGINT) {
				rc = 0;
				break;
			}
			if (signal_event.ssi_signo == SIGHUP) {
				eigrp_owrt_config_t *new_config = NULL;
				memset(&diagnostic, 0, sizeof(diagnostic));
				result = eigrp_owrt_uci_load(config_dir, &new_config, &diagnostic);
				if (result == EIGRP_RESULT_SUCCESS) {
					/* Serialize with runtime callbacks during ownership swap. */
					eigrp_owrt_runtime_enter();
					result = eigrp_owrt_config_replace(&active, new_config, &diagnostic);
					eigrp_owrt_runtime_leave();
					if (result == EIGRP_RESULT_SUCCESS)
						fprintf(stderr, "openeigrpd: UCI reload successful\n");
					else
						eigrp_owrt_config_free(new_config);
				}
				if (result != EIGRP_RESULT_SUCCESS)
					eigrp_owrt_daemon_diagnostic("UCI reload (previous state retained where possible)",
						result, &diagnostic);
			}
		}
		if (fds[1].revents & POLLIN)
			eigrp_owrt_mgnt_handle(listener);
	}
finish:
	/* Stop external producer threads before the portable hierarchy is freed. */
	if (listener >= 0) eigrp_owrt_mgnt_close(listener, socket_path);
	if (interfaces_started) eigrp_owrt_interface_stop();
	if (rib_started) eigrp_owrt_rib_stop();
	if (core_started) eigrp_terminate(); /* also finishes host event runtime */
	if (core_started) eigrp_sys_policy_finish();
	eigrp_owrt_config_policy_bind(NULL);
	eigrp_owrt_config_free(active);
	eigrp_owrt_config_free(candidate);
	if (signal_fd >= 0) close(signal_fd);
	fprintf(stderr, "openeigrpd: stopped\n");
	return rc;
}

int main(int argc, char **argv)
{
	const char *config_dir = NULL, *socket_path = EIGRP_OWRT_SOCKET_DEFAULT;
	eigrp_owrt_config_t *candidate = NULL;
	eigrp_owrt_config_diagnostic_t diagnostic = {0};
	eigrp_result_t result;
	bool check = false;
	int i;
	for (i=1; i<argc; i++) {
		if (!strcmp(argv[i], "--check")) check = true;
		else if (!strcmp(argv[i], "--foreground")) { /* procd runs in foreground */ }
		else if (!strcmp(argv[i], "--config-dir") && i+1 < argc) config_dir = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i+1 < argc) socket_path = argv[++i];
		else if (!strcmp(argv[i], "--version")) {puts("OpenEIGRP OpenWrt daemon OW-09"); return 0;}
		else if (!strcmp(argv[i], "--help")) {eigrp_owrt_daemon_usage(stdout); return 0;}
		else {eigrp_owrt_daemon_usage(stderr); return 2;}
	}
	if (!socket_path[0]) return 2;
	result = eigrp_owrt_uci_load(config_dir, &candidate, &diagnostic);
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_owrt_daemon_diagnostic("UCI validation", result, &diagnostic);
		return 1;
	}
	if (check) {
		eigrp_owrt_config_free(candidate);
		puts("openeigrpd: UCI configuration valid");
		return 0;
	}
	return eigrp_owrt_daemon_run(config_dir, socket_path, candidate);
}
