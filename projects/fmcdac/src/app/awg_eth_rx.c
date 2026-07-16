#include <errno.h>
#include <string.h>

#include "awg_eth_rx.h"

#define AWG_ETH_RX_DMA_ALIGNMENT 32U

static bool awg_eth_rx_is_power_of_two(size_t value)
{
	return value && ((value & (value - 1U)) == 0U);
}

static void awg_eth_rx_invalidate(struct awg_eth_rx *rx, uint8_t index,
				  size_t length)
{
	uintptr_t start;
	uintptr_t end;
	size_t line;

	if (!rx->config.invalidate || !length)
		return;

	start = (uintptr_t)rx->config.buffer[index];
	line = rx->config.cache_line_size;
	if (line) {
		end = start + length;
		start &= ~((uintptr_t)line - 1U);
		end = (end + line - 1U) & ~((uintptr_t)line - 1U);
		length = (size_t)(end - start);
	}
	rx->config.invalidate(rx->config.cache_ctx, start, length);
}

static int32_t awg_eth_rx_select_free(struct awg_eth_rx *rx, uint8_t *index)
{
	uint8_t occupied;
	uint8_t candidate;
	uint8_t count;

	occupied = rx->ready_mask | rx->leased_mask;
	for (count = 0U; count < AWG_ETH_RX_BUFFER_COUNT; count++) {
		candidate = (uint8_t)((rx->next_arm_index + count) %
				      AWG_ETH_RX_BUFFER_COUNT);
		if (!(occupied & (uint8_t)(1U << candidate))) {
			*index = candidate;
			return 0;
		}
	}

	return -ENOSPC;
}

static int32_t awg_eth_rx_arm_next(struct awg_eth_rx *rx)
{
	struct axi_dma_transfer transfer;
	uint32_t flags;
	uint8_t index;
	int32_t ret;

	if (rx->in_flight)
		return -EBUSY;

	ret = awg_eth_rx_select_free(rx, &index);
	if (ret) {
		rx->stats.no_buffer_count++;
		return ret;
	}

	/* Invalidate before DMA as well as after completion so no dirty cache
	 * line can later overwrite bytes written by the device. */
	awg_eth_rx_invalidate(rx, index, rx->config.buffer_size);

	ret = axi_dmac_read(rx->config.dmac, AWG_ETH_RX_DMAC_REG_FLAGS, &flags);
	if (ret)
		return ret;
	flags &= ~(uint32_t)DMA_CYCLIC;
	flags |= (uint32_t)DMA_LAST;
	ret = axi_dmac_write(rx->config.dmac, AWG_ETH_RX_DMAC_REG_FLAGS, flags);
	if (ret)
		return ret;
	ret = axi_dmac_set_partial_reporting(rx->config.dmac, true);
	if (ret)
		return ret;

	memset(&transfer, 0, sizeof(transfer));
	transfer.size = (uint32_t)rx->config.buffer_size;
	transfer.dest_addr = rx->config.dma_address[index];
	transfer.cyclic = NO;

	/* Older no-OS AXI-DMAC revisions do not clear this flag at submit. */
	rx->config.dmac->transfer.transfer_done = false;
	rx->completion_pending = false;
	ret = axi_dmac_transfer_start(rx->config.dmac, &transfer);
	if (ret) {
		rx->stats.dma_errors++;
		return ret;
	}

	rx->active_index = index;
	rx->next_arm_index = (uint8_t)((index + 1U) % AWG_ETH_RX_BUFFER_COUNT);
	rx->in_flight = true;
	rx->stats.transfers_started++;
	return 0;
}

int32_t awg_eth_rx_init(struct awg_eth_rx *rx,
			const struct awg_eth_rx_config *config)
{
	uint8_t index;

	if (!rx || !config || !config->dmac || !config->buffer_size ||
	    config->buffer_size > UINT32_MAX)
		return -EINVAL;
	if (config->dmac->direction != DMA_DEV_TO_MEM)
		return -EINVAL;
	if (config->cache_line_size &&
	    !awg_eth_rx_is_power_of_two(config->cache_line_size))
		return -EINVAL;

	for (index = 0U; index < AWG_ETH_RX_BUFFER_COUNT; index++) {
		if (!config->buffer[index])
			return -EINVAL;
		if (config->dma_address[index] % AWG_ETH_RX_DMA_ALIGNMENT)
			return -EINVAL;
		if (config->cache_line_size &&
		    ((uintptr_t)config->buffer[index] % config->cache_line_size))
			return -EINVAL;
	}

	memset(rx, 0, sizeof(*rx));
	rx->config = *config;
	rx->initialized = true;
	return 0;
}

int32_t awg_eth_rx_start(struct awg_eth_rx *rx)
{
	if (!rx || !rx->initialized)
		return -EINVAL;
	return awg_eth_rx_arm_next(rx);
}

void awg_eth_rx_irq(void *instance)
{
	struct awg_eth_rx *rx;

	rx = (struct awg_eth_rx *)instance;
	if (!rx || !rx->initialized || !rx->in_flight)
		return;

	/* The generic ISR acknowledges SOT/EOT and handles large transfers. */
	axi_dmac_dev_to_mem_isr(rx->config.dmac);
	rx->stats.irq_count++;
	if (rx->config.dmac->transfer.transfer_done)
		rx->completion_pending = true;
}

int32_t awg_eth_rx_poll(struct awg_eth_rx *rx)
{
	uint32_t pending;
	int32_t ret;

	if (!rx || !rx->initialized)
		return -EINVAL;
	if (!rx->in_flight)
		return 0;

	ret = axi_dmac_read(rx->config.dmac, AXI_DMAC_REG_IRQ_PENDING, &pending);
	if (ret)
		return ret;
	if (!(pending & (AXI_DMAC_IRQ_SOT | AXI_DMAC_IRQ_EOT)))
		return 0;

	awg_eth_rx_irq(rx);
	return rx->completion_pending ? 1 : 0;
}

int32_t awg_eth_rx_service(struct awg_eth_rx *rx)
{
	uint32_t partial_length;
	uint32_t partial_id;
	size_t frame_length;
	uint8_t completed_index;
	int32_t ret;

	if (!rx || !rx->initialized)
		return -EINVAL;
	if (!rx->in_flight)
		return 0;
	if (!rx->completion_pending &&
	    !rx->config.dmac->transfer.transfer_done)
		return 0;

	completed_index = rx->active_index;
	ret = axi_dmac_get_partial_transfer(rx->config.dmac, &partial_length,
					    &partial_id);
	if (ret == -EAGAIN) {
		/* Ethernet TLAST is mandatory; never guess a frame length. */
		frame_length = 0U;
	} else if (ret) {
		rx->stats.dma_errors++;
		return ret;
	} else {
		(void)partial_id;
		frame_length = (size_t)partial_length;
	}

	rx->in_flight = false;
	rx->completion_pending = false;
	rx->config.dmac->transfer.transfer_done = false;

	if (!frame_length || frame_length > rx->config.buffer_size) {
		rx->stats.invalid_length_count++;
		rx->stats.frames_dropped++;
	} else {
		awg_eth_rx_invalidate(rx, completed_index, frame_length);
		rx->ready_length[completed_index] = frame_length;
		rx->ready_mask |= (uint8_t)(1U << completed_index);
		rx->stats.frames_completed++;
	}

	ret = awg_eth_rx_arm_next(rx);
	if (ret && ret != -ENOSPC)
		return ret;

	return 1;
}

int32_t awg_eth_rx_acquire(struct awg_eth_rx *rx,
			   struct awg_eth_rx_frame *frame)
{
	uint8_t index;
	uint8_t count;

	if (!rx || !frame || !rx->initialized)
		return -EINVAL;

	for (count = 0U; count < AWG_ETH_RX_BUFFER_COUNT; count++) {
		index = (uint8_t)((rx->next_consume_index + count) %
				  AWG_ETH_RX_BUFFER_COUNT);
		if (rx->ready_mask & (uint8_t)(1U << index)) {
			rx->ready_mask &= (uint8_t)~(1U << index);
			rx->leased_mask |= (uint8_t)(1U << index);
			rx->next_consume_index =
				(uint8_t)((index + 1U) % AWG_ETH_RX_BUFFER_COUNT);
			frame->data = rx->config.buffer[index];
			frame->length = rx->ready_length[index];
			frame->buffer_index = index;
			return 0;
		}
	}

	return -EAGAIN;
}

int32_t awg_eth_rx_release(struct awg_eth_rx *rx, uint8_t buffer_index)
{
	uint8_t bit;
	int32_t ret;

	if (!rx || !rx->initialized || buffer_index >= AWG_ETH_RX_BUFFER_COUNT)
		return -EINVAL;

	bit = (uint8_t)(1U << buffer_index);
	if (!(rx->leased_mask & bit))
		return -EINVAL;

	rx->leased_mask &= (uint8_t)~bit;
	rx->ready_length[buffer_index] = 0U;
	if (rx->in_flight)
		return 0;

	ret = awg_eth_rx_arm_next(rx);
	if (ret == -ENOSPC)
		return 0;
	return ret;
}

void awg_eth_rx_stop(struct awg_eth_rx *rx)
{
	if (!rx || !rx->initialized)
		return;

	axi_dmac_transfer_stop(rx->config.dmac);
	rx->in_flight = false;
	rx->completion_pending = false;
	rx->ready_mask = 0U;
	rx->leased_mask = 0U;
	rx->ready_length[0] = 0U;
	rx->ready_length[1] = 0U;
}
