// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Sending and Receiving EIGRP Hello Packets.
 * Copyright (C) 2013-2016
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
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_tlv1.h"
#include "eigrpd/eigrp_tlv2.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_errors.h"

/*
 * @fn eigrp_hello_timer
 *
 * @param[in]   event  current execution event timer is associated with
 *
 * @return void
 *
 * @par
 * Called once per "hello" time interval, default 5 seconds
 * Sends hello packet via multicast for all interfaces eigrp
 * is configured for
 */
void eigrp_hello_timer(struct event *event)
{
	eigrp_interface_t *ei;

	ei = EVENT_ARG(event);

	if (IS_DEBUG_EIGRP(0, TIMERS))
		zlog_debug("Start Hello Timer (%s) Expire [%u]",
			   EIGRP_INTF_NAME(ei), ei->params.v_hello);

	/* Sending hello packet. */
	eigrp_hello_send(ei, EIGRP_HELLO_NORMAL, NULL);

	/* Hello timer set. */
	event_add_timer(eigrpd_event, eigrp_hello_timer, ei, ei->params.v_hello,
			 &ei->t_hello);

	return;
}

static bool eigrp_hello_k_same(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr)
{
	if ((eigrp->k_values[0] == nbr->K1) &&
	    (eigrp->k_values[1] == nbr->K2) &&
	    (eigrp->k_values[2] == nbr->K3) &&
	    (eigrp->k_values[3] == nbr->K4) &&
	    (eigrp->k_values[4] == nbr->K5)) {
		return TRUE;
	}
	return FALSE;
}

static void eigrp_hello_k_update(eigrp_neighbor_t *nbr,
				 struct TLV_Parameter_Type *param)
{
	/* copy over the values passed in by the neighbor */
	nbr->K1 = param->K1;
	nbr->K2 = param->K2;
	nbr->K3 = param->K3;
	nbr->K4 = param->K4;
	nbr->K5 = param->K5;
	nbr->K6 = param->K6;
}

/**
 * @fn eigrp_hello_parameter_decode
 *
 * @param[in]		eigrp	which AS/AFI we working with
 * @param[in]		nbr	neighbor the ACK should be sent to
 * @param[in]		param	pointer packet TLV is stored to
 *
 * @return uint16_t	number of bytes added to packet stream
 *
 * @par
 * Encode Parameter TLV, used to convey metric weights and the hold time.
 *
 * @usage
 * Note the addition of K6 for the new extended metrics, and does not apply to
 * older TLV packet formats.
 */
static eigrp_neighbor_t *
eigrp_hello_parameter_decode(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			     struct eigrp_tlv_hdr_type *tlv)
{
	struct TLV_Parameter_Type *param = (struct TLV_Parameter_Type *)tlv;

	/* update nbr k values */
	eigrp_hello_k_update(nbr, param);

	/* upader holding param */
	nbr->v_holddown = ntohs(param->hold_time);

	/*
	 * Check K1-K5 have the correct values to be able to become neighbors
	 * K6 does not have to match
	 */
	if (eigrp_hello_k_same(eigrp, nbr)) {
		if (eigrp_nbr_state_get(nbr) == EIGRP_NEIGHBOR_DOWN) {
			zlog_info("Neighbor %s (%s) is pending: new adjacency",
				  eigrp_print_addr(&nbr->src),
				  ifindex2ifname(nbr->ei->ifp->ifindex,
						 eigrp->vrf_id));

			/* Expedited hello sent */
			eigrp_hello_send(nbr->ei, EIGRP_HELLO_NORMAL, &nbr->src);

			//     if(ntohl(nbr->ei->address->u.prefix4.s_addr) >
			//     ntohl(nbr->src.s_addr))
			eigrp_update_send_init(eigrp, nbr);
			eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_PENDING);
		}
	} else {
		if (eigrp_nbr_state_get(nbr) != EIGRP_NEIGHBOR_DOWN) {
			if ((param->K1 & param->K2 & param->K3 & param->K4 & param->K5) == 255) {
				zlog_info(
					"Neighbor %s (%s) is down: Interface PEER-TERMINATION received",
					eigrp_print_addr(&nbr->src),
					ifindex2ifname(nbr->ei->ifp->ifindex,
						       eigrp->vrf_id));
				eigrp_nbr_delete(nbr);
				return NULL;
			} else {
				zlog_info(
					"Neighbor %s (%s) going down: Kvalue mismatch",
					eigrp_print_addr(&nbr->src),
					ifindex2ifname(nbr->ei->ifp->ifindex,
						       eigrp->vrf_id));
				eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
			}
		}
	}

	return nbr;
}

/**
 * @fn eigrp_sw_version_decode
 *
 * @param[in]		nbr	neighbor the ACK shoudl be sent to
 * @param[in]		param	pointer to TLV software version information
 *
 * @return void
 *
 * @par
 * Read the software version in the specified location.
 * This consists of two bytes of OS version, and two bytes of EIGRP
 * revision number.
 */
static void eigrp_sw_version_decode(eigrp_neighbor_t *nbr,
				    eigrp_interface_t *ei,
				    struct eigrp_tlv_hdr_type *tlv, bool new)
{
	struct TLV_Software_Type *version = (struct TLV_Software_Type *)tlv;

	(void)new;

	nbr->os_rel_major = version->vender_major;
	nbr->os_rel_minor = version->vender_minor;
	nbr->tlv_rel_major = version->eigrp_major;
	nbr->tlv_rel_minor = version->eigrp_minor;

	/*
	 * TLV major version 2 peers use multiprotocol/wide TLVs. Any other
	 * currently supported peer uses classic TLV1 encoding.
	 */
	if (nbr->tlv_rel_major == EIGRP_TLV_64B_VERSION)
		eigrp_tlv2_neighbor_bind(nbr, &ei->eigrp->tlv2_codec);
	else
		eigrp_tlv1_neighbor_bind(nbr, &ei->eigrp->tlv1_codec);
}

/**
 * @fn eigrp_peer_termination_decode
 *
 * Read the address in the TLV and match to out address. If
 * a match is found, move the sending neighbor to the down state. If
 * out address is not in the TLV, then ignore the peer termination
 */
static void eigrp_peer_termination_decode(eigrp_instance_t *eigrp,
					  eigrp_neighbor_t *nbr,
					  struct eigrp_tlv_hdr_type *tlv)
{
	struct TLV_Peer_Termination_type *param =
		(struct TLV_Peer_Termination_type *)tlv;

	uint32_t my_ip = nbr->ei->address.u.prefix4.s_addr;
	uint32_t received_ip = param->neighbor_ip;

	if (my_ip == received_ip) {
		zlog_info("Neighbor %s (%s) is down: Peer Termination received",
			  eigrp_print_addr(&nbr->src),
			  ifindex2ifname(nbr->ei->ifp->ifindex, eigrp->vrf_id));
		/* set neighbor to DOWN */
		eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
		/* delete neighbor */
		eigrp_nbr_delete(nbr);
	}
}

/**
 * @fn eigrp_peer_termination_encode
 *
 * Function used to encode Peer Termination TLV to Hello packet.
 */
static uint16_t eigrp_peer_termination_encode(struct stream *s,
					      eigrp_addr_t *nbr_addr)
{
	uint16_t length = EIGRP_TLV_PEER_TERMINATION_LEN;

	/* fill in type and length */
	stream_putw(s, EIGRP_TLV_PEER_TERMINATION);
	stream_putw(s, length);

	/* fill in unknown field 0x04 */
	stream_putc(s, 0x04);

	/* finally neighbor IP address */
	//DVS: Ipv6 issue
	stream_put_ipv4(s, nbr_addr->ip.v4.s_addr);

	return (length);
}

/*
 * @fn eigrp_hello_receive
 *
 * @param[in]   eigrp           eigrp routing process
 * @param[in]   eigrph          pointer to eigrp header
 * @param[in]   src             source address of neighbor sending hello
 * @param[in]   ei              eigrp interface packet arrived on
 * @param[in]   s               input ip stream
 * @param[in]   size            size of eigrp packet
 *
 * @return void
 *
 * @par
 * This is the main worker function for processing hello packets. It
 * will validate the peer associated with the src ip address of the ip
 * header, and then decode each of the general TLVs which the packet
 * may contain.
 *
 * @usage
 * Not all TLVs are current decoder.  This is a work in progress..
 */
void eigrp_hello_receive(eigrp_instance_t *eigrp, struct eigrp_header *eigrph,
			 eigrp_addr_t *src, eigrp_interface_t *ei,
			 struct stream *s, int size)
{
	eigrp_neighbor_t *nbr;
	struct eigrp_tlv_hdr_type *tlv_header;
	uint16_t type;
	uint16_t length;
	bool new_nbr = FALSE;

	/* check for mall formed packet, if so abort now */
	size -= EIGRP_HEADER_LEN;
	if (size < 0)
		return;

	/* see if we know this neighbor, if not, then lets make friends */
	nbr = eigrp_nbr_lookup(ei, eigrph, src);
	if (!nbr) {
		nbr = eigrp_nbr_create(ei, src);
		new_nbr = TRUE;
	}

	/* humm... inti failed, this is bad it means we are out of memory.. */
	if (!nbr) {
		return;
	}

	tlv_header = (struct eigrp_tlv_hdr_type *)eigrph->tlv;
	do {
		type = ntohs(tlv_header->type);
		length = ntohs(tlv_header->length);

		if ((length > 0) && (length <= size)) {
			// determine what General TLV is being processed
			switch (type) {
			case EIGRP_TLV_PARAMETER:
				nbr = eigrp_hello_parameter_decode(eigrp, nbr,
								   tlv_header);
				if (!nbr)
					return;
				break;
			case EIGRP_TLV_AUTH:
				/*
				 * Authentication policy and digest validation are packet-level
				 * checks completed before Hello TLV processing.
				 */
				break;
			case EIGRP_TLV_SEQ:
				break;
			case EIGRP_TLV_SW_VERSION:
				eigrp_sw_version_decode(nbr, ei, tlv_header,
							new_nbr);
				break;
			case EIGRP_TLV_NEXT_MCAST_SEQ:
				break;
			case EIGRP_TLV_PEER_TERMINATION:
				eigrp_peer_termination_decode(eigrp, nbr,
							      tlv_header);
				return;
				break;
			case EIGRP_TLV_PEER_MTRLIST:
			case EIGRP_TLV_PEER_TIDLIST:
				break;
			default:
				break;
			}
		}

		tlv_header = (struct eigrp_tlv_hdr_type *)(((char *)tlv_header)
							   + length);
		size -= length;
	} while (size > 0);

	/*If received packet is hello with Parameter TLV*/
	if (ntohl(eigrph->ack) == 0) {
		/* increment statistics. */
		ei->stats.rcvd.hello++;
		if (nbr)
			eigrp_nbr_state_update(nbr);
	}

}

uint32_t FRR_MAJOR;
uint32_t FRR_MINOR;

void eigrp_sw_version_init(void)
{
	char ver_string[] = VERSION;
	char *dash = strstr(ver_string, "-");
	int ret;

	if (dash)
		dash[0] = '\0';

	ret = sscanf(ver_string, "%" SCNu32 ".%" SCNu32, &FRR_MAJOR,
		     &FRR_MINOR);
	if (ret != 2)
		flog_err(EC_EIGRP_PACKET,
			 "Did not Properly parse %s, please fix VERSION string",
			 VERSION);
}

/**
 * @fn eigrp_sw_version_encode
 *
 * @param[in,out]	s	packet stream TLV is stored to
 *
 * @return uint16_t	number of bytes added to packet stream
 *
 * @par
 * Store the software version in the specified location.
 * This consists of two bytes of OS version, and two bytes of EIGRP
 * revision number.
 */
static uint16_t eigrp_sw_version_encode(struct stream *s)
{
	uint16_t length = EIGRP_TLV_SW_VERSION_LEN;

	// setup the tlv fields
	stream_putw(s, EIGRP_TLV_SW_VERSION);
	stream_putw(s, length);

	stream_putc(s, FRR_MAJOR); //!< major os version
	stream_putc(s, FRR_MINOR); //!< minor os version

	/* and the core eigrp version */
	stream_putc(s, EIGRP_MAJOR_VERSION);
	stream_putc(s, EIGRP_MINOR_VERSION);

	return (length);
}

/**
 * @fn eigrp_tidlist_encode
 *
 * @param[in,out]	s	packet stream TLV is stored to
 *
 * @return void
 *
 * @par
 * If doing mutli-topology, then store the supported TID list.
 * This is currently a place holder function
 */
static uint16_t eigrp_tidlist_encode(struct stream *s)
{
	// uint16_t length = EIGRP_TLV_SW_VERSION_LEN;
	return 0;
}

/**
 * @fn eigrp_sequence_encode
 *
 * @param[in,out]       s       packet stream TLV is stored to
 *
 * @return uint16_t    number of bytes added to packet stream
 *
 * @par
 * Part of conditional receive process
 *
 */
static uint16_t eigrp_sequence_encode(eigrp_instance_t *eigrp, struct stream *s)
{
	uint16_t length = EIGRP_TLV_SEQ_BASE_LEN;
	eigrp_interface_t *ei;
	struct listnode *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;
	size_t backup_end, size_end;
	int found;

	// add in the parameters TLV
	backup_end = stream_get_endp(s);
	stream_putw(s, EIGRP_TLV_SEQ);
	size_end = s->endp;
	stream_putw(s, 0x0000);
	stream_putc(s, IPV4_MAX_BYTELEN);

	found = 0;
	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
			if (nbr->multicast_queue->count > 0) {
				length += (uint16_t)stream_put_ipv4(
					s, nbr->src.ip.v4.s_addr);
				found = 1;
			}
		}
	}

	if (found == 0) {
		stream_set_endp(s, backup_end);
		return 0;
	}

	backup_end = stream_get_endp(s);
	stream_set_endp(s, size_end);
	stream_putw(s, length);
	stream_set_endp(s, backup_end);

	return length;
}

/**
 * @fn eigrp_sequence_encode
 *
 * @param[in,out]       s       packet stream TLV is stored to
 *
 * @return uint16_t    number of bytes added to packet stream
 *
 * @par
 * Part of conditional receive process
 *
 */
static uint16_t eigrp_next_sequence_encode(eigrp_instance_t *eigrp,
					   struct stream *s)
{
	uint16_t length = EIGRP_NEXT_SEQUENCE_TLV_SIZE;

	// add in the parameters TLV
	stream_putw(s, EIGRP_TLV_NEXT_MCAST_SEQ);
	stream_putw(s, EIGRP_NEXT_SEQUENCE_TLV_SIZE);
	stream_putl(s, eigrp->sequence_number + 1);

	return length;
}

/**
 * @fn eigrp_hello_parameter_encode
 *
 * @param[in]		ei	pointer to interface hello packet came in on
 * @param[in,out]	s	packet stream TLV is stored to
 *
 * @return uint16_t	number of bytes added to packet stream
 *
 * @par
 * Encode Parameter TLV, used to convey metric weights and the hold time.
 *
 * @usage
 * Note the addition of K6 for the new extended metrics, and does not apply to
 * older TLV packet formats.
 */
static uint16_t eigrp_hello_parameter_encode(eigrp_interface_t *ei,
					     struct stream *s, uint8_t flags)
{
	// add in the parameters TLV
	stream_putw(s, EIGRP_TLV_PARAMETER);
	stream_putw(s, EIGRP_TLV_PARAMETER_LEN);

	// if graceful shutdown is needed to be announced, send all 255 in K
	// values
	if (flags & EIGRP_HELLO_GRACEFUL_SHUTDOWN) {
		stream_putc(s, 0xff); /* K1 */
		stream_putc(s, 0xff); /* K2 */
		stream_putc(s, 0xff); /* K3 */
		stream_putc(s, 0xff); /* K4 */
		stream_putc(s, 0xff); /* K5 */
		stream_putc(s, 0xff); /* K6 */
	} else			      // set k values
	{
		stream_putc(s, ei->eigrp->k_values[0]); /* K1 */
		stream_putc(s, ei->eigrp->k_values[1]); /* K2 */
		stream_putc(s, ei->eigrp->k_values[2]); /* K3 */
		stream_putc(s, ei->eigrp->k_values[3]); /* K4 */
		stream_putc(s, ei->eigrp->k_values[4]); /* K5 */
		stream_putc(s, ei->eigrp->k_values[5]); /* K6 */
	}

	// and set hold time value..
	stream_putw(s, ei->params.v_wait);

	return EIGRP_TLV_PARAMETER_LEN;
}

/**
 * @fn eigrp_hello_encode
 *
 * @param[in]		ei	pointer to interface hello packet came in on
 * @param[in]		s	packet stream TLV is stored to
 * @param[in]		ack	 if non-zero, neigbors sequence packet to ack
 * @param[in]		flags  type of hello packet
 * @param[in]		nbr_addr  pointer to neighbor address for Peer
 * Termination TLV
 *
 * @return eigrp_packet		pointer initialize hello packet
 *
 * @par
 * Allocate an EIGRP hello packet, and add in the the approperate TLVs
 *
 */
static eigrp_packet_t *eigrp_hello_encode(eigrp_interface_t *ei, in_addr_t addr,
					  uint32_t ack, uint8_t flags,
					  eigrp_addr_t *nbr_addr)
{
	eigrp_packet_t *packet;
	uint16_t length = EIGRP_HEADER_LEN;

	// allocate a new packet to be sent
	packet = eigrp_packet_new(EIGRP_PACKET_MTU(ei->ifp->mtu), NULL);

	if (packet) {
		// encode common header feilds
		eigrp_packet_header_init(EIGRP_OPC_HELLO, ei->eigrp, packet->s, 0,
					 0, ack);

		// encode Authentication TLV
		if ((ei->params.auth_type == EIGRP_AUTH_TYPE_MD5)
		    && (ei->params.auth_keychain != NULL)) {
			length += eigrp_add_authTLV_MD5_encode(packet->s, ei);
		} else if ((ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256)
			   && (ei->params.auth_keychain != NULL)) {
			length += eigrp_add_authTLV_SHA256_encode(packet->s, ei);
		}

		/* encode appropriate parameters to Hello packet */
		if (flags & EIGRP_HELLO_GRACEFUL_SHUTDOWN)
			length += eigrp_hello_parameter_encode(
				ei, packet->s, EIGRP_HELLO_GRACEFUL_SHUTDOWN);
		else
			length += eigrp_hello_parameter_encode(
				ei, packet->s, EIGRP_HELLO_NORMAL);

		// figure out the version of code we're running
		length += eigrp_sw_version_encode(packet->s);

		if (flags & EIGRP_HELLO_ADD_SEQUENCE) {
			length += eigrp_sequence_encode(ei->eigrp, packet->s);
			length += eigrp_next_sequence_encode(ei->eigrp, packet->s);
		}

		// add in the TID list if doing multi-topology
		length += eigrp_tidlist_encode(packet->s);

		/* encode Peer Termination TLV if needed */
		if (flags & EIGRP_HELLO_GRACEFUL_SHUTDOWN_NBR)
			length += eigrp_peer_termination_encode(packet->s, nbr_addr);

		// Set packet length
		packet->length = length;

		// set soruce address for the hello packet
		//DVS: ipv6 issue
		//eigrp_addr_copy(packet->dst, addr);
		packet->dst.ip.v4.s_addr = addr;

		if ((ei->params.auth_type == EIGRP_AUTH_TYPE_MD5)
		    && (ei->params.auth_keychain != NULL)) {
			eigrp_make_md5_digest(ei, packet->s,
					      EIGRP_AUTH_BASIC_HELLO_FLAG);
		} else if ((ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256)
			   && (ei->params.auth_keychain != NULL)) {
			eigrp_make_sha256_digest(ei, packet->s,
						 EIGRP_AUTH_BASIC_HELLO_FLAG);
		}

		// EIGRP Checksum
		eigrp_packet_checksum(ei, packet->s, length);
	}

	return (packet);
}

/**
 * @fn eigrp_hello_send_ack
 *
 * @param[in]		nbr	neighbor the ACK should be sent to
 *
 * @return void
 *
 * @par
 *  Send (unicast) a hello packet with the destination address
 *  associated with the neighbor.  The eigrp header ACK feild will be
 *  updated to the neighbor's sequence number to acknolodge any
 *  outstanding packets
 */
void eigrp_hello_send_ack(eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *packet;

	/* if packet succesfully created, add it to the interface queue */
	packet = eigrp_hello_encode(nbr->ei, nbr->src.ip.v4.s_addr,
				nbr->recv_sequence_number, EIGRP_HELLO_NORMAL,
				&nbr->src);

	if (packet) {
		/* Add packet to the top of the interface output queue*/
		eigrp_packet_enqueue(nbr->ei->obuf, packet);

		/* Hook event to write packet. */
		if (nbr->ei->on_write_q == 0) {
			listnode_add(nbr->ei->eigrp->oi_write_q, nbr->ei);
			nbr->ei->on_write_q = 1;
		}
		EIGRP_EVENT_ADD_WRITE(nbr->ei->eigrp);
	}
}

/**
 * @fn eigrp_hello_send
 *
 * @param[in]		ei	pointer to interface hello should be sent
 * @param[in]		flags type of hello packet
 * @param[in]		nbr_addr  pointer to neighbor address for Peer
 * Termination TLV
 *
 * @return void
 *
 * @par
 * Build and enqueue a generic (multicast) periodic hello packet for
 * sending.  If no packets are currently queues, the packet will be
 * sent immadiatly
 */
void eigrp_hello_send(eigrp_interface_t *ei, uint8_t flags,
		      eigrp_addr_t *nbr_addr)
{
	eigrp_packet_t *packet = NULL;

	/* If this is passive interface, do not send EIGRP Hello.
	   if ((EIGRP_INTF_PASSIVE_STATUS (ei) == EIGRP_INTF_PASSIVE) ||
	   (ei->type != EIGRP_IFTYPE_NBMA))
	   return;
	*/

	/* if packet was succesfully created, then add it to the interface queue
	 */
	packet = eigrp_hello_encode(ei, htonl(EIGRP_MULTICAST_ADDRESS), 0, flags, nbr_addr);

	if (packet) {
		// Add packet to the top of the interface output queue
		eigrp_packet_enqueue(ei->obuf, packet);

		/* Hook event to write packet. */
		if (ei->on_write_q == 0) {
			listnode_add(ei->eigrp->oi_write_q, ei);
			ei->on_write_q = 1;
		}

		if (ei->eigrp->t_write == NULL) {
			if (flags & EIGRP_HELLO_GRACEFUL_SHUTDOWN) {
				event_execute(eigrpd_event, eigrp_packet_write,
					      ei->eigrp, ei->eigrp->fd, NULL);
			} else {
				EIGRP_EVENT_ADD_WRITE(ei->eigrp);
			}
		}
	}
}
