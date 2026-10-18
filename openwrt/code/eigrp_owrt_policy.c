// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt-owned basic prefix-policy and keychain lookup services.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "eigrp_sys.h"
#include "eigrp_owrt_config.h"

/* Borrowed snapshot. The OW-09 service must serialize reload with policy
 * consumers and call policy_bind(NULL) before freeing its last snapshot. */
static const eigrp_owrt_config_t *eigrp_owrt_policy_config;
void eigrp_owrt_config_policy_bind(const eigrp_owrt_config_t *config)
{
	eigrp_owrt_policy_config=config;
}
static const char *eigrp_owrt_policy_value(const eigrp_owrt_config_section_t *s,const char *key)
{
	return eigrp_owrt_config_option(s,key);
}
static int eigrp_owrt_policy_prefix_parse(const char *value,eigrp_prefix_t *p)
{
	char buffer[INET6_ADDRSTRLEN+5],*slash,*end;unsigned long n;
	if(!value||strlen(value)>=sizeof(buffer))return 0;
	strcpy(buffer,value);slash=strchr(buffer,'/');if(!slash)return 0;
	*slash++=0;
	if(!*slash||*slash=='-')return 0;
	n=strtoul(slash,&end,10);
	if(*end)return 0;
	memset(p,0,sizeof(*p));
	p->address.afi=strchr(buffer,':')?EIGRP_AFI_IPV6:EIGRP_AFI_IPV4;
	if(n>(p->address.afi==EIGRP_AFI_IPV4?32U:128U) ||
	   inet_pton(p->address.afi==EIGRP_AFI_IPV4?AF_INET:AF_INET6,buffer,p->address.bytes)!=1)return 0;
	p->prefix_length=(uint8_t)n;
	return 1;
}
static int eigrp_owrt_policy_contains(const eigrp_prefix_t *rule,const eigrp_prefix_t *prefix)
{
	unsigned bits=rule->prefix_length,bytes=bits/8U;
	if(rule->address.afi!=prefix->address.afi || bits>prefix->prefix_length)return 0;
	if(memcmp(rule->address.bytes,prefix->address.bytes,bytes))return 0;
	return !(bits%8U) || !((rule->address.bytes[bytes]^prefix->address.bytes[bytes]) & (0xffU<<(8U-bits%8U)));
}
static eigrp_result_t eigrp_owrt_policy_prefix_evaluate(const char *name,
	const eigrp_prefix_t *prefix,eigrp_filter_decision_t *decision)
{
	const eigrp_owrt_config_section_t *s;
	int found=0;
	if(!name||!prefix||!decision)return EIGRP_RESULT_INVALID_ARGUMENT;
	if(!eigrp_owrt_policy_config)return EIGRP_RESULT_NOT_FOUND;
	for(s=eigrp_owrt_config_first(eigrp_owrt_policy_config);s;s=eigrp_owrt_config_next(s)){
		eigrp_prefix_t rule;const char *v;
		if(strcmp(eigrp_owrt_config_section_type(s),"prefix_rule") ||
		   !(v=eigrp_owrt_policy_value(s,"name")) || strcmp(v,name))continue;
		found=1;
		if(!eigrp_owrt_policy_prefix_parse(eigrp_owrt_policy_value(s,"prefix"),&rule))
			return EIGRP_RESULT_INVALID_ARGUMENT;
		if(eigrp_owrt_policy_contains(&rule,prefix)){
			*decision=!strcmp(eigrp_owrt_policy_value(s,"action"),"permit") ?
				EIGRP_FILTER_DECISION_PERMIT:EIGRP_FILTER_DECISION_DENY;
			return EIGRP_RESULT_SUCCESS;
		}
	}
	if(!found)return EIGRP_RESULT_NOT_FOUND;
	*decision=EIGRP_FILTER_DECISION_DENY;
	return EIGRP_RESULT_SUCCESS;
}
void eigrp_sys_policy_init(void) { eigrp_owrt_config_policy_bind(NULL); }
void eigrp_sys_policy_finish(void) { eigrp_owrt_config_policy_bind(NULL); }
eigrp_result_t eigrp_sys_policy_instance_create(eigrp_instance_t *eigrp)
{
	return eigrp?EIGRP_RESULT_SUCCESS:EIGRP_RESULT_INVALID_ARGUMENT;
}
void eigrp_sys_policy_instance_delete(eigrp_instance_t *eigrp) { (void)eigrp; }
eigrp_result_t eigrp_sys_filter_evaluate(eigrp_instance_t *eigrp,
	eigrp_distribute_list_type_t type,const char *name,
	const eigrp_prefix_t *prefix,eigrp_filter_decision_t *decision)
{
	(void)eigrp;
	if(type!=EIGRP_DISTRIBUTE_PREFIX_LIST)return EIGRP_RESULT_NOT_IMPLEMENTED;
	return eigrp_owrt_policy_prefix_evaluate(name,prefix,decision);
}
eigrp_result_t eigrp_sys_redistribute_route_map_evaluate(eigrp_instance_t *eigrp,
	const char *name,const eigrp_rib_route_t *route,eigrp_filter_decision_t *decision)
{
	(void)eigrp;(void)name;(void)route;(void)decision;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
eigrp_result_t eigrp_sys_summary_leak_map_evaluate(eigrp_instance_t *eigrp,
	const char *name,const eigrp_prefix_t *prefix,eigrp_filter_decision_t *decision)
{
	(void)eigrp;
	return eigrp_owrt_policy_prefix_evaluate(name,prefix,decision);
}
bool eigrp_sys_auth_key_lookup(const char *name,uint32_t *key_id,char *key_string,size_t size)
{
	const eigrp_owrt_config_section_t *s;
	if(!name||!key_id||!key_string||size==0||!eigrp_owrt_policy_config)return false;
	for(s=eigrp_owrt_config_first(eigrp_owrt_policy_config);s;s=eigrp_owrt_config_next(s)){
		const char *n,*secret,*number;
		char *end;unsigned long id;
		if(strcmp(eigrp_owrt_config_section_type(s),"keychain"))continue;
		n=eigrp_owrt_policy_value(s,"name");
		if(!n||strcmp(n,name))continue;
		secret=eigrp_owrt_policy_value(s,"secret");number=eigrp_owrt_policy_value(s,"key_id");
		if(!secret||!number||strlen(secret)>=size)return false;
		id=strtoul(number,&end,10);
		if(*end||id==0||id>UINT32_MAX)return false;
		memcpy(key_string,secret,strlen(secret)+1);
		*key_id=(uint32_t)id;
		return true;
	}
	return false;
}
