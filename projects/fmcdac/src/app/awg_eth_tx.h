#ifndef AWG_ETH_TX_H
#define AWG_ETH_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "axi_dmac.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_ETH_TX_BUFFER_COUNT 2U

/* Ethernet frame length at the MAC client interface when FCS insertion is on. */
#define AWG_ETH_TX_MIN_CLIENT_FRAME_SIZE 60U

#ifndef AWG_ETH_CACHE_OP_T_DEFINED
#define AWG_ETH_CACHE_OP_T_DEFINED
typedef void (*awg_eth_cache_op_t)(void *ctx, uintptr_t address, size_t length);
#endif

struct awg_eth_tx_config {
	struct axi_dmac *dmac;
	uint8_t *buffer[AWG_ETH_TX_BUFFER_COUNT];
	uint32_t dma_address[AWG_ETH_TX_BUFFER_COUNT];
	size_t buffer_size;
	size_t min_frame_size;
	size_t cache_line_size;
	awg_eth_cache_op_t flush;
	void *cache_ctx;
};

struct awg_eth_tx_stats {
	uint32_t frames_submitted;
	uint32_t frames_completed;
	uint32_t frames_rejected;
	uint32_t dma_errors;
	uint32_t irq_count;
	uint32_t padded_frames;
};

struct awg_eth_tx {
	struct awg_eth_tx_config config;
	struct awg_eth_tx_stats stats;
	size_t queued_length[AWG_ETH_TX_BUFFER_COUNT];
	uint8_t active_index;
	uint8_t next_submit_index;
	uint8_t queued_mask;
	bool initialized;
	bool in_flight;
	volatile bool completion_pending;
};

int32_t awg_eth_tx_init(struct awg_eth_tx *tx,
			const struct awg_eth_tx_config *config);
int32_t awg_eth_tx_submit(struct awg_eth_tx *tx, const uint8_t *frame,
			  size_t length);
void awg_eth_tx_irq(void *instance);
int32_t awg_eth_tx_poll(struct awg_eth_tx *tx);
int32_t awg_eth_tx_service(struct awg_eth_tx *tx);
uint32_t awg_eth_tx_available(const struct awg_eth_tx *tx);
void awg_eth_tx_stop(struct awg_eth_tx *tx);

#ifdef __cplusplus
}
#endif

#endif /* AWG_ETH_TX_H */
