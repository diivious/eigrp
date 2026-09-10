// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP named-mode configuration ownership.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_named.h"

struct eigrp_named_network {
	eigrp_named_prefix_t prefix;
	struct eigrp_named_network *next;
};

struct eigrp_named_neighbor {
	eigrp_named_address_t address;
	char *interface_name;
	struct eigrp_named_neighbor *next;
};

struct eigrp_named_summary_address {
	eigrp_named_address_t address;
	eigrp_named_address_t mask;
	struct eigrp_named_summary_address *next;
};

struct eigrp_named_af_interface {
	char *interface_name;
	bool bandwidth_percent_configured;
	uint32_t bandwidth_percent;
	bool hello_interval_configured;
	uint16_t hello_interval;
	bool hold_time_configured;
	uint16_t hold_time;
	bool passive;
	bool authentication_mode_configured;
	eigrp_named_authentication_mode_t authentication_mode;
	char *keychain;
	bool next_hop_self;
	bool split_horizon;
	bool shutdown;
	struct eigrp_named_summary_address *summaries;
	struct eigrp_named_af_interface *next;
};

struct eigrp_named_address_family {
	eigrp_address_family_t afi;
	uint16_t asn;
	char *vrf_name;
	bool router_id_configured;
	uint32_t router_id;
	bool shutdown;
	struct eigrp_named_network *networks;
	struct eigrp_named_neighbor *neighbors;
	eigrp_named_af_interface_t *af_interfaces;
	struct eigrp_named_address_family *next;
};

struct eigrp_named_process {
	char *name;
	eigrp_named_address_family_t *address_families;
	struct eigrp_named_process *next;
};

static eigrp_named_process_t *eigrp_named_processes;

static char *eigrp_named_string_duplicate(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;

	len = strlen(value) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;
	memcpy(copy, value, len);
	return copy;
}

static bool eigrp_named_afi_valid(eigrp_address_family_t afi)
{
	return afi == EIGRP_ADDRESS_FAMILY_IPV4
	       || afi == EIGRP_ADDRESS_FAMILY_IPV6;
}

eigrp_named_process_t *eigrp_named_process_lookup(const char *name)
{
	eigrp_named_process_t *process;

	if (!name || !name[0])
		return NULL;

	for (process = eigrp_named_processes; process; process = process->next) {
		if (strcmp(process->name, name) == 0)
			return process;
	}
	return NULL;
}

eigrp_result_t eigrp_named_process_create(const char *name)
{
	eigrp_named_process_t *process;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (eigrp_named_process_lookup(name))
		return EIGRP_RESULT_SUCCESS;

	process = calloc(1, sizeof(*process));
	if (!process)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	process->name = eigrp_named_string_duplicate(name);
	if (!process->name) {
		free(process);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	process->next = eigrp_named_processes;
	eigrp_named_processes = process;
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_named_networks_free(eigrp_named_address_family_t *af)
{
	struct eigrp_named_network *network;
	struct eigrp_named_network *next;

	for (network = af->networks; network; network = next) {
		next = network->next;
		free(network);
	}
	af->networks = NULL;
}

static void eigrp_named_neighbors_free(eigrp_named_address_family_t *af)
{
	struct eigrp_named_neighbor *neighbor;
	struct eigrp_named_neighbor *next;

	for (neighbor = af->neighbors; neighbor; neighbor = next) {
		next = neighbor->next;
		free(neighbor->interface_name);
		free(neighbor);
	}
	af->neighbors = NULL;
}

static void eigrp_named_summaries_free(eigrp_named_af_interface_t *interface)
{
	struct eigrp_named_summary_address *summary;
	struct eigrp_named_summary_address *next;

	for (summary = interface->summaries; summary; summary = next) {
		next = summary->next;
		free(summary);
	}
	interface->summaries = NULL;
}

static void eigrp_named_af_interfaces_free(eigrp_named_address_family_t *af)
{
	eigrp_named_af_interface_t *interface;
	eigrp_named_af_interface_t *next;

	for (interface = af->af_interfaces; interface; interface = next) {
		next = interface->next;
		eigrp_named_summaries_free(interface);
		free(interface->keychain);
		free(interface->interface_name);
		free(interface);
	}
	af->af_interfaces = NULL;
}

static void eigrp_named_address_family_free(eigrp_named_address_family_t *af)
{
	if (!af)
		return;
	eigrp_named_networks_free(af);
	eigrp_named_neighbors_free(af);
	eigrp_named_af_interfaces_free(af);
	free(af->vrf_name);
	free(af);
}

static void eigrp_named_address_families_free(eigrp_named_process_t *process)
{
	eigrp_named_address_family_t *af;
	eigrp_named_address_family_t *next;

	for (af = process->address_families; af; af = next) {
		next = af->next;
		eigrp_named_address_family_free(af);
	}
	process->address_families = NULL;
}

eigrp_result_t eigrp_named_process_delete(const char *name)
{
	eigrp_named_process_t **cursor;
	eigrp_named_process_t *process;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (cursor = &eigrp_named_processes; *cursor; cursor = &(*cursor)->next) {
		process = *cursor;
		if (strcmp(process->name, name) != 0)
			continue;

		*cursor = process->next;
		eigrp_named_address_families_free(process);
		free(process->name);
		free(process);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_named_address_family_t *eigrp_named_address_family_lookup(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_process_t *process;
	eigrp_named_address_family_t *af;

	process = eigrp_named_process_lookup(name);
	if (!process || !vrf_name)
		return NULL;

	for (af = process->address_families; af; af = af->next) {
		if (af->afi == afi && af->asn == asn
		    && strcmp(af->vrf_name, vrf_name) == 0)
			return af;
	}
	return NULL;
}

eigrp_result_t eigrp_named_address_family_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_process_t *process;
	eigrp_named_address_family_t *af;
	eigrp_result_t result;

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_named_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	result = eigrp_named_process_create(name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_SUCCESS;

	process = eigrp_named_process_lookup(name);
	if (!process)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	af = calloc(1, sizeof(*af));
	if (!af)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	af->vrf_name = eigrp_named_string_duplicate(vrf_name);
	if (!af->vrf_name) {
		free(af);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	af->afi = afi;
	af->asn = asn;
	af->next = process->address_families;
	process->address_families = af;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_address_family_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_process_t *process;
	eigrp_named_address_family_t **cursor;
	eigrp_named_address_family_t *af;

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_named_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	process = eigrp_named_process_lookup(name);
	if (!process)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &process->address_families; *cursor;
	     cursor = &(*cursor)->next) {
		af = *cursor;
		if (af->afi != afi || af->asn != asn
		    || strcmp(af->vrf_name, vrf_name) != 0)
			continue;

		*cursor = af->next;
		eigrp_named_address_family_free(af);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

static bool eigrp_named_address_equal(const eigrp_named_address_t *a,
				      const eigrp_named_address_t *b)
{
	size_t len;

	if (!a || !b || a->afi != b->afi)
		return false;
	len = a->afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4 : 16;
	return memcmp(a->bytes, b->bytes, len) == 0;
}

static bool eigrp_named_prefix_equal(const eigrp_named_prefix_t *a,
				     const eigrp_named_prefix_t *b)
{
	return a && b && a->prefix_length == b->prefix_length
	       && eigrp_named_address_equal(&a->address, &b->address);
}

eigrp_result_t eigrp_named_router_id_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint32_t router_id)
{
	eigrp_named_address_family_t *af;

	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	af->router_id = router_id;
	af->router_id_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_router_id_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_address_family_t *af;

	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	af->router_id = 0;
	af->router_id_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_network_add(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix)
{
	eigrp_named_address_family_t *af;
	struct eigrp_named_network *network;

	if (!prefix || prefix->address.afi != afi
	    || afi != EIGRP_ADDRESS_FAMILY_IPV4 || prefix->prefix_length > 32)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	for (network = af->networks; network; network = network->next)
		if (eigrp_named_prefix_equal(&network->prefix, prefix))
			return EIGRP_RESULT_SUCCESS;
	network = calloc(1, sizeof(*network));
	if (!network)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	network->prefix = *prefix;
	network->next = af->networks;
	af->networks = network;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_network_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix)
{
	eigrp_named_address_family_t *af;
	struct eigrp_named_network **cursor;
	struct eigrp_named_network *network;

	if (!prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	for (cursor = &af->networks; *cursor; cursor = &(*cursor)->next) {
		network = *cursor;
		if (!eigrp_named_prefix_equal(&network->prefix, prefix))
			continue;
		*cursor = network->next;
		free(network);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_named_neighbor_add(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_address_t *address,
	const char *interface_name)
{
	eigrp_named_address_family_t *af;
	struct eigrp_named_neighbor *neighbor;

	if (!address || address->afi != afi || !interface_name || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	for (neighbor = af->neighbors; neighbor; neighbor = neighbor->next)
		if (eigrp_named_address_equal(&neighbor->address, address)
		    && strcmp(neighbor->interface_name, interface_name) == 0)
			return EIGRP_RESULT_SUCCESS;
	neighbor = calloc(1, sizeof(*neighbor));
	if (!neighbor)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	neighbor->interface_name = eigrp_named_string_duplicate(interface_name);
	if (!neighbor->interface_name) {
		free(neighbor);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	neighbor->address = *address;
	neighbor->next = af->neighbors;
	af->neighbors = neighbor;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_neighbor_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_address_t *address,
	const char *interface_name)
{
	eigrp_named_address_family_t *af;
	struct eigrp_named_neighbor **cursor;
	struct eigrp_named_neighbor *neighbor;

	if (!address || !interface_name)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	for (cursor = &af->neighbors; *cursor; cursor = &(*cursor)->next) {
		neighbor = *cursor;
		if (!eigrp_named_address_equal(&neighbor->address, address)
		    || strcmp(neighbor->interface_name, interface_name) != 0)
			continue;
		*cursor = neighbor->next;
		free(neighbor->interface_name);
		free(neighbor);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_named_address_family_shutdown_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, bool shutdown)
{
	eigrp_named_address_family_t *af;

	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	af->shutdown = shutdown;
	return EIGRP_RESULT_SUCCESS;
}

static bool eigrp_named_ipv4_pair_valid(const eigrp_named_address_t *address,
				       const eigrp_named_address_t *mask)
{
	return address && mask && address->afi == EIGRP_ADDRESS_FAMILY_IPV4
	       && mask->afi == EIGRP_ADDRESS_FAMILY_IPV4;
}

eigrp_named_af_interface_t *eigrp_named_af_interface_lookup(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_address_family_t *af;
	eigrp_named_af_interface_t *interface;

	if (!interface_name || !interface_name[0])
		return NULL;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return NULL;
	for (interface = af->af_interfaces; interface; interface = interface->next)
		if (strcmp(interface->interface_name, interface_name) == 0)
			return interface;
	return NULL;
}

eigrp_result_t eigrp_named_af_interface_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_address_family_t *af;
	eigrp_named_af_interface_t *interface;

	if (!interface_name || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_named_af_interface_lookup(name, afi, vrf_name, asn,
					    interface_name))
		return EIGRP_RESULT_SUCCESS;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	interface = calloc(1, sizeof(*interface));
	if (!interface)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	interface->interface_name = eigrp_named_string_duplicate(interface_name);
	if (!interface->interface_name) {
		free(interface);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	interface->next_hop_self = true;
	interface->split_horizon = true;
	interface->next = af->af_interfaces;
	af->af_interfaces = interface;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_address_family_t *af;
	eigrp_named_af_interface_t **cursor;
	eigrp_named_af_interface_t *interface;

	if (!interface_name || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_named_address_family_lookup(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	for (cursor = &af->af_interfaces; *cursor; cursor = &(*cursor)->next) {
		interface = *cursor;
		if (strcmp(interface->interface_name, interface_name) != 0)
			continue;
		*cursor = interface->next;
		eigrp_named_summaries_free(interface);
		free(interface->keychain);
		free(interface->interface_name);
		free(interface);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

static eigrp_named_af_interface_t *eigrp_named_af_interface_require(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	return eigrp_named_af_interface_lookup(name, afi, vrf_name, asn,
					       interface_name);
}

eigrp_result_t eigrp_named_af_interface_bandwidth_percent_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, uint32_t percent)
{
	eigrp_named_af_interface_t *interface;

	if (percent == 0 || percent > 999999)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->bandwidth_percent = percent;
	interface->bandwidth_percent_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_bandwidth_percent_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->bandwidth_percent = 0;
	interface->bandwidth_percent_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_hello_interval_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, uint16_t seconds)
{
	eigrp_named_af_interface_t *interface;

	if (seconds == 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->hello_interval = seconds;
	interface->hello_interval_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_hello_interval_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->hello_interval = 0;
	interface->hello_interval_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_hold_time_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, uint16_t seconds)
{
	eigrp_named_af_interface_t *interface;

	if (seconds == 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->hold_time = seconds;
	interface->hold_time_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_hold_time_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->hold_time = 0;
	interface->hold_time_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_passive_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool passive)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->passive = passive;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_authentication_mode_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name,
	eigrp_named_authentication_mode_t mode)
{
	eigrp_named_af_interface_t *interface;

	if (mode != EIGRP_NAMED_AUTHENTICATION_MD5
	    && mode != EIGRP_NAMED_AUTHENTICATION_HMAC_SHA256)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->authentication_mode = mode;
	interface->authentication_mode_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_authentication_mode_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->authentication_mode = EIGRP_NAMED_AUTHENTICATION_NONE;
	interface->authentication_mode_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_keychain_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, const char *keychain)
{
	eigrp_named_af_interface_t *interface;
	char *copy;

	if (!keychain || !keychain[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	copy = eigrp_named_string_duplicate(keychain);
	if (!copy)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	free(interface->keychain);
	interface->keychain = copy;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_keychain_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	free(interface->keychain);
	interface->keychain = NULL;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_next_hop_self_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool enabled)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->next_hop_self = enabled;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_split_horizon_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool enabled)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->split_horizon = enabled;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_summary_add(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name,
	const eigrp_named_address_t *address, const eigrp_named_address_t *mask)
{
	eigrp_named_af_interface_t *interface;
	struct eigrp_named_summary_address *summary;

	if (afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || !eigrp_named_ipv4_pair_valid(address, mask))
		return EIGRP_RESULT_UNSUPPORTED;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	for (summary = interface->summaries; summary; summary = summary->next)
		if (eigrp_named_address_equal(&summary->address, address)
		    && eigrp_named_address_equal(&summary->mask, mask))
			return EIGRP_RESULT_SUCCESS;
	summary = calloc(1, sizeof(*summary));
	if (!summary)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	summary->address = *address;
	summary->mask = *mask;
	summary->next = interface->summaries;
	interface->summaries = summary;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_af_interface_summary_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name,
	const eigrp_named_address_t *address, const eigrp_named_address_t *mask)
{
	eigrp_named_af_interface_t *interface;
	struct eigrp_named_summary_address **cursor;
	struct eigrp_named_summary_address *summary;

	if (!eigrp_named_ipv4_pair_valid(address, mask))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	for (cursor = &interface->summaries; *cursor; cursor = &(*cursor)->next) {
		summary = *cursor;
		if (!eigrp_named_address_equal(&summary->address, address)
		    || !eigrp_named_address_equal(&summary->mask, mask))
			continue;
		*cursor = summary->next;
		free(summary);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_named_af_interface_shutdown_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool shutdown)
{
	eigrp_named_af_interface_t *interface;

	interface = eigrp_named_af_interface_require(name, afi, vrf_name, asn,
						      interface_name);
	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	interface->shutdown = shutdown;
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_named_finish(void)
{
	eigrp_named_process_t *process;
	eigrp_named_process_t *next;

	for (process = eigrp_named_processes; process; process = next) {
		next = process->next;
		eigrp_named_address_families_free(process);
		free(process->name);
		free(process);
	}
	eigrp_named_processes = NULL;
}

eigrp_result_t eigrp_named_process_shutdown_set(const char *name, bool shutdown)
{
	(void)shutdown;
	if (!eigrp_named_process_lookup(name))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_distance_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint8_t internal_distance, uint8_t external_distance)
{
	if (!internal_distance || !external_distance)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_offset_list_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *access_list, eigrp_named_offset_direction_t direction,
	uint32_t offset, const char *interface_name)
{
	(void)offset;
	(void)interface_name;
	if (!access_list || !access_list[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (direction != EIGRP_NAMED_OFFSET_IN
	    && direction != EIGRP_NAMED_OFFSET_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_summary_metric_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix,
	const eigrp_named_metric_values_t *metric)
{
	if (!prefix || !metric)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

static eigrp_result_t eigrp_named_target_context_validate(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_named_topology_base_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	return eigrp_named_target_context_validate(name, afi, vrf_name, asn) ==
		       EIGRP_RESULT_SUCCESS
	       ? EIGRP_RESULT_NOT_IMPLEMENTED
	       : EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_named_topology_base_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	return eigrp_named_topology_base_create(name, afi, vrf_name, asn);
}

eigrp_result_t eigrp_named_auto_summary_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, bool enabled)
{
	(void)enabled;
	if (afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return EIGRP_RESULT_UNSUPPORTED;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_default_information_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, eigrp_named_default_information_direction_t direction,
	bool enabled)
{
	(void)enabled;
	if (direction != EIGRP_NAMED_DEFAULT_INFORMATION_IN
	    && direction != EIGRP_NAMED_DEFAULT_INFORMATION_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_default_metric_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_metric_values_t *metric)
{
	if (!metric || !metric->bandwidth || !metric->load || !metric->mtu)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_default_metric_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_distance_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_maximum_prefix_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint32_t maximum)
{
	if (!maximum)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_maximum_prefix_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_metric_weights_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_metric_weights_t *weights)
{
	if (!weights || weights->tos != 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_metric_weights_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_offset_list_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *access_list, eigrp_named_offset_direction_t direction,
	uint32_t offset, const char *interface_name)
{
	return eigrp_named_offset_list_set(name, afi, vrf_name, asn, access_list,
					   direction, offset, interface_name);
}

eigrp_result_t eigrp_named_redistribute_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *protocol, const eigrp_named_metric_values_t *metric)
{
	if (!protocol || !protocol[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (metric && (!metric->bandwidth || !metric->load || !metric->mtu))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_redistribute_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *protocol)
{
	return eigrp_named_redistribute_set(name, afi, vrf_name, asn, protocol, NULL);
}

eigrp_result_t eigrp_named_summary_metric_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix)
{
	if (!prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_active_time_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint16_t seconds)
{
	(void)seconds;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_active_time_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_traffic_share_balanced_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, bool enabled)
{
	(void)enabled;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_variance_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint8_t variance)
{
	if (variance < 1 || variance > 128)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_named_variance_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	if (!eigrp_named_address_family_lookup(name, afi, vrf_name, asn))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
