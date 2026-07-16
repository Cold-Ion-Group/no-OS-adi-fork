#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "awg_phase_f.h"
#include "awg_sched.h"
#include "awg_sched_regs.h"
#include "no_os_axi_io.h"

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
	awg_stream_proto_ack_t ack;
	size_t length;
	int32_t proto_ret;
	int32_t ret;

	proto_ret = awg_stream_proto_handle_frame_ctx(&phase->protocol,
			packet->payload, packet->payload_length,
			&phase->config.stream, &ack);
	if (proto_ret)
		phase->stats.protocol_rejects++;
	else
		phase->stats.protocol_accepts++;

	awg_stream_proto_ack_to_le(&ack, phase->ack_payload);
	ret = awg_net_build_udp(&phase->net, packet->source_mac,
				packet->source_ipv4, phase->config.net.udp_port,
				packet->source_port, phase->ack_payload,
				sizeof(phase->ack_payload), phase->tx_frame,
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

	memset(phase, 0, sizeof(*phase));
	phase->config = *config;
	awg_stream_proto_session_init(&phase->protocol);

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

	phase->mac_poll_countdown = config->mac_status_poll_divider;
	phase->initialized = true;
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
