#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "awg_phase_f.h"
#include "awg_sched.h"
#include "awg_sched_regs.h"
#include "no_os_axi_io.h"

static int32_t awg_phase_f_extension_init(struct awg_phase_f *phase)
{
	uint32_t id;
	uint32_t version;
	uint32_t required;
	int32_t ret;

	if (!phase->config.extension_base)
		return -ENODEV;
	ret = no_os_axi_io_read(phase->config.extension_base,
		AWG_PHASE_F_AWGX_REG_ID, &id);
	if (ret)
		return ret;
	ret = no_os_axi_io_read(phase->config.extension_base,
		AWG_PHASE_F_AWGX_REG_VERSION, &version);
	if (ret)
		return ret;
	ret = no_os_axi_io_read(phase->config.extension_base,
		AWG_PHASE_F_AWGX_REG_CAPS, &phase->extension_caps);
	if (ret)
		return ret;

	required = AWG_PHASE_F_AWGX_CAP_DMA | AWG_PHASE_F_AWGX_CAP_UDP |
		AWG_PHASE_F_AWGX_CAP_DIRECT_BYPASS;
	if (id != AWG_PHASE_F_AWGX_ID)
		return -ENODEV;
	if (version != AWG_PHASE_F_AWGX_VERSION)
		return -ENOTSUP;
	if ((phase->extension_caps & required) != required)
		return -ENOTSUP;
	return 0;
}

static int awg_phase_f_protocol_prepare(void *ctx)
{
	struct awg_phase_f *phase = ctx;
	int32_t ret;

	if (!phase || !phase->initialized)
		return -EINVAL;

	if (phase->scheduler_dma_initialized) {
		ret = awg_sched_dma_abort(&phase->scheduler_dma);
		if (ret)
			return ret;
		memset(&phase->scheduler_dma, 0, sizeof(phase->scheduler_dma));
		phase->scheduler_dma_initialized = false;
	} else if (phase->protocol.active) {
		ret = awg_sched_stream_reset_soft();
		if (ret)
			return ret;
	}

	/* Every session starts with a clean decoder and direct bypass selected. */
	ret = no_os_axi_io_write(phase->config.extension_base,
		AWG_PHASE_F_AWGX_REG_CONTROL,
		AWG_PHASE_F_AWGX_CONTROL_SOFT_RESET);
	if (ret)
		return ret;
	ret = no_os_axi_io_write(phase->config.extension_base,
		AWG_PHASE_F_AWGX_REG_CONTROL, 0U);
	if (!ret)
		phase->selected_payload_kind = AWG_STREAM_PROTO_V2_KIND_CONTROL;
	return ret;
}

static int awg_phase_f_protocol_select_kind(void *ctx, uint8_t kind)
{
	struct awg_phase_f *phase = ctx;
	awg_stream_ring_t *ring;
	uint32_t control;

	if (!phase || !phase->initialized ||
	    !awg_sched_stream_dma_mode_enabled())
		return -EINVAL;
	if (phase->scheduler_dma_initialized &&
	    awg_sched_dma_in_flight(&phase->scheduler_dma))
		return -EBUSY;
	ring = awg_sched_stream_ring_get();
	if (!ring || awg_stream_ring_count(ring) != 0U)
		return -EBUSY;

	if (kind == AWG_STREAM_PROTO_V2_KIND_EVENTS) {
		if ((phase->extension_caps &
		     AWG_PHASE_F_AWGX_CAP_DIRECT_BYPASS) == 0U)
			return -ENOTSUP;
		control = 0U;
	} else if (kind == AWG_STREAM_PROTO_V2_KIND_C1) {
		if ((phase->extension_caps & AWG_PHASE_F_AWGX_CAP_C1) == 0U)
			return -ENOTSUP;
		control = AWG_PHASE_F_AWGX_CONTROL_C1_ENABLE;
	} else {
		return -EINVAL;
	}

	if (no_os_axi_io_write(phase->config.extension_base,
				AWG_PHASE_F_AWGX_REG_CONTROL, control))
		return -EIO;
	phase->selected_payload_kind = kind;
	return 0;
}

static int32_t awg_phase_f_tx(struct awg_phase_f *phase, size_t length)
{
	int32_t ret;

	ret = awg_eth_tx_submit(&phase->tx, phase->tx_frame, length);
	if (ret == -EAGAIN || ret == -ENOSPC)
		phase->stats.tx_busy_drops++;
	return ret;
}

static int32_t awg_phase_f_reply_arp(struct awg_phase_f *phase,
				     const struct awg_net_packet *packet)
{
	size_t length;
	int32_t ret;

	ret = awg_net_build_arp_reply(&phase->net, packet, phase->tx_frame,
				      sizeof(phase->tx_frame), &length);
	if (ret)
		return ret;
	ret = awg_phase_f_tx(phase, length);
	if (!ret)
		phase->stats.arp_replies++;
	return ret;
}

static int32_t awg_phase_f_reply_udp(struct awg_phase_f *phase,
				     const struct awg_net_packet *packet)
{
	const awg_stream_proto_v2_ops_t ops = {
		.prepare = awg_phase_f_protocol_prepare,
		.select_kind = awg_phase_f_protocol_select_kind,
		.ctx = phase,
	};
	awg_stream_proto_v2_ack_t ack;
	awg_stream_proto_ack_t legacy_ack;
	size_t length;
	size_t ack_length;
	bool is_v2;
	int32_t proto_ret;
	int32_t ret;

	/* Keep the completed GWAS/1 sender usable as a diagnostic fallback. */
	is_v2 = packet->payload_length >= AWG_STREAM_PROTO_V2_HEADER_BYTES +
		AWG_STREAM_PROTO_CRC_BYTES &&
		((packet->payload[18U] == AWG_STREAM_PROTO_V2_HEADER_BYTES &&
		  packet->payload[19U] == 0U) ||
		 (phase->protocol.active &&
		  packet->payload[8U] == (uint8_t)phase->protocol.session_id &&
		  packet->payload[9U] == (uint8_t)(phase->protocol.session_id >> 8) &&
		  packet->payload[10U] == (uint8_t)(phase->protocol.session_id >> 16) &&
		  packet->payload[11U] == (uint8_t)(phase->protocol.session_id >> 24)));
	if (is_v2) {
		proto_ret = awg_stream_proto_v2_handle_frame(&phase->protocol,
				packet->payload, packet->payload_length,
				&phase->config.stream, &ops, &ack);
		awg_stream_proto_v2_ack_to_le(&ack, phase->ack_payload);
		ack_length = AWG_STREAM_PROTO_V2_ACK_BYTES;
	} else {
		proto_ret = awg_stream_proto_handle_frame_ctx(&phase->legacy_protocol,
				packet->payload, packet->payload_length,
				&phase->config.stream, &legacy_ack);
		awg_stream_proto_ack_to_le(&legacy_ack, phase->ack_payload);
		ack_length = AWG_STREAM_PROTO_ACK_BYTES;
	}
	if (proto_ret)
		phase->stats.protocol_rejects++;
	else
		phase->stats.protocol_accepts++;

	ret = awg_net_build_udp(&phase->net, packet->source_mac,
				packet->source_ipv4, phase->config.net.udp_port,
				packet->source_port, phase->ack_payload,
				ack_length, phase->tx_frame,
				sizeof(phase->tx_frame), &length);
	if (ret)
		return ret;
	ret = awg_phase_f_tx(phase, length);
	if (!ret)
		phase->stats.ack_frames++;
	return ret;
}

static int32_t awg_phase_f_process_rx(struct awg_phase_f *phase)
{
	struct awg_eth_rx_frame frame;
	struct awg_net_packet packet;
	enum awg_net_parse_result parsed;
	int32_t first_error = 0;
	int32_t ret;

	for (;;) {
		ret = awg_eth_rx_acquire(&phase->rx, &frame);
		if (ret == -EAGAIN)
			break;
		if (ret)
			return ret;

		memset(&packet, 0, sizeof(packet));
		parsed = awg_net_parse_frame(&phase->net, frame.data, frame.length,
					     &packet);
		if (parsed == AWG_NET_PARSE_ARP_REQUEST)
			ret = awg_phase_f_reply_arp(phase, &packet);
		else if (parsed == AWG_NET_PARSE_UDP) {
			phase->stats.udp_frames++;
			ret = awg_phase_f_reply_udp(phase, &packet);
		} else if (parsed < 0) {
			/* Parser statistics retain the reason; malformed wire data is a drop. */
			ret = 0;
		} else {
			ret = 0;
		}
		if (ret && ret != -EAGAIN && !first_error)
			first_error = ret;

		ret = awg_eth_rx_release(&phase->rx, frame.buffer_index);
		if (ret && !first_error)
			first_error = ret;
	}

	return first_error;
}

static int32_t awg_phase_f_init_scheduler_dma(struct awg_phase_f *phase)
{
	struct awg_sched_dma_config config;
	awg_stream_ring_t *ring;
	int32_t ret;

	if (phase->scheduler_dma_initialized)
		return 0;
	if (!awg_sched_stream_dma_mode_enabled())
		return 0;

	ring = awg_sched_stream_ring_get();
	if (!ring)
		return 0;

	memset(&config, 0, sizeof(config));
	config.dmac = phase->config.scheduler_dmac;
	config.ring = ring;
	config.scheduler_base = phase->config.scheduler_base;
	config.max_events = phase->config.scheduler_dma_max_events;
	config.cache_line_size = phase->config.scheduler_cache_line_size;
	config.cache_flush = phase->config.scheduler_cache_flush;
	config.cache_ctx = phase->config.scheduler_cache_ctx;
	ret = awg_sched_dma_init(&phase->scheduler_dma, &config);
	if (ret)
		return ret;

	phase->scheduler_dma_initialized = true;
	return 0;
}

int32_t awg_phase_f_init(struct awg_phase_f *phase,
			 const struct awg_phase_f_config *config)
{
	int32_t ret;

	if (!phase || !config)
		return -EINVAL;
	if (config->rx.buffer_size < AWG_PHASE_F_MAX_RX_FRAME_BYTES ||
	    config->mac.rx_max_frame_size < AWG_PHASE_F_MAX_RX_FRAME_BYTES ||
	    config->tx.buffer_size < AWG_NET_ETH_HEADER_LEN +
		AWG_NET_IPV4_MIN_HEADER_LEN + AWG_NET_UDP_HEADER_LEN +
		AWG_STREAM_PROTO_V2_ACK_BYTES)
		return -EMSGSIZE;

	memset(phase, 0, sizeof(*phase));
	phase->config = *config;
	awg_stream_proto_v2_session_init(&phase->protocol);
	awg_stream_proto_session_init(&phase->legacy_protocol);

	ret = awg_phase_f_extension_init(phase);
	if (ret)
		return ret;

	ret = awg_eth_mac_init(&phase->mac, &config->mac);
	if (ret)
		return ret;
	ret = awg_eth_mac_bring_up(&phase->mac);
	if (ret)
		return ret;
	ret = awg_eth_rx_init(&phase->rx, &config->rx);
	if (ret)
		return ret;
	ret = awg_eth_tx_init(&phase->tx, &config->tx);
	if (ret)
		return ret;
	ret = awg_net_init(&phase->net, &config->net);
	if (ret)
		return ret;
	ret = awg_eth_rx_start(&phase->rx);
	if (ret)
		return ret;

	phase->initialized = true;
	phase->mac_poll_countdown = config->mac_status_poll_divider;
	return 0;
}

int32_t awg_phase_f_service(struct awg_phase_f *phase)
{
	int32_t first_error = 0;
	int32_t ret;

	if (!phase || !phase->initialized)
		return -EINVAL;
	phase->stats.service_calls++;

	ret = awg_eth_tx_poll(&phase->tx);
	if (ret < 0 && !first_error)
		first_error = ret;
	ret = awg_eth_tx_service(&phase->tx);
	if (ret < 0 && !first_error)
		first_error = ret;
	ret = awg_eth_rx_poll(&phase->rx);
	if (ret < 0 && !first_error)
		first_error = ret;
	ret = awg_eth_rx_service(&phase->rx);
	if (ret < 0 && !first_error)
		first_error = ret;
	ret = awg_phase_f_process_rx(phase);
	if (ret && !first_error)
		first_error = ret;

	ret = awg_phase_f_init_scheduler_dma(phase);
	if (ret && !first_error)
		first_error = ret;
	if (phase->scheduler_dma_initialized) {
		ret = awg_sched_dma_service(&phase->scheduler_dma);
		if (ret && ret != -EAGAIN && !first_error)
			first_error = ret;
	}

	if (phase->config.mac_status_poll_divider != 0U) {
		if (phase->mac_poll_countdown == 0U) {
			ret = awg_eth_mac_read_status(&phase->mac, &phase->mac_status);
			if (ret && !first_error)
				first_error = ret;
			phase->mac_poll_countdown =
				phase->config.mac_status_poll_divider;
		} else {
			phase->mac_poll_countdown--;
		}
	}

	if (first_error)
		phase->stats.service_errors++;
	return first_error;
}

void awg_phase_f_scheduler_irq(void *instance)
{
	struct awg_phase_f *phase = instance;
	uint32_t irq_status;

	if (!phase || !phase->initialized)
		return;
	if (no_os_axi_io_read(phase->config.scheduler_base,
			      AWG_SCHED_REG_IRQ_STATUS, &irq_status))
		return;

	awg_sched_stream_irq_handler(irq_status);
	if (phase->scheduler_dma_initialized &&
	    (irq_status & (AWG_SCHED_IRQ_LOW_WATERMARK |
			   AWG_SCHED_IRQ_EMPTY_STALL)) != 0U)
		awg_sched_dma_request_service(&phase->scheduler_dma);
}

void awg_phase_f_abort(struct awg_phase_f *phase)
{
	if (!phase || !phase->initialized)
		return;
	if (phase->scheduler_dma_initialized)
		(void)awg_sched_dma_abort(&phase->scheduler_dma);
	awg_eth_rx_stop(&phase->rx);
	awg_eth_tx_stop(&phase->tx);
	phase->initialized = false;
}
