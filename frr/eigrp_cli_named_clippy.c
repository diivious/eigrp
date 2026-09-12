/* show_eigrp_interface => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] interfaces [IFNAME$ifname] [detail]$detail" */
DEFUN_CMD_FUNC_DECL(show_eigrp_interface)
#define funcdecl_show_eigrp_interface static int show_eigrp_interface_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * ifname,\
	const char * detail)
funcdecl_show_eigrp_interface;
DEFUN_CMD_FUNC_TEXT(show_eigrp_interface)
{
#if 5 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *ifname = NULL;
	const char *detail = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "detail")) {
			detail = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_interface_magic(self, vty, argc, argv, afi, vrf, as, as_str, ifname, detail);
}

/* show_eigrp_neighbor => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] neighbors [static] [detail]$detail [IFNAME$ifname]" */
DEFUN_CMD_FUNC_DECL(show_eigrp_neighbor)
#define funcdecl_show_eigrp_neighbor static int show_eigrp_neighbor_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * detail,\
	const char * ifname)
funcdecl_show_eigrp_neighbor;
DEFUN_CMD_FUNC_TEXT(show_eigrp_neighbor)
{
#if 5 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *detail = NULL;
	const char *ifname = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "detail")) {
			detail = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_neighbor_magic(self, vty, argc, argv, afi, vrf, as, as_str, detail, ifname);
}

/* show_eigrp_topology_all => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] topology [all-links]$all" */
DEFUN_CMD_FUNC_DECL(show_eigrp_topology_all)
#define funcdecl_show_eigrp_topology_all static int show_eigrp_topology_all_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * all)
funcdecl_show_eigrp_topology_all;
DEFUN_CMD_FUNC_TEXT(show_eigrp_topology_all)
{
#if 4 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *all = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "all")) {
			all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_topology_all_magic(self, vty, argc, argv, afi, vrf, as, as_str, all);
}

/* show_eigrp_topology => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] topology WORD$target [all-links]$all" */
DEFUN_CMD_FUNC_DECL(show_eigrp_topology)
#define funcdecl_show_eigrp_topology static int show_eigrp_topology_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * target,\
	const char * all)
funcdecl_show_eigrp_topology;
DEFUN_CMD_FUNC_TEXT(show_eigrp_topology)
{
#if 5 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *target = NULL;
	const char *all = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "target")) {
			target = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "all")) {
			all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	if (!target) {
		vty_out(vty, "Internal CLI error [%s]\n", "target");
		return CMD_WARNING;
	}

	return show_eigrp_topology_magic(self, vty, argc, argv, afi, vrf, as, as_str, target, all);
}

/* show_eigrp_accounting => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] accounting" */
DEFUN_CMD_FUNC_DECL(show_eigrp_accounting)
#define funcdecl_show_eigrp_accounting static int show_eigrp_accounting_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_accounting;
DEFUN_CMD_FUNC_TEXT(show_eigrp_accounting)
{
#if 3 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_accounting_magic(self, vty, argc, argv, afi, vrf, as, as_str);
}

/* show_eigrp_event => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] events" */
DEFUN_CMD_FUNC_DECL(show_eigrp_event)
#define funcdecl_show_eigrp_event static int show_eigrp_event_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_event;
DEFUN_CMD_FUNC_TEXT(show_eigrp_event)
{
#if 3 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_event_magic(self, vty, argc, argv, afi, vrf, as, as_str);
}

/* show_eigrp_timer => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] timers" */
DEFUN_CMD_FUNC_DECL(show_eigrp_timer)
#define funcdecl_show_eigrp_timer static int show_eigrp_timer_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_timer;
DEFUN_CMD_FUNC_TEXT(show_eigrp_timer)
{
#if 3 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_timer_magic(self, vty, argc, argv, afi, vrf, as, as_str);
}

/* show_eigrp_traffic => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] traffic" */
DEFUN_CMD_FUNC_DECL(show_eigrp_traffic)
#define funcdecl_show_eigrp_traffic static int show_eigrp_traffic_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_traffic;
DEFUN_CMD_FUNC_TEXT(show_eigrp_traffic)
{
#if 3 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return show_eigrp_traffic_magic(self, vty, argc, argv, afi, vrf, as, as_str);
}

/* show_eigrp_protocol => "show eigrp protocols" */
DEFUN_CMD_FUNC_DECL(show_eigrp_protocol)
#define funcdecl_show_eigrp_protocol static int show_eigrp_protocol_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)))
funcdecl_show_eigrp_protocol;
DEFUN_CMD_FUNC_TEXT(show_eigrp_protocol)
{
#if 0 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

#if 0 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 0 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif

	return show_eigrp_protocol_magic(self, vty, argc, argv);
}

/* show_eigrp_tech_support => "show eigrp tech-support" */
DEFUN_CMD_FUNC_DECL(show_eigrp_tech_support)
#define funcdecl_show_eigrp_tech_support static int show_eigrp_tech_support_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)))
funcdecl_show_eigrp_tech_support;
DEFUN_CMD_FUNC_TEXT(show_eigrp_tech_support)
{
#if 0 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

#if 0 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 0 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif

	return show_eigrp_tech_support_magic(self, vty, argc, argv);
}

/* clear_eigrp_topology => "clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_topology)
#define funcdecl_clear_eigrp_topology static int clear_eigrp_topology_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * vrf,\
	const char * vrf_all,\
	const char * afi)
funcdecl_clear_eigrp_topology;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_topology)
{
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *vrf = NULL;
	const char *vrf_all = NULL;
	const char *afi = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf_all"))
			vrf_all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname,
				argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	return clear_eigrp_topology_magic(self, vty, argc, argv, as, as_str, vrf,
					  vrf_all, afi);
}

/* clear_eigrp_topology_prefix => "clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology <A.B.C.D/M$ipv4_prefix|X:X::X:X/M$ipv6_prefix>" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_topology_prefix)
#define funcdecl_clear_eigrp_topology_prefix static int clear_eigrp_topology_prefix_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * vrf,\
	const char * vrf_all,\
	const char * afi,\
	const struct prefix_ipv4 * ipv4_prefix,\
	const char * ipv4_prefix_str __attribute__ ((unused)),\
	const struct prefix_ipv6 * ipv6_prefix,\
	const char * ipv6_prefix_str __attribute__ ((unused)))
funcdecl_clear_eigrp_topology_prefix;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_topology_prefix)
{
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *vrf = NULL;
	const char *vrf_all = NULL;
	const char *afi = NULL;
	struct prefix_ipv4 ipv4_prefix = { };
	const char *ipv4_prefix_str = NULL;
	struct prefix_ipv6 ipv6_prefix = { };
	const char *ipv6_prefix_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf_all"))
			vrf_all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "ipv4_prefix")) {
			ipv4_prefix_str = argv[_i]->arg;
			_fail = !str2prefix_ipv4(argv[_i]->arg, &ipv4_prefix);
		}
		if (!strcmp(argv[_i]->varname, "ipv6_prefix")) {
			ipv6_prefix_str = argv[_i]->arg;
			_fail = !str2prefix_ipv6(argv[_i]->arg, &ipv6_prefix);
		}
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname,
				argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	return clear_eigrp_topology_prefix_magic(
		self, vty, argc, argv, as, as_str, vrf, vrf_all, afi, &ipv4_prefix,
		ipv4_prefix_str, &ipv6_prefix, ipv6_prefix_str);
}

/* clear_eigrp_topology_mask => "clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology A.B.C.D$network A.B.C.D$mask" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_topology_mask)
#define funcdecl_clear_eigrp_topology_mask static int clear_eigrp_topology_mask_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * vrf,\
	const char * vrf_all,\
	const char * afi,\
	struct in_addr network,\
	const char * network_str __attribute__ ((unused)),\
	struct in_addr mask,\
	const char * mask_str __attribute__ ((unused)))
funcdecl_clear_eigrp_topology_mask;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_topology_mask)
{
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *vrf = NULL;
	const char *vrf_all = NULL;
	const char *afi = NULL;
	struct in_addr network = { INADDR_ANY };
	const char *network_str = NULL;
	struct in_addr mask = { INADDR_ANY };
	const char *mask_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf_all"))
			vrf_all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "network")) {
			network_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &network);
		}
		if (!strcmp(argv[_i]->varname, "mask")) {
			mask_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &mask);
		}
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname,
				argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
	if (!afi || !network_str || !mask_str) {
		vty_out(vty, "Internal CLI error [%s]\n",
			!afi ? "afi" : !network_str ? "network_str" : "mask_str");
		return CMD_WARNING;
	}
	return clear_eigrp_topology_mask_magic(self, vty, argc, argv, as, as_str,
					       vrf, vrf_all, afi, network,
					       network_str, mask, mask_str);
}

/* clear_eigrp_neighbor => "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors [soft]$soft" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_neighbor)
#define funcdecl_clear_eigrp_neighbor static int clear_eigrp_neighbor_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * soft)
funcdecl_clear_eigrp_neighbor;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_neighbor)
{
#if 4 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *soft = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "soft")) {
			soft = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return clear_eigrp_neighbor_magic(self, vty, argc, argv, afi, vrf, as, as_str, soft);
}

/* clear_eigrp_neighbor_interface => "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors IFNAME$ifname [soft]$soft" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_neighbor_interface)
#define funcdecl_clear_eigrp_neighbor_interface static int clear_eigrp_neighbor_interface_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	const char * ifname,\
	const char * soft)
funcdecl_clear_eigrp_neighbor_interface;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_neighbor_interface)
{
#if 5 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *ifname = NULL;
	const char *soft = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "soft")) {
			soft = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	if (!ifname) {
		vty_out(vty, "Internal CLI error [%s]\n", "ifname");
		return CMD_WARNING;
	}

	return clear_eigrp_neighbor_interface_magic(self, vty, argc, argv, afi, vrf, as, as_str, ifname, soft);
}

/* clear_eigrp_neighbor_address => "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors <A.B.C.D$ipv4_addr|X:X::X:X$ipv6_addr> [soft]$soft" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_neighbor_address)
#define funcdecl_clear_eigrp_neighbor_address static int clear_eigrp_neighbor_address_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)),\
	struct in_addr ipv4_addr,\
	const char * ipv4_addr_str __attribute__ ((unused)),\
	struct in6_addr ipv6_addr,\
	const char * ipv6_addr_str __attribute__ ((unused)),\
	const char * soft)
funcdecl_clear_eigrp_neighbor_address;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_neighbor_address)
{
#if 6 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	struct in_addr ipv4_addr = { INADDR_ANY };
	const char *ipv4_addr_str = NULL;
	struct in6_addr ipv6_addr = {};
	const char *ipv6_addr_str = NULL;
	const char *soft = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "ipv4_addr")) {
			ipv4_addr_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &ipv4_addr);
		}
		if (!strcmp(argv[_i]->varname, "ipv6_addr")) {
			ipv6_addr_str = argv[_i]->arg;
			_fail = !inet_pton(AF_INET6, argv[_i]->arg, &ipv6_addr);
		}
		if (!strcmp(argv[_i]->varname, "soft")) {
			soft = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return clear_eigrp_neighbor_address_magic(self, vty, argc, argv, afi, vrf, as, as_str, ipv4_addr, ipv4_addr_str, ipv6_addr, ipv6_addr_str, soft);
}

/* clear_eigrp_address_family_events => "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] events" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_address_family_events)
#define funcdecl_clear_eigrp_address_family_events static int clear_eigrp_address_family_events_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * afi,\
	const char * vrf,\
	int64_t as,\
	const char * as_str __attribute__ ((unused)))
funcdecl_clear_eigrp_address_family_events;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_address_family_events)
{
#if 3 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "afi")) {
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
#if 1 /* anything that can fail? */
		if (_fail)
			vty_out (vty, "%% invalid input for %s: %s\n",
				   argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
#endif
	}
#if 1 /* anything that can fail? */
	if (_failcnt)
		return CMD_WARNING;
#endif
#endif
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}

	return clear_eigrp_address_family_events_magic(self, vty, argc, argv, afi, vrf, as, as_str);
}

