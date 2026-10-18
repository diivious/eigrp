// SPDX-License-Identifier: GPL-2.0-or-later
/* OW-08 semantic configuration and policy UUT with portable targets mocked.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "eigrp_cli.h"
#include "eigrp_sys.h"
#include "eigrp_owrt_config.h"

static char trace[8192];
static int create_count,delete_count,ipv6_count,iface_count,network_count;
static uint32_t last_router_id;
static eigrp_result_t inject_failure=EIGRP_RESULT_SUCCESS;
static void record(const char *type,const char *name)
{
	size_t offset=strlen(trace);
	(void)snprintf(trace+offset,sizeof(trace)-offset,"%s:%s ",type,name);
}
#define RESULT EIGRP_RESULT_SUCCESS
#define AF ((eigrp_af_config_t *)(uintptr_t)1)
#define NAMED ((eigrp_named_config_t *)(uintptr_t)2)
#define INTF ((eigrp_intf_config_t *)(uintptr_t)3)

eigrp_result_t eigrp_named_config_create(const char *name)
{
	create_count++;record("parent",name);return RESULT;
}
eigrp_named_config_t *eigrp_named_config_read(const char *name)
{
	(void)name;return NAMED;
}
eigrp_result_t eigrp_named_config_delete(const char *name)
{
	delete_count++;record("del",name);return RESULT;
}
eigrp_result_t eigrp_named_config_shutdown_update(eigrp_operation_t op,eigrp_named_config_t *n)
{
	(void)op;(void)n;return RESULT;
}
eigrp_result_t eigrp_af_config_create(const char *name,eigrp_afi_t afi,const char *vrf,uint16_t asn)
{
	(void)vrf;(void)asn;
	record("af",name);
	if(afi==EIGRP_AFI_IPV6)ipv6_count++;
	return RESULT;
}
eigrp_af_config_t *eigrp_af_config_read(const char *name,eigrp_afi_t afi,const char *vrf,uint16_t asn)
{
	(void)name;(void)afi;(void)vrf;(void)asn;return AF;
}
eigrp_result_t eigrp_af_config_context_read(const char *name,eigrp_afi_t afi,const char *vrf,uint16_t asn,eigrp_instance_context_t *ctx)
{
	(void)name;(void)afi;(void)vrf;(void)asn;
	ctx->config=AF;ctx->runtime=NULL;ctx->topology_id=0;return RESULT;
}
eigrp_result_t eigrp_af_config_shutdown_update(eigrp_operation_t op,eigrp_af_config_t *af)
{
	(void)op;(void)af;return RESULT;
}
eigrp_result_t eigrp_af_config_distance_update(eigrp_operation_t op,eigrp_af_config_t *af,uint8_t in,uint8_t out)
{
	assert(in==90 && out==170);(void)op;(void)af;return RESULT;
}
eigrp_result_t eigrp_instance_router_id_update(eigrp_operation_t op,eigrp_instance_context_t *ctx,uint32_t id)
{
	(void)ctx;assert(op==EIGRP_SET);last_router_id=id;return RESULT;
}
eigrp_result_t eigrp_metric_variance_update(eigrp_operation_t op,eigrp_instance_context_t *ctx,uint8_t n)
{
	(void)op;(void)ctx;(void)n;return RESULT;
}
eigrp_result_t eigrp_topology_maximum_paths_update(eigrp_operation_t op,eigrp_instance_context_t *ctx,uint8_t n)
{
	(void)op;(void)ctx;(void)n;return RESULT;
}
eigrp_result_t eigrp_timer_active_time_update(eigrp_operation_t op,eigrp_instance_context_t *ctx,uint16_t n)
{
	(void)op;(void)ctx;(void)n;return RESULT;
}
eigrp_result_t eigrp_network_create(eigrp_instance_context_t *ctx,const eigrp_prefix_t *prefix)
{
	(void)ctx;assert(prefix->address.afi==EIGRP_AFI_IPV4);
	network_count++;record("network","ipv4");return inject_failure;
}
eigrp_result_t eigrp_intf_config_create(eigrp_af_config_t *af,const char *ifname)
{
	(void)af;iface_count++;record("intf",ifname);return RESULT;
}
eigrp_intf_config_t *eigrp_intf_config_read(eigrp_af_config_t *af,const char *name)
{
	(void)af;(void)name;return INTF;
}
eigrp_intf_t *eigrp_intf_runtime_lookup(eigrp_instance_t *eigrp,const char *name)
{
	(void)eigrp;(void)name;return NULL;
}
#define IMPL_UINT(name,kind) \
 eigrp_result_t name(eigrp_operation_t op,eigrp_intf_context_t *ctx,kind value) \
 { (void)op;(void)ctx;(void)value;return RESULT; }
IMPL_UINT(eigrp_intf_hello_interval_update,uint16_t)
IMPL_UINT(eigrp_intf_hold_time_update,uint16_t)
IMPL_UINT(eigrp_intf_bandwidth_update,uint32_t)
IMPL_UINT(eigrp_intf_delay_update,uint32_t)
IMPL_UINT(eigrp_intf_bandwidth_percent_update,uint32_t)
#define IMPL_BOOL(name) \
 eigrp_result_t name(eigrp_operation_t op,eigrp_intf_context_t *ctx) \
 { (void)op;(void)ctx;return RESULT; }
IMPL_BOOL(eigrp_intf_shutdown_update)
IMPL_BOOL(eigrp_intf_passive_update)
IMPL_BOOL(eigrp_intf_split_horizon_update)
IMPL_BOOL(eigrp_intf_nexthop_self_update)
eigrp_result_t eigrp_auth_keychain_update(eigrp_operation_t op,eigrp_intf_context_t *ctx,const char *name)
{
	(void)op;(void)ctx;(void)name;return RESULT;
}
eigrp_result_t eigrp_auth_mode_update(eigrp_operation_t op,eigrp_intf_context_t *ctx,eigrp_authentication_mode_t mode,const eigrp_auth_hmac_config_t *hmac)
{
	(void)op;(void)ctx;(void)mode;(void)hmac;return RESULT;
}
static eigrp_owrt_config_section_t *add(eigrp_owrt_config_t *c,const char *type,const char *id)
{
	eigrp_owrt_config_section_t *s=eigrp_owrt_config_section_add(c,type,id);
	assert(s);return s;
}
static void opt(eigrp_owrt_config_section_t *s,const char *k,const char *v)
{
	assert(eigrp_owrt_config_option_set(s,k,v)==RESULT);
}
static eigrp_owrt_config_section_t *af(eigrp_owrt_config_t *c,const char *id,
	const char *name,const char *version,const char *asn)
{
	eigrp_owrt_config_section_t *s=add(c,"address_family",id);
	opt(s,"process",name);opt(s,"afi",version);opt(s,"asn",asn);
	return s;
}
static eigrp_owrt_config_t *fixture(void)
{
	eigrp_owrt_config_t *c=eigrp_owrt_config_create();
	eigrp_owrt_config_section_t *s;
	assert(c);
	/* Deliberately unordered UCI sections. Replay must order dependencies. */
	s=add(c,"interface","lan");opt(s,"process","savage");opt(s,"afi","ipv4");opt(s,"asn","4453");opt(s,"ifname","lan");opt(s,"hello_interval","5");opt(s,"hold_time","15");opt(s,"passive","0");opt(s,"auth_mode","md5");opt(s,"keychain","core");
	s=add(c,"network","net0");opt(s,"process","savage");opt(s,"afi","ipv4");opt(s,"asn","4453");opt(s,"prefix","10.0.0.0/8");
	s=af(c,"af0","savage","ipv4","4453");opt(s,"router_id","10.1.2.3");
	af(c,"af1","savage","ipv6","4453");
	af(c,"af2","savage","ipv4","4454");
	af(c,"af3","SAVAGE","ipv4","4453");
	s=add(c,"process","savage");opt(s,"name","savage");opt(s,"shutdown","0");
	s=add(c,"process","SAVAGE");opt(s,"name","SAVAGE");
	s=add(c,"keychain","k0");opt(s,"name","core");opt(s,"key_id","7");opt(s,"secret","correctKey");
	s=add(c,"prefix_rule","p0");opt(s,"name","customers");opt(s,"prefix","10.0.0.0/8");opt(s,"action","permit");
	return c;
}
static void check_policy(void)
{
	eigrp_filter_decision_t decision=EIGRP_FILTER_DECISION_DENY;
	eigrp_prefix_t prefix={.address={.afi=EIGRP_AFI_IPV4,.bytes={10,1,0,0}},.prefix_length=16};
	char key[40];uint32_t key_id=0;
	assert(eigrp_sys_auth_key_lookup("core",&key_id,key,sizeof(key)));
	assert(key_id==7 && !strcmp(key,"correctKey"));
	assert(!eigrp_sys_auth_key_lookup("missing",&key_id,key,sizeof(key)));
	assert(eigrp_sys_filter_evaluate(NULL,EIGRP_DISTRIBUTE_PREFIX_LIST,"customers",&prefix,&decision)==RESULT);
	assert(decision==EIGRP_FILTER_DECISION_PERMIT);
	prefix.address.bytes[0]=192;
	assert(eigrp_sys_filter_evaluate(NULL,EIGRP_DISTRIBUTE_PREFIX_LIST,"customers",&prefix,&decision)==RESULT);
	assert(decision==EIGRP_FILTER_DECISION_DENY);
	assert(eigrp_sys_filter_evaluate(NULL,EIGRP_DISTRIBUTE_ACCESS_LIST,"customers",&prefix,&decision)==EIGRP_RESULT_NOT_IMPLEMENTED);
	assert(eigrp_sys_redistribute_route_map_evaluate(NULL,"foo",NULL,&decision)==EIGRP_RESULT_NOT_IMPLEMENTED);
}
int main(void)
{
	eigrp_owrt_config_t *c,*bad,*active=NULL;
	eigrp_owrt_config_section_t *s;
	eigrp_owrt_config_diagnostic_t d;
	c=fixture();
	assert(eigrp_owrt_config_validate(c,&d)==RESULT);
	assert(eigrp_owrt_config_replace(&active,c,&d)==RESULT);
	assert(active==c && create_count==2 && ipv6_count==1 &&
	       iface_count==1 && network_count==1 && last_router_id==0x0a010203U);
	assert(strstr(trace,"parent:savage ")<strstr(trace,"af:savage "));
	assert(strstr(trace,"af:savage ")<strstr(trace,"network:ipv4 "));
	check_policy();
	bad=eigrp_owrt_config_create();
	s=add(bad,"process","savage");opt(s,"name","savage");
	s=af(bad,"bad","savage","ipv4","4453");opt(s,"maximum_paths","999");
	assert(eigrp_owrt_config_replace(&active,bad,&d)==EIGRP_RESULT_INVALID_ARGUMENT);
	assert(active==c && delete_count==0);
	eigrp_owrt_config_free(bad);
	bad=fixture();
	inject_failure=EIGRP_RESULT_NOT_IMPLEMENTED;
	assert(eigrp_owrt_config_replace(&active,bad,&d)==EIGRP_RESULT_NOT_IMPLEMENTED);
	assert(active==c && d.result==EIGRP_RESULT_NOT_IMPLEMENTED);
	/* Failed replay restored the previous snapshot as the active owner. */
	inject_failure=EIGRP_RESULT_SUCCESS;
	eigrp_owrt_config_free(bad);
	eigrp_owrt_config_policy_bind(NULL);
	eigrp_owrt_config_remove(active);
	eigrp_owrt_config_free(active);
	puts("OpenWrt OW-08 UCI normalized configuration and semantic policy UUT: PASS");
	return 0;
}
