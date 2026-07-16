#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "awg_net.h"

#define TEST_FRAME_CAPACITY 256U
#define TEST_IPV4_OFFSET AWG_NET_ETH_HEADER_LEN
#define TEST_UDP_OFFSET (TEST_IPV4_OFFSET + AWG_NET_IPV4_MIN_HEADER_LEN)
#define TEST_PAYLOAD_OFFSET (TEST_UDP_OFFSET + AWG_NET_UDP_HEADER_LEN)

static const uint8_t test_device_mac[AWG_NET_ETH_ADDR_LEN] = {
	0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U
};
static const uint8_t test_host_mac[AWG_NET_ETH_ADDR_LEN] = {
	0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
};
static const uint8_t test_other_mac[AWG_NET_ETH_ADDR_LEN] = {
	0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U
};
static const uint8_t test_device_ip[AWG_NET_IPV4_ADDR_LEN] = {
	192U, 0U, 2U, 2U
};
static const uint8_t test_host_ip[AWG_NET_IPV4_ADDR_LEN] = {
	192U, 0U, 2U, 1U
};
static const uint8_t test_other_ip[AWG_NET_IPV4_ADDR_LEN] = {
	192U, 0U, 2U, 99U
};

static unsigned int test_checks;

#define CHECK(condition) do { \
	test_checks++; \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
			__func__, __LINE__, #condition); \
		return false; \
	} \
} while (0)

static void put_be16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)(value >> 8);
	data[1] = (uint8_t)value;
}

static uint16_t get_be16(const uint8_t *data)
{
	return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static struct awg_net_config make_config(const uint8_t mac[6],
					 const uint8_t ipv4[4], uint16_t port,
					 enum awg_net_udp_checksum_policy policy,
					 bool transmit_checksum)
{
	struct awg_net_config config;

	memset(&config, 0, sizeof(config));
	memcpy(config.mac_address, mac, AWG_NET_ETH_ADDR_LEN);
	memcpy(config.ipv4_address, ipv4, AWG_NET_IPV4_ADDR_LEN);
	config.udp_port = port;
	config.rx_udp_checksum = policy;
	config.tx_udp_checksum = transmit_checksum;
	return config;
}

static bool init_endpoint(struct awg_net *net, const uint8_t mac[6],
			  const uint8_t ipv4[4], uint16_t port,
			  enum awg_net_udp_checksum_policy policy,
			  bool transmit_checksum)
{
	struct awg_net_config config = make_config(mac, ipv4, port, policy,
						    transmit_checksum);

	return awg_net_init(net, &config) == 0;
}

static bool build_udp_to(struct awg_net *sender,
			 const uint8_t destination_mac[6],
			 const uint8_t destination_ipv4[4],
			 uint16_t destination_port,
			 const uint8_t *payload, size_t payload_length,
			 uint8_t frame[TEST_FRAME_CAPACITY], size_t *length)
{
	memset(frame, 0xA5, TEST_FRAME_CAPACITY);
	return awg_net_build_udp(sender, destination_mac, destination_ipv4,
				 UINT16_C(41000), destination_port, payload,
				 payload_length, frame, TEST_FRAME_CAPACITY,
				 length) == 0;
}

static void refresh_ipv4_checksum(uint8_t *frame)
{
	uint8_t *ipv4 = frame + TEST_IPV4_OFFSET;
	size_t header_length = (size_t)(ipv4[0] & 0x0FU) * 4U;

	put_be16(ipv4 + 10U, 0U);
	put_be16(ipv4 + 10U, awg_net_ipv4_checksum(ipv4, header_length));
}

static bool test_init_validation(void)
{
	static const uint8_t zero_mac[AWG_NET_ETH_ADDR_LEN] = { 0U };
	static const uint8_t broadcast_mac[AWG_NET_ETH_ADDR_LEN] = {
		0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
	};
	static const uint8_t multicast_mac[AWG_NET_ETH_ADDR_LEN] = {
		0x01U, 0x00U, 0x5EU, 0x00U, 0x00U, 0x01U
	};
	static const uint8_t zero_ip[AWG_NET_IPV4_ADDR_LEN] = { 0U };
	static const uint8_t multicast_ip[AWG_NET_IPV4_ADDR_LEN] = {
		224U, 0U, 0U, 1U
	};
	struct awg_net_config config = make_config(test_device_mac,
		test_device_ip, UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
		true);
	struct awg_net net;

	CHECK(awg_net_init(NULL, &config) == -EINVAL);
	CHECK(awg_net_init(&net, NULL) == -EINVAL);
	config.udp_port = 0U;
	CHECK(awg_net_init(&net, &config) == -EINVAL);

	config = make_config(zero_mac, test_device_ip, UINT16_C(5000),
			     AWG_NET_UDP_CHECKSUM_REQUIRED, true);
	CHECK(awg_net_init(&net, &config) == -EINVAL);
	memcpy(config.mac_address, broadcast_mac, sizeof(broadcast_mac));
	CHECK(awg_net_init(&net, &config) == -EINVAL);
	memcpy(config.mac_address, multicast_mac, sizeof(multicast_mac));
	CHECK(awg_net_init(&net, &config) == -EINVAL);

	config = make_config(test_device_mac, zero_ip, UINT16_C(5000),
			     AWG_NET_UDP_CHECKSUM_REQUIRED, true);
	CHECK(awg_net_init(&net, &config) == -EINVAL);
	memcpy(config.ipv4_address, multicast_ip, sizeof(multicast_ip));
	CHECK(awg_net_init(&net, &config) == -EINVAL);

	config = make_config(test_device_mac, test_device_ip, UINT16_C(5000),
			     (enum awg_net_udp_checksum_policy)3, true);
	CHECK(awg_net_init(&net, &config) == -EINVAL);

	config = make_config(test_device_mac, test_device_ip, UINT16_C(5000),
			     AWG_NET_UDP_CHECKSUM_REQUIRED, true);
	memset(&net, 0xA5, sizeof(net));
	CHECK(awg_net_init(&net, &config) == 0);
	CHECK(memcmp(&net.config, &config, sizeof(config)) == 0);
	CHECK(net.next_ipv4_id == 1U);
	CHECK(net.frames_seen == 0U && net.frames_ignored == 0U);
	CHECK(net.malformed_frames == 0U && net.checksum_drops == 0U);
	CHECK(net.arp_requests == 0U && net.udp_packets == 0U);
	return true;
}

static bool test_ipv4_checksum_golden_and_roundtrip(void)
{
	/* RFC 1071-style IPv4 example: checksum field is 0xB861. */
	static const uint8_t header_with_checksum[AWG_NET_IPV4_MIN_HEADER_LEN] = {
		0x45U, 0x00U, 0x00U, 0x73U, 0x00U, 0x00U, 0x40U, 0x00U,
		0x40U, 0x11U, 0xB8U, 0x61U, 0xC0U, 0xA8U, 0x00U, 0x01U,
		0xC0U, 0xA8U, 0x00U, 0xC7U
	};
	uint8_t header[AWG_NET_IPV4_MIN_HEADER_LEN];

	memcpy(header, header_with_checksum, sizeof(header));
	put_be16(header + 10U, 0U);
	CHECK(awg_net_ipv4_checksum(header, sizeof(header)) == UINT16_C(0xB861));
	memcpy(header, header_with_checksum, sizeof(header));
	CHECK(awg_net_ipv4_checksum(header, sizeof(header)) == 0U);
	CHECK(awg_net_ipv4_checksum(NULL, 0U) == UINT16_C(0xFFFF));
	CHECK(awg_net_ipv4_checksum(NULL, 1U) == 0U);
	return true;
}

static bool test_udp_build_parse_roundtrip(void)
{
	static const uint8_t payload[] = {
		0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0xA5U
	};
	struct awg_net sender;
	struct awg_net receiver;
	struct awg_net_packet packet;
	uint8_t frame[TEST_FRAME_CAPACITY];
	size_t length = 0U;

	CHECK(init_endpoint(&sender, test_host_mac, test_host_ip,
			    UINT16_C(41000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	CHECK(length == TEST_PAYLOAD_OFFSET + sizeof(payload));
	CHECK(memcmp(frame, test_device_mac, AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(memcmp(frame + 6U, test_host_mac, AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(get_be16(frame + 12U) == AWG_NET_ETHERTYPE_IPV4);
	CHECK(awg_net_ipv4_checksum(frame + TEST_IPV4_OFFSET,
				    AWG_NET_IPV4_MIN_HEADER_LEN) == 0U);
	CHECK(get_be16(frame + TEST_UDP_OFFSET + 6U) != 0U);
	CHECK(awg_net_udp_checksum_ipv4(frame + TEST_IPV4_OFFSET + 12U,
		frame + TEST_IPV4_OFFSET + 16U, frame + TEST_UDP_OFFSET,
		AWG_NET_UDP_HEADER_LEN + sizeof(payload)) == 0U);

	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_UDP);
	CHECK(packet.type == AWG_NET_PARSE_UDP);
	CHECK(memcmp(packet.source_mac, test_host_mac,
		     AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(memcmp(packet.source_ipv4, test_host_ip,
		     AWG_NET_IPV4_ADDR_LEN) == 0);
	CHECK(packet.source_port == UINT16_C(41000));
	CHECK(packet.destination_port == UINT16_C(5000));
	CHECK(packet.payload == frame + TEST_PAYLOAD_OFFSET);
	CHECK(packet.payload_length == sizeof(payload));
	CHECK(memcmp(packet.payload, payload, sizeof(payload)) == 0);
	CHECK(receiver.frames_seen == 1U && receiver.udp_packets == 1U);
	CHECK(sender.next_ipv4_id == 2U);
	return true;
}

static bool test_destination_filtering(void)
{
	static const uint8_t payload[] = { 1U, 2U, 3U };
	struct awg_net sender;
	struct awg_net receiver;
	struct awg_net_packet packet;
	uint8_t frame[TEST_FRAME_CAPACITY];
	size_t length;

	CHECK(init_endpoint(&sender, test_host_mac, test_host_ip,
			    UINT16_C(41000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));

	CHECK(build_udp_to(&sender, test_other_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_IGNORED);

	CHECK(build_udp_to(&sender, test_device_mac, test_other_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_IGNORED);

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5001), payload, sizeof(payload), frame,
			   &length));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_IGNORED);
	CHECK(receiver.frames_ignored == 3U);
	CHECK(receiver.udp_packets == 0U);
	return true;
}

static bool test_udp_zero_checksum_policy(void)
{
	static const uint8_t payload[] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };
	struct awg_net sender;
	struct awg_net receiver;
	struct awg_net_packet packet;
	uint8_t frame[TEST_FRAME_CAPACITY];
	size_t length;

	CHECK(init_endpoint(&sender, test_host_mac, test_host_ip,
			    UINT16_C(41000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    false));
	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	CHECK(get_be16(frame + TEST_UDP_OFFSET + 6U) == 0U);

	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_BAD_CHECKSUM);
	CHECK(receiver.checksum_drops == 1U);

	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_ACCEPT_ZERO,
			    true));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_UDP);

	put_be16(frame + TEST_UDP_OFFSET + 6U, UINT16_C(0x1234));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_BAD_CHECKSUM);

	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_DISABLED,
			    true));
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_UDP);
	return true;
}

static bool test_bad_ipv4_and_udp_checksums(void)
{
	static const uint8_t payload[] = { 9U, 8U, 7U, 6U, 5U };
	struct awg_net sender;
	struct awg_net receiver;
	struct awg_net_packet packet;
	uint8_t frame[TEST_FRAME_CAPACITY];
	size_t length;

	CHECK(init_endpoint(&sender, test_host_mac, test_host_ip,
			    UINT16_C(41000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	frame[TEST_IPV4_OFFSET + 8U] ^= 1U;
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_BAD_CHECKSUM);

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	frame[TEST_PAYLOAD_OFFSET + 2U] ^= 1U;
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_BAD_CHECKSUM);
	CHECK(receiver.checksum_drops == 2U);
	CHECK(receiver.udp_packets == 0U);
	return true;
}

static bool test_fragmentation_rejection(void)
{
	static const uint8_t payload[] = { 0x5AU };
	struct awg_net sender;
	struct awg_net receiver;
	struct awg_net_packet packet;
	uint8_t frame[TEST_FRAME_CAPACITY];
	size_t length;

	CHECK(init_endpoint(&sender, test_host_mac, test_host_ip,
			    UINT16_C(41000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	put_be16(frame + TEST_IPV4_OFFSET + 6U, UINT16_C(0x2000));
	refresh_ipv4_checksum(frame);
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_FRAGMENTED);

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	put_be16(frame + TEST_IPV4_OFFSET + 6U, UINT16_C(0x0001));
	refresh_ipv4_checksum(frame);
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_FRAGMENTED);
	CHECK(receiver.malformed_frames == 2U);
	return true;
}

static bool test_truncated_and_malformed_lengths(void)
{
	static const uint8_t payload[] = { 1U, 3U, 5U, 7U };
	struct awg_net sender;
	struct awg_net receiver;
	struct awg_net_packet packet;
	uint8_t frame[TEST_FRAME_CAPACITY];
	size_t length;

	CHECK(init_endpoint(&sender, test_host_mac, test_host_ip,
			    UINT16_C(41000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(init_endpoint(&receiver, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));

	CHECK(awg_net_parse_frame(&receiver, frame,
				  AWG_NET_ETH_HEADER_LEN - 1U, &packet) ==
	      AWG_NET_PARSE_TRUNCATED);
	CHECK(awg_net_parse_frame(&receiver, frame,
				  AWG_NET_ETH_HEADER_LEN +
				  AWG_NET_IPV4_MIN_HEADER_LEN - 1U,
				  &packet) == AWG_NET_PARSE_TRUNCATED);

	put_be16(frame + TEST_IPV4_OFFSET + 2U,
		 (uint16_t)(length - AWG_NET_ETH_HEADER_LEN + 1U));
	refresh_ipv4_checksum(frame);
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_TRUNCATED);

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	frame[TEST_IPV4_OFFSET] = 0x44U;
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_TRUNCATED);

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	put_be16(frame + TEST_UDP_OFFSET + 4U,
		 AWG_NET_UDP_HEADER_LEN - 1U);
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_MALFORMED);

	CHECK(build_udp_to(&sender, test_device_mac, test_device_ip,
			   UINT16_C(5000), payload, sizeof(payload), frame,
			   &length));
	put_be16(frame + TEST_UDP_OFFSET + 4U,
		 AWG_NET_UDP_HEADER_LEN + sizeof(payload) - 1U);
	CHECK(awg_net_parse_frame(&receiver, frame, length, &packet) ==
	      AWG_NET_PARSE_MALFORMED);
	CHECK(receiver.malformed_frames == 6U);
	return true;
}

static size_t build_arp_request(uint8_t frame[TEST_FRAME_CAPACITY])
{
	static const uint8_t broadcast[AWG_NET_ETH_ADDR_LEN] = {
		0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
	};
	uint8_t *arp;

	memset(frame, 0, TEST_FRAME_CAPACITY);
	memcpy(frame, broadcast, AWG_NET_ETH_ADDR_LEN);
	memcpy(frame + 6U, test_host_mac, AWG_NET_ETH_ADDR_LEN);
	put_be16(frame + 12U, AWG_NET_ETHERTYPE_ARP);
	arp = frame + AWG_NET_ETH_HEADER_LEN;
	put_be16(arp, 1U);
	put_be16(arp + 2U, AWG_NET_ETHERTYPE_IPV4);
	arp[4] = AWG_NET_ETH_ADDR_LEN;
	arp[5] = AWG_NET_IPV4_ADDR_LEN;
	put_be16(arp + 6U, 1U);
	memcpy(arp + 8U, test_host_mac, AWG_NET_ETH_ADDR_LEN);
	memcpy(arp + 14U, test_host_ip, AWG_NET_IPV4_ADDR_LEN);
	memcpy(arp + 24U, test_device_ip, AWG_NET_IPV4_ADDR_LEN);
	return AWG_NET_ARP_FRAME_LEN;
}

static bool test_arp_request_and_reply(void)
{
	struct awg_net net;
	struct awg_net_packet request;
	uint8_t request_frame[TEST_FRAME_CAPACITY];
	uint8_t reply[TEST_FRAME_CAPACITY];
	const uint8_t *arp;
	size_t request_length;
	size_t reply_length = 0U;

	CHECK(init_endpoint(&net, test_device_mac, test_device_ip,
			    UINT16_C(5000), AWG_NET_UDP_CHECKSUM_REQUIRED,
			    true));
	request_length = build_arp_request(request_frame);
	CHECK(awg_net_parse_frame(&net, request_frame, request_length,
				  &request) == AWG_NET_PARSE_ARP_REQUEST);
	CHECK(request.type == AWG_NET_PARSE_ARP_REQUEST);
	CHECK(memcmp(request.source_mac, test_host_mac,
		     AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(memcmp(request.source_ipv4, test_host_ip,
		     AWG_NET_IPV4_ADDR_LEN) == 0);
	CHECK(net.arp_requests == 1U);

	memset(reply, 0xA5, sizeof(reply));
	CHECK(awg_net_build_arp_reply(&net, &request, reply,
				      AWG_NET_ARP_FRAME_LEN - 1U,
				      &reply_length) == -ENOSPC);
	CHECK(awg_net_build_arp_reply(&net, &request, reply, sizeof(reply),
				      &reply_length) == 0);
	CHECK(reply_length == AWG_NET_ARP_FRAME_LEN);
	CHECK(memcmp(reply, test_host_mac, AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(memcmp(reply + 6U, test_device_mac, AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(get_be16(reply + 12U) == AWG_NET_ETHERTYPE_ARP);

	arp = reply + AWG_NET_ETH_HEADER_LEN;
	CHECK(get_be16(arp) == 1U);
	CHECK(get_be16(arp + 2U) == AWG_NET_ETHERTYPE_IPV4);
	CHECK(arp[4] == AWG_NET_ETH_ADDR_LEN &&
	      arp[5] == AWG_NET_IPV4_ADDR_LEN);
	CHECK(get_be16(arp + 6U) == 2U);
	CHECK(memcmp(arp + 8U, test_device_mac, AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(memcmp(arp + 14U, test_device_ip,
		     AWG_NET_IPV4_ADDR_LEN) == 0);
	CHECK(memcmp(arp + 18U, test_host_mac, AWG_NET_ETH_ADDR_LEN) == 0);
	CHECK(memcmp(arp + 24U, test_host_ip, AWG_NET_IPV4_ADDR_LEN) == 0);
	CHECK(reply[AWG_NET_ARP_FRAME_LEN] == 0xA5U);
	return true;
}

struct test_case {
	const char *name;
	bool (*run)(void);
};

int main(void)
{
	static const struct test_case tests[] = {
		{ "init validation", test_init_validation },
		{ "IPv4 checksum golden and roundtrip",
		  test_ipv4_checksum_golden_and_roundtrip },
		{ "UDP build/parse roundtrip", test_udp_build_parse_roundtrip },
		{ "destination filtering", test_destination_filtering },
		{ "UDP zero-checksum policy", test_udp_zero_checksum_policy },
		{ "bad IPv4 and UDP checksums", test_bad_ipv4_and_udp_checksums },
		{ "fragmentation rejection", test_fragmentation_rejection },
		{ "truncated and malformed lengths",
		  test_truncated_and_malformed_lengths },
		{ "ARP request and reply", test_arp_request_and_reply },
	};
	size_t index;

	for (index = 0U; index < sizeof(tests) / sizeof(tests[0]); index++) {
		if (!tests[index].run()) {
			fprintf(stderr, "FAIL: %s\n", tests[index].name);
			return 1;
		}
		printf("PASS: %s\n", tests[index].name);
	}

	printf("%u checks passed\n", test_checks);
	return 0;
}
