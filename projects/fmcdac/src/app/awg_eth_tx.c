#include <errno.h>
#include <string.h>

#include "awg_eth_tx.h"

#define AWG_ETH_TX_DMA_ALIGNMENT 32U

static bool awg_eth_tx_is_power_of_two(size_t value)
{
	return value && ((value & (value - 1U)) == 0U);
}

static void awg_eth_tx_flush(struct awg_eth_tx *tx, uint8_t index,
			     size_t length)
{
	uintptr_t start;
	uintptr_t end;
	size_t line;

	if (!tx->config.flush || !length)
		return;

	start = (uintptr_t)tx->config.buffer[index];
	line = tx->config.cache_line_size;
	if (line) {
		end = start + length;
		start &= ~((uintptr_t)line - 1U);
		end = (end + line - 1U) & ~((uintptr_t)line - 1U);
		length = (size_t)(end - start);
	}
	tx->config.flush(tx->config.cache_ctx, start, length);
}

static int32_t awg_eth_tx_start_index(struct awg_eth_tx *tx, uint8_t index)
{
	struct axi_dma_transfer transfer;
	uint32_t flags;
	uint8_t bit;
	int32_t ret;

	if (tx->in_flight)
		return -EBUSY;
	bit = (uint8_t)(1U << index);
	if (!(tx->queued_mask & bit) || !tx->queued_length[index])
		return -EINVAL;

	ret = axi_dmac_read(tx->config.dmac, AXI_DMAC_REG_FLAGS, &flags);
	if (ret)
		return ret;
	flags &= ~(uint32_t)DMA_CYCLIC;
	flags |= (uint32_t)DMA_LAST;
	ret = axi_dmac_write(tx->config.dmac, AXI_DMAC_REG_FLAGS, flags);
	if (ret)
		return ret;

	awg_eth_tx_flush(tx, index, tx->queued_length[index]);
	memset(&transfer, 0, sizeof(transfer));
	transfer.size = (uint32_t)tx->queued_length[index];
	transfer.src_addr = tx->config.dma_address[index];
	transfer.cyclic = NO;

	/* Older no-OS AXI-DMAC revisions do not clear this flag at submit. */
	tx->config.dmac->transfer.transfer_done = false;
	tx->completion_pending = false;
	ret = axi_dmac_transfer_start(tx->config.dmac, &transfer);
	if (ret) {
		tx->stats.dma_errors++;
		return ret;
	}

	tx->queued_mask &= (uint8_t)~bit;
	tx->active_index = index;
	tx->in_flight = true;
	return 0;
}

static int32_t awg_eth_tx_start_queued(struct awg_eth_tx *tx)
{
	uint8_t index;
	uint8_t count;

	if (tx->in_flight)
		return -EBUSY;
	for (count = 0U; count < AWG_ETH_TX_BUFFER_COUNT; count++) {
		index = (uint8_t)((tx->active_index + count) %
				  AWG_ETH_TX_BUFFER_COUNT);
		if (tx->queued_mask & (uint8_t)(1U << index))
			return awg_eth_tx_start_index(tx, index);
	}
	return -EAGAIN;
}

static int32_t awg_eth_tx_select_free(struct awg_eth_tx *tx, uint8_t *index)
{
	uint8_t occupied;
	uint8_t candidate;
	uint8_t count;

	occupied = tx->queued_mask;
	if (tx->in_flight)
		occupied |= (uint8_t)(1U << tx->active_index);

	for (count = 0U; count < AWG_ETH_TX_BUFFER_COUNT; count++) {
		candidate = (uint8_t)((tx->next_submit_index + count) %
				      AWG_ETH_TX_BUFFER_COUNT);
		if (!(occupied & (uint8_t)(1U << candidate))) {
			*index = candidate;
			return 0;
		}
	}

	return -ENOSPC;
}

int32_t awg_eth_tx_init(struct awg_eth_tx *tx,
			const struct awg_eth_tx_config *config)
{
	uint8_t index;

	if (!tx || !config || !config->dmac || !config->buffer_size ||
	    config->buffer_size > UINT32_MAX || !config->min_frame_size ||
	    config->min_frame_size > config->buffer_size)
		return -EINVAL;
	if (config->dmac->direction != DMA_MEM_TO_DEV)
		return -EINVAL;
	if (config->cache_line_size &&
	    !awg_eth_tx_is_power_of_two(config->cache_line_size))
		return -EINVAL;

	for (index = 0U; index < AWG_ETH_TX_BUFFER_COUNT; index++) {
		if (!config->buffer[index])
			return -EINVAL;
		if (config->dma_address[index] % AWG_ETH_TX_DMA_ALIGNMENT)
			return -EINVAL;
		if (config->cache_line_size &&
		    ((uintptr_t)config->buffer[index] % config->cache_line_size))
			return -EINVAL;
	}

	memset(tx, 0, sizeof(*tx));
	tx->config = *config;
	tx->initialized = true;
	return 0;
}

int32_t awg_eth_tx_submit(struct awg_eth_tx *tx, const uint8_t *frame,
			  size_t length)
{
	size_t dma_length;
	uint8_t index;
	uint8_t bit;
	int32_t ret;

	if (!tx || !frame || !tx->initialized || !length)
		return -EINVAL;
	if (length > tx->config.buffer_size) {
		tx->stats.frames_rejected++;
		return -EMSGSIZE;
	}

	ret = awg_eth_tx_select_free(tx, &index);
	if (ret) {
		tx->stats.frames_rejected++;
		return ret;
	}

	dma_length = length;
	if (dma_length < tx->config.min_frame_size)
		dma_length = tx->config.min_frame_size;
	if (dma_length > tx->config.buffer_size) {
		tx->stats.frames_rejected++;
		return -EMSGSIZE;
	}

	memmove(tx->config.buffer[index], frame, length);
	if (dma_length > length) {
		memset(tx->config.buffer[index] + length, 0, dma_length - length);
		tx->stats.padded_frames++;
	}

	bit = (uint8_t)(1U << index);
	tx->queued_length[index] = dma_length;
	tx->queued_mask |= bit;
	tx->next_submit_index =
		(uint8_t)((index + 1U) % AWG_ETH_TX_BUFFER_COUNT);

	if (!tx->in_flight) {
		ret = awg_eth_tx_start_queued(tx);
		if (ret) {
			tx->queued_mask &= (uint8_t)~bit;
			tx->queued_length[index] = 0U;
			tx->stats.frames_rejected++;
			return ret;
		}
	}

	tx->stats.frames_submitted++;
	return 0;
}

void awg_eth_tx_irq(void *instance)
{
	struct awg_eth_tx *tx;

	tx = (struct awg_eth_tx *)instance;
	if (!tx || !tx->initialized || !tx->in_flight)
		return;

	axi_dmac_mem_to_dev_isr(tx->config.dmac);
	tx->stats.irq_count++;
	if (tx->config.dmac->transfer.transfer_done)
		tx->completion_pending = true;
}

int32_t awg_eth_tx_poll(struct awg_eth_tx *tx)
{
	uint32_t pending;
	int32_t ret;

	if (!tx || !tx->initialized)
		return -EINVAL;
	if (!tx->in_flight)
		return 0;

	ret = axi_dmac_read(tx->config.dmac, AXI_DMAC_REG_IRQ_PENDING, &pending);
	if (ret)
		return ret;
	if (!(pending & (AXI_DMAC_IRQ_SOT | AXI_DMAC_IRQ_EOT)))
		return 0;

	awg_eth_tx_irq(tx);
	return tx->completion_pending ? 1 : 0;
}

int32_t awg_eth_tx_service(struct awg_eth_tx *tx)
{
	uint8_t completed_index;
	int32_t ret;

	if (!tx || !tx->initialized)
		return -EINVAL;

	if (!tx->in_flight) {
		if (!tx->queued_mask)
			return 0;
		ret = awg_eth_tx_start_queued(tx);
		return ret == -EAGAIN ? 0 : ret;
	}
	if (!tx->completion_pending &&
	    !tx->config.dmac->transfer.transfer_done)
		return 0;

	completed_index = tx->active_index;
	tx->in_flight = false;
	tx->completion_pending = false;
	tx->config.dmac->transfer.transfer_done = false;
	tx->queued_length[completed_index] = 0U;
	tx->stats.frames_completed++;

	if (tx->queued_mask) {
		ret = awg_eth_tx_start_queued(tx);
		if (ret)
			return ret;
	}

	return 1;
}

uint32_t awg_eth_tx_available(const struct awg_eth_tx *tx)
{
	uint8_t occupied;
	uint32_t count;
	uint8_t index;

	if (!tx || !tx->initialized)
		return 0U;

	occupied = tx->queued_mask;
	if (tx->in_flight)
		occupied |= (uint8_t)(1U << tx->active_index);
	count = 0U;
	for (index = 0U; index < AWG_ETH_TX_BUFFER_COUNT; index++) {
		if (!(occupied & (uint8_t)(1U << index)))
			count++;
	}
	return count;
}

void awg_eth_tx_stop(struct awg_eth_tx *tx)
{
	if (!tx || !tx->initialized)
		return;

	axi_dmac_transfer_stop(tx->config.dmac);
	tx->in_flight = false;
	tx->completion_pending = false;
	tx->queued_mask = 0U;
	tx->queued_length[0] = 0U;
	tx->queued_length[1] = 0U;
}
