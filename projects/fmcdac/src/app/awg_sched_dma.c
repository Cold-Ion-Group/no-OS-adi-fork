#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "awg_sched.h"
#include "awg_sched_dma.h"
#include "awg_sched_regs.h"
#include "no_os_axi_io.h"

static uint32_t awg_sched_dma_min(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static bool awg_sched_dma_power_of_two(size_t value)
{
	return value != 0U && (value & (value - 1U)) == 0U;
}

static uint32_t awg_sched_dma_driver_max_events(const struct axi_dmac *dmac)
{
	uint64_t bytes;

	bytes = (uint64_t)dmac->max_length + 1ULL;
	return (uint32_t)(bytes / AWG_SCHED_DMA_EVENT_BYTES);
}

static void awg_sched_dma_flush(struct awg_sched_dma *refill,
				const void *address, size_t length)
{
	uintptr_t start;
	uintptr_t end;
	size_t line;

	if (!refill->config.cache_flush || length == 0U)
		return;

	line = refill->config.cache_line_size;
	start = (uintptr_t)address & ~((uintptr_t)line - 1U);
	end = ((uintptr_t)address + length + line - 1U) &
	      ~((uintptr_t)line - 1U);
	refill->config.cache_flush(refill->config.cache_ctx, start, end - start);
}

int32_t awg_sched_dma_init(struct awg_sched_dma *refill,
			   const struct awg_sched_dma_config *config)
{
	if (!refill || !config || !config->dmac || !config->ring ||
	    config->scheduler_base == 0U || config->max_events == 0U ||
	    !awg_sched_dma_power_of_two(config->cache_line_size))
		return -EINVAL;
	if (config->dmac->direction != DMA_MEM_TO_DEV)
		return -ENODEV;
	if (awg_sched_dma_driver_max_events(config->dmac) == 0U)
		return -EMSGSIZE;

	memset(refill, 0, sizeof(*refill));
	refill->config = *config;
	refill->initialized = true;
	refill->service_pending = true;
	return 0;
}

void awg_sched_dma_irq(void *instance)
{
	struct awg_sched_dma *refill = instance;

	if (!refill || !refill->initialized)
		return;

	axi_dmac_mem_to_dev_isr(refill->config.dmac);
	if (refill->config.dmac->transfer.transfer_done)
		refill->completion_pending = true;
}

void awg_sched_dma_request_service(struct awg_sched_dma *refill)
{
	if (!refill || !refill->initialized)
		return;
	refill->stats.low_watermark_requests++;
	refill->service_pending = true;
}

static int32_t awg_sched_dma_complete(struct awg_sched_dma *refill)
{
	int ret;

	if (!refill->in_flight || !refill->config.dmac->transfer.transfer_done)
		return 0;

	ret = awg_stream_ring_consume(refill->config.ring,
				      refill->pending_events);
	if (ret) {
		refill->stats.dma_errors++;
		return ret;
	}

	refill->stats.transfers_completed++;
	refill->stats.events_completed += refill->pending_events;
	refill->pending_events = 0U;
	refill->in_flight = false;
	refill->completion_pending = false;
	refill->config.dmac->transfer.transfer_done = false;

	if (!refill->scheduler_started) {
		ret = awg_sched_start();
		if (ret) {
			refill->stats.scheduler_errors++;
			return ret;
		}
		refill->scheduler_started = true;
	}

	refill->service_pending = true;
	return 0;
}

int32_t awg_sched_dma_service(struct awg_sched_dma *refill)
{
	awg_sched_status_t status;
	struct axi_dma_transfer transfer;
	const void *source;
	uint32_t irq_pending;
	uint32_t free_space;
	uint32_t available;
	uint32_t contiguous;
	uint32_t driver_max;
	uint32_t count;
	int ret;

	if (!refill || !refill->initialized)
		return -EINVAL;
	refill->stats.service_calls++;

	/* Polling is a recovery path; the normal completion path is the IRQ. */
	if (refill->in_flight && !refill->completion_pending) {
		ret = axi_dmac_read(refill->config.dmac, AXI_DMAC_REG_IRQ_PENDING,
				    &irq_pending);
		if (ret)
			return ret;
		if ((irq_pending & AXI_DMAC_IRQ_EOT) != 0U)
			awg_sched_dma_irq(refill);
	}

	ret = awg_sched_dma_complete(refill);
	if (ret || refill->in_flight)
		return ret;

	ret = awg_sched_get_status(&status);
	if (ret)
		return ret;
	if (status.error) {
		refill->stats.scheduler_errors++;
		return -EIO;
	}

	available = awg_stream_ring_count(refill->config.ring);
	if (available == 0U) {
		refill->service_pending = false;
		return 0;
	}

	ret = no_os_axi_io_read(refill->config.scheduler_base,
				 AWG_SCHED_REG_FREE_SPACE, &free_space);
	if (ret)
		return ret;
	if (free_space == 0U) {
		refill->service_pending = false;
		return 0;
	}

	contiguous = awg_stream_ring_consumer_contiguous(refill->config.ring);
	driver_max = awg_sched_dma_driver_max_events(refill->config.dmac);
	count = awg_sched_dma_min(available, free_space);
	count = awg_sched_dma_min(count, contiguous);
	count = awg_sched_dma_min(count, refill->config.max_events);
	count = awg_sched_dma_min(count, driver_max);
	if (count == 0U)
		return -EMSGSIZE;

	source = awg_stream_ring_consumer_const_ptr(refill->config.ring);
	if (!source)
		return -EFAULT;
	awg_sched_dma_flush(refill, source,
			    (size_t)count * AWG_SCHED_DMA_EVENT_BYTES);

	memset(&transfer, 0, sizeof(transfer));
	transfer.size = count * AWG_SCHED_DMA_EVENT_BYTES;
	transfer.cyclic = NO;
	transfer.src_addr = (uint32_t)(uintptr_t)source;
	ret = axi_dmac_transfer_start(refill->config.dmac, &transfer);
	if (ret) {
		refill->stats.dma_errors++;
		return ret;
	}

	refill->transfer = transfer;
	refill->pending_events = count;
	refill->in_flight = true;
	refill->service_pending = false;
	refill->stats.transfers_started++;
	refill->stats.events_submitted += count;
	return 0;
}

int32_t awg_sched_dma_abort(struct awg_sched_dma *refill)
{
	int ret;

	if (!refill || !refill->initialized)
		return -EINVAL;

	axi_dmac_transfer_stop(refill->config.dmac);
	refill->in_flight = false;
	refill->pending_events = 0U;
	refill->completion_pending = false;
	refill->service_pending = false;
	refill->scheduler_started = false;
	ret = awg_sched_stream_reset_soft();
	return ret;
}

bool awg_sched_dma_in_flight(const struct awg_sched_dma *refill)
{
	return refill && refill->initialized && refill->in_flight;
}
