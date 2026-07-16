#ifndef AWG_SCHED_DMA_H
#define AWG_SCHED_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "awg_stream_ring.h"
#include "axi_dmac.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_SCHED_DMA_EVENT_BYTES 32U

typedef void (*awg_sched_dma_cache_flush_fn)(void *ctx, uintptr_t address,
					      size_t length);

struct awg_sched_dma_config {
	struct axi_dmac *dmac;
	awg_stream_ring_t *ring;
	uint32_t scheduler_base;
	uint32_t max_events;
	size_t cache_line_size;
	awg_sched_dma_cache_flush_fn cache_flush;
	void *cache_ctx;
};

struct awg_sched_dma_stats {
	uint32_t transfers_started;
	uint32_t transfers_completed;
	uint32_t events_submitted;
	uint32_t events_completed;
	uint32_t service_calls;
	uint32_t low_watermark_requests;
	uint32_t dma_errors;
	uint32_t scheduler_errors;
};

struct awg_sched_dma {
	struct awg_sched_dma_config config;
	struct awg_sched_dma_stats stats;
	struct axi_dma_transfer transfer;
	uint32_t pending_events;
	bool initialized;
	bool in_flight;
	bool scheduler_started;
	volatile bool completion_pending;
	volatile bool service_pending;
};

int32_t awg_sched_dma_init(struct awg_sched_dma *refill,
			   const struct awg_sched_dma_config *config);
void awg_sched_dma_irq(void *instance);
void awg_sched_dma_request_service(struct awg_sched_dma *refill);
int32_t awg_sched_dma_service(struct awg_sched_dma *refill);
int32_t awg_sched_dma_abort(struct awg_sched_dma *refill);
bool awg_sched_dma_in_flight(const struct awg_sched_dma *refill);

#ifdef __cplusplus
}
#endif

#endif /* AWG_SCHED_DMA_H */
