#ifndef AWG_PHASE_F_H
#define AWG_PHASE_F_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "awg_eth_mac.h"
#include "awg_eth_rx.h"
#include "awg_eth_tx.h"
#include "awg_c1_validate.h"
#include "awg_net.h"
#include "awg_sched_dma.h"
#include "awg_stream_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_PHASE_F_FRAME_BYTES              9216U
#define AWG_PHASE_F_MAX_RX_FRAME_BYTES \
	(AWG_NET_ETH_HEADER_LEN + AWG_NET_IPV4_MIN_HEADER_LEN + \
	 AWG_NET_UDP_HEADER_LEN + AWG_STREAM_PROTO_V2_HEADER_BYTES + \
	 (AWG_STREAM_PROTO_V2_MAX_FRAME_RECORDS * \
	  AWG_STREAM_PROTO_V2_RECORD_BYTES) + AWG_STREAM_PROTO_CRC_BYTES)

#define AWG_PHASE_F_AWGX_ID                  0x41574758U
#define AWG_PHASE_F_AWGX_VERSION             0x00010000U
#define AWG_PHASE_F_AWGX_REG_ID              0x0000U
#define AWG_PHASE_F_AWGX_REG_VERSION         0x0004U
#define AWG_PHASE_F_AWGX_REG_CAPS            0x0008U
#define AWG_PHASE_F_AWGX_REG_CONTROL         0x000CU
#define AWG_PHASE_F_AWGX_REG_STATUS          0x0010U
#define AWG_PHASE_F_AWGX_CAP_DMA             (1UL << 0)
#define AWG_PHASE_F_AWGX_CAP_UDP             (1UL << 1)
#define AWG_PHASE_F_AWGX_CAP_DIRECT_BYPASS   (1UL << 3)
#define AWG_PHASE_F_AWGX_CAP_C1              (1UL << 4)
#define AWG_PHASE_F_AWGX_CONTROL_C1_ENABLE   (1UL << 0)
#define AWG_PHASE_F_AWGX_CONTROL_SOFT_RESET  (1UL << 1)

#define AWG_PHASE_F_AWGC_ID                  0x41574743U
#define AWG_PHASE_F_AWGC_VERSION             0x00010000U
#define AWG_PHASE_F_AWGC_CAPS                 0x00402010U
#define AWG_PHASE_F_AWGC_REG_ID               0x0100U
#define AWG_PHASE_F_AWGC_REG_VERSION          0x0104U
#define AWG_PHASE_F_AWGC_REG_CAPS             0x0108U
#define AWG_PHASE_F_AWGC_REG_STATUS           0x010CU
#define AWG_PHASE_F_AWGC_REG_ERROR            0x0110U
#define AWG_PHASE_F_AWGC_REG_RECORDS_LO       0x0118U
#define AWG_PHASE_F_AWGC_REG_RECORDS_HI       0x011CU
#define AWG_PHASE_F_AWGC_REG_EVENTS_LO        0x0120U
#define AWG_PHASE_F_AWGC_REG_EVENTS_HI        0x0124U
#define AWG_PHASE_F_AWGC_REG_BUSY_LO          0x0128U
#define AWG_PHASE_F_AWGC_REG_BUSY_HI          0x012CU
#define AWG_PHASE_F_AWGC_REG_STALL_LO         0x0130U
#define AWG_PHASE_F_AWGC_REG_STALL_HI         0x0134U
#define AWG_PHASE_F_AWGC_REG_OUTPUT_CRC_LO    0x0138U
#define AWG_PHASE_F_AWGC_REG_OUTPUT_CRC_HI    0x013CU
#define AWG_PHASE_F_AWGC_REG_DECLARED_LO      0x0140U
#define AWG_PHASE_F_AWGC_REG_DECLARED_HI      0x0144U
#define AWG_PHASE_F_AWGC_REG_MAX_DEPTH        0x0148U
#define AWG_PHASE_F_AWGC_REG_MAX_COMMANDS     0x014CU

/* UART/API telemetry schema; independent of the fixed 40-byte GWAS/2 ACK. */
#define AWG_PHASE_F_STREAMSTATUS2_VERSION      2U

struct awg_phase_f_streamstatus2 {
	uint32_t version;
	uint8_t selected_payload_kind;
	bool c1_preflight_complete;
	uint32_t awgx_id;
	uint32_t awgx_version;
	uint32_t awgx_caps;
	uint32_t awgx_control;
	uint32_t awgx_status;
	uint32_t awgc_id;
	uint32_t awgc_version;
	uint32_t awgc_caps;
	uint32_t awgc_status;
	uint32_t awgc_error;
	uint64_t accepted_commands;
	uint64_t emitted_logical_events;
	uint64_t busy_cycles;
	uint64_t stall_cycles;
	uint64_t logical_output_crc;
	uint64_t declared_logical_events;
	uint32_t maximum_repeat_depth;
	uint32_t maximum_commands;
};

struct awg_phase_f_config {
	struct awg_eth_mac_config mac;
	struct awg_eth_rx_config rx;
	struct awg_eth_tx_config tx;
	struct awg_net_config net;
	awg_sched_stream_cfg_t stream;
	struct axi_dmac *scheduler_dmac;
	uint32_t scheduler_base;
	uint32_t extension_base;
	uint32_t scheduler_dma_max_events;
	size_t scheduler_cache_line_size;
	awg_sched_dma_cache_flush_fn scheduler_cache_flush;
	void *scheduler_cache_ctx;
	uint32_t mac_status_poll_divider;
};

struct awg_phase_f_stats {
	uint32_t service_calls;
	uint32_t arp_replies;
	uint32_t udp_frames;
	uint32_t protocol_accepts;
	uint32_t protocol_rejects;
	uint32_t ack_frames;
	uint32_t tx_busy_drops;
	uint32_t service_errors;
};

struct awg_phase_f {
	struct awg_phase_f_config config;
	struct awg_eth_mac mac;
	struct awg_eth_mac_status mac_status;
	struct awg_eth_rx rx;
	struct awg_eth_tx tx;
	struct awg_net net;
	struct awg_sched_dma scheduler_dma;
	awg_stream_proto_v2_session_t protocol;
	awg_stream_proto_session_t legacy_protocol;
	struct awg_phase_f_stats stats;
	uint8_t tx_frame[AWG_PHASE_F_FRAME_BYTES];
	uint8_t ack_payload[AWG_STREAM_PROTO_V2_ACK_BYTES];
	uint32_t extension_caps;
	uint32_t compression_caps;
	awg_c1_preflight_result_t c1_preflight;
	awg_c1_preflight_report_t c1_preflight_report;
	uint8_t selected_payload_kind;
	uint32_t mac_poll_countdown;
	bool initialized;
	bool scheduler_dma_initialized;
};

int32_t awg_phase_f_init(struct awg_phase_f *phase,
			 const struct awg_phase_f_config *config);
int32_t awg_phase_f_service(struct awg_phase_f *phase);
int32_t awg_phase_f_read_streamstatus2(
	struct awg_phase_f *phase,
	struct awg_phase_f_streamstatus2 *status);
bool awg_phase_f_scheduler_release_allowed(const struct awg_phase_f *phase);
void awg_phase_f_scheduler_irq(void *instance);
void awg_phase_f_abort(struct awg_phase_f *phase);

#ifdef __cplusplus
}
#endif

#endif /* AWG_PHASE_F_H */
