// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt read-only local management over a protected Unix socket.
 * All protocol state is obtained through public eigrp_mgnt.h snapshots.
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_owrt_mgnt.h"
#include "eigrp_owrt_runtime.h"

struct eigrp_owrt_mgnt_output {
	FILE *out;
	const char *command;
	unsigned entries;
};

static const char *eigrp_owrt_mgnt_address(const eigrp_address_t *address,
	char *buf, size_t len)
{
	int af;
	if (!address)
		return "?";
	af = address->afi == EIGRP_AFI_IPV4 ? AF_INET :
		address->afi == EIGRP_AFI_IPV6 ? AF_INET6 : AF_UNSPEC;
	return af != AF_UNSPEC && inet_ntop(af, address->bytes, buf, len) ? buf : "?";
}

static eigrp_result_t eigrp_owrt_mgnt_interface_cb(
	const eigrp_intf_state_t *s, void *arg)
{
	struct eigrp_owrt_mgnt_output *ctx = arg;
	fprintf(ctx->out, "interface=%s present=%u passive=%u shutdown=%u mtu=%u peers=%u hello=%u hold=%u\n",
		s->interface_name ? s->interface_name : "?", s->runtime_present,
		s->passive, s->shutdown, s->mtu, s->peer_count,
		s->hello_interval, s->hold_time);
	ctx->entries++;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_owrt_mgnt_neighbor_cb(
	const eigrp_nbr_state_t *s, void *arg)
{
	struct eigrp_owrt_mgnt_output *ctx = arg;
	char ip[INET6_ADDRSTRLEN];
	fprintf(ctx->out, "neighbor=%s interface=%s state=%s uptime=%llu srtt=%u rto=%u retries=%u\n",
		eigrp_owrt_mgnt_address(&s->address, ip, sizeof(ip)),
		s->interface_name ? s->interface_name : "?",
		s->state_name ? s->state_name : "?",
		(unsigned long long)s->uptime_seconds, s->srtt_msec,
		s->rto_msec, s->retry_count);
	ctx->entries++;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_owrt_mgnt_prefix_cb(
	const eigrp_topology_prefix_state_t *s, void *arg)
{
	struct eigrp_owrt_mgnt_output *ctx = arg;
	char ip[INET6_ADDRSTRLEN];
	fprintf(ctx->out, "prefix=%s/%u active=%u feasible_distance=%u successors=%u\n",
		eigrp_owrt_mgnt_address(&s->destination.address, ip, sizeof(ip)),
		s->destination.prefix_length, s->active, s->feasible_distance,
		s->successor_count);
	ctx->entries++;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_owrt_mgnt_route_cb(
	const eigrp_topology_route_state_t *s, void *arg)
{
	struct eigrp_owrt_mgnt_output *ctx = arg;
	char ip[INET6_ADDRSTRLEN];
	fprintf(ctx->out, " route_nexthop=%s interface=%s distance=%u reported=%u successor=%u feasible=%u\n",
		eigrp_owrt_mgnt_address(&s->next_hop, ip, sizeof(ip)),
		s->interface_name ? s->interface_name : "?", s->distance,
		s->reported_distance, s->successor, s->feasible_successor);
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_owrt_mgnt_event_cb(uint32_t number,
	const eigrp_eventlog_msg_t *s, const char *format, void *arg)
{
	struct eigrp_owrt_mgnt_output *ctx = arg;
	char message[320];
	(void)format;
	if (eigrp_eventlog_msg_format(s, message, sizeof(message)) != EIGRP_RESULT_SUCCESS)
		(void)snprintf(message, sizeof(message), "event opcode=%u", s->opcode);
	fprintf(ctx->out, "event=%u timestamp=%llu %s\n", number,
		(unsigned long long)s->timestamp, message);
	ctx->entries++;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_owrt_mgnt_instance_cb(
	const eigrp_status_protocol_state_t *s, void *arg)
{
	struct eigrp_owrt_mgnt_output *ctx = arg;
	eigrp_instance_context_t instance = {0};
	eigrp_result_t result;
	fprintf(ctx->out, "instance=%s afi=%s asn=%u vrf=%s runtime=%u shutdown=%u\n",
		s->instance_name ? s->instance_name : "?",
		s->afi == EIGRP_AFI_IPV4 ? "ipv4" : "ipv6", s->asn,
		s->vrf_name ? s->vrf_name : "default", s->runtime_present,
		s->shutdown);
	ctx->entries++;
	if (!strcmp(ctx->command, "status"))
		return EIGRP_RESULT_SUCCESS;
	result = eigrp_af_config_context_read(s->instance_name, s->afi,
		s->vrf_name ? s->vrf_name : "default", s->asn, &instance);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!strcmp(ctx->command, "interfaces"))
		result = eigrp_intf_state_iterate(instance.config, instance.runtime,
			NULL, eigrp_owrt_mgnt_interface_cb, ctx);
	else if (!strcmp(ctx->command, "neighbors"))
		result = eigrp_nbr_state_iterate(instance.config, instance.runtime,
			NULL, false, eigrp_owrt_mgnt_neighbor_cb, ctx);
	else if (!strcmp(ctx->command, "topology"))
		result = eigrp_topology_state_iterate(instance.config, instance.runtime,
			NULL, false, eigrp_owrt_mgnt_prefix_cb,
			eigrp_owrt_mgnt_route_cb, ctx);
	else if (!strcmp(ctx->command, "events"))
		result = instance.runtime ? eigrp_eventlog_msg_iterate(&instance,
			eigrp_owrt_mgnt_event_cb, ctx) : EIGRP_RESULT_NOT_FOUND;
	else if (!strcmp(ctx->command, "traffic")) {
		eigrp_statistics_traffic_state_t stats = {0};
		result = eigrp_statistics_traffic_state_read(&instance, &stats);
		if (result == EIGRP_RESULT_SUCCESS) {
			fprintf(ctx->out,
				"traffic sent_hello=%llu received_hello=%llu sent_update=%llu received_update=%llu sent_query=%llu received_query=%llu sent_reply=%llu received_reply=%llu\n",
				(unsigned long long)stats.sent_hello,
				(unsigned long long)stats.received_hello,
				(unsigned long long)stats.sent_update,
				(unsigned long long)stats.received_update,
				(unsigned long long)stats.sent_query,
				(unsigned long long)stats.received_query,
				(unsigned long long)stats.sent_reply,
				(unsigned long long)stats.received_reply);
		}
	}
	return result == EIGRP_RESULT_NOT_FOUND ? EIGRP_RESULT_SUCCESS : result;
}

int eigrp_owrt_mgnt_show(FILE *out, const char *command)
{
	struct eigrp_owrt_mgnt_output ctx = {.out=out, .command=command};
	eigrp_result_t result;
	if (!out || !command)
		return -1;
	if (strcmp(command, "status") && strcmp(command, "interfaces") &&
	    strcmp(command, "neighbors") && strcmp(command, "topology") &&
	    strcmp(command, "traffic") && strcmp(command, "events")) {
		fprintf(out, "error: unknown command\n");
		return -1;
	}
	/* Read-only API. No C pointers or host objects are returned to clients. */
	eigrp_owrt_runtime_enter();
	result = eigrp_status_protocol_iterate(eigrp_owrt_mgnt_instance_cb, &ctx);
	eigrp_owrt_runtime_leave();
	if (result != EIGRP_RESULT_SUCCESS && result != EIGRP_RESULT_NOT_FOUND) {
		fprintf(out, "error: EIGRP management result %d\n", (int)result);
		return -1;
	}
	if (ctx.entries == 0)
		fprintf(out, "no configured instances\n");
	return 0;
}

int eigrp_owrt_mgnt_listen(const char *path)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	struct stat st;
	int fd;
	if (!path || !*path || strlen(path) >= sizeof(address.sun_path))
		return -1;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	if (lstat(path, &st) == 0) {
		int probe;
		int prior_error;
		/* Never replace a live daemon's control socket. */
		if (!S_ISSOCK(st.st_mode))
			return -1;
		probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (probe < 0)
			return -1;
		if (connect(probe, (struct sockaddr *)&address, sizeof(address)) == 0) {
			close(probe);
			errno = EADDRINUSE;
			return -1;
		}
		prior_error = errno;
		close(probe);
		if (prior_error != ECONNREFUSED && prior_error != ENOENT)
			return -1;
		if (unlink(path) != 0)
			return -1;
	} else if (errno != ENOENT)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		/* A racing instance may now own this path; never unlink it. */
		close(fd);
		return -1;
	}
	if (chmod(path, 0600) != 0 || listen(fd, 8) != 0) {
		close(fd);
		(void)unlink(path);
		return -1;
	}
	return fd;
}

void eigrp_owrt_mgnt_handle(int listener)
{
	char command[64];
	ssize_t n;
	struct timeval timeout = {.tv_sec = 2};
	int fd = accept(listener, NULL, NULL);
	FILE *out;
	if (fd < 0)
		return;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	n = read(fd, command, sizeof(command) - 1);
	if (n <= 0) {
		close(fd);
		return;
	}
	command[n] = '\0';
	command[strcspn(command, "\r\n \t")] = '\0';
	out = fdopen(fd, "w");
	if (!out) {
		close(fd);
		return;
	}
	(void)eigrp_owrt_mgnt_show(out, command);
	(void)fclose(out);
}

void eigrp_owrt_mgnt_close(int listener, const char *path)
{
	if (listener >= 0)
		close(listener);
	if (path)
		(void)unlink(path);
}
