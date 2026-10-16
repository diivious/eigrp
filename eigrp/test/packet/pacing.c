// SPDX-License-Identifier: ISC
/* Packet pacing rate, idle-burst bound, slow-link precision, live updates. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_packet.h"

static void test_line_rate(void)
{
	eigrp_intf_t ei = {0};
	uint32_t i;

	ei.params.bandwidth = 1000000;  /* 1 Gbps in kbps */
	ei.bandwidth_percent = 50;
	for (i = 0; i < 41; i++) {
		assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 1000) == 0);
		eigrp_packet_pacing_charge(&ei, 1500);
	}
	assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 1000) == 1);
	/* A 20-second backlog at 50% does not create any additional pause. */
	assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 1001) == 0);
	/* And a 1-hour idle period cannot save an hour of burst credit. */
	assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 3601001) == 0);
	assert(ei.pacing_credit == ei.pacing_rate);
}

static void test_slow_link(void)
{
	eigrp_intf_t ei = {0};
	ei.params.bandwidth = 56;
	ei.bandwidth_percent = 50;
	assert(eigrp_packet_pacing_delay_msec(&ei, 512, 500) == 0);
	eigrp_packet_pacing_charge(&ei, 512);
	assert(eigrp_packet_pacing_delay_msec(&ei, 512, 500) == 147);
	assert(eigrp_packet_pacing_delay_msec(&ei, 512, 646) == 1);
	assert(eigrp_packet_pacing_delay_msec(&ei, 512, 647) == 0);
	eigrp_packet_pacing_charge(&ei, 512);
	assert(eigrp_packet_pacing_delay_msec(&ei, 512, 647) == 147);
}

static void test_configuration_change(void)
{
	eigrp_intf_t ei = {0};
	ei.params.bandwidth = 100000;
	ei.bandwidth_percent = 50;
	assert(eigrp_packet_pacing_delay_msec(&ei, 1400, 100) == 0);
	eigrp_packet_pacing_charge(&ei, 1400);
	ei.bandwidth_percent = 1;
	assert(eigrp_packet_pacing_delay_msec(&ei, 1400, 100) == 0);
	eigrp_packet_pacing_charge(&ei, 1400);
	assert(eigrp_packet_pacing_delay_msec(&ei, 1400, 100) == 12);
	ei.bandwidth_percent = 200;
	assert(eigrp_packet_pacing_delay_msec(&ei, 1400, 100) == 0);
}

static void test_slow_control(void)
{
	eigrp_intf_t ei = {0};
	ei.params.bandwidth = 1;
	ei.bandwidth_percent = 1;
	assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 0) == 0);
	eigrp_packet_pacing_charge(&ei, 1500);
	assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 0) == 1200000);
	assert(eigrp_packet_pacing_delay_msec(&ei, 1500, 1200000) == 0);
}

int main(void)
{
	test_line_rate();
	test_slow_link();
	test_configuration_change();
	test_slow_control();
	puts("pacing budget passed");
	return 0;
}
