// SPDX-License-Identifier: GPL-2.0-or-later
/* UCI-specific parsing, reload, and persistent option mutation.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uci.h>

#include "eigrp_owrt_config.h"

static eigrp_result_t eigrp_owrt_uci_parse(struct uci_context *ctx,
	struct uci_package *package, eigrp_owrt_config_t **out,
	eigrp_owrt_config_diagnostic_t *diagnostic)
{
	eigrp_owrt_config_t *config;
	struct uci_element *element, *option;
	if (!ctx || !package || !out) return EIGRP_RESULT_INVALID_ARGUMENT;
	config=eigrp_owrt_config_create();
	if (!config)return EIGRP_RESULT_INTERNAL_FAILURE;
	uci_foreach_element(&package->sections,element) {
		struct uci_section *source=uci_to_section(element);
		eigrp_owrt_config_section_t *section=eigrp_owrt_config_section_add(
			config,source->type,element->name);
		if (!section)goto failed;
		uci_foreach_element(&source->options,option) {
			struct uci_option *o=uci_to_option(option);
			if(o->type!=UCI_TYPE_STRING ||
				eigrp_owrt_config_option_set(section,option->name,o->v.string)!=EIGRP_RESULT_SUCCESS)goto failed;
		}
	}
	if(eigrp_owrt_config_validate(config,diagnostic)!=EIGRP_RESULT_SUCCESS){
		eigrp_owrt_config_free(config);
		return diagnostic?diagnostic->result:EIGRP_RESULT_INVALID_ARGUMENT;
	}
	*out=config;
	return EIGRP_RESULT_SUCCESS;
failed:
	if (diagnostic) {
		memset(diagnostic,0,sizeof(*diagnostic));
		diagnostic->result=EIGRP_RESULT_INVALID_ARGUMENT;
		(void)snprintf(diagnostic->message,sizeof(diagnostic->message),
			"invalid or duplicate UCI option, or allocation failed");
	}
	eigrp_owrt_config_free(config);
	return EIGRP_RESULT_INVALID_ARGUMENT;
}
static struct uci_context *eigrp_owrt_uci_context(const char *directory)
{
	struct uci_context *ctx=uci_alloc_context();
	if(ctx && directory && *directory && uci_set_confdir(ctx,directory)!=UCI_OK) {
		uci_free_context(ctx);
		return NULL;
	}
	return ctx;
}
eigrp_result_t eigrp_owrt_uci_load(const char *directory,
	eigrp_owrt_config_t **config,eigrp_owrt_config_diagnostic_t *diagnostic)
{
	struct uci_context *ctx;
	struct uci_package *package=NULL;
	eigrp_result_t result;
	if (!config)return EIGRP_RESULT_INVALID_ARGUMENT;
	*config=NULL;
	ctx=eigrp_owrt_uci_context(directory);
	if(!ctx)return EIGRP_RESULT_INTERNAL_FAILURE;
	if(uci_load(ctx,"openeigrp",&package)!=UCI_OK){
		uci_free_context(ctx);
		if(diagnostic){
			memset(diagnostic,0,sizeof(*diagnostic));
			diagnostic->result=EIGRP_RESULT_NOT_FOUND;
			(void)snprintf(diagnostic->message,sizeof(diagnostic->message),"cannot load UCI openeigrp package");
		}
		return EIGRP_RESULT_NOT_FOUND;
	}
	result=eigrp_owrt_uci_parse(ctx,package,config,diagnostic);
	uci_unload(ctx,package);
	uci_free_context(ctx);
	return result;
}
eigrp_result_t eigrp_owrt_uci_reload(const char *directory,
	eigrp_owrt_config_t **active,eigrp_owrt_config_diagnostic_t *diagnostic)
{
	eigrp_owrt_config_t *candidate=NULL;
	eigrp_result_t result;
	if (!active)return EIGRP_RESULT_INVALID_ARGUMENT;
	result=eigrp_owrt_uci_load(directory,&candidate,diagnostic);
	if(result!=EIGRP_RESULT_SUCCESS)return result;
	result=eigrp_owrt_config_replace(active,candidate,diagnostic);
	if(result!=EIGRP_RESULT_SUCCESS)eigrp_owrt_config_free(candidate);
	return result;
}
/* Validate names used in a libuci extended lookup so caller strings cannot
 * select other UCI packages or contain assignment syntax. */
static int eigrp_owrt_uci_identifier(const char *s)
{
	const unsigned char *p=(const unsigned char *)s;
	if(!p || !*p)return 0;
	for(;*p;p++)if(!isalnum(*p) && *p!='_' && *p!='-')return 0;
	return 1;
}
eigrp_result_t eigrp_owrt_uci_option_write(const char *directory,
	const char *section,const char *option,const char *value,
	eigrp_operation_t operation,eigrp_owrt_config_diagnostic_t *diagnostic)
{
	struct uci_context *ctx=NULL;
	struct uci_package *pkg=NULL;
	struct uci_ptr ptr;
	char lookup[2048];
	eigrp_owrt_config_t *candidate=NULL;
	eigrp_result_t result=EIGRP_RESULT_INTERNAL_FAILURE;
	if(!eigrp_owrt_uci_identifier(section) || !eigrp_owrt_uci_identifier(option)
	   || (operation!=EIGRP_SET && operation!=EIGRP_RESET)
	   || (operation==EIGRP_SET && (!value || strlen(value)>1024 ||
	       !uci_validate_text(value))))return EIGRP_RESULT_INVALID_ARGUMENT;
	ctx=eigrp_owrt_uci_context(directory);
	if(!ctx)return EIGRP_RESULT_INTERNAL_FAILURE;
	if(uci_load(ctx,"openeigrp",&pkg)!=UCI_OK)goto finish;
	if((size_t)snprintf(lookup,sizeof(lookup),"openeigrp.%s.%s%s%s",section,option,
		operation==EIGRP_SET?"=":"",operation==EIGRP_SET?value:"")>=sizeof(lookup)){
		result=EIGRP_RESULT_INVALID_ARGUMENT;goto finish;
	}
	memset(&ptr,0,sizeof(ptr));
	if(uci_lookup_ptr(ctx,&ptr,lookup,true)!=UCI_OK || !ptr.s){
		result=EIGRP_RESULT_NOT_FOUND;goto finish;
	}
	if(operation==EIGRP_SET){
		if(uci_set(ctx,&ptr)!=UCI_OK)goto finish;
	}else{
		if(!ptr.o){result=EIGRP_RESULT_NOT_FOUND;goto finish;}
		if(uci_delete(ctx,&ptr)!=UCI_OK)goto finish;
	}
	result=eigrp_owrt_uci_parse(ctx,pkg,&candidate,diagnostic);
	if(result!=EIGRP_RESULT_SUCCESS)goto finish;
	/* Only valid candidate UCI state reaches persistent storage.  Semantic
	 * replay follows procd reload in OW-09, not inside this write operation. */
	if(uci_commit(ctx,&pkg,false)!=UCI_OK){result=EIGRP_RESULT_INTERNAL_FAILURE;goto finish;}
	result=EIGRP_RESULT_SUCCESS;
finish:
	eigrp_owrt_config_free(candidate);
	if(pkg)uci_unload(ctx,pkg);
	uci_free_context(ctx);
	return result;
}
