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
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *ifname = NULL;
	const char *detail = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "ifname"))
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "detail"))
			detail = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
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
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *detail = NULL;
	const char *ifname = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "detail"))
			detail = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "ifname"))
			ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
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
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	const char *all = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "all"))
			all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	return show_eigrp_topology_all_magic(self, vty, argc, argv, afi, vrf, as, as_str, all);
}

/* show_eigrp_topology => "show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] [multicast] topology <A.B.C.D$address|A.B.C.D/M$prefix> [all-links]$all" */
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
	struct in_addr address,\
	const char * address_str __attribute__ ((unused)),\
	const struct prefix_ipv4 * prefix,\
	const char * prefix_str __attribute__ ((unused)),\
	const char * all)
funcdecl_show_eigrp_topology;
DEFUN_CMD_FUNC_TEXT(show_eigrp_topology)
{
	int _i;
	unsigned _fail = 0, _failcnt = 0;
	const char *afi = NULL;
	const char *vrf = NULL;
	int64_t as = 0;
	const char *as_str = NULL;
	struct in_addr address = { INADDR_ANY };
	const char *address_str = NULL;
	struct prefix_ipv4 prefix = { };
	const char *prefix_str = NULL;
	const char *all = NULL;

	for (_i = 0; _i < argc; _i++) {
		if (!argv[_i]->varname)
			continue;
		_fail = 0;
		if (!strcmp(argv[_i]->varname, "afi"))
			afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "vrf"))
			vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (!strcmp(argv[_i]->varname, "as")) {
			as_str = argv[_i]->arg;
			char *_end;
			as = strtoll(argv[_i]->arg, &_end, 10);
			_fail = (_end == argv[_i]->arg) || (*_end != '\0');
		}
		if (!strcmp(argv[_i]->varname, "address")) {
			address_str = argv[_i]->arg;
			_fail = !inet_aton(argv[_i]->arg, &address);
		}
		if (!strcmp(argv[_i]->varname, "prefix")) {
			prefix_str = argv[_i]->arg;
			_fail = !str2prefix_ipv4(argv[_i]->arg, &prefix);
		}
		if (!strcmp(argv[_i]->varname, "all"))
			all = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg;
		if (_fail)
			vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg);
		_failcnt += _fail;
	}
	if (_failcnt)
		return CMD_WARNING;
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	return show_eigrp_topology_magic(self, vty, argc, argv, afi, vrf, as, as_str, address, address_str, &prefix, prefix_str, all);
}

/* show_eigrp_accounting */
DEFUN_CMD_FUNC_DECL(show_eigrp_accounting)
#define funcdecl_show_eigrp_accounting static int show_eigrp_accounting_magic(const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi __attribute__ ((unused)), const char * vrf __attribute__ ((unused)), int64_t as __attribute__ ((unused)), const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_accounting;
DEFUN_CMD_FUNC_TEXT(show_eigrp_accounting) { return show_eigrp_accounting_magic(self, vty, argc, argv, NULL, NULL, 0, NULL); }
/* show_eigrp_event */
DEFUN_CMD_FUNC_DECL(show_eigrp_event)
#define funcdecl_show_eigrp_event static int show_eigrp_event_magic(const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi __attribute__ ((unused)), const char * vrf __attribute__ ((unused)), int64_t as __attribute__ ((unused)), const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_event;
DEFUN_CMD_FUNC_TEXT(show_eigrp_event) { return show_eigrp_event_magic(self, vty, argc, argv, NULL, NULL, 0, NULL); }
/* show_eigrp_timer */
DEFUN_CMD_FUNC_DECL(show_eigrp_timer)
#define funcdecl_show_eigrp_timer static int show_eigrp_timer_magic(const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi __attribute__ ((unused)), const char * vrf __attribute__ ((unused)), int64_t as __attribute__ ((unused)), const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_timer;
DEFUN_CMD_FUNC_TEXT(show_eigrp_timer) { return show_eigrp_timer_magic(self, vty, argc, argv, NULL, NULL, 0, NULL); }
/* show_eigrp_traffic */
DEFUN_CMD_FUNC_DECL(show_eigrp_traffic)
#define funcdecl_show_eigrp_traffic static int show_eigrp_traffic_magic(const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi __attribute__ ((unused)), const char * vrf __attribute__ ((unused)), int64_t as __attribute__ ((unused)), const char * as_str __attribute__ ((unused)))
funcdecl_show_eigrp_traffic;
DEFUN_CMD_FUNC_TEXT(show_eigrp_traffic) { return show_eigrp_traffic_magic(self, vty, argc, argv, NULL, NULL, 0, NULL); }

/* simple show commands */
DEFUN_CMD_FUNC_DECL(show_eigrp_protocol)
#define funcdecl_show_eigrp_protocol static int show_eigrp_protocol_magic(const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)))
funcdecl_show_eigrp_protocol;
DEFUN_CMD_FUNC_TEXT(show_eigrp_protocol) { return show_eigrp_protocol_magic(self, vty, argc, argv); }
DEFUN_CMD_FUNC_DECL(show_eigrp_tech_support)
#define funcdecl_show_eigrp_tech_support static int show_eigrp_tech_support_magic(const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)))
funcdecl_show_eigrp_tech_support;
DEFUN_CMD_FUNC_TEXT(show_eigrp_tech_support) { return show_eigrp_tech_support_magic(self, vty, argc, argv); }

/* clear_eigrp_neighbor => "clear eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] [(1-65535)$as] neighbors [soft]$soft" */
DEFUN_CMD_FUNC_DECL(clear_eigrp_neighbor)
#define funcdecl_clear_eigrp_neighbor static int clear_eigrp_neighbor_magic(\
	const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi, const char * vrf, int64_t as, const char * as_str __attribute__ ((unused)), const char * soft)
funcdecl_clear_eigrp_neighbor;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_neighbor)
{
	int _i; unsigned _fail = 0, _failcnt = 0; const char *afi = NULL; const char *vrf = NULL; int64_t as = 0; const char *as_str = NULL; const char *soft = NULL;
	for (_i = 0; _i < argc; _i++) { if (!argv[_i]->varname) continue; _fail = 0; if (!strcmp(argv[_i]->varname, "afi")) afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "vrf")) vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "as")) { as_str = argv[_i]->arg; char *_end; as = strtoll(argv[_i]->arg, &_end, 10); _fail = (_end == argv[_i]->arg) || (*_end != '\0'); } if (!strcmp(argv[_i]->varname, "soft")) soft = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (_fail) vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg); _failcnt += _fail; }
	if (_failcnt)
		return CMD_WARNING;
	if (!afi) {
		vty_out(vty, "Internal CLI error [%s]\n", "afi");
		return CMD_WARNING;
	}
	return clear_eigrp_neighbor_magic(self, vty, argc, argv, afi, vrf, as, as_str, soft);
}

/* clear_eigrp_neighbor_interface */
DEFUN_CMD_FUNC_DECL(clear_eigrp_neighbor_interface)
#define funcdecl_clear_eigrp_neighbor_interface static int clear_eigrp_neighbor_interface_magic(\
	const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi, const char * vrf, int64_t as, const char * as_str __attribute__ ((unused)), const char * ifname, const char * soft)
funcdecl_clear_eigrp_neighbor_interface;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_neighbor_interface)
{
	int _i; unsigned _fail = 0, _failcnt = 0; const char *afi = NULL; const char *vrf = NULL; int64_t as = 0; const char *as_str = NULL; const char *ifname = NULL; const char *soft = NULL;
	for (_i = 0; _i < argc; _i++) { if (!argv[_i]->varname) continue; _fail = 0; if (!strcmp(argv[_i]->varname, "afi")) afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "vrf")) vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "as")) { as_str = argv[_i]->arg; char *_end; as = strtoll(argv[_i]->arg, &_end, 10); _fail = (_end == argv[_i]->arg) || (*_end != '\0'); } if (!strcmp(argv[_i]->varname, "ifname")) ifname = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "soft")) soft = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (_fail) vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg); _failcnt += _fail; }
	if (_failcnt)
		return CMD_WARNING;
	if (!afi || !ifname) {
		vty_out(vty, "Internal CLI error [%s]\n", !afi ? "afi" : "ifname");
		return CMD_WARNING;
	}
	return clear_eigrp_neighbor_interface_magic(self, vty, argc, argv, afi, vrf, as, as_str, ifname, soft);
}

/* clear_eigrp_neighbor_address */
DEFUN_CMD_FUNC_DECL(clear_eigrp_neighbor_address)
#define funcdecl_clear_eigrp_neighbor_address static int clear_eigrp_neighbor_address_magic(\
	const struct cmd_element *self __attribute__ ((unused)), struct vty *vty __attribute__ ((unused)), int argc __attribute__ ((unused)), struct cmd_token *argv[] __attribute__ ((unused)), const char * afi, const char * vrf, int64_t as, const char * as_str __attribute__ ((unused)), struct in_addr nbr_addr, const char * nbr_addr_str __attribute__ ((unused)), const char * soft)
funcdecl_clear_eigrp_neighbor_address;
DEFUN_CMD_FUNC_TEXT(clear_eigrp_neighbor_address)
{
	int _i; unsigned _fail = 0, _failcnt = 0; const char *afi = NULL; const char *vrf = NULL; int64_t as = 0; const char *as_str = NULL; struct in_addr nbr_addr = { INADDR_ANY }; const char *nbr_addr_str = NULL; const char *soft = NULL;
	for (_i = 0; _i < argc; _i++) { if (!argv[_i]->varname) continue; _fail = 0; if (!strcmp(argv[_i]->varname, "afi")) afi = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "vrf")) vrf = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (!strcmp(argv[_i]->varname, "as")) { as_str = argv[_i]->arg; char *_end; as = strtoll(argv[_i]->arg, &_end, 10); _fail = (_end == argv[_i]->arg) || (*_end != '\0'); } if (!strcmp(argv[_i]->varname, "nbr_addr")) { nbr_addr_str = argv[_i]->arg; _fail = !inet_aton(argv[_i]->arg, &nbr_addr); } if (!strcmp(argv[_i]->varname, "soft")) soft = (argv[_i]->type == WORD_TKN) ? argv[_i]->text : argv[_i]->arg; if (_fail) vty_out(vty, "%% invalid input for %s: %s\n", argv[_i]->varname, argv[_i]->arg); _failcnt += _fail; }
	if (_failcnt)
		return CMD_WARNING;
	if (!afi || !nbr_addr_str) {
		vty_out(vty, "Internal CLI error [%s]\n", !afi ? "afi" : "nbr_addr_str");
		return CMD_WARNING;
	}
	return clear_eigrp_neighbor_address_magic(self, vty, argc, argv, afi, vrf, as, as_str, nbr_addr, nbr_addr_str, soft);
}
