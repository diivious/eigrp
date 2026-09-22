// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Main Routine.
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
#include <zebra.h>
#include <lib/version.h>

#include "getopt.h"
#include "frrevent.h"
#include "prefix.h"
#include "if.h"
#include "vector.h"
#include "vty.h"
#include "command.h"
#include "filter.h"
#include "privs.h"
#include "sigevent.h"
#include "keychain.h"
#include "libfrr.h"
#include "routemap.h"
#include "vrf.h"
#include "libagentx.h"

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_snmp.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
#include "eigrpd/eigrp_errors.h"
#include "eigrpd/eigrp_vrf.h"
#include "eigrpd/eigrp_cli_classic.h"
#include "eigrpd/eigrp_cli_named.h"
#include "eigrpd/eigrp_yang.h"

/* EIGRPd privileges. */
static zebra_capabilities_t eigrpd_caps[] = {
	ZCAP_NET_RAW, ZCAP_BIND, ZCAP_NET_ADMIN,
};

struct zebra_privs_t eigrpd_privs = {
#if defined(FRR_USER) && defined(FRR_GROUP)
	.user = FRR_USER,
	.group = FRR_GROUP,
#endif
#if defined(VTY_GROUP)
	.vty_group = VTY_GROUP,
#endif
	.caps_p = eigrpd_caps,
	.cap_num_p = array_size(eigrpd_caps),
	.cap_num_i = 0,
};

/* EIGRPd options. */
struct option longopts[] = {{0}};

/* Master of events.  master is the current FRR integration name.
 * eigrpd_event is kept as an EIGRP-local compatibility alias for the
 * existing packet/timer code in this tree.
 */
struct event_loop *master;
struct event_loop *eigrpd_event;

/* Forward declaration of daemon info structure. */
static struct frr_daemon_info eigrpd_di;

/* SIGHUP handler. */
static void sighup(void)
{
	zlog_info("SIGHUP received");

	/* Reload config file. */
	vty_read_config(NULL, eigrpd_di.config_file, config_default);
}

/* SIGINT / SIGTERM handler. */
static FRR_NORETURN void sigint(void)
{
	zlog_notice("Terminating on signal");
	keychain_terminate();
	eigrp_terminate();
	eigrp_sys_policy_finish();
	exit(0);
}

/* SIGUSR1 handler. */
static void sigusr1(void)
{
	zlog_rotate();
}

struct frr_signal_t eigrp_signals[] = {
	{
		.signal = SIGHUP,
		.handler = &sighup,
	},
	{
		.signal = SIGUSR1,
		.handler = &sigusr1,
	},
	{
		.signal = SIGINT,
		.handler = &sigint,
	},
	{
		.signal = SIGTERM,
		.handler = &sigint,
	},
};

static const struct frr_yang_module_info *const eigrpd_yang_modules[] = {
	&frr_eigrpd_info,
	&frr_filter_info,
	&frr_interface_info,
	&frr_route_map_info,
	&frr_vrf_info,
	&ietf_key_chain_info,
	&ietf_key_chain_deviation_info,
};

FRR_DAEMON_INFO(eigrpd, EIGRP,
		.vty_port = EIGRP_VTY_PORT,
		.proghelp = "Implementation of the EIGRP routing protocol.",
		.signals = eigrp_signals,
		.n_signals = array_size(eigrp_signals),
		.privs = &eigrpd_privs,
		.yang_modules = eigrpd_yang_modules,
		.n_yang_modules = array_size(eigrpd_yang_modules),
);

/* EIGRPd main routine. */
int main(int argc, char **argv, char **envp)
{
	frr_preinit(&eigrpd_di, argc, argv);
	frr_opt_add("", longopts, "");

	while (1) {
		int opt;

		opt = frr_getopt(argc, argv, NULL);

		if (opt == EOF)
			break;

		switch (opt) {
		case 0:
			break;
		default:
			frr_help_exit(1);
		}
	}

	eigrp_sw_version_init();

	/* EIGRP frr event init. */
	eigrp_init();
	master = frr_init();
	eigrpd_event = master;
	libagentx_init();

	eigrp_error_init();
	eigrp_vrf_init();

	/* EIGRPd init. */
	eigrp_sys_runtime_init();
	eigrp_rib_init();
	eigrp_debug_init();

	/* EIGRP VTY inits. */
	eigrp_vty_init();
	keychain_init();
	eigrp_vty_show_init();
	eigrp_cli_classic_init();
	eigrp_cli_named_init();

#ifdef HAVE_SNMP
	eigrp_snmp_init();
#endif /* HAVE_SNMP */

	/* FRR policy object lifecycle and callbacks stay behind southbound. */
	eigrp_sys_policy_init();

	frr_config_fork();
	frr_run(master);

	/* Not reached. */
	return 0;
}
