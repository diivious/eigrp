/* show_ip_eigrp_topology_all => "show ip eigrp [vrf NAME] topology [all-links$all]" */
DEFUN_CMD_FUNC_DECL(show_ip_eigrp_topology_all)
#define funcdecl_show_ip_eigrp_topology_all static int show_ip_eigrp_topology_all_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	const char * all)
funcdecl_show_ip_eigrp_topology_all;
DEFUN_CMD_FUNC_TEXT(show_ip_eigrp_topology_all)
{
#if 2 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	const char *all = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "all")) {
			all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return show_ip_eigrp_topology_all_magic(self, vty, argc, argv, vrf, all);
}

/* show_ip_eigrp_topology => "show ip eigrp [vrf NAME] topology <A.B.C.D$address|A.B.C.D/M$prefix>" */
DEFUN_CMD_FUNC_DECL(show_ip_eigrp_topology)
#define funcdecl_show_ip_eigrp_topology static int show_ip_eigrp_topology_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	struct in_addr address,\
	const char * address_str __attribute__ ((unused)),\
	const struct prefix_ipv4 * prefix,\
	const char * prefix_str __attribute__ ((unused)))
funcdecl_show_ip_eigrp_topology;
DEFUN_CMD_FUNC_TEXT(show_ip_eigrp_topology)
{
#if 3 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	struct in_addr address = { INADDR_ANY };
	const char *address_str = NULL;
	struct prefix_ipv4 prefix = { };
	const char *prefix_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "address")) {
			address_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &address);
		}
		if (!strcmp(argv[_i]->varname, "prefix")) {
			prefix_str = argv[_i]->arg;
			_fail = !str2prefix_ipv4(argv[_i]->arg, &prefix);
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

	return show_ip_eigrp_topology_magic(self, vty, argc, argv, vrf, address, address_str, &prefix, prefix_str);
}

/* show_ip_eigrp_interfaces => "show ip eigrp [vrf NAME] interfaces [IFNAME] [detail]$detail" */
DEFUN_CMD_FUNC_DECL(show_ip_eigrp_interfaces)
#define funcdecl_show_ip_eigrp_interfaces static int show_ip_eigrp_interfaces_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	const char * ifname,\
	const char * detail)
funcdecl_show_ip_eigrp_interfaces;
DEFUN_CMD_FUNC_TEXT(show_ip_eigrp_interfaces)
{
#if 3 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	const char *ifname = NULL;
	const char *detail = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "detail")) {
			detail = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return show_ip_eigrp_interfaces_magic(self, vty, argc, argv, vrf, ifname, detail);
}

/* show_ip_eigrp_neighbors => "show ip eigrp [vrf NAME] neighbors [IFNAME] [detail]$detail" */
DEFUN_CMD_FUNC_DECL(show_ip_eigrp_neighbors)
#define funcdecl_show_ip_eigrp_neighbors static int show_ip_eigrp_neighbors_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	const char * ifname,\
	const char * detail)
funcdecl_show_ip_eigrp_neighbors;
DEFUN_CMD_FUNC_TEXT(show_ip_eigrp_neighbors)
{
#if 3 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	const char *ifname = NULL;
	const char *detail = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "detail")) {
			detail = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return show_ip_eigrp_neighbors_magic(self, vty, argc, argv, vrf, ifname, detail);
}

/* clear_ip_eigrp_neighbors => "clear ip eigrp [vrf NAME] neighbors" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_neighbors)
#define funcdecl_clear_ip_eigrp_neighbors static int clear_ip_eigrp_neighbors_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf)
funcdecl_clear_ip_eigrp_neighbors;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_neighbors)
{
#if 1 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return clear_ip_eigrp_neighbors_magic(self, vty, argc, argv, vrf);
}

/* clear_ip_eigrp_neighbors_int => "clear ip eigrp [vrf NAME] neighbors IFNAME" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_neighbors_int)
#define funcdecl_clear_ip_eigrp_neighbors_int static int clear_ip_eigrp_neighbors_int_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	const char * ifname)
funcdecl_clear_ip_eigrp_neighbors_int;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_neighbors_int)
{
#if 2 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	const char *ifname = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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
	if (!ifname) {
		vty_out(vty, "Internal CLI error [%s]\n", "ifname");
		return CMD_WARNING;
	}

	return clear_ip_eigrp_neighbors_int_magic(self, vty, argc, argv, vrf, ifname);
}

/* clear_ip_eigrp_neighbors_IP => "clear ip eigrp [vrf NAME] neighbors A.B.C.D$nbr_addr" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_neighbors_IP)
#define funcdecl_clear_ip_eigrp_neighbors_IP static int clear_ip_eigrp_neighbors_IP_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	struct in_addr nbr_addr,\
	const char * nbr_addr_str __attribute__ ((unused)))
funcdecl_clear_ip_eigrp_neighbors_IP;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_neighbors_IP)
{
#if 2 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	struct in_addr nbr_addr = { INADDR_ANY };
	const char *nbr_addr_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "nbr_addr")) {
			nbr_addr_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &nbr_addr);
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
	if (!nbr_addr_str) {
		vty_out(vty, "Internal CLI error [%s]\n", "nbr_addr_str");
		return CMD_WARNING;
	}

	return clear_ip_eigrp_neighbors_IP_magic(self, vty, argc, argv, vrf, nbr_addr, nbr_addr_str);
}

/* clear_ip_eigrp_neighbors_soft => "clear ip eigrp [vrf NAME] neighbors soft" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_neighbors_soft)
#define funcdecl_clear_ip_eigrp_neighbors_soft static int clear_ip_eigrp_neighbors_soft_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf)
funcdecl_clear_ip_eigrp_neighbors_soft;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_neighbors_soft)
{
#if 1 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return clear_ip_eigrp_neighbors_soft_magic(self, vty, argc, argv, vrf);
}

/* clear_ip_eigrp_neighbors_int_soft => "clear ip eigrp [vrf NAME] neighbors IFNAME soft" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_neighbors_int_soft)
#define funcdecl_clear_ip_eigrp_neighbors_int_soft static int clear_ip_eigrp_neighbors_int_soft_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	const char * ifname)
funcdecl_clear_ip_eigrp_neighbors_int_soft;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_neighbors_int_soft)
{
#if 2 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	const char *ifname = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "ifname")) {
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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
	if (!ifname) {
		vty_out(vty, "Internal CLI error [%s]\n", "ifname");
		return CMD_WARNING;
	}

	return clear_ip_eigrp_neighbors_int_soft_magic(self, vty, argc, argv, vrf, ifname);
}

/* clear_ip_eigrp_neighbors_IP_soft => "clear ip eigrp [vrf NAME] neighbors A.B.C.D$nbr_addr soft" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_neighbors_IP_soft)
#define funcdecl_clear_ip_eigrp_neighbors_IP_soft static int clear_ip_eigrp_neighbors_IP_soft_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf,\
	struct in_addr nbr_addr,\
	const char * nbr_addr_str __attribute__ ((unused)))
funcdecl_clear_ip_eigrp_neighbors_IP_soft;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_neighbors_IP_soft)
{
#if 2 /* anything to parse? */
	int _i;
#if 1 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;
	struct in_addr nbr_addr = { INADDR_ANY };
	const char *nbr_addr_str = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 1 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
		if (!strcmp(argv[_i]->varname, "nbr_addr")) {
			nbr_addr_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &nbr_addr);
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
	if (!nbr_addr_str) {
		vty_out(vty, "Internal CLI error [%s]\n", "nbr_addr_str");
		return CMD_WARNING;
	}

	return clear_ip_eigrp_neighbors_IP_soft_magic(self, vty, argc, argv, vrf, nbr_addr, nbr_addr_str);
}

/* show_ip_eigrp_events => "show ip eigrp [vrf NAME$vrf] events" */
DEFUN_CMD_FUNC_DECL(show_ip_eigrp_events)
#define funcdecl_show_ip_eigrp_events static int show_ip_eigrp_events_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf)
funcdecl_show_ip_eigrp_events;
DEFUN_CMD_FUNC_TEXT(show_ip_eigrp_events)
{
#if 1 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return show_ip_eigrp_events_magic(self, vty, argc, argv, vrf);
}

/* clear_ip_eigrp_events => "clear ip eigrp [vrf NAME$vrf] events" */
DEFUN_CMD_FUNC_DECL(clear_ip_eigrp_events)
#define funcdecl_clear_ip_eigrp_events static int clear_ip_eigrp_events_magic(\
	const struct cmd_element *self __attribute__ ((unused)),\
	struct vty *vty __attribute__ ((unused)),\
	int argc __attribute__ ((unused)),\
	struct cmd_token *argv[] __attribute__ ((unused)),\
	const char * vrf)
funcdecl_clear_ip_eigrp_events;
DEFUN_CMD_FUNC_TEXT(clear_ip_eigrp_events)
{
#if 1 /* anything to parse? */
	int _i;
#if 0 /* anything that can fail? */
	unsigned _fail = 0, _failcnt = 0;
#endif
	const char *vrf = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
#if 0 /* anything that can fail? */
		_fail = 0;
#endif

		if (!strcmp(argv[_i]->varname, "vrf")) {
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		}
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

	return clear_ip_eigrp_events_magic(self, vty, argc, argv, vrf);
}

