// SPDX-License-Identifier: ISC
/* RTP sequencing and deferred multicast admission unit regression. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Keep the TRANSMIT ordering helpers private to the owning implementation;
 * compile them into this translation unit for an executable regression. */
#include "eigrp_packet.c"

static void test_adjacency_epoch(void)
{
	eigrp_packet_queue_t rtp = {0};
	eigrp_nbr_t nbr = {0};

	nbr.retrans_queue = &rtp;
	nbr.state = EIGRP_NEIGHBOR_UP;
	nbr.multicast_epoch = 8;
	assert(eigrp_packet_multicast_peer_eligible(&nbr, 8));
	assert(!eigrp_packet_multicast_peer_eligible(&nbr, 7));
	nbr.multicast_epoch = 9; /* neighbor went DOWN then UP */
	assert(!eigrp_packet_multicast_peer_eligible(&nbr, 8));
	nbr.state = EIGRP_NEIGHBOR_DOWN;
	assert(!eigrp_packet_multicast_peer_eligible(&nbr, 9));
}

static void test_deferred_multicast_precedes_later_unicast(void)
{
	eigrp_packet_queue_t rtp = {0};
	eigrp_packet_queue_t pending = {0};
	eigrp_intf_t ei = {0};
	eigrp_nbr_t nbr = {0};
	eigrp_packet_t older = {.sequence_number = 100};
	eigrp_packet_t later = {.sequence_number = 102};
	eigrp_packet_t multicast = {.sequence_number = 101, .multicast_epoch = 5};

	ei.multicast_pending = pending;
	nbr.ei = &ei;
	nbr.state = EIGRP_NEIGHBOR_UP;
	nbr.multicast_epoch = 5;
	nbr.retrans_queue = &rtp;

	/* An older, already-active unicast is eligible to advance. */
	eigrp_packet_enqueue(&rtp, &older);
	assert(!eigrp_packet_unicast_behind_multicast(&nbr, 100));
	assert(eigrp_packet_reliable_prior_count(&nbr, 101) == 1);

	/* Multicast is not admitted yet. Later unicast must wait for it. */
	eigrp_packet_enqueue(&ei.multicast_pending, &multicast);
	assert(eigrp_packet_unicast_behind_multicast(&nbr, 102));
	eigrp_packet_enqueue(&rtp, &later);
	assert(eigrp_packet_reliable_prior_count(&nbr, 101) == 1);
	assert(eigrp_packet_reliable_prior_count(&nbr, 103) == 2);

	/* Admission splices the multicast before the later unicast. */
	eigrp_packet_reliable_ordered_enqueue(&rtp, &multicast);
	assert(rtp.count == 3);
	assert(rtp.tail == &older);
	assert(older.previous == &multicast);
	assert(multicast.previous == &later);
	assert(rtp.head == &later);

	/* On an adjacency restart, the older pending packet must not block
	 * or be delivered to the newly established adjacency. */
	nbr.multicast_epoch = 6;
	assert(!eigrp_packet_unicast_behind_multicast(&nbr, 102));
	/* But an eligible later multicast must still block a newer unicast. */
	{
		eigrp_packet_t fresh = {.sequence_number = 103,
					      .multicast_epoch = 6};
		eigrp_packet_enqueue(&ei.multicast_pending, &fresh);
		assert(eigrp_packet_unicast_behind_multicast(&nbr, 104));
		assert(!eigrp_packet_unicast_behind_multicast(&nbr, 103));
	}
}

static void test_wrapped_sequence_order(void)
{
	eigrp_packet_queue_t rtp = {0};
	eigrp_packet_t a = {.sequence_number = UINT32_MAX - 1};
	eigrp_packet_t c = {.sequence_number = 1};
	eigrp_packet_t b = {.sequence_number = UINT32_MAX};

	eigrp_packet_enqueue(&rtp, &a);
	eigrp_packet_enqueue(&rtp, &c);
	eigrp_packet_reliable_ordered_enqueue(&rtp, &b);
	assert(rtp.tail == &a);
	assert(a.previous == &b);
	assert(b.previous == &c);
	assert(eigrp_packet_sequence_before(UINT32_MAX, 1));
}

int main(void)
{
	test_adjacency_epoch();
	test_deferred_multicast_precedes_later_unicast();
	test_wrapped_sequence_order();
	puts("RTP deferred multicast ordering passed");
	return 0;
}
