#ifndef AWG_ETH_RX_H
#define AWG_ETH_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "axi_dmac.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_ETH_RX_BUFFER_COUNT 2U

/* Byte offsets from the ADI DMAC register map. */
#define AWG_ETH_RX_DMAC_REG_FLAGS                   0x040CU
#define AWG_ETH_RX_DMAC_REG_TRANSFER_DONE           0x0428U
#define AWG_ETH_RX_DMAC_REG_PARTIAL_TRANSFER_LENGTH 0x044CU
#define AWG_ETH_RX_DMAC_REG_PARTIAL_TRANSFER_ID     0x0450U
#define AWG_ETH_RX_DMAC_PARTIAL_TRANSFER_DONE       (1UL << 31)

#ifndef AWG_ETH_CACHE_OP_T_DEFINED
#define AWG_ETH_CACHE_OP_T_DEFINED
typedef void (*awg_eth_cache_op_t)(void *ctx, uintptr_t address, size_t length);
#endif

struct awg_eth_rx_config {
	struct axi_dmac *dmac;
	uint8_t *buffer[AWG_ETH_RX_BUFFER_COUNT];
	uint32_t dma_address[AWG_ETH_RX_BUFFER_COUNT];
	size_t buffer_size;
	size_t cache_line_size;
	awg_eth_cache_op_t invalidate;
	void *cache_ctx;
};

struct awg_eth_rx_frame {
	uint8_t *data;
	size_t length;
	uint8_t buffer_index;
};

struct awg_eth_rx_stats {
	uint32_t transfers_started;
	uint32_t frames_completed;
	uint32_t frames_dropped;
	uint32_t dma_errors;
	uint32_t irq_count;
	uint32_t no_buffer_count;
	uint32_t invalid_length_count;
};

struct awg_eth_rx {
	struct awg_eth_rx_config config;
	struct awg_eth_rx_stats stats;
	size_t ready_length[AWG_ETH_RX_BUFFER_COUNT];
	uint8_t active_index;
	uint8_t next_arm_index;
	uint8_t next_consume_index;
	uint8_t ready_mask;
	uint8_t leased_mask;
	bool initialized;
	bool in_flight;
	volatile bool completion_pending;
};

int32_t awg_eth_rx_init(struct awg_eth_rx *rx,
			const struct awg_eth_rx_config *config);
int32_t awg_eth_rx_start(struct awg_eth_rx *rx);
void awg_eth_rx_irq(void *instance);
int32_t awg_eth_rx_poll(struct awg_eth_rx *rx);
int32_t awg_eth_rx_service(struct awg_eth_rx *rx);
int32_t awg_eth_rx_acquire(struct awg_eth_rx *rx,
			   struct awg_eth_rx_frame *frame);
int32_t awg_eth_rx_release(struct awg_eth_rx *rx, uint8_t buffer_index);
void awg_eth_rx_stop(struct awg_eth_rx *rx);

#ifdef __cplusplus
}
#endif

#endif /* AWG_ETH_RX_H */
