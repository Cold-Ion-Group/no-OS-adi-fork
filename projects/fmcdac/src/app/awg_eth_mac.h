#ifndef AWG_ETH_MAC_H
#define AWG_ETH_MAC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PG210 v4.0 management register offsets for the MAC+PCS/PMA variant. */
#define AWG_ETH_MAC_REG_GT_RESET             0x0000U
#define AWG_ETH_MAC_REG_RESET                0x0004U
#define AWG_ETH_MAC_REG_TX_CONFIGURATION     0x000CU
#define AWG_ETH_MAC_REG_RX_CONFIGURATION     0x0014U
#define AWG_ETH_MAC_REG_RX_MTU               0x0018U
#define AWG_ETH_MAC_REG_TX_STATUS            0x0400U
#define AWG_ETH_MAC_REG_RX_STATUS            0x0404U
#define AWG_ETH_MAC_REG_STATUS               0x0408U
#define AWG_ETH_MAC_REG_RX_BLOCK_LOCK        0x040CU

#define AWG_ETH_MAC_GT_RESET_ALL             (1UL << 0)
#define AWG_ETH_MAC_GT_RESET_RX              (1UL << 1)
#define AWG_ETH_MAC_GT_RESET_TX              (1UL << 2)

#define AWG_ETH_MAC_RESET_RX_SERDES          (1UL << 0)
#define AWG_ETH_MAC_RESET_TX_SERDES          (1UL << 29)
#define AWG_ETH_MAC_RESET_RX                 (1UL << 30)
#define AWG_ETH_MAC_RESET_TX                 (1UL << 31)

#define AWG_ETH_MAC_TX_ENABLE                (1UL << 0)
#define AWG_ETH_MAC_TX_INSERT_FCS            (1UL << 1)
#define AWG_ETH_MAC_TX_SEND_LFI              (1UL << 3)
#define AWG_ETH_MAC_TX_SEND_RFI              (1UL << 4)
#define AWG_ETH_MAC_TX_SEND_IDLE             (1UL << 5)
#define AWG_ETH_MAC_TX_IPG_SHIFT             10U
#define AWG_ETH_MAC_TX_IPG_MASK              (0xFUL << AWG_ETH_MAC_TX_IPG_SHIFT)

#define AWG_ETH_MAC_RX_ENABLE                (1UL << 0)
#define AWG_ETH_MAC_RX_DELETE_FCS            (1UL << 1)
#define AWG_ETH_MAC_RX_IGNORE_FCS            (1UL << 2)
#define AWG_ETH_MAC_RX_CHECK_SFD             (1UL << 4)
#define AWG_ETH_MAC_RX_CHECK_PREAMBLE        (1UL << 5)

#define AWG_ETH_MAC_RX_MTU_MIN_MASK          0xFFUL
#define AWG_ETH_MAC_RX_MTU_MAX_SHIFT         16U
#define AWG_ETH_MAC_RX_MTU_MAX_MASK          (0x7FFFUL << AWG_ETH_MAC_RX_MTU_MAX_SHIFT)

#define AWG_ETH_MAC_TX_STATUS_LOCAL_FAULT    (1UL << 0)
#define AWG_ETH_MAC_TX_STATUS_FIFO_OVERFLOW  (1UL << 1)
#define AWG_ETH_MAC_TX_STATUS_FIFO_UNDERFLOW (1UL << 2)
#define AWG_ETH_MAC_TX_STATUS_BAD_PARITY     (1UL << 7)

#define AWG_ETH_MAC_RX_STATUS_LINK           (1UL << 0)
#define AWG_ETH_MAC_RX_STATUS_HIGH_BER       (1UL << 4)
#define AWG_ETH_MAC_RX_STATUS_REMOTE_FAULT   (1UL << 5)
#define AWG_ETH_MAC_RX_STATUS_LOCAL_FAULT    (1UL << 6)
#define AWG_ETH_MAC_RX_STATUS_INTERNAL_FAULT (1UL << 7)
#define AWG_ETH_MAC_RX_STATUS_RECEIVED_FAULT (1UL << 8)
#define AWG_ETH_MAC_RX_BLOCK_LOCKED          (1UL << 0)

/* KCU116 GPIO bit. A cleared bit enables the SFP0 transmitter. */
#define AWG_ETH_MAC_SFP0_TX_DISABLE_MASK     (1UL << 26)

typedef int32_t (*awg_eth_mac_read_fn)(void *ctx, uint32_t base,
				       uint32_t reg, uint32_t *value);
typedef int32_t (*awg_eth_mac_write_fn)(void *ctx, uint32_t base,
					uint32_t reg, uint32_t value);
typedef void (*awg_eth_mac_delay_fn)(void *ctx, uint32_t usec);
typedef int32_t (*awg_eth_mac_sfp_tx_disable_fn)(void *ctx, bool disabled);

struct awg_eth_mac_config {
	uint32_t base;
	uint32_t gt_reset_mask;
	uint32_t core_reset_mask;
	uint32_t tx_configuration;
	uint32_t rx_configuration;
	uint16_t rx_min_frame_size;
	uint16_t rx_max_frame_size;
	uint32_t reset_hold_us;
	uint32_t poll_delay_us;
	uint32_t poll_limit;
	awg_eth_mac_read_fn read;
	awg_eth_mac_write_fn write;
	awg_eth_mac_delay_fn delay;
	awg_eth_mac_sfp_tx_disable_fn set_sfp0_tx_disable;
	void *io_ctx;
	void *delay_ctx;
	void *sfp_ctx;
};

struct awg_eth_mac_status {
	uint32_t tx_status;
	uint32_t rx_status;
	uint32_t combined_status;
	uint32_t rx_block_lock;
	bool link_up;
	bool block_locked;
	bool local_fault;
	bool remote_fault;
	bool high_ber;
};

struct awg_eth_mac {
	struct awg_eth_mac_config config;
	bool initialized;
	bool configured;
	bool sfp_tx_enabled;
	uint32_t polls;
	uint32_t link_transitions;
	bool last_link_up;
};

void awg_eth_mac_config_defaults(struct awg_eth_mac_config *config);
int32_t awg_eth_mac_init(struct awg_eth_mac *mac,
			 const struct awg_eth_mac_config *config);
int32_t awg_eth_mac_reset(struct awg_eth_mac *mac);
int32_t awg_eth_mac_configure(struct awg_eth_mac *mac);
int32_t awg_eth_mac_bring_up(struct awg_eth_mac *mac);
int32_t awg_eth_mac_set_sfp_tx_enabled(struct awg_eth_mac *mac, bool enabled);
int32_t awg_eth_mac_read_status(struct awg_eth_mac *mac,
				struct awg_eth_mac_status *status);
int32_t awg_eth_mac_poll_link(struct awg_eth_mac *mac, bool expected_up,
			      struct awg_eth_mac_status *status);

#ifdef __cplusplus
}
#endif

#endif /* AWG_ETH_MAC_H */
