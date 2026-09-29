// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eigrp_cli.h"
#include "eigrp_unix_config.h"

static unsigned parent_calls;
static unsigned af_calls;
static unsigned router_id_calls;
static unsigned shutdown_calls;
static bool af_shutdown;
static uint32_t configured_router_id;
static int fake_af;
static int fake_runtime;

eigrp_result_t eigrp_instance_parent_create(const char *name)
{
	assert(strcmp(name, "savage") == 0);
	parent_calls++;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_af_instance_create(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn)
{
	assert(strcmp(name, "savage") == 0);
	assert(afi == EIGRP_AFI_IPV4);
	assert(strcmp(vrf_name, "default") == 0);
	assert(asn == 4453);
	af_calls++;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_af_instance_t *eigrp_af_instance_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name, uint16_t asn)
{
	(void)name;
	(void)afi;
	(void)vrf_name;
	(void)asn;
	return (eigrp_af_instance_t *)&fake_af;
}

eigrp_result_t eigrp_af_instance_context_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name, uint16_t asn,
	eigrp_instance_context_t *context)
{
	(void)name;
	(void)afi;
	(void)vrf_name;
	(void)asn;
	context->config = (eigrp_af_instance_t *)&fake_af;
	context->runtime = (eigrp_instance_t *)&fake_runtime;
	context->topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_router_id_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context, uint32_t router_id)
{
	assert(operation == EIGRP_SET);
	assert(context->config == (eigrp_af_instance_t *)&fake_af);
	assert(context->runtime == (eigrp_instance_t *)&fake_runtime);
	router_id_calls++;
	configured_router_id = router_id;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_af_instance_shutdown_update(eigrp_operation_t operation,
	eigrp_af_instance_t *af)
{
	assert(af == (eigrp_af_instance_t *)&fake_af);
	shutdown_calls++;
	af_shutdown = operation == EIGRP_SET;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

int main(void)
{
	static const char config_text[] =
		"router eigrp savage\n"
		" address-family ipv4 unicast autonomous-system 4453\n"
		"  eigrp router-id 10.44.53.1\n"
		"  shutdown\n"
		"  no shutdown\n"
		" exit-address-family\n"
		"exit\n";
	eigrp_unix_config_t *config = eigrp_unix_config_create();
	unsigned error_line = 99;

	assert(config != NULL);
	assert(eigrp_unix_config_apply_text(config, config_text, &error_line)
	       == EIGRP_RESULT_SUCCESS);
	assert(error_line == 0);
	assert(parent_calls == 1);
	assert(af_calls == 1);
	assert(router_id_calls == 1);
	assert(configured_router_id == 0x0a2c3501U);
	assert(shutdown_calls == 2);
	assert(!af_shutdown);
	eigrp_unix_config_delete(config);
	puts("unix config tests: PASS");
	return 0;
}
