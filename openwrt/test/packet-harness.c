// SPDX-License-Identifier: GPL-2.0-or-later
/* Native OW-06 framing and protocol-88 socket UUT. No route changes. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <net/if.h>

#include "eigrp_owrt_packet.h"
#include "eigrp_owrt_runtime.h"

struct eigrp_instance { eigrp_afi_t afi; eigrp_vrf_id_t vrf; };
struct eigrp_interface { unsigned int ifindex; eigrp_prefix_t address; };

/* Only the EIGRP-owned accessors and OW-04 FD registration are linked here.
 * The production implementation links the actual core and OW-04 scheduler. */
static int bindings;
eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *eigrp)
{ return eigrp->afi; }
eigrp_vrf_id_t eigrp_instance_vrf_id(const eigrp_instance_t *eigrp)
{ return eigrp->vrf; }
eigrp_ifindex_t eigrp_intf_ifindex(const eigrp_intf_t *intf)
{ return intf->ifindex; }
eigrp_result_t eigrp_intf_address_read(const eigrp_intf_t *intf,
	eigrp_prefix_t *prefix)
{ *prefix = intf->address; return EIGRP_RESULT_SUCCESS; }
bool eigrp_owrt_runtime_fd_set(eigrp_instance_t *eigrp, int fd)
{ (void)eigrp; (void)fd; bindings++; return true; }
void eigrp_owrt_runtime_fd_clear(eigrp_instance_t *eigrp)
{ (void)eigrp; bindings--; }

static void ipv4_test(void)
{
	uint8_t bytes[64] = {0};
	eigrp_address_t source, destination;
	eigrp_packet_rx_meta_t meta;
	uint16_t len = htons(40);
	bytes[0] = 0x45;
	memcpy(bytes + 2, &len, sizeof(len));
	bytes[9] = EIGRP_OWRT_IP_PROTOCOL;
	assert(inet_pton(AF_INET, "192.0.2.1", bytes + 12) == 1);
	assert(inet_pton(AF_INET, "224.0.0.10", bytes + 16) == 1);
	assert(eigrp_owrt_packet_ipv4_decode(bytes, 40, 8, &source,
		&destination, &meta));
	assert(meta.network_header_length == 20 && meta.eigrp_length == 20);
	assert(meta.destination_multicast && source.afi == EIGRP_AFI_IPV4);
	assert(destination.afi == EIGRP_AFI_IPV4);
	assert(!eigrp_owrt_packet_ipv4_decode(bytes, 40, 0, &source, &destination, &meta));
	bytes[0] = 0x4f;
	assert(!eigrp_owrt_packet_ipv4_decode(bytes, 40, 8, &source, &destination, &meta));
	bytes[0] = 0x45;
	bytes[9] = 17;
	assert(!eigrp_owrt_packet_ipv4_decode(bytes, 40, 8, &source, &destination, &meta));
	bytes[9] = 88;
	bytes[7] = 1; /* Noninitial fragment. */
	assert(!eigrp_owrt_packet_ipv4_decode(bytes, 40, 8, &source, &destination, &meta));
	bytes[7] = 0;
	bytes[3] = 41; /* Stated length is larger than copied packet. */
	assert(!eigrp_owrt_packet_ipv4_decode(bytes, 40, 8, &source, &destination, &meta));
	bytes[3] = 40;
	bytes[0] = 0x46;
	assert(!eigrp_owrt_packet_ipv4_decode(bytes, 40, 8, &source, &destination, &meta));
	/* Header length 24 with only 16 bytes of EIGRP must be rejected. */
}

static void ipv6_test(void)
{
	uint8_t data[24] = {0}, src[16], dest[16];
	eigrp_address_t source, destination;
	eigrp_packet_rx_meta_t meta;
	assert(inet_pton(AF_INET6, "fe80::10", src) == 1);
	assert(inet_pton(AF_INET6, "ff02::a", dest) == 1);
	assert(eigrp_owrt_packet_ipv6_decode(data, 24, 8, src, dest,
		&source, &destination, &meta));
	assert(meta.network_header_length == 0 && meta.eigrp_length == 24);
	assert(meta.destination_multicast && source.afi == EIGRP_AFI_IPV6);
	assert(!eigrp_owrt_packet_ipv6_decode(data, 19, 8, src, dest,
		&source, &destination, &meta));
	assert(!eigrp_owrt_packet_ipv6_decode(data, 24, 0, src, dest,
		&source, &destination, &meta));
	assert(inet_pton(AF_INET6, "fe80::20", dest) == 1);
	assert(eigrp_owrt_packet_ipv6_decode(data, 24, 8, src, dest,
		&source, &destination, &meta));
	assert(!meta.destination_multicast);
}

static void raw_io_test(void)
{
	struct eigrp_instance v4 = {EIGRP_AFI_IPV4, 0},
		v6 = {EIGRP_AFI_IPV6, 0}, other_vrf = {EIGRP_AFI_IPV4, 7};
	struct eigrp_interface lo = {0};
	eigrp_address_t dst, src, received_dst;
	eigrp_packet_rx_meta_t meta;
	uint8_t pkt[20] = {2, 5}, received[1024];
	size_t length = 0;
	eigrp_ifindex_t ifindex = 0;
	int probe, attempt;
	bool got = false;

	/* A container without CAP_NET_RAW can still run all parser tests. */
	probe = socket(AF_INET, SOCK_RAW, EIGRP_OWRT_IP_PROTOCOL);
	if (probe < 0 && (errno == EPERM || errno == EACCES)) {
		puts("OW-06 raw I/O: SKIP (CAP_NET_RAW unavailable)");
		return;
	}
	assert(probe >= 0);
	close(probe);
	lo.ifindex = if_nametoindex("lo");
	assert(lo.ifindex != 0);
	lo.address.address.afi = EIGRP_AFI_IPV4;
	lo.address.prefix_length = 8;
	assert(inet_pton(AF_INET, "127.0.0.1", lo.address.address.bytes) == 1);
	memset(&dst, 0, sizeof(dst));
	dst.afi = EIGRP_AFI_IPV4;
	memcpy(dst.bytes, lo.address.address.bytes, 4);
	assert(eigrp_sys_socket_open(&other_vrf) == EIGRP_RESULT_UNSUPPORTED);
	assert(eigrp_sys_socket_open(&v4) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_sys_socket_open(&v4) == EIGRP_RESULT_SUCCESS);
	assert(bindings == 1);
	assert(eigrp_sys_packet_send(&v4, &lo, &dst, pkt, sizeof(pkt)) == (int)sizeof(pkt));
	for (attempt = 0; attempt < 250 && !got; ++attempt) {
		got = eigrp_sys_packet_receive(&v4, received, sizeof(received),
			&length, &ifindex, &src, &received_dst, &meta);
		if (!got)
			usleep(1000);
	}
	assert(got);
	assert(length == 40 && meta.network_header_length == 20);
	assert(ifindex == lo.ifindex && src.afi == EIGRP_AFI_IPV4);
	assert(meta.eigrp_length == 20 && !meta.destination_multicast);
	eigrp_sys_socket_close(&v4);
	eigrp_sys_socket_close(&v4);
	assert(bindings == 0);
	assert(eigrp_sys_packet_send(&v4, &lo, &dst, pkt, 20) == -ENOTCONN);
	/* IPv6 raw unicast, when the namespace supports IPv6. */
	probe = socket(AF_INET6, SOCK_RAW, EIGRP_OWRT_IP_PROTOCOL);
	if (probe >= 0) {
		close(probe);
		assert(eigrp_sys_socket_open(&v6) == EIGRP_RESULT_SUCCESS);
		assert(bindings == 1);
		lo.address.address.afi = EIGRP_AFI_IPV6;
		lo.address.prefix_length = 128;
		assert(inet_pton(AF_INET6, "::1", lo.address.address.bytes) == 1);
		dst.afi = EIGRP_AFI_IPV6;
		memcpy(dst.bytes, lo.address.address.bytes, 16);
		assert(eigrp_sys_packet_send(&v6, &lo, &dst, pkt, sizeof(pkt)) ==
			(int)sizeof(pkt));
		got = false;
		for (attempt = 0; attempt < 250 && !got; ++attempt) {
			got = eigrp_sys_packet_receive(&v6, received, sizeof(received),
				&length, &ifindex, &src, &received_dst, &meta);
			if (!got)
				usleep(1000);
		}
		assert(got);
		assert(length == 20 && meta.network_header_length == 0);
		assert(meta.eigrp_length == 20 && ifindex == lo.ifindex);
		assert(src.afi == EIGRP_AFI_IPV6 && !meta.destination_multicast);
		eigrp_sys_socket_close(&v6);
		assert(bindings == 0);
		puts("OW-06 raw IPv6 loopback I/O: PASS");
	} else {
		puts("OW-06 raw IPv6 I/O: SKIP (IPv6 raw socket unavailable)");
	}
	puts("OW-06 raw IPv4 loopback I/O: PASS");
}

int main(void)
{
	ipv4_test();
	ipv6_test();
	raw_io_test();
	puts("OpenWrt OW-06 packet framing/socket UUT: PASS");
	return 0;
}
