#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "awg_eth_mac.h"
#include "no_os_axi_io.h"
#include "no_os_delay.h"

static int32_t awg_eth_mac_default_read(void *ctx, uint32_t base,
					uint32_t reg, uint32_t *value)
{
	(void)ctx;
	return no_os_axi_io_read(base, reg, value);
}

static int32_t awg_eth_mac_default_write(void *ctx, uint32_t base,
					 uint32_t reg, uint32_t value)
{
	(void)ctx;
	return no_os_axi_io_write(base, reg, value);
}

static void awg_eth_mac_default_delay(void *ctx, uint32_t usec)
{
	(void)ctx;
	no_os_udelay(usec);
}

static int32_t awg_eth_mac_read_reg(struct awg_eth_mac *mac, uint32_t reg,
				    uint32_t *value)
{
	return mac->config.read(mac->config.io_ctx, mac->config.base, reg, value);
}

static int32_t awg_eth_mac_write_reg(struct awg_eth_mac *mac, uint32_t reg,
				     uint32_t value)
{
	return mac->config.write(mac->config.io_ctx, mac->config.base, reg, value);
}

void awg_eth_mac_config_defaults(struct awg_eth_mac_config *config)
{
	if (!config)
		return;

	memset(config, 0, sizeof(*config));
	config->gt_reset_mask = AWG_ETH_MAC_GT_RESET_ALL;
	config->core_reset_mask = AWG_ETH_MAC_RESET_RX_SERDES |
				  AWG_ETH_MAC_RESET_TX_SERDES |
				  AWG_ETH_MAC_RESET_RX |
				  AWG_ETH_MAC_RESET_TX;
	config->tx_configuration = AWG_ETH_MAC_TX_ENABLE |
				   AWG_ETH_MAC_TX_INSERT_FCS |
				   (12UL << AWG_ETH_MAC_TX_IPG_SHIFT);
	config->rx_configuration = AWG_ETH_MAC_RX_ENABLE |
				   AWG_ETH_MAC_RX_DELETE_FCS |
				   AWG_ETH_MAC_RX_CHECK_SFD |
				   AWG_ETH_MAC_RX_CHECK_PREAMBLE;
	config->rx_min_frame_size = 64U;
	config->rx_max_frame_size = 2048U;
	config->reset_hold_us = 10U;
	config->poll_delay_us = 1000U;
	config->poll_limit = 1000U;
}

int32_t awg_eth_mac_init(struct awg_eth_mac *mac,
			 const struct awg_eth_mac_config *config)
{
	if (!mac || !config)
		return -EINVAL;
	if (!config->read && !config->base)
		return -EINVAL;
	if (!config->write && !config->base)
		return -EINVAL;
	if (!config->rx_min_frame_size ||
	    config->rx_min_frame_size > config->rx_max_frame_size ||
	    config->rx_max_frame_size > 0x7FFFU)
		return -EINVAL;
	if (!config->poll_limit)
		return -EINVAL;

	memset(mac, 0, sizeof(*mac));
	mac->config = *config;
	if (!mac->config.read)
		mac->config.read = awg_eth_mac_default_read;
	if (!mac->config.write)
		mac->config.write = awg_eth_mac_default_write;
	if (!mac->config.delay)
		mac->config.delay = awg_eth_mac_default_delay;
	mac->initialized = true;

	return 0;
}

int32_t awg_eth_mac_set_sfp_tx_enabled(struct awg_eth_mac *mac, bool enabled)
{
	int32_t ret;

	if (!mac || !mac->initialized)
		return -EINVAL;
	if (!mac->config.set_sfp0_tx_disable)
		return -ENOSYS;

	/* sfp0_tx_disable is active high; clear GPIO bit 26 to transmit. */
	ret = mac->config.set_sfp0_tx_disable(mac->config.sfp_ctx, !enabled);
	if (!ret)
		mac->sfp_tx_enabled = enabled;

	return ret;
}

int32_t awg_eth_mac_reset(struct awg_eth_mac *mac)
{
	int32_t ret;

	if (!mac || !mac->initialized)
		return -EINVAL;

	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_GT_RESET,
				    mac->config.gt_reset_mask);
	if (ret)
		return ret;
	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_RESET,
				    mac->config.core_reset_mask);
	if (ret)
		return ret;

	mac->config.delay(mac->config.delay_ctx, mac->config.reset_hold_us);

	/* The PG210 reset controls are clear-on-write pulses. Writing zero also
	 * makes the intended deasserted state explicit in register-model tests. */
	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_GT_RESET, 0U);
	if (ret)
		return ret;
	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_RESET, 0U);
	if (ret)
		return ret;

	mac->config.delay(mac->config.delay_ctx, mac->config.reset_hold_us);
	mac->configured = false;

	return 0;
}

int32_t awg_eth_mac_configure(struct awg_eth_mac *mac)
{
	uint32_t mtu;
	int32_t ret;

	if (!mac || !mac->initialized)
		return -EINVAL;

	mtu = ((uint32_t)mac->config.rx_min_frame_size &
	       AWG_ETH_MAC_RX_MTU_MIN_MASK) |
	      (((uint32_t)mac->config.rx_max_frame_size <<
		AWG_ETH_MAC_RX_MTU_MAX_SHIFT) & AWG_ETH_MAC_RX_MTU_MAX_MASK);

	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_RX_MTU, mtu);
	if (ret)
		return ret;
	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_TX_CONFIGURATION,
				    mac->config.tx_configuration);
	if (ret)
		return ret;
	ret = awg_eth_mac_write_reg(mac, AWG_ETH_MAC_REG_RX_CONFIGURATION,
				    mac->config.rx_configuration);
	if (ret)
		return ret;

	mac->configured = true;
	return 0;
}

int32_t awg_eth_mac_bring_up(struct awg_eth_mac *mac)
{
	int32_t ret;

	if (!mac || !mac->initialized)
		return -EINVAL;

	ret = awg_eth_mac_set_sfp_tx_enabled(mac, false);
	if (ret)
		return ret;
	ret = awg_eth_mac_reset(mac);
	if (ret)
		return ret;
	ret = awg_eth_mac_configure(mac);
	if (ret)
		return ret;

	return awg_eth_mac_set_sfp_tx_enabled(mac, true);
}

int32_t awg_eth_mac_read_status(struct awg_eth_mac *mac,
				struct awg_eth_mac_status *status)
{
	bool previous_link;
	int32_t ret;

	if (!mac || !status || !mac->initialized)
		return -EINVAL;

	memset(status, 0, sizeof(*status));
	ret = awg_eth_mac_read_reg(mac, AWG_ETH_MAC_REG_TX_STATUS,
				   &status->tx_status);
	if (ret)
		return ret;
	ret = awg_eth_mac_read_reg(mac, AWG_ETH_MAC_REG_RX_STATUS,
				   &status->rx_status);
	if (ret)
		return ret;
	ret = awg_eth_mac_read_reg(mac, AWG_ETH_MAC_REG_STATUS,
				   &status->combined_status);
	if (ret)
		return ret;
	ret = awg_eth_mac_read_reg(mac, AWG_ETH_MAC_REG_RX_BLOCK_LOCK,
				   &status->rx_block_lock);
	if (ret)
		return ret;

	status->block_locked =
		(status->rx_block_lock & AWG_ETH_MAC_RX_BLOCK_LOCKED) != 0U;
	status->remote_fault =
		(status->rx_status & AWG_ETH_MAC_RX_STATUS_REMOTE_FAULT) != 0U;
	status->local_fault =
		(status->tx_status & AWG_ETH_MAC_TX_STATUS_LOCAL_FAULT) != 0U ||
		(status->rx_status & (AWG_ETH_MAC_RX_STATUS_LOCAL_FAULT |
				      AWG_ETH_MAC_RX_STATUS_INTERNAL_FAULT |
				      AWG_ETH_MAC_RX_STATUS_RECEIVED_FAULT)) != 0U;
	status->high_ber =
		(status->rx_status & AWG_ETH_MAC_RX_STATUS_HIGH_BER) != 0U;
	status->link_up =
		(status->rx_status & AWG_ETH_MAC_RX_STATUS_LINK) != 0U &&
		status->block_locked && !status->local_fault &&
		!status->remote_fault && !status->high_ber;

	previous_link = mac->last_link_up;
	mac->last_link_up = status->link_up;
	if (status->link_up != previous_link)
		mac->link_transitions++;
	mac->polls++;

	return 0;
}

int32_t awg_eth_mac_poll_link(struct awg_eth_mac *mac, bool expected_up,
			      struct awg_eth_mac_status *status)
{
	uint32_t attempt;
	int32_t ret;

	if (!mac || !status || !mac->initialized)
		return -EINVAL;

	for (attempt = 0U; attempt < mac->config.poll_limit; attempt++) {
		ret = awg_eth_mac_read_status(mac, status);
		if (ret)
			return ret;
		if (status->link_up == expected_up)
			return 0;
		if ((attempt + 1U) < mac->config.poll_limit)
			mac->config.delay(mac->config.delay_ctx,
					  mac->config.poll_delay_us);
	}

	return -ETIMEDOUT;
}
