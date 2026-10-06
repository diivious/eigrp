// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP private implementation types.
 * Copyright (C) 2018, 2026
 * Authors:
 *   Donnie Savage
 */
#ifndef _ZEBRA_EIGRP_TYPES_H_
#define _ZEBRA_EIGRP_TYPES_H_

#include <time.h>
#include <pthread.h>

#include "eigrp.h"
#include "eigrp_const.h"
#include "eigrp_list.h"
#include "eigrp_stream.h"
#include "eigrp_sys.h"

/* Portable EIGRP process/ownership hierarchy. */
typedef struct eigrp_packet_input eigrp_packet_input_t;
typedef struct eigrp_timer_event eigrp_timer_event_t;
typedef struct eigrp_packetizer_work eigrp_packetizer_work_t;
typedef struct eigrp_message_request eigrp_message_request_t;
typedef struct eigrp_af_event eigrp_af_event_t;

struct eigrp_process {
	eigrp_list_t *virt_router;
	time_t start_time;
	uint8_t options;

	/* Process-owned receive/demux execution context. */
	pthread_mutex_t hierarchy_lock;
	pthread_mutex_t receive_lock;
	pthread_cond_t receive_cond;
	pthread_t receive_thread;
	bool receive_running;
	bool receive_started;
	eigrp_packet_input_t *receive_head;
	eigrp_packet_input_t *receive_tail;
	size_t receive_count;
	uint64_t receive_drop_count;
};


typedef eigrp_result_t (*eigrp_message_handler_t)(eigrp_instance_t *eigrp, void *arg);

struct eigrp_message_request {
	eigrp_message_handler_t handler;
	void *arg;
	eigrp_result_t result;
	bool complete;
	pthread_cond_t complete_cond;
	eigrp_message_request_t *next;
};

typedef enum eigrp_af_event_type {
	EIGRP_AF_EVENT_ROUTERID_UPDATE,
	EIGRP_AF_EVENT_INTF_UPDATE,
	EIGRP_AF_EVENT_INTF_DOWN,
	EIGRP_AF_EVENT_INTF_REMOVE,
	EIGRP_AF_EVENT_INTF_ADDR_UPDATE,
} eigrp_af_event_type_t;

struct eigrp_af_event {
	eigrp_af_event_type_t type;
	/* Owned copy for events carrying interface runtime state. */
	char *interface_name;
	union {
		eigrp_intf_runtime_state_t intf_update;
		struct {
			eigrp_ifindex_t ifindex;
			uint8_t type;
			uint32_t bandwidth;
			uint32_t mtu;
		} intf_down;
		struct {
			eigrp_ifindex_t ifindex;
			eigrp_intf_remove_reason_t reason;
		} intf_remove;
		struct {
			eigrp_ifindex_t ifindex;
			eigrp_prefix_t address;
			eigrp_intf_remove_reason_t reason;
		} intf_addr_update;
	} data;
	eigrp_af_event_t *next;
};

struct eigrp_virt_router {
	eigrp_process_t *process;
	eigrp_vrid_t vrid;
	char *name;
	bool shutdown;
	eigrp_list_t *af_instance;
};

extern eigrp_process_t eigrp_process;

#define EIGRP_SHUTDOWN (1U << 0) /* deferred shutdown */

/* Internal mutable policy state. Public adapters receive only snapshots. */
typedef struct eigrp_filter_runtime_state {
	char *access_list[EIGRP_FILTER_MAX];
	char *prefix_list[EIGRP_FILTER_MAX];
} eigrp_filter_runtime_state_t;

/* Private implementation objects. */
typedef struct eigrp_addr eigrp_addr_t;
typedef struct eigrp_prefix_descriptor eigrp_prefix_descriptor_t;
typedef struct eigrp_route_descriptor eigrp_route_descriptor_t;
typedef struct eigrp_fsm_action_message eigrp_fsm_action_message_t;
typedef struct eigrp_eventlog eigrp_eventlog_t;
typedef struct eigrp_table eigrp_table_t;
typedef struct eigrp_table_node eigrp_table_node_t;
typedef struct eigrp_network_config eigrp_network_config_t;
typedef struct eigrp_nbr_config eigrp_nbr_config_t;
typedef struct eigrp_summary_config eigrp_summary_config_t;
typedef struct eigrp_redist_config eigrp_redist_config_t;
typedef struct eigrp_distribute_list_config eigrp_distribute_list_config_t;
typedef struct eigrp_offset_config eigrp_offset_config_t;
typedef struct eigrp_metric_config eigrp_metric_config_t;
typedef struct eigrp_summary_state eigrp_summary_state_t;
typedef struct eigrp_timer_config eigrp_timer_config_t;
typedef struct eigrp_nbr_policy_state eigrp_nbr_policy_state_t;
typedef struct eigrp_nbr_warning_state eigrp_nbr_warning_state_t;
typedef struct eigrp_redist_policy_config eigrp_redist_policy_config_t;

// basic packet processor definitions
typedef struct eigrp_packet eigrp_packet_t;
typedef struct eigrp_tlv_header eigrp_tlv_header_t;

typedef eigrp_route_descriptor_t *(*eigrp_packet_decoder_t)(
	eigrp_instance_t *eigrp, eigrp_nbr_t *nbr, eigrp_stream_t *pkt,
	uint16_t pktlen);

typedef uint16_t (*eigrp_packet_encoder_t)(
	eigrp_instance_t *eigrp, eigrp_intf_t *ei, eigrp_nbr_t *nbr,
	eigrp_stream_t *pkt, eigrp_route_descriptor_t *route);

typedef struct eigrp_message {
	int value;
	const char *name;
} eigrp_message_t;

const char *eigrp_message_lookup(const eigrp_message_t *messages, int value,
                                 const char *fallback);

typedef struct eigrp_tlv_codec {
	eigrp_packet_encoder_t encoder;
	eigrp_packet_decoder_t decoder;
} eigrp_tlv_codec_t;

typedef struct eigrp_af_vectors {
	eigrp_afi_t afi;

	/*
	 * Network-layer packet envelope.  Common EIGRP packet processing owns
	 * queueing, acknowledgements, checksum/authentication, and opcode
	 * dispatch; the AF owns the IP header/socket representation and extracts
	 * the native EIGRP source/destination addresses.  Send/receive may be
	 * unbound only while an address-family data path is capability-gated.
	 * Runtime creation validates the complete vector before packet processing
	 * is started; common packet code does not probe callbacks before use.
	 */
	int (*packet_send)(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
			   eigrp_packet_t *packet);
	bool (*packet_receive)(eigrp_instance_t *eigrp,
			       eigrp_stream_t *stream, eigrp_intf_t **ei,
			       eigrp_addr_t *source, eigrp_addr_t *destination,
			       eigrp_packet_rx_meta_t *meta);
	bool (*packet_source_on_link)(eigrp_intf_t *ei,
				      const eigrp_addr_t *source);

	/*
	 * Address-family wire primitives.  Address encoding is for a complete
	 * runtime address (for example, a classic-TLV next hop); prefix encoding
	 * owns the EIGRP prefix-length/significant-byte representation.
	 */
	uint8_t packet_address_bytes;
	uint16_t (*packet_address_decode)(eigrp_stream_t *stream,
					eigrp_addr_t *address);
	uint16_t (*packet_address_encode)(eigrp_stream_t *stream,
					const eigrp_addr_t *address);
	uint16_t (*packet_prefix_decode)(eigrp_stream_t *stream,
				       eigrp_prefix_t *prefix);
	uint16_t (*packet_prefix_encode)(eigrp_stream_t *stream,
				       const eigrp_prefix_t *prefix);

	/*
	 * Route-TLV address-family selectors.  TLV1/TLV2 remain the route codec
	 * owners because classic and multiprotocol metrics/exterior sections are
	 * independent of address family.  The AF supplies only the family-specific
	 * route type/AFI values and destination representation.
	 */
	uint16_t classic_internal_tlv_type;
	uint16_t classic_external_tlv_type;
	uint16_t multiprotocol_afi;

	/* Portable text presentation used by show/debug callers. */
	int (*addr_snprintf)(char *buf, size_t len,
			     const eigrp_addr_t *address);

	/* AF-specific classful automatic-summary derivation.  Every AF binds a
	 * function; families without classful semantics return UNSUPPORTED.
	 */
	eigrp_result_t (*summary_auto_prefix)(const eigrp_prefix_t *component,
					      eigrp_prefix_t *summary);
} eigrp_af_vectors_t;

/* AF modules expose only vector initialization. */
void eigrp_ipv4_init(eigrp_af_vectors_t *vectors);
void eigrp_ipv6_init(eigrp_af_vectors_t *vectors);


#endif /* _ZEBRA_EIGRP_TYPES_H_ */
