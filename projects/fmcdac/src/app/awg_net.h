#ifndef AWG_NET_H
#define AWG_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_NET_ETH_ADDR_LEN        6U
#define AWG_NET_IPV4_ADDR_LEN       4U
#define AWG_NET_ETH_HEADER_LEN      14U
#define AWG_NET_ARP_PACKET_LEN      28U
#define AWG_NET_IPV4_MIN_HEADER_LEN 20U
#define AWG_NET_UDP_HEADER_LEN      8U
#define AWG_NET_ARP_FRAME_LEN       (AWG_NET_ETH_HEADER_LEN + AWG_NET_ARP_PACKET_LEN)

#define AWG_NET_ETHERTYPE_IPV4      0x0800U
#define AWG_NET_ETHERTYPE_ARP       0x0806U
#define AWG_NET_IP_PROTOCOL_UDP     17U

enum awg_net_parse_result {
	AWG_NET_PARSE_IGNORED = 0,
	AWG_NET_PARSE_ARP_REQUEST = 1,
	AWG_NET_PARSE_UDP = 2,
	AWG_NET_PARSE_BAD_ARGUMENT = -1,
	AWG_NET_PARSE_TRUNCATED = -2,
	AWG_NET_PARSE_MALFORMED = -3,
	AWG_NET_PARSE_BAD_CHECKSUM = -4,
	AWG_NET_PARSE_FRAGMENTED = -5
};

enum awg_net_udp_checksum_policy {
	AWG_NET_UDP_CHECKSUM_DISABLED = 0,
	AWG_NET_UDP_CHECKSUM_ACCEPT_ZERO = 1,
	AWG_NET_UDP_CHECKSUM_REQUIRED = 2
};

struct awg_net_config {
	uint8_t mac_address[AWG_NET_ETH_ADDR_LEN];
	uint8_t ipv4_address[AWG_NET_IPV4_ADDR_LEN];
	uint16_t udp_port;
	enum awg_net_udp_checksum_policy rx_udp_checksum;
	bool tx_udp_checksum;
};

struct awg_net_packet {
	enum awg_net_parse_result type;
	uint8_t source_mac[AWG_NET_ETH_ADDR_LEN];
	uint8_t source_ipv4[AWG_NET_IPV4_ADDR_LEN];
	uint16_t source_port;
	uint16_t destination_port;
	const uint8_t *payload;
	size_t payload_length;
};

struct awg_net {
	struct awg_net_config config;
	uint16_t next_ipv4_id;
	uint32_t frames_seen;
	uint32_t frames_ignored;
	uint32_t malformed_frames;
	uint32_t checksum_drops;
	uint32_t arp_requests;
	uint32_t udp_packets;
};

int32_t awg_net_init(struct awg_net *net,
		     const struct awg_net_config *config);
enum awg_net_parse_result awg_net_parse_frame(struct awg_net *net,
					       const uint8_t *frame,
					       size_t length,
					       struct awg_net_packet *packet);
int32_t awg_net_build_arp_reply(const struct awg_net *net,
				const struct awg_net_packet *request,
				uint8_t *frame, size_t capacity,
				size_t *frame_length);
int32_t awg_net_build_udp(struct awg_net *net, const uint8_t destination_mac[6],
			  const uint8_t destination_ipv4[4],
			  uint16_t source_port, uint16_t destination_port,
			  const uint8_t *payload, size_t payload_length,
			  uint8_t *frame, size_t capacity,
			  size_t *frame_length);
uint16_t awg_net_ipv4_checksum(const uint8_t *data, size_t length);
uint16_t awg_net_udp_checksum_ipv4(const uint8_t source_ipv4[4],
				   const uint8_t destination_ipv4[4],
				   const uint8_t *udp, size_t udp_length);

#ifdef __cplusplus
}
#endif

#endif /* AWG_NET_H */
