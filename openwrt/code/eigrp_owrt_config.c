// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenEIGRP OpenWrt retained configuration and semantic replay.
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_cli.h"
#include "eigrp_owrt_config.h"

struct eigrp_owrt_option {
	char *key;
	char *value;
	struct eigrp_owrt_option *next;
};
struct eigrp_owrt_config_section {
	char *type;
	char *id;
	struct eigrp_owrt_option *options;
	struct eigrp_owrt_config_section *next;
};
struct eigrp_owrt_config {
	struct eigrp_owrt_config_section *head;
	struct eigrp_owrt_config_section *tail;
};

static eigrp_result_t eigrp_owrt_config_error(
	eigrp_owrt_config_diagnostic_t *d, const char *section,
	eigrp_result_t code, const char *format, ...)
{
	va_list ap;
	if (d) {
		memset(d, 0, sizeof(*d));
		d->result = code;
		(void)snprintf(d->section, sizeof(d->section), "%s", section ? section : "");
		va_start(ap, format);
		(void)vsnprintf(d->message, sizeof(d->message), format, ap);
		va_end(ap);
	}
	return code;
}

const eigrp_owrt_config_section_t *eigrp_owrt_config_first(const eigrp_owrt_config_t *c)
{
	return c ? c->head : NULL;
}
const eigrp_owrt_config_section_t *eigrp_owrt_config_next(const eigrp_owrt_config_section_t *s)
{
	return s ? s->next : NULL;
}
const char *eigrp_owrt_config_section_type(const eigrp_owrt_config_section_t *s)
{
	return s ? s->type : NULL;
}
const char *eigrp_owrt_config_section_id(const eigrp_owrt_config_section_t *s)
{
	return s ? s->id : NULL;
}
const char *eigrp_owrt_config_option(const eigrp_owrt_config_section_t *s, const char *key)
{
	const struct eigrp_owrt_option *o;
	if (!s || !key) return NULL;
	for (o = s->options; o; o = o->next)
		if (!strcmp(o->key, key)) return o->value;
	return NULL;
}

eigrp_owrt_config_t *eigrp_owrt_config_create(void)
{
	return calloc(1, sizeof(eigrp_owrt_config_t));
}
void eigrp_owrt_config_free(eigrp_owrt_config_t *c)
{
	struct eigrp_owrt_config_section *s, *next;
	if (!c) return;
	for (s = c->head; s; s = next) {
		struct eigrp_owrt_option *o = s->options, *onext;
		next = s->next;
		while (o) {
			onext = o->next;
			/* Do not leave key material behind in heap allocations. */
			if (!strcmp(o->key, "secret") || !strcmp(o->key, "password"))
				memset(o->value, 0, strlen(o->value));
			free(o->key); free(o->value); free(o); o = onext;
		}
		free(s->type); free(s->id); free(s);
	}
	free(c);
}
eigrp_owrt_config_section_t *eigrp_owrt_config_section_add(
	eigrp_owrt_config_t *c, const char *type, const char *id)
{
	struct eigrp_owrt_config_section *s;
	if (!c || !type || !id || !*type || !*id || strlen(id) > 80) return NULL;
	s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->type = strdup(type); s->id = strdup(id);
	if (!s->type || !s->id) {
		free(s->type); free(s->id); free(s); return NULL;
	}
	if (c->tail) c->tail->next = s;
	else c->head = s;
	c->tail = s;
	return s;
}
eigrp_result_t eigrp_owrt_config_option_set(eigrp_owrt_config_section_t *s,
	const char *key, const char *value)
{
	struct eigrp_owrt_option *o;
	if (!s || !key || !*key || !value || strlen(key) > 64 || strlen(value) > 1024)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (o = s->options; o; o = o->next)
		if (!strcmp(o->key, key)) return EIGRP_RESULT_CONFLICT;
	o = calloc(1, sizeof(*o));
	if (!o) return EIGRP_RESULT_INTERNAL_FAILURE;
	o->key = strdup(key); o->value = strdup(value);
	if (!o->key || !o->value) {
		free(o->key); free(o->value); free(o);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	o->next = s->options;
	s->options = o;
	return EIGRP_RESULT_SUCCESS;
}

static int eigrp_owrt_config_key_allowed(const char *type, const char *key)
{
	struct allowed { const char *type, *keys; } const table[] = {
		{"process", " name shutdown "},
		{"address_family", " process afi asn vrf shutdown router_id distance_internal distance_external variance maximum_paths active_time "},
		{"network", " process afi asn vrf prefix "},
		{"interface", " process afi asn vrf ifname hello_interval hold_time passive split_horizon next_hop_self shutdown bandwidth delay bandwidth_percent auth_mode keychain hmac_password "},
		{"keychain", " name key_id secret "},
		{"prefix_rule", " name prefix action "},
	};
	char needle[72]; size_t i;
	if (strlen(key) > 64) return 0;
	(void)snprintf(needle, sizeof(needle), " %s ", key);
	for (i=0; i<sizeof(table)/sizeof(table[0]);i++)
		if (!strcmp(table[i].type,type)) return strstr(table[i].keys,needle)!=NULL;
	return 0;
}
static const char *eigrp_owrt_config_get(const eigrp_owrt_config_section_t *s,
	const char *key, const char *fallback)
{
	const char *v=eigrp_owrt_config_option(s,key);
	return v ? v : fallback;
}
static int eigrp_owrt_config_num(const char *v, uint32_t min, uint32_t max, uint32_t *out)
{
	unsigned long n; char *end;
	if (!v || !*v || *v=='-' || *v=='+') return 0;
	errno=0; n=strtoul(v,&end,10);
	if (errno || *end || n<min || n>max) return 0;
	if (out) *out=(uint32_t)n;
	return 1;
}
static int eigrp_owrt_config_bool(const char *v, int *out)
{
	if (!strcmp(v,"1") || !strcmp(v,"true")) { *out=1;return 1; }
	if (!strcmp(v,"0") || !strcmp(v,"false")) { *out=0;return 1; }
	return 0;
}
static int eigrp_owrt_config_af(const char *v, eigrp_afi_t *af)
{
	if (!strcmp(v,"ipv4")) { *af=EIGRP_AFI_IPV4; return 1; }
	if (!strcmp(v,"ipv6")) { *af=EIGRP_AFI_IPV6; return 1; }
	return 0;
}
static int eigrp_owrt_config_prefix(const char *value, eigrp_prefix_t *prefix)
{
	char ip[INET6_ADDRSTRLEN+5], *slash; unsigned long bits; char *end;
	size_t len;
	if (!value || !prefix || strlen(value)>=sizeof(ip)) return 0;
	memcpy(ip,value,strlen(value)+1); slash=strchr(ip,'/');
	if (!slash) return 0;
	*slash++='\0';
	if (!*slash || *slash=='-') return 0;
	errno=0; bits=strtoul(slash,&end,10);
	if (errno || *end) return 0;
	memset(prefix,0,sizeof(*prefix));
	prefix->address.afi=strchr(ip,':')?EIGRP_AFI_IPV6:EIGRP_AFI_IPV4;
	len=prefix->address.afi==EIGRP_AFI_IPV4?32:128;
	if (bits>len || inet_pton(prefix->address.afi==EIGRP_AFI_IPV4?AF_INET:AF_INET6,
		ip,prefix->address.bytes)!=1) return 0;
	prefix->prefix_length=(uint8_t)bits;
	return 1;
}
static const eigrp_owrt_config_section_t *eigrp_owrt_config_parent(
	const eigrp_owrt_config_t *c, const char *name)
{
	const eigrp_owrt_config_section_t *s;
	for (s=c->head;s;s=s->next)
		if (!strcmp(s->type,"process") && !strcmp(eigrp_owrt_config_get(s,"name",s->id),name))
			return s;
	return NULL;
}
static int eigrp_owrt_config_identity(const eigrp_owrt_config_section_t *s,
	const char **name, eigrp_afi_t *afi, uint16_t *asn, const char **vrf)
{
	const char *a=eigrp_owrt_config_option(s,"afi"),*n=eigrp_owrt_config_option(s,"process");
	uint32_t v;
	if (!a || !n || !*n || !eigrp_owrt_config_af(a,afi) ||
		!eigrp_owrt_config_num(eigrp_owrt_config_option(s,"asn"),1,65535,&v))
		return 0;
	*name=n; *asn=(uint16_t)v; *vrf=eigrp_owrt_config_get(s,"vrf","default");
	return !strcmp(*vrf,"default"); /* OW-08: no non-default VRF adapter */
}
static int eigrp_owrt_config_af_exists(const eigrp_owrt_config_t *c,
	const char *name,eigrp_afi_t afi,uint16_t asn,const char *vrf)
{
	const eigrp_owrt_config_section_t *s;
	for(s=c->head;s;s=s->next) {
		const char *n,*v; eigrp_afi_t a;uint16_t id;
		if(!strcmp(s->type,"address_family") &&
			eigrp_owrt_config_identity(s,&n,&a,&id,&v) &&
			!strcmp(name,n)&&a==afi&&id==asn&&!strcmp(v,vrf))return 1;
	}
	return 0;
}

eigrp_result_t eigrp_owrt_config_validate(const eigrp_owrt_config_t *c,
	eigrp_owrt_config_diagnostic_t *d)
{
	const eigrp_owrt_config_section_t *s,*t;
	const struct eigrp_owrt_option *o;
	if (!c) return eigrp_owrt_config_error(d,"",EIGRP_RESULT_INVALID_ARGUMENT,"null configuration");
	for (s=c->head;s;s=s->next) {
		const char *name,*vrf; eigrp_afi_t afi; uint16_t asn;
		uint32_t n; eigrp_prefix_t prefix;
		if (!eigrp_owrt_config_key_allowed(s->type,"name") &&
			!eigrp_owrt_config_key_allowed(s->type,"process"))
			return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_UNSUPPORTED,"unsupported UCI section type %s",s->type);
		for(o=s->options;o;o=o->next)
			if(!eigrp_owrt_config_key_allowed(s->type,o->key))
				return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_UNSUPPORTED,"unsupported option %s.%s",s->type,o->key);
		if (!strcmp(s->type,"process")) {
			name=eigrp_owrt_config_get(s,"name",s->id);
			if (!*name || strlen(name)>63) goto invalid;
			for(t=c->head;t!=s;t=t->next)
				if (!strcmp(t->type,"process") && !strcmp(eigrp_owrt_config_get(t,"name",t->id),name))goto duplicate;
		} else if (!strcmp(s->type,"keychain")) {
			name=eigrp_owrt_config_option(s,"name");
			if (!name || !*name || !eigrp_owrt_config_option(s,"secret") ||
				!*eigrp_owrt_config_option(s,"secret") ||
				!eigrp_owrt_config_num(eigrp_owrt_config_option(s,"key_id"),1,UINT32_MAX,&n))goto invalid;
			for(t=c->head;t!=s;t=t->next)
				if(!strcmp(t->type,"keychain") &&
				   eigrp_owrt_config_option(t,"name") &&
				   !strcmp(eigrp_owrt_config_option(t,"name"),name))goto duplicate;
		} else if (!strcmp(s->type,"prefix_rule")) {
			name=eigrp_owrt_config_option(s,"name");
			vrf=eigrp_owrt_config_option(s,"action");
			if (!name || !*name || !vrf || (strcmp(vrf,"permit") && strcmp(vrf,"deny")) ||
				!eigrp_owrt_config_prefix(eigrp_owrt_config_option(s,"prefix"),&prefix))goto invalid;
		} else {
			if (eigrp_owrt_config_option(s,"vrf") &&
				strcmp(eigrp_owrt_config_option(s,"vrf"),"default"))
				return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_UNSUPPORTED,
					"OW-08 supports the default VRF only");
			if(!eigrp_owrt_config_identity(s,&name,&afi,&asn,&vrf))goto invalid;
			if(!eigrp_owrt_config_parent(c,name))
				return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_NOT_FOUND,"named parent %s not found",name);
			if (strcmp(s->type,"address_family") && !eigrp_owrt_config_af_exists(c,name,afi,asn,vrf))
				return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_NOT_FOUND,"address family not found");
			if (!strcmp(s->type,"address_family")) {
				for(t=c->head;t!=s;t=t->next) {
					const char *other,*ov; eigrp_afi_t oa;uint16_t oi;
					if (!strcmp(t->type,"address_family") && eigrp_owrt_config_identity(t,&other,&oa,&oi,&ov)
						&& !strcmp(name,other)&&afi==oa&&asn==oi&&!strcmp(vrf,ov)) goto duplicate;
				}
			} else if (!strcmp(s->type,"network")) {
				if (!eigrp_owrt_config_prefix(eigrp_owrt_config_option(s,"prefix"),&prefix) ||
					prefix.address.afi!=afi)goto invalid;
				if (afi==EIGRP_AFI_IPV6)
					return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_UNSUPPORTED,"network statement is IPv4-only; use IPv6 interface participation");
			} else if (!strcmp(s->type,"interface")) {
				const char *auth=eigrp_owrt_config_option(s,"auth_mode");
				const char *pw=eigrp_owrt_config_option(s,"hmac_password");
				if (!eigrp_owrt_config_option(s,"ifname") || !*eigrp_owrt_config_option(s,"ifname"))goto invalid;
				if (auth && !strcmp(auth,"hmac-sha256") && (!pw || !*pw || strlen(pw)>32))goto invalid;
				if (pw && (!auth || strcmp(auth,"hmac-sha256")))goto invalid;
			}
		}
		for(o=s->options;o;o=o->next) {
			if (!strcmp(o->key,"shutdown") || !strcmp(o->key,"passive") ||
				!strcmp(o->key,"split_horizon") || !strcmp(o->key,"next_hop_self")) {
				int b;if(!eigrp_owrt_config_bool(o->value,&b))goto invalid;
			} else if (!strcmp(o->key,"router_id")) {
				struct in_addr a;if(inet_pton(AF_INET,o->value,&a)!=1)goto invalid;
			} else if (!strcmp(o->key,"hello_interval") || !strcmp(o->key,"hold_time") || !strcmp(o->key,"active_time")) {
				if(!eigrp_owrt_config_num(o->value,1,65535,NULL))goto invalid;
			} else if (!strcmp(o->key,"maximum_paths")) {
				if(!eigrp_owrt_config_num(o->value,1,32,NULL))goto invalid;
			} else if (!strcmp(o->key,"distance_internal") || !strcmp(o->key,"distance_external") || !strcmp(o->key,"variance")) {
				if(!eigrp_owrt_config_num(o->value,1,255,NULL))goto invalid;
			} else if (!strcmp(o->key,"bandwidth_percent")) {
				if(!eigrp_owrt_config_num(o->value,1,999999,NULL))goto invalid;
			} else if (!strcmp(o->key,"bandwidth")) {
				if(!eigrp_owrt_config_num(o->value,EIGRP_INTERFACE_BANDWIDTH_MIN,EIGRP_INTERFACE_BANDWIDTH_MAX,NULL))goto invalid;
			} else if (!strcmp(o->key,"delay")) {
				if(!eigrp_owrt_config_num(o->value,EIGRP_INTERFACE_DELAY_MIN,EIGRP_INTERFACE_DELAY_MAX,NULL))goto invalid;
			} else if (!strcmp(o->key,"auth_mode")) {
				if(strcmp(o->value,"none") && strcmp(o->value,"md5") && strcmp(o->value,"hmac-sha256"))goto invalid;
			}
		}
	}
	if(d)memset(d,0,sizeof(*d));
	return EIGRP_RESULT_SUCCESS;
invalid:
	return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_INVALID_ARGUMENT,"invalid value or missing required field in %s",s->type);
duplicate:
	return eigrp_owrt_config_error(d,s->id,EIGRP_RESULT_CONFLICT,"duplicate %s identity",s->type);
}

static eigrp_result_t eigrp_owrt_config_apply_af(const eigrp_owrt_config_section_t *s)
{
	const char *name,*vrf,*value; eigrp_afi_t afi;uint16_t asn;
	eigrp_instance_context_t ctx; eigrp_af_config_t *af; eigrp_result_t r;
	uint32_t n; struct in_addr id;
	if(!eigrp_owrt_config_identity(s,&name,&afi,&asn,&vrf))return EIGRP_RESULT_INVALID_ARGUMENT;
	r=eigrp_af_config_create(name,afi,vrf,asn);
	if(r!=EIGRP_RESULT_SUCCESS)return r;
	af=eigrp_af_config_read(name,afi,vrf,asn);
	if(!af)return EIGRP_RESULT_INTERNAL_FAILURE;
	value=eigrp_owrt_config_option(s,"shutdown");
	if(value){int b;eigrp_owrt_config_bool(value,&b);
		r=eigrp_af_config_shutdown_update(b?EIGRP_SET:EIGRP_RESET,af);
		if(r!=EIGRP_RESULT_SUCCESS)return r;}
	value=eigrp_owrt_config_option(s,"distance_internal");
	if(value || eigrp_owrt_config_option(s,"distance_external")){
		uint32_t in=90,out=170; /* Default pair; update as one semantic call. */
		if(value)eigrp_owrt_config_num(value,1,255,&in);
		value=eigrp_owrt_config_option(s,"distance_external");
		if(value)eigrp_owrt_config_num(value,1,255,&out);
		r=eigrp_af_config_distance_update(EIGRP_SET,af,(uint8_t)in,(uint8_t)out);
		if(r!=EIGRP_RESULT_SUCCESS)return r;}
	value=eigrp_owrt_config_option(s,"router_id");
	if(value){
		(void)inet_pton(AF_INET,value,&id);
		r=eigrp_af_config_context_read(name,afi,vrf,asn,&ctx);
		if(r!=EIGRP_RESULT_SUCCESS)return r;
		r=eigrp_instance_router_id_update(EIGRP_SET,&ctx,ntohl(id.s_addr));
		if(r!=EIGRP_RESULT_SUCCESS)return r;
	}
	value=eigrp_owrt_config_option(s,"variance");
	if(value){
		eigrp_owrt_config_num(value,1,255,&n);
		r=eigrp_af_config_context_read(name,afi,vrf,asn,&ctx);
		if(r!=EIGRP_RESULT_SUCCESS)return r;
		r=eigrp_metric_variance_update(EIGRP_SET,&ctx,(uint8_t)n);
		if(r!=EIGRP_RESULT_SUCCESS)return r;
	}
	value=eigrp_owrt_config_option(s,"maximum_paths");
	if(value){
		eigrp_owrt_config_num(value,1,32,&n);
		r=eigrp_af_config_context_read(name,afi,vrf,asn,&ctx);
		if(r!=EIGRP_RESULT_SUCCESS)return r;
		r=eigrp_topology_maximum_paths_update(EIGRP_SET,&ctx,(uint8_t)n);
		if(r!=EIGRP_RESULT_SUCCESS)return r;
	}
	value=eigrp_owrt_config_option(s,"active_time");
	if(value){
		eigrp_owrt_config_num(value,1,65535,&n);
		r=eigrp_af_config_context_read(name,afi,vrf,asn,&ctx);
		if(r!=EIGRP_RESULT_SUCCESS)return r;
		r=eigrp_timer_active_time_update(EIGRP_SET,&ctx,(uint16_t)n);
	}
	return r;
}
static eigrp_result_t eigrp_owrt_config_apply_network(const eigrp_owrt_config_section_t *s)
{
	const char *name,*vrf; eigrp_afi_t afi;uint16_t asn; eigrp_instance_context_t ctx;
	eigrp_prefix_t prefix; eigrp_result_t r;
	eigrp_owrt_config_identity(s,&name,&afi,&asn,&vrf);
	eigrp_owrt_config_prefix(eigrp_owrt_config_option(s,"prefix"),&prefix);
	r=eigrp_af_config_context_read(name,afi,vrf,asn,&ctx);
	return r==EIGRP_RESULT_SUCCESS?eigrp_network_create(&ctx,&prefix):r;
}
static eigrp_result_t eigrp_owrt_config_apply_interface(const eigrp_owrt_config_section_t *s)
{
	const char *name,*vrf,*ifname,*v; eigrp_afi_t afi;uint16_t asn;
	eigrp_af_config_t *af; eigrp_instance_context_t ctx;
	eigrp_intf_context_t ictx; eigrp_result_t r;uint32_t n;
	eigrp_owrt_config_identity(s,&name,&afi,&asn,&vrf);
	af=eigrp_af_config_read(name,afi,vrf,asn);
	if(!af)return EIGRP_RESULT_NOT_FOUND;
	ifname=eigrp_owrt_config_option(s,"ifname");
	r=eigrp_intf_config_create(af,ifname);
	if(r!=EIGRP_RESULT_SUCCESS)return r;
	r=eigrp_af_config_context_read(name,afi,vrf,asn,&ctx);
	if(r!=EIGRP_RESULT_SUCCESS)return r;
	ictx.address_family=af;
	ictx.config=eigrp_intf_config_read(af,ifname);
	ictx.runtime=ctx.runtime?eigrp_intf_runtime_lookup(ctx.runtime,ifname):NULL;
	if(!ictx.config)return EIGRP_RESULT_INTERNAL_FAILURE;
#define APPLY_INT_NUM(key, func, type) do { \
	v=eigrp_owrt_config_option(s,key); \
	if(v) { eigrp_owrt_config_num(v,1,UINT32_MAX,&n); \
		r=func(EIGRP_SET,&ictx,(type)n); if(r!=EIGRP_RESULT_SUCCESS)return r; } \
} while(0)
#define APPLY_INT_BOOL(key,func) do { \
	v=eigrp_owrt_config_option(s,key); \
	if(v) { int b;eigrp_owrt_config_bool(v,&b); \
		r=func(b?EIGRP_SET:EIGRP_RESET,&ictx); if(r!=EIGRP_RESULT_SUCCESS)return r; } \
}while(0)
	APPLY_INT_NUM("hello_interval",eigrp_intf_hello_interval_update,uint16_t);
	APPLY_INT_NUM("hold_time",eigrp_intf_hold_time_update,uint16_t);
	APPLY_INT_NUM("bandwidth",eigrp_intf_bandwidth_update,uint32_t);
	APPLY_INT_NUM("delay",eigrp_intf_delay_update,uint32_t);
	APPLY_INT_NUM("bandwidth_percent",eigrp_intf_bandwidth_percent_update,uint32_t);
	APPLY_INT_BOOL("shutdown",eigrp_intf_shutdown_update);
	APPLY_INT_BOOL("passive",eigrp_intf_passive_update);
	APPLY_INT_BOOL("next_hop_self",eigrp_intf_nexthop_self_update);
	APPLY_INT_BOOL("split_horizon",eigrp_intf_split_horizon_update);
#undef APPLY_INT_BOOL
#undef APPLY_INT_NUM
	v=eigrp_owrt_config_option(s,"keychain");
	if(v){r=eigrp_auth_keychain_update(EIGRP_SET,&ictx,v);if(r!=EIGRP_RESULT_SUCCESS)return r;}
	v=eigrp_owrt_config_option(s,"auth_mode");
	if(v){
		eigrp_authentication_mode_t mode=!strcmp(v,"md5")?EIGRP_AUTHENTICATION_MD5:
			!strcmp(v,"hmac-sha256")?EIGRP_AUTHENTICATION_HMAC_SHA256:EIGRP_AUTHENTICATION_NONE;
		eigrp_auth_hmac_config_t hmac={.encryption_type=0,.password=eigrp_owrt_config_option(s,"hmac_password")};
		r=eigrp_auth_mode_update(mode==EIGRP_AUTHENTICATION_NONE?EIGRP_RESET:EIGRP_SET,
			&ictx,mode,hmac.password?&hmac:NULL);
	}
	return r;
}

eigrp_result_t eigrp_owrt_config_apply(const eigrp_owrt_config_t *c,
	eigrp_owrt_config_diagnostic_t *d)
{
	const eigrp_owrt_config_section_t *s; int pass;
	eigrp_result_t r=eigrp_owrt_config_validate(c,d);
	if(r!=EIGRP_RESULT_SUCCESS)return r;
	/* Semantic replay always establishes parents, then AF, then children,
	 * independent of UCI section ordering. Parent identity is case-sensitive. */
	for(pass=0;pass<4;pass++)for(s=c->head;s;s=s->next){
		if(pass==0 && !strcmp(s->type,"process")){
			const char *name=eigrp_owrt_config_get(s,"name",s->id);
			const char *v=eigrp_owrt_config_option(s,"shutdown");
			r=eigrp_named_config_create(name);
			if(r==EIGRP_RESULT_SUCCESS && v){
				int b;eigrp_owrt_config_bool(v,&b);
				r=eigrp_named_config_shutdown_update(b?EIGRP_SET:EIGRP_RESET,eigrp_named_config_read(name));
			}
		}else if(pass==1 && !strcmp(s->type,"address_family")) r=eigrp_owrt_config_apply_af(s);
		else if(pass==2 && !strcmp(s->type,"network")) r=eigrp_owrt_config_apply_network(s);
		else if(pass==3 && !strcmp(s->type,"interface")) r=eigrp_owrt_config_apply_interface(s);
		else continue;
		if(r!=EIGRP_RESULT_SUCCESS)return eigrp_owrt_config_error(d,s->id,r,"semantic target returned EIGRP result %d",(int)r);
	}
	if(d)memset(d,0,sizeof(*d));
	return EIGRP_RESULT_SUCCESS;
}
void eigrp_owrt_config_remove(const eigrp_owrt_config_t *c)
{
	const eigrp_owrt_config_section_t *s;
	if (!c)return;
	/* Delete owning named parents only: portable teardown recursively removes
	 * retained AF objects, interface config and network ownership. */
	for(s=c->head;s;s=s->next)
		if(!strcmp(s->type,"process"))
			(void)eigrp_named_config_delete(eigrp_owrt_config_get(s,"name",s->id));
}
eigrp_result_t eigrp_owrt_config_replace(eigrp_owrt_config_t **active,
	eigrp_owrt_config_t *candidate,eigrp_owrt_config_diagnostic_t *d)
{
	eigrp_owrt_config_t *old; eigrp_result_t r;
	if (!active || !candidate || candidate==*active)return EIGRP_RESULT_INVALID_ARGUMENT;
	r=eigrp_owrt_config_validate(candidate,d);
	if(r!=EIGRP_RESULT_SUCCESS)return r;
	old=*active;
	if(old)eigrp_owrt_config_remove(old);
	r=eigrp_owrt_config_apply(candidate,d);
	if(r!=EIGRP_RESULT_SUCCESS){
		eigrp_owrt_config_remove(candidate);
		if(old)(void)eigrp_owrt_config_apply(old,NULL);
		return r;
	}
	*active=candidate;
	eigrp_owrt_config_policy_bind(candidate);
	eigrp_owrt_config_free(old);
	return EIGRP_RESULT_SUCCESS;
}
