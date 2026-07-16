#include <errno.h>
#include <string.h>

#include "awg_net.h"

#define AWG_NET_ARP_HTYPE_ETHERNET 1U
#define AWG_NET_ARP_OPERATION_REQUEST 1U
#define AWG_NET_ARP_OPERATION_REPLY 2U
#define AWG_NET_IPV4_FLAG_MORE_FRAGMENTS 0x2000U
#define AWG_NET_IPV4_FLAG_DONT_FRAGMENT 0x4000U
#define AWG_NET_IPV4_FRAGMENT_OFFSET_MASK 0x1FFFU
#define AWG_NET_IPV4_DEFAULT_TTL 64U

static uint16_t awg_net_read_be16(const uint8_t *data)
{
	return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void awg_net_write_be16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)value;
}

static bool awg_net_bytes_equal(const uint8_t *left, const uint8_t *right,
				size_t length)
{
	size_t index;

	for (index = 0U; index < length; index++) {
		if (left[index] != right[index])
			return false;
	}
	return true;
}

static bool awg_net_mac_is_broadcast(const uint8_t *address)
{
	uint8_t index;

	for (index = 0U; index < AWG_NET_ETH_ADDR_LEN; index++) {
		if (address[index] != 0xFFU)
			return false;
	}
	return true;
}

static bool awg_net_mac_is_zero(const uint8_t *address)
{
	uint8_t index;

	for (index = 0U; index < AWG_NET_ETH_ADDR_LEN; index++) {
		if (address[index] != 0U)
			return false;
	}
	return true;
}

static uint32_t awg_net_checksum_add(uint32_t sum, const uint8_t *data,
				     size_t length)
{
	while (length >= 2U) {
		sum += ((uint32_t)data[0] << 8) | data[1];
		data += 2;
		length -= 2U;
	}
	if (length)
		sum += (uint32_t)data[0] << 8;
	return sum;
}

static uint16_t awg_net_checksum_finish(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFFU) + (sum >> 16);
	return (uint16_t)~sum;
}

uint16_t awg_net_ipv4_checksum(const uint8_t *data, size_t length)
{
	if (!data && length)
		return 0U;
	return awg_net_checksum_finish(awg_net_checksum_add(0U, data, length));
}

uint16_t awg_net_udp_checksum_ipv4(const uint8_t source_ipv4[4],
				   const uint8_t destination_ipv4[4],
				   const uint8_t *udp, size_t udp_length)
{
	uint8_t pseudo_tail[4];
	uint32_t sum;

	if (!source_ipv4 || !destination_ipv4 || (!udp && udp_length) ||
	    udp_length > 0xFFFFU)
		return 0U;

	sum = awg_net_checksum_add(0U, source_ipv4, AWG_NET_IPV4_ADDR_LEN);
	sum = awg_net_checksum_add(sum, destination_ipv4,
				   AWG_NET_IPV4_ADDR_LEN);
	pseudo_tail[0] = 0U;
	pseudo_tail[1] = AWG_NET_IP_PROTOCOL_UDP;
	pseudo_tail[2] = (uint8_t)(udp_length >> 8);
	pseudo_tail[3] = (uint8_t)udp_length;
	sum = awg_net_checksum_add(sum, pseudo_tail, sizeof(pseudo_tail));
	sum = awg_net_checksum_add(sum, udp, udp_length);
	return awg_net_checksum_finish(sum);
}

int32_t awg_net_init(struct awg_net *net,
		     const struct awg_net_config *config)
{
	if (!net || !config || !config->udp_port)
		return -EINVAL;
	if (awg_net_mac_is_zero(config->mac_address) ||
	    awg_net_mac_is_broadcast(config->mac_address) ||
	    (config->mac_address[0] & 1U))
		return -EINVAL;
	if ((!config->ipv4_address[0] && !config->ipv4_address[1] &&
	     !config->ipv4_address[2] && !config->ipv4_address[3]) ||
	    config->ipv4_address[0] >= 224U)
		return -EINVAL;
	if (config->rx_udp_checksum < AWG_NET_UDP_CHECKSUM_DISABLED ||
	    config->rx_udp_checksum > AWG_NET_UDP_CHECKSUM_REQUIRED)
		return -EINVAL;

	memset(net, 0, sizeof(*net));
	net->config = *config;
	net->next_ipv4_id = 1U;
	return 0;
}

static enum awg_net_parse_result awg_net_ignore(struct awg_net *net)
{
	net->frames_ignored++;
	return AWG_NET_PARSE_IGNORED;
}

static enum awg_net_parse_result awg_net_malformed(
	struct awg_net *net, enum awg_net_parse_result error)
{
	net->malformed_frames++;
	if (error == AWG_NET_PARSE_BAD_CHECKSUM)
		net->checksum_drops++;
	return error;
}

static enum awg_net_parse_result awg_net_parse_arp(
	struct awg_net *net, const uint8_t *frame, size_t length,
	struct awg_net_packet *packet)
{
	const uint8_t *arp;

	if (length < AWG_NET_ARP_FRAME_LEN)
		return awg_net_malformed(net, AWG_NET_PARSE_TRUNCATED);

	arp = frame + AWG_NET_ETH_HEADER_LEN;
	if (awg_net_read_be16(arp) != AWG_NET_ARP_HTYPE_ETHERNET ||
	    awg_net_read_be16(arp + 2U) != AWG_NET_ETHERTYPE_IPV4 ||
	    arp[4] != AWG_NET_ETH_ADDR_LEN || arp[5] != AWG_NET_IPV4_ADDR_LEN)
		return awg_net_malformed(net, AWG_NET_PARSE_MALFORMED);
	if (awg_net_read_be16(arp + 6U) != AWG_NET_ARP_OPERATION_REQUEST)
		return awg_net_ignore(net);
	if (!awg_net_bytes_equal(arp + 24U, net->config.ipv4_address,
				 AWG_NET_IPV4_ADDR_LEN))
		return awg_net_ignore(net);
	if (!awg_net_bytes_equal(frame + 6U, arp + 8U,
				 AWG_NET_ETH_ADDR_LEN))
		return awg_net_malformed(net, AWG_NET_PARSE_MALFORMED);

	memcpy(packet->source_mac, arp + 8U, AWG_NET_ETH_ADDR_LEN);
	memcpy(packet->source_ipv4, arp + 14U, AWG_NET_IPV4_ADDR_LEN);
	packet->type = AWG_NET_PARSE_ARP_REQUEST;
	net->arp_requests++;
	return AWG_NET_PARSE_ARP_REQUEST;
}

static enum awg_net_parse_result awg_net_parse_ipv4(
	struct awg_net *net, const uint8_t *frame, size_t length,
	struct awg_net_packet *packet)
{
	const uint8_t *ipv4;
	const uint8_t *udp;
	size_t ipv4_available;
	size_t ipv4_header_length;
	size_t ipv4_total_length;
	size_t udp_length;
	uint16_t fragments;
	uint16_t udp_checksum;

	if (length < AWG_NET_ETH_HEADER_LEN + AWG_NET_IPV4_MIN_HEADER_LEN)
		return awg_net_malformed(net, AWG_NET_PARSE_TRUNCATED);

	ipv4 = frame + AWG_NET_ETH_HEADER_LEN;
	ipv4_available = length - AWG_NET_ETH_HEADER_LEN;
	if ((ipv4[0] >> 4) != 4U)
		return awg_net_malformed(net, AWG_NET_PARSE_MALFORMED);
	ipv4_header_length = (size_t)(ipv4[0] & 0x0FU) * 4U;
	if (ipv4_header_length < AWG_NET_IPV4_MIN_HEADER_LEN ||
	    ipv4_header_length > ipv4_available)
		return awg_net_malformed(net, AWG_NET_PARSE_TRUNCATED);

	ipv4_total_length = awg_net_read_be16(ipv4 + 2U);
	if (ipv4_total_length < ipv4_header_length ||
	    ipv4_total_length > ipv4_available)
		return awg_net_malformed(net, AWG_NET_PARSE_TRUNCATED);
	if (awg_net_ipv4_checksum(ipv4, ipv4_header_length) != 0U)
		return awg_net_malformed(net, AWG_NET_PARSE_BAD_CHECKSUM);

	fragments = awg_net_read_be16(ipv4 + 6U);
	if (fragments & (AWG_NET_IPV4_FLAG_MORE_FRAGMENTS |
			 AWG_NET_IPV4_FRAGMENT_OFFSET_MASK))
		return awg_net_malformed(net, AWG_NET_PARSE_FRAGMENTED);
	if (ipv4[9] != AWG_NET_IP_PROTOCOL_UDP)
		return awg_net_ignore(net);
	if (!awg_net_bytes_equal(ipv4 + 16U, net->config.ipv4_address,
				 AWG_NET_IPV4_ADDR_LEN))
		return awg_net_ignore(net);
	if (ipv4_total_length < ipv4_header_length + AWG_NET_UDP_HEADER_LEN)
		return awg_net_malformed(net, AWG_NET_PARSE_TRUNCATED);

	udp = ipv4 + ipv4_header_length;
	udp_length = awg_net_read_be16(udp + 4U);
	if (udp_length < AWG_NET_UDP_HEADER_LEN ||
	    udp_length != ipv4_total_length - ipv4_header_length)
		return awg_net_malformed(net, AWG_NET_PARSE_MALFORMED);
	if (awg_net_read_be16(udp + 2U) != net->config.udp_port)
		return awg_net_ignore(net);

	udp_checksum = awg_net_read_be16(udp + 6U);
	if (!udp_checksum &&
	    net->config.rx_udp_checksum == AWG_NET_UDP_CHECKSUM_REQUIRED)
		return awg_net_malformed(net, AWG_NET_PARSE_BAD_CHECKSUM);
	if (udp_checksum &&
	    net->config.rx_udp_checksum != AWG_NET_UDP_CHECKSUM_DISABLED &&
	    awg_net_udp_checksum_ipv4(ipv4 + 12U, ipv4 + 16U, udp,
				      udp_length) != 0U)
		return awg_net_malformed(net, AWG_NET_PARSE_BAD_CHECKSUM);

	memcpy(packet->source_mac, frame + 6U, AWG_NET_ETH_ADDR_LEN);
	memcpy(packet->source_ipv4, ipv4 + 12U, AWG_NET_IPV4_ADDR_LEN);
	packet->source_port = awg_net_read_be16(udp);
	packet->destination_port = awg_net_read_be16(udp + 2U);
	packet->payload = udp + AWG_NET_UDP_HEADER_LEN;
	packet->payload_length = udp_length - AWG_NET_UDP_HEADER_LEN;
	packet->type = AWG_NET_PARSE_UDP;
	net->udp_packets++;
	return AWG_NET_PARSE_UDP;
}

enum awg_net_parse_result awg_net_parse_frame(struct awg_net *net,
					       const uint8_t *frame,
					       size_t length,
					       struct awg_net_packet *packet)
{
	uint16_t ethertype;

	if (!net || !frame || !packet)
		return AWG_NET_PARSE_BAD_ARGUMENT;

	memset(packet, 0, sizeof(*packet));
	net->frames_seen++;
	if (length < AWG_NET_ETH_HEADER_LEN)
		return awg_net_malformed(net, AWG_NET_PARSE_TRUNCATED);
	if (!awg_net_bytes_equal(frame, net->config.mac_address,
				 AWG_NET_ETH_ADDR_LEN) &&
	    !awg_net_mac_is_broadcast(frame))
		return awg_net_ignore(net);

	ethertype = awg_net_read_be16(frame + 12U);
	if (ethertype == AWG_NET_ETHERTYPE_ARP)
		return awg_net_parse_arp(net, frame, length, packet);
	if (ethertype == AWG_NET_ETHERTYPE_IPV4)
		return awg_net_parse_ipv4(net, frame, length, packet);
	return awg_net_ignore(net);
}

int32_t awg_net_build_arp_reply(const struct awg_net *net,
				const struct awg_net_packet *request,
				uint8_t *frame, size_t capacity,
				size_t *frame_length)
{
	uint8_t *arp;

	if (!net || !request || !frame || !frame_length ||
	    request->type != AWG_NET_PARSE_ARP_REQUEST)
		return -EINVAL;
	if (capacity < AWG_NET_ARP_FRAME_LEN)
		return -ENOSPC;

	memcpy(frame, request->source_mac, AWG_NET_ETH_ADDR_LEN);
	memcpy(frame + 6U, net->config.mac_address, AWG_NET_ETH_ADDR_LEN);
	awg_net_write_be16(frame + 12U, AWG_NET_ETHERTYPE_ARP);

	arp = frame + AWG_NET_ETH_HEADER_LEN;
	awg_net_write_be16(arp, AWG_NET_ARP_HTYPE_ETHERNET);
	awg_net_write_be16(arp + 2U, AWG_NET_ETHERTYPE_IPV4);
	arp[4] = AWG_NET_ETH_ADDR_LEN;
	arp[5] = AWG_NET_IPV4_ADDR_LEN;
	awg_net_write_be16(arp + 6U, AWG_NET_ARP_OPERATION_REPLY);
	memcpy(arp + 8U, net->config.mac_address, AWG_NET_ETH_ADDR_LEN);
	memcpy(arp + 14U, net->config.ipv4_address, AWG_NET_IPV4_ADDR_LEN);
	memcpy(arp + 18U, request->source_mac, AWG_NET_ETH_ADDR_LEN);
	memcpy(arp + 24U, request->source_ipv4, AWG_NET_IPV4_ADDR_LEN);

	*frame_length = AWG_NET_ARP_FRAME_LEN;
	return 0;
}

int32_t awg_net_build_udp(struct awg_net *net, const uint8_t destination_mac[6],
			  const uint8_t destination_ipv4[4],
			  uint16_t source_port, uint16_t destination_port,
			  const uint8_t *payload, size_t payload_length,
			  uint8_t *frame, size_t capacity,
			  size_t *frame_length)
{
	uint8_t *ipv4;
	uint8_t *udp;
	size_t ipv4_length;
	size_t udp_length;
	size_t total_length;
	uint16_t checksum;

	if (!net || !destination_mac || !destination_ipv4 || !source_port ||
	    !destination_port || (!payload && payload_length) || !frame ||
	    !frame_length)
		return -EINVAL;
	if (awg_net_mac_is_zero(destination_mac) ||
	    payload_length > 0xFFFFU - AWG_NET_IPV4_MIN_HEADER_LEN -
			     AWG_NET_UDP_HEADER_LEN)
		return -EMSGSIZE;

	udp_length = AWG_NET_UDP_HEADER_LEN + payload_length;
	ipv4_length = AWG_NET_IPV4_MIN_HEADER_LEN + udp_length;
	total_length = AWG_NET_ETH_HEADER_LEN + ipv4_length;
	if (capacity < total_length)
		return -ENOSPC;

	/* Move payload first so an overlapping source inside frame is safe. */
	if (payload_length)
		memmove(frame + AWG_NET_ETH_HEADER_LEN +
			AWG_NET_IPV4_MIN_HEADER_LEN + AWG_NET_UDP_HEADER_LEN,
			payload, payload_length);

	memcpy(frame, destination_mac, AWG_NET_ETH_ADDR_LEN);
	memcpy(frame + 6U, net->config.mac_address, AWG_NET_ETH_ADDR_LEN);
	awg_net_write_be16(frame + 12U, AWG_NET_ETHERTYPE_IPV4);

	ipv4 = frame + AWG_NET_ETH_HEADER_LEN;
	memset(ipv4, 0, AWG_NET_IPV4_MIN_HEADER_LEN);
	ipv4[0] = 0x45U;
	awg_net_write_be16(ipv4 + 2U, (uint16_t)ipv4_length);
	awg_net_write_be16(ipv4 + 4U, net->next_ipv4_id++);
	awg_net_write_be16(ipv4 + 6U, AWG_NET_IPV4_FLAG_DONT_FRAGMENT);
	ipv4[8] = AWG_NET_IPV4_DEFAULT_TTL;
	ipv4[9] = AWG_NET_IP_PROTOCOL_UDP;
	memcpy(ipv4 + 12U, net->config.ipv4_address,
	       AWG_NET_IPV4_ADDR_LEN);
	memcpy(ipv4 + 16U, destination_ipv4, AWG_NET_IPV4_ADDR_LEN);
	checksum = awg_net_ipv4_checksum(ipv4, AWG_NET_IPV4_MIN_HEADER_LEN);
	awg_net_write_be16(ipv4 + 10U, checksum);

	udp = ipv4 + AWG_NET_IPV4_MIN_HEADER_LEN;
	awg_net_write_be16(udp, source_port);
	awg_net_write_be16(udp + 2U, destination_port);
	awg_net_write_be16(udp + 4U, (uint16_t)udp_length);
	awg_net_write_be16(udp + 6U, 0U);
	if (net->config.tx_udp_checksum) {
		checksum = awg_net_udp_checksum_ipv4(ipv4 + 12U, ipv4 + 16U,
						     udp, udp_length);
		/* RFC 768 transmits a computed zero checksum as all ones. */
		if (!checksum)
			checksum = 0xFFFFU;
		awg_net_write_be16(udp + 6U, checksum);
	}

	*frame_length = total_length;
	return 0;
}
