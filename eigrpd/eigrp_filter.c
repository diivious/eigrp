// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Filter Functions.
 * Copyright (C) 2013-2015
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_const.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_southbound.h"

#include "plist.h"
#include "privs.h"

/*
 * FRR policy objects still consume struct prefix.  Keep that host-specific
 * representation local to the filter boundary while protocol callers pass the
 * native EIGRP prefix representation.
 */
bool eigrp_filter_prefix_apply(eigrp_instance_t *eigrp,
			       eigrp_interface_t *ei, int direction,
			       const eigrp_prefix_t *prefix)
{
	struct access_list *alist;
	struct prefix_list *plist;
	struct prefix host_prefix;

	if (!eigrp || !ei || !prefix)
		return false;

	memset(&host_prefix, 0, sizeof(host_prefix));
	if (prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV4) {
		if (prefix->prefix_length > IPV4_MAX_BITLEN)
			return false;
		host_prefix.family = AF_INET;
		host_prefix.prefixlen = prefix->prefix_length;
		memcpy(&host_prefix.u.prefix4, prefix->address.bytes,
		       sizeof(host_prefix.u.prefix4));
	} else if (prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV6) {
		if (prefix->prefix_length > 128)
			return false;
		host_prefix.family = AF_INET6;
		host_prefix.prefixlen = prefix->prefix_length;
		memcpy(&host_prefix.u.prefix6, prefix->address.bytes,
		       sizeof(host_prefix.u.prefix6));
	} else {
		return false;
	}

	alist = eigrp->list[direction];
	if (alist && access_list_apply(alist, &host_prefix) == FILTER_DENY)
		return true;

	plist = eigrp->prefix[direction];
	if (plist && prefix_list_apply(plist, &host_prefix) == PREFIX_DENY)
		return true;

	alist = ei->list[direction];
	if (alist && access_list_apply(alist, &host_prefix) == FILTER_DENY)
		return true;

	plist = ei->prefix[direction];
	if (plist && prefix_list_apply(plist, &host_prefix) == PREFIX_DENY)
		return true;

	return false;
}

/*
 * Distribute-list update functions.
 */
static eigrp_instance_t *eigrp_distribute_instance_lookup(
	struct distribute_ctx *ctx)
{
	eigrp_instance_t *eigrp;
	struct listnode *node;

	if (!ctx || !eigrp_om || !eigrp_om->eigrp)
		return NULL;

	for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (eigrp->distribute_ctx == ctx)
			return eigrp;
	}

	return NULL;
}

void eigrp_distribute_update(struct distribute_ctx *ctx,
			     struct distribute *dist)
{
	eigrp_instance_t *eigrp = eigrp_distribute_instance_lookup(ctx);
	eigrp_interface_t *ei = NULL;
	struct access_list *alist;
	struct prefix_list *plist;
	// struct route_map *routemap;

	if (!eigrp || !dist)
		return;

	/* if no interface address is present, set list to eigrp process struct
	 */

	/* Check if distribute-list was set for process or interface */
	if (!dist->ifname) {
		/* access list IN for whole process */
		if (dist->list[DISTRIBUTE_V4_IN]) {
			alist = access_list_lookup(
				AFI_IP, dist->list[DISTRIBUTE_V4_IN]);
			if (alist)
				eigrp->list[EIGRP_FILTER_IN] = alist;
			else
				eigrp->list[EIGRP_FILTER_IN] = NULL;
		} else {
			eigrp->list[EIGRP_FILTER_IN] = NULL;
		}

		/* access list OUT for whole process */
		if (dist->list[DISTRIBUTE_V4_OUT]) {
			alist = access_list_lookup(
				AFI_IP, dist->list[DISTRIBUTE_V4_OUT]);
			if (alist)
				eigrp->list[EIGRP_FILTER_OUT] = alist;
			else
				eigrp->list[EIGRP_FILTER_OUT] = NULL;
		} else {
			eigrp->list[EIGRP_FILTER_OUT] = NULL;
		}

		/* PREFIX_LIST IN for process */
		if (dist->prefix[DISTRIBUTE_V4_IN]) {
			plist = prefix_list_lookup(
				AFI_IP, dist->prefix[DISTRIBUTE_V4_IN]);
			if (plist) {
				eigrp->prefix[EIGRP_FILTER_IN] = plist;
			} else
				eigrp->prefix[EIGRP_FILTER_IN] = NULL;
		} else
			eigrp->prefix[EIGRP_FILTER_IN] = NULL;

		/* PREFIX_LIST OUT for process */
		if (dist->prefix[DISTRIBUTE_V4_OUT]) {
			plist = prefix_list_lookup(
				AFI_IP, dist->prefix[DISTRIBUTE_V4_OUT]);
			if (plist) {
				eigrp->prefix[EIGRP_FILTER_OUT] = plist;

			} else
				eigrp->prefix[EIGRP_FILTER_OUT] = NULL;
		} else
			eigrp->prefix[EIGRP_FILTER_OUT] = NULL;

// This is commented out, because the distribute.[ch] code
// changes looked poorly written from first glance
// commit was 133bdf2d
// TODO: DBS
#if 0
	/* route-map IN for whole process */
	if (dist->route[DISTRIBUTE_V4_IN])
        {
	    routemap = route_map_lookup_by_name (dist->route[DISTRIBUTE_V4_IN]);
	    if (routemap)
		eigrp->routemap[EIGRP_FILTER_IN] = routemap;
	    else
		eigrp->routemap[EIGRP_FILTER_IN] = NULL;
        }
	else
        {
	    eigrp->routemap[EIGRP_FILTER_IN] = NULL;
        }

	/* route-map OUT for whole process */
	if (dist->route[DISTRIBUTE_V4_OUT])
        {
	    routemap = route_map_lookup_by_name (dist->route[DISTRIBUTE_V4_OUT]);
	    if (routemap)
		eigrp->routemap[EIGRP_FILTER_OUT] = routemap;
	    else
		eigrp->routemap[EIGRP_FILTER_OUT] = NULL;
        }
	else
        {
	    eigrp->routemap[EIGRP_FILTER_OUT] = NULL;
        }
#endif
		// TODO: check Graceful restart after 10sec

		/* check if there is already GR scheduled */
		if (eigrp->t_distribute != NULL)
			eigrp_southbound_event_cancel(&eigrp->t_distribute);
		eigrp_southbound_timer_add(&eigrp->t_distribute,
			eigrp_distribute_timer_process, eigrp, 10);

		return;
	}

	ei = eigrp_intf_lookup_by_name(eigrp, dist->ifname);
	if (!ei)
		return;

	/* Access-list for interface in */
	if (dist->list[DISTRIBUTE_V4_IN]) {
		alist = access_list_lookup(AFI_IP,
					   dist->list[DISTRIBUTE_V4_IN]);
		if (alist) {
			ei->list[EIGRP_FILTER_IN] = alist;
		} else
			ei->list[EIGRP_FILTER_IN] = NULL;
	} else {
		ei->list[EIGRP_FILTER_IN] = NULL;
	}

	/* Access-list for interface in */
	if (dist->list[DISTRIBUTE_V4_OUT]) {
		alist = access_list_lookup(AFI_IP,
					   dist->list[DISTRIBUTE_V4_OUT]);
		if (alist)
			ei->list[EIGRP_FILTER_OUT] = alist;
		else
			ei->list[EIGRP_FILTER_OUT] = NULL;

	} else
		ei->list[EIGRP_FILTER_OUT] = NULL;

	/* Prefix-list for interface in */
	if (dist->prefix[DISTRIBUTE_V4_IN]) {
		plist = prefix_list_lookup(AFI_IP,
					   dist->prefix[DISTRIBUTE_V4_IN]);
		if (plist)
			ei->prefix[EIGRP_FILTER_IN] = plist;
		else
			ei->prefix[EIGRP_FILTER_IN] = NULL;
	} else
		ei->prefix[EIGRP_FILTER_IN] = NULL;

	/* Prefix-list for interface out */
	if (dist->prefix[DISTRIBUTE_V4_OUT]) {
		plist = prefix_list_lookup(AFI_IP,
					   dist->prefix[DISTRIBUTE_V4_OUT]);
		if (plist)
			ei->prefix[EIGRP_FILTER_OUT] = plist;
		else
			ei->prefix[EIGRP_FILTER_OUT] = NULL;
	} else
		ei->prefix[EIGRP_FILTER_OUT] = NULL;

	// TODO: check Graceful restart after 10sec

	/* Cancel and reschedule GR for this interface. */
	eigrp_southbound_event_cancel(&ei->t_distribute);
	eigrp_southbound_timer_add(&ei->t_distribute,
		eigrp_distribute_timer_interface, ei, 10);
}

/*
 * Function called by prefix-list and access-list update
 */
static void eigrp_distribute_update_interface(eigrp_instance_t *eigrp,
					      const char *interface_name)
{
	struct distribute *dist;

	if (!eigrp || !interface_name)
		return;

	dist = distribute_lookup(eigrp->distribute_ctx, interface_name);
	if (dist)
		eigrp_distribute_update(eigrp->distribute_ctx, dist);
}

/* Update all runtime interfaces after a prefix/access-list change. */
void eigrp_distribute_update_all(struct prefix_list *notused)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	struct listnode *instance_node;
	struct listnode *interface_node;

	(void)notused;
	if (!eigrp_om || !eigrp_om->eigrp)
		return;

	for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp, instance_node, eigrp))
		for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, interface_node, ei))
			eigrp_distribute_update_interface(eigrp, ei->name);
}

void eigrp_distribute_update_all_wrapper(struct access_list *notused)
{
	(void)notused;
	eigrp_distribute_update_all(NULL);
}

void eigrp_distribute_timer_process(void *arg)
{
	eigrp_instance_t *eigrp = arg;

	if (!eigrp)
		return;
	eigrp->t_distribute = NULL;
	eigrp_update_send_process_GR(eigrp, EIGRP_GR_FILTER, NULL);
}

void eigrp_distribute_timer_interface(void *arg)
{
	eigrp_interface_t *ei = arg;

	if (!ei)
		return;
	ei->t_distribute = NULL;
	eigrp_update_send_interface_GR(ei, EIGRP_GR_FILTER, NULL);
}

eigrp_result_t eigrp_offset_update(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name)
{
	(void)offset;
	(void)interface_name;
	if (!access_list || !access_list[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (direction != EIGRP_OFFSET_IN && direction != EIGRP_OFFSET_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_offset_delete(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name)
{
	return eigrp_offset_update(context, access_list, direction, offset,
				   interface_name);
}

struct eigrp_distribute_list_config {
	eigrp_distribute_list_type_t type;
	eigrp_offset_direction_t direction;
	char *name;
	char *interface_name;
	eigrp_distribute_list_config_t *next;
};

static char *eigrp_distribute_string_duplicate(const char *value)
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

static bool eigrp_distribute_interface_equal(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return strcmp(a, b) == 0;
}

static eigrp_distribute_list_config_t *eigrp_distribute_list_config_find(
	eigrp_address_family_config_t *af, eigrp_distribute_list_type_t type,
	eigrp_offset_direction_t direction, const char *interface_name)
{
	eigrp_distribute_list_config_t *config;

	if (!af)
		return NULL;
	for (config = af->distribute_lists; config; config = config->next) {
		if (config->type == type && config->direction == direction
		    && eigrp_distribute_interface_equal(config->interface_name,
							 interface_name))
			return config;
	}
	return NULL;
}

static eigrp_result_t eigrp_distribute_list_validate(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (interface_name && !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (type != EIGRP_DISTRIBUTE_ACCESS_LIST
	    && type != EIGRP_DISTRIBUTE_PREFIX_LIST)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (direction != EIGRP_OFFSET_IN && direction != EIGRP_OFFSET_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

static bool eigrp_distribute_runtime_result_committable(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS
	       || result == EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_distribute_list_update(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	eigrp_distribute_list_config_t *config = NULL;
	eigrp_distribute_list_config_t *new_config = NULL;
	char *new_name;
	eigrp_result_t result;

	result = eigrp_distribute_list_validate(context, type, name, direction,
						 interface_name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	new_name = eigrp_distribute_string_duplicate(name);
	if (!new_name)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	if (context->config) {
		config = eigrp_distribute_list_config_find(
			context->config, type, direction, interface_name);
		if (!config) {
			new_config = calloc(1, sizeof(*new_config));
			if (!new_config) {
				free(new_name);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
			new_config->type = type;
			new_config->direction = direction;
			new_config->name = new_name;
			new_name = NULL;
			if (interface_name) {
				new_config->interface_name =
					eigrp_distribute_string_duplicate(interface_name);
				if (!new_config->interface_name) {
					free(new_config->name);
					free(new_config);
					return EIGRP_RESULT_INTERNAL_FAILURE;
				}
			}
		}
	}

	result = EIGRP_RESULT_SUCCESS;
	if (context->runtime)
		result = eigrp_southbound_distribute_list_update(
			context->runtime, type, name, direction, interface_name);
	if (!eigrp_distribute_runtime_result_committable(result)) {
		free(new_name);
		if (new_config) {
			free(new_config->interface_name);
			free(new_config->name);
			free(new_config);
		}
		return result;
	}

	if (context->config) {
		if (config) {
			free(config->name);
			config->name = new_name;
			new_name = NULL;
		} else if (new_config) {
			new_config->next = context->config->distribute_lists;
			context->config->distribute_lists = new_config;
			new_config = NULL;
		}
	}

	free(new_name);
	return result;
}

eigrp_result_t eigrp_distribute_list_delete(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	eigrp_distribute_list_config_t **cursor = NULL;
	eigrp_distribute_list_config_t *config = NULL;
	eigrp_result_t result;

	result = eigrp_distribute_list_validate(context, type, name, direction,
						 interface_name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (context->config) {
		for (cursor = &context->config->distribute_lists; *cursor;
		     cursor = &(*cursor)->next) {
			if ((*cursor)->type != type
			    || (*cursor)->direction != direction
			    || strcmp((*cursor)->name, name) != 0
			    || !eigrp_distribute_interface_equal(
				    (*cursor)->interface_name, interface_name))
				continue;
			config = *cursor;
			break;
		}
		/* The retained named filter entry is the ownership marker for this
		 * host mutation.  Do not clear a filter installed by classic FRR
		 * configuration when no named entry exists.
		 */
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
	}

	result = EIGRP_RESULT_NOT_FOUND;
	if (context->runtime) {
		result = eigrp_southbound_distribute_list_delete(
			context->runtime, type, name, direction, interface_name);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND
		    && result != EIGRP_RESULT_NOT_IMPLEMENTED)
			return result;
	}

	if (config) {
		*cursor = config->next;
		free(config->interface_name);
		free(config->name);
		free(config);
		if (result == EIGRP_RESULT_NOT_FOUND || !context->runtime)
			result = EIGRP_RESULT_SUCCESS;
	}
	return result;
}

void eigrp_distribute_list_config_delete_all(eigrp_address_family_config_t *af)
{
	eigrp_distribute_list_config_t *config;
	eigrp_distribute_list_config_t *next;

	if (!af)
		return;
	for (config = af->distribute_lists; config; config = next) {
		next = config->next;
		free(config->interface_name);
		free(config->name);
		free(config);
	}
	af->distribute_lists = NULL;
}
