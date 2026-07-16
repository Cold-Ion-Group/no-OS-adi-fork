#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "awg_eth_mac.h"
#include "awg_eth_rx.h"
#include "awg_eth_tx.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static uint32_t test_checks;
static uint32_t test_failures;

static void test_check(bool condition, const char *expression,
		       const char *file, int line)
{
	test_checks++;
	if (condition)
		return;

	test_failures++;
	fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expression);
}

#define CHECK(expression) \
	test_check((expression), #expression, __FILE__, __LINE__)

enum fake_op {
	FAKE_OP_MAC_READ,
	FAKE_OP_MAC_WRITE,
	FAKE_OP_DELAY,
	FAKE_OP_GPIO_DISABLE,
	FAKE_OP_DMAC_READ,
	FAKE_OP_DMAC_WRITE,
	FAKE_OP_DMAC_START,
	FAKE_OP_DMAC_ISR,
	FAKE_OP_DMAC_STOP,
	FAKE_OP_INVALIDATE,
	FAKE_OP_FLUSH
};

struct fake_event {
	enum fake_op op;
	uintptr_t address;
	size_t length;
	uint32_t reg;
	uint32_t value;
};

#define FAKE_EVENT_CAPACITY 256U

static struct fake_event fake_events[FAKE_EVENT_CAPACITY];
static size_t fake_event_count;

static void fake_trace_reset(void)
{
	fake_event_count = 0U;
}

static void fake_trace(enum fake_op op, uintptr_t address, size_t length,
		       uint32_t reg, uint32_t value)
{
	CHECK(fake_event_count < ARRAY_SIZE(fake_events));
	if (fake_event_count >= ARRAY_SIZE(fake_events))
		return;

	fake_events[fake_event_count].op = op;
	fake_events[fake_event_count].address = address;
	fake_events[fake_event_count].length = length;
	fake_events[fake_event_count].reg = reg;
	fake_events[fake_event_count].value = value;
	fake_event_count++;
}

static size_t fake_count_op(enum fake_op op)
{
	size_t count = 0U;
	size_t index;

	for (index = 0U; index < fake_event_count; index++) {
		if (fake_events[index].op == op)
			count++;
	}

	return count;
}

static size_t fake_find_op(enum fake_op op, size_t occurrence)
{
	size_t seen = 0U;
	size_t index;

	for (index = 0U; index < fake_event_count; index++) {
		if (fake_events[index].op != op)
			continue;
		if (seen == occurrence)
			return index;
		seen++;
	}

	return SIZE_MAX;
}

struct fake_mac {
	uint32_t tx_status;
	uint32_t rx_status;
	uint32_t combined_status;
	uint32_t rx_block_lock;
	uint32_t gpio_value;
};

static int32_t fake_mac_read(void *ctx, uint32_t base, uint32_t reg,
			     uint32_t *value)
{
	struct fake_mac *fake = ctx;

	if (!fake || !value)
		return -EINVAL;

	switch (reg) {
	case AWG_ETH_MAC_REG_TX_STATUS:
		*value = fake->tx_status;
		break;
	case AWG_ETH_MAC_REG_RX_STATUS:
		*value = fake->rx_status;
		break;
	case AWG_ETH_MAC_REG_STATUS:
		*value = fake->combined_status;
		break;
	case AWG_ETH_MAC_REG_RX_BLOCK_LOCK:
		*value = fake->rx_block_lock;
		break;
	default:
		*value = 0U;
		break;
	}

	fake_trace(FAKE_OP_MAC_READ, (uintptr_t)base, 0U, reg, *value);
	return 0;
}

static int32_t fake_mac_write(void *ctx, uint32_t base, uint32_t reg,
			      uint32_t value)
{
	if (!ctx)
		return -EINVAL;

	fake_trace(FAKE_OP_MAC_WRITE, (uintptr_t)base, 0U, reg, value);
	return 0;
}

static void fake_delay(void *ctx, uint32_t usec)
{
	CHECK(ctx != NULL);
	fake_trace(FAKE_OP_DELAY, 0U, (size_t)usec, 0U, usec);
}

static int32_t fake_set_sfp_disable(void *ctx, bool disabled)
{
	struct fake_mac *fake = ctx;

	if (!fake)
		return -EINVAL;

	if (disabled)
		fake->gpio_value |= AWG_ETH_MAC_SFP0_TX_DISABLE_MASK;
	else
		fake->gpio_value &= ~AWG_ETH_MAC_SFP0_TX_DISABLE_MASK;
	fake_trace(FAKE_OP_GPIO_DISABLE, (uintptr_t)fake->gpio_value, 0U, 0U,
		   disabled ? 1U : 0U);
	return 0;
}

/* The MAC production file retains default no-OS backends even when tests
 * inject callbacks, so provide link-complete host stubs for those backends. */
int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
	(void)base;
	(void)offset;
	if (!data)
		return -EINVAL;
	*data = 0U;
	return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
	(void)base;
	(void)offset;
	(void)data;
	return 0;
}

void no_os_udelay(uint32_t usecs)
{
	(void)usecs;
}

#define FAKE_DMAC_START_CAPACITY 16U

struct fake_dmac {
	struct axi_dmac dmac;
	uint32_t flags;
	uint32_t irq_pending;
	uint32_t partial_length;
	uint32_t partial_id;
	bool partial_available;
	struct axi_dma_transfer starts[FAKE_DMAC_START_CAPACITY];
	size_t start_count;
	uint32_t stop_count;
};

static struct fake_dmac fake_rx_dmac;
static struct fake_dmac fake_tx_dmac;

static struct fake_dmac *fake_dmac_from_instance(struct axi_dmac *dmac)
{
	if (dmac == &fake_rx_dmac.dmac)
		return &fake_rx_dmac;
	if (dmac == &fake_tx_dmac.dmac)
		return &fake_tx_dmac;
	return NULL;
}

static void fake_dmac_reset(struct fake_dmac *fake,
			    enum dma_direction direction)
{
	memset(fake, 0, sizeof(*fake));
	fake->dmac.direction = direction;
	fake->dmac.max_length = UINT32_MAX;
}

int32_t axi_dmac_read(struct axi_dmac *dmac, uint32_t reg_addr,
		      uint32_t *reg_data)
{
	struct fake_dmac *fake = fake_dmac_from_instance(dmac);

	if (!fake || !reg_data)
		return -EINVAL;

	switch (reg_addr) {
	case AXI_DMAC_REG_FLAGS:
		*reg_data = fake->flags;
		break;
	case AXI_DMAC_REG_IRQ_PENDING:
		*reg_data = fake->irq_pending;
		break;
	case AXI_DMAC_REG_TRANSFER_DONE:
		*reg_data = fake->partial_available ?
			    AXI_DMAC_PARTIAL_TRANSFER_DONE : 0U;
		break;
	case AXI_DMAC_REG_PARTIAL_TRANSFER_LENGTH:
		*reg_data = fake->partial_length;
		break;
	case AXI_DMAC_REG_PARTIAL_TRANSFER_ID:
		*reg_data = fake->partial_id;
		fake->partial_available = false;
		break;
	default:
		*reg_data = 0U;
		break;
	}

	fake_trace(FAKE_OP_DMAC_READ, (uintptr_t)dmac, 0U, reg_addr, *reg_data);
	return 0;
}

int32_t axi_dmac_write(struct axi_dmac *dmac, uint32_t reg_addr,
		       uint32_t reg_data)
{
	struct fake_dmac *fake = fake_dmac_from_instance(dmac);

	if (!fake)
		return -EINVAL;

	switch (reg_addr) {
	case AXI_DMAC_REG_FLAGS:
		fake->flags = reg_data;
		break;
	case AXI_DMAC_REG_IRQ_PENDING:
		fake->irq_pending &= ~reg_data;
		break;
	default:
		break;
	}

	fake_trace(FAKE_OP_DMAC_WRITE, (uintptr_t)dmac, 0U, reg_addr, reg_data);
	return 0;
}

int32_t axi_dmac_set_partial_reporting(struct axi_dmac *dmac, bool enable)
{
	uint32_t flags;
	int32_t ret;

	if (!dmac)
		return -EINVAL;

	ret = axi_dmac_read(dmac, AXI_DMAC_REG_FLAGS, &flags);
	if (ret)
		return ret;
	if (enable)
		flags |= DMA_PARTIAL_REPORTING_EN;
	else
		flags &= ~(uint32_t)DMA_PARTIAL_REPORTING_EN;

	return axi_dmac_write(dmac, AXI_DMAC_REG_FLAGS, flags);
}

int32_t axi_dmac_get_partial_transfer(struct axi_dmac *dmac,
				      uint32_t *length,
				      uint32_t *transfer_id)
{
	uint32_t done;
	uint32_t report_length;
	uint32_t report_id;
	int32_t ret;

	if (!dmac || !length || !transfer_id)
		return -EINVAL;

	ret = axi_dmac_read(dmac, AXI_DMAC_REG_TRANSFER_DONE, &done);
	if (ret)
		return ret;
	if (!(done & AXI_DMAC_PARTIAL_TRANSFER_DONE))
		return -EAGAIN;

	ret = axi_dmac_read(dmac, AXI_DMAC_REG_PARTIAL_TRANSFER_LENGTH,
			    &report_length);
	if (ret)
		return ret;
	/* Reading the ID consumes the report, so it is deliberately last. */
	ret = axi_dmac_read(dmac, AXI_DMAC_REG_PARTIAL_TRANSFER_ID, &report_id);
	if (ret)
		return ret;

	*length = report_length;
	*transfer_id = report_id & AXI_DMAC_PARTIAL_TRANSFER_ID_MASK;
	return 0;
}

int32_t axi_dmac_transfer_start(struct axi_dmac *dmac,
				struct axi_dma_transfer *transfer)
{
	struct fake_dmac *fake = fake_dmac_from_instance(dmac);

	if (!fake || !transfer || !transfer->size)
		return -EINVAL;
	if (fake->start_count >= ARRAY_SIZE(fake->starts))
		return -ENOSPC;

	fake->starts[fake->start_count] = *transfer;
	fake->start_count++;
	dmac->transfer.size = transfer->size;
	dmac->transfer.cyclic = transfer->cyclic;
	dmac->transfer.src_addr = transfer->src_addr;
	dmac->transfer.dest_addr = transfer->dest_addr;
	dmac->transfer.transfer_done = false;
	fake_trace(FAKE_OP_DMAC_START, (uintptr_t)dmac, transfer->size,
		   transfer->src_addr, transfer->dest_addr);
	return 0;
}

static void fake_dmac_isr(struct axi_dmac *dmac)
{
	struct fake_dmac *fake = fake_dmac_from_instance(dmac);
	uint32_t pending;

	CHECK(fake != NULL);
	if (!fake)
		return;

	pending = fake->irq_pending;
	fake_trace(FAKE_OP_DMAC_ISR, (uintptr_t)dmac, 0U, 0U, pending);
	if (pending & AXI_DMAC_IRQ_EOT)
		dmac->transfer.transfer_done = true;
	fake->irq_pending &= ~pending;
}

void axi_dmac_dev_to_mem_isr(void *instance)
{
	fake_dmac_isr(instance);
}

void axi_dmac_mem_to_dev_isr(void *instance)
{
	fake_dmac_isr(instance);
}

void axi_dmac_transfer_stop(struct axi_dmac *dmac)
{
	struct fake_dmac *fake = fake_dmac_from_instance(dmac);

	CHECK(fake != NULL);
	if (!fake)
		return;
	fake->stop_count++;
	fake_trace(FAKE_OP_DMAC_STOP, (uintptr_t)dmac, 0U, 0U, 0U);
}

static void fake_dmac_complete(struct fake_dmac *fake, bool partial_available,
			       uint32_t length, uint32_t transfer_id)
{
	fake->partial_available = partial_available;
	fake->partial_length = length;
	fake->partial_id = transfer_id;
	fake->irq_pending = AXI_DMAC_IRQ_EOT;
}

static void fake_cache_invalidate(void *ctx, uintptr_t address, size_t length)
{
	CHECK(ctx != NULL);
	fake_trace(FAKE_OP_INVALIDATE, address, length, 0U, 0U);
}

static void fake_cache_flush(void *ctx, uintptr_t address, size_t length)
{
	CHECK(ctx != NULL);
	fake_trace(FAKE_OP_FLUSH, address, length, 0U, 0U);
}

static void test_mac_bring_up_and_status(void)
{
	const uint32_t unrelated_gpio_bits = 0xA5U;
	struct awg_eth_mac_config config;
	struct awg_eth_mac_status status;
	struct awg_eth_mac mac;
	struct fake_mac fake;
	uint32_t expected_mtu;
	uint32_t reads_before;
	size_t index;
	int32_t ret;

	memset(&fake, 0, sizeof(fake));
	fake.gpio_value = unrelated_gpio_bits;
	awg_eth_mac_config_defaults(&config);
	config.base = 0x44C00000U;
	config.poll_limit = 3U;
	config.poll_delay_us = 7U;
	config.read = fake_mac_read;
	config.write = fake_mac_write;
	config.delay = fake_delay;
	config.set_sfp0_tx_disable = fake_set_sfp_disable;
	config.io_ctx = &fake;
	config.delay_ctx = &fake;
	config.sfp_ctx = &fake;

	CHECK(AWG_ETH_MAC_SFP0_TX_DISABLE_MASK == (1UL << 26));
	CHECK(config.gt_reset_mask == AWG_ETH_MAC_GT_RESET_ALL);
	CHECK(config.core_reset_mask ==
	      (AWG_ETH_MAC_RESET_RX_SERDES | AWG_ETH_MAC_RESET_TX_SERDES |
	       AWG_ETH_MAC_RESET_RX | AWG_ETH_MAC_RESET_TX));
	CHECK(config.tx_configuration ==
	      (AWG_ETH_MAC_TX_ENABLE | AWG_ETH_MAC_TX_INSERT_FCS |
	       (12UL << AWG_ETH_MAC_TX_IPG_SHIFT)));
	CHECK(config.rx_configuration ==
	      (AWG_ETH_MAC_RX_ENABLE | AWG_ETH_MAC_RX_DELETE_FCS |
	       AWG_ETH_MAC_RX_CHECK_SFD | AWG_ETH_MAC_RX_CHECK_PREAMBLE));

	ret = awg_eth_mac_init(&mac, &config);
	CHECK(ret == 0);
	fake_trace_reset();
	ret = awg_eth_mac_bring_up(&mac);
	CHECK(ret == 0);
	CHECK(mac.configured);
	CHECK(mac.sfp_tx_enabled);
	CHECK(fake_event_count == 11U);

	CHECK(fake_events[0].op == FAKE_OP_GPIO_DISABLE);
	CHECK(fake_events[0].value == 1U);
	CHECK((fake_events[0].address & AWG_ETH_MAC_SFP0_TX_DISABLE_MASK) != 0U);
	CHECK(fake_events[1].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[1].reg == AWG_ETH_MAC_REG_GT_RESET);
	CHECK(fake_events[1].value == config.gt_reset_mask);
	CHECK(fake_events[2].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[2].reg == AWG_ETH_MAC_REG_RESET);
	CHECK(fake_events[2].value == config.core_reset_mask);
	CHECK(fake_events[3].op == FAKE_OP_DELAY);
	CHECK(fake_events[3].value == config.reset_hold_us);
	CHECK(fake_events[4].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[4].reg == AWG_ETH_MAC_REG_GT_RESET);
	CHECK(fake_events[4].value == 0U);
	CHECK(fake_events[5].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[5].reg == AWG_ETH_MAC_REG_RESET);
	CHECK(fake_events[5].value == 0U);
	CHECK(fake_events[6].op == FAKE_OP_DELAY);
	CHECK(fake_events[6].value == config.reset_hold_us);

	expected_mtu = ((uint32_t)config.rx_min_frame_size &
			AWG_ETH_MAC_RX_MTU_MIN_MASK) |
		       (((uint32_t)config.rx_max_frame_size <<
			 AWG_ETH_MAC_RX_MTU_MAX_SHIFT) &
			AWG_ETH_MAC_RX_MTU_MAX_MASK);
	CHECK(fake_events[7].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[7].reg == AWG_ETH_MAC_REG_RX_MTU);
	CHECK(fake_events[7].value == expected_mtu);
	CHECK(fake_events[8].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[8].reg == AWG_ETH_MAC_REG_TX_CONFIGURATION);
	CHECK(fake_events[8].value == config.tx_configuration);
	CHECK(fake_events[9].op == FAKE_OP_MAC_WRITE);
	CHECK(fake_events[9].reg == AWG_ETH_MAC_REG_RX_CONFIGURATION);
	CHECK(fake_events[9].value == config.rx_configuration);
	CHECK(fake_events[10].op == FAKE_OP_GPIO_DISABLE);
	CHECK(fake_events[10].value == 0U);
	CHECK((fake.gpio_value & AWG_ETH_MAC_SFP0_TX_DISABLE_MASK) == 0U);
	CHECK((fake.gpio_value & ~AWG_ETH_MAC_SFP0_TX_DISABLE_MASK) ==
	      unrelated_gpio_bits);

	for (index = 1U; index <= 9U; index++) {
		if (fake_events[index].op == FAKE_OP_MAC_WRITE)
			CHECK(fake_events[index].address == config.base);
	}

	fake.tx_status = AWG_ETH_MAC_TX_STATUS_LOCAL_FAULT;
	fake.rx_status = AWG_ETH_MAC_RX_STATUS_LINK |
			 AWG_ETH_MAC_RX_STATUS_HIGH_BER |
			 AWG_ETH_MAC_RX_STATUS_REMOTE_FAULT;
	fake.combined_status = 0x12345678U;
	fake.rx_block_lock = AWG_ETH_MAC_RX_BLOCK_LOCKED;
	fake_trace_reset();
	ret = awg_eth_mac_read_status(&mac, &status);
	CHECK(ret == 0);
	CHECK(status.tx_status == fake.tx_status);
	CHECK(status.rx_status == fake.rx_status);
	CHECK(status.combined_status == fake.combined_status);
	CHECK(status.rx_block_lock == fake.rx_block_lock);
	CHECK(status.block_locked);
	CHECK(status.local_fault);
	CHECK(status.remote_fault);
	CHECK(status.high_ber);
	CHECK(!status.link_up);
	CHECK(fake_event_count == 4U);
	CHECK(fake_events[0].reg == AWG_ETH_MAC_REG_TX_STATUS);
	CHECK(fake_events[1].reg == AWG_ETH_MAC_REG_RX_STATUS);
	CHECK(fake_events[2].reg == AWG_ETH_MAC_REG_STATUS);
	CHECK(fake_events[3].reg == AWG_ETH_MAC_REG_RX_BLOCK_LOCK);

	fake.tx_status = 0U;
	fake.rx_status = AWG_ETH_MAC_RX_STATUS_LINK;
	fake.rx_block_lock = AWG_ETH_MAC_RX_BLOCK_LOCKED;
	ret = awg_eth_mac_read_status(&mac, &status);
	CHECK(ret == 0);
	CHECK(status.link_up);
	CHECK(mac.link_transitions == 1U);

	fake.rx_status = 0U;
	fake.rx_block_lock = 0U;
	fake_trace_reset();
	reads_before = mac.polls;
	ret = awg_eth_mac_poll_link(&mac, true, &status);
	CHECK(ret == -ETIMEDOUT);
	CHECK(mac.polls - reads_before == config.poll_limit);
	CHECK(fake_count_op(FAKE_OP_MAC_READ) == 4U * config.poll_limit);
	CHECK(fake_count_op(FAKE_OP_DELAY) == config.poll_limit - 1U);
	CHECK(fake_events[fake_find_op(FAKE_OP_DELAY, 0U)].value ==
	      config.poll_delay_us);
}

_Alignas(64) static uint8_t rx_buffer0[128];
_Alignas(64) static uint8_t rx_buffer1[128];

static void test_rx_ping_pong_and_partial_reports(void)
{
	struct awg_eth_rx_config config;
	struct awg_eth_rx_frame frame0;
	struct awg_eth_rx_frame frame1;
	struct awg_eth_rx rx;
	size_t invalidates_before;
	int32_t ret;

	fake_dmac_reset(&fake_rx_dmac, DMA_DEV_TO_MEM);
	fake_rx_dmac.flags = DMA_CYCLIC;
	memset(&config, 0, sizeof(config));
	config.dmac = &fake_rx_dmac.dmac;
	config.buffer[0] = rx_buffer0;
	config.buffer[1] = rx_buffer1;
	config.dma_address[0] = 0x1000U;
	config.dma_address[1] = 0x1080U;
	config.buffer_size = sizeof(rx_buffer0);
	config.cache_line_size = 64U;
	config.invalidate = fake_cache_invalidate;
	config.cache_ctx = &rx;

	ret = awg_eth_rx_init(&rx, &config);
	CHECK(ret == 0);
	fake_trace_reset();
	ret = awg_eth_rx_start(&rx);
	CHECK(ret == 0);
	CHECK(rx.in_flight);
	CHECK(rx.active_index == 0U);
	CHECK(rx.stats.transfers_started == 1U);
	CHECK(fake_rx_dmac.start_count == 1U);
	CHECK(fake_rx_dmac.starts[0].dest_addr == config.dma_address[0]);
	CHECK(fake_rx_dmac.starts[0].size == config.buffer_size);
	CHECK(fake_rx_dmac.flags == (DMA_LAST | DMA_PARTIAL_REPORTING_EN));
	CHECK(fake_events[0].op == FAKE_OP_INVALIDATE);
	CHECK(fake_events[0].address == (uintptr_t)rx_buffer0);
	CHECK(fake_events[0].length == sizeof(rx_buffer0));
	CHECK(fake_find_op(FAKE_OP_DMAC_START, 0U) > 0U);

	fake_dmac_complete(&fake_rx_dmac, true, 47U, 5U);
	awg_eth_rx_irq(&rx);
	CHECK(rx.completion_pending);
	fake_trace_reset();
	ret = awg_eth_rx_service(&rx);
	CHECK(ret == 1);
	CHECK(rx.in_flight);
	CHECK(rx.active_index == 1U);
	CHECK(rx.ready_mask == 1U);
	CHECK(rx.stats.frames_completed == 1U);
	CHECK(fake_rx_dmac.start_count == 2U);
	CHECK(!fake_rx_dmac.partial_available);
	CHECK(fake_event_count >= 6U);
	CHECK(fake_events[0].op == FAKE_OP_DMAC_READ);
	CHECK(fake_events[0].reg == AXI_DMAC_REG_TRANSFER_DONE);
	CHECK(fake_events[1].op == FAKE_OP_DMAC_READ);
	CHECK(fake_events[1].reg == AXI_DMAC_REG_PARTIAL_TRANSFER_LENGTH);
	CHECK(fake_events[2].op == FAKE_OP_DMAC_READ);
	CHECK(fake_events[2].reg == AXI_DMAC_REG_PARTIAL_TRANSFER_ID);
	CHECK(fake_events[3].op == FAKE_OP_INVALIDATE);
	CHECK(fake_events[3].address == (uintptr_t)rx_buffer0);
	CHECK(fake_events[3].length == 64U);
	CHECK(fake_events[4].op == FAKE_OP_INVALIDATE);
	CHECK(fake_events[4].address == (uintptr_t)rx_buffer1);
	CHECK(fake_events[4].length == sizeof(rx_buffer1));
	CHECK(fake_find_op(FAKE_OP_DMAC_START, 0U) > 4U);

	ret = awg_eth_rx_acquire(&rx, &frame0);
	CHECK(ret == 0);
	CHECK(frame0.data == rx_buffer0);
	CHECK(frame0.length == 47U);
	CHECK(frame0.buffer_index == 0U);

	/* Holding buffer zero while buffer one completes leaves no armable buffer. */
	fake_dmac_complete(&fake_rx_dmac, true, 65U, 2U);
	awg_eth_rx_irq(&rx);
	fake_trace_reset();
	ret = awg_eth_rx_service(&rx);
	CHECK(ret == 1);
	CHECK(!rx.in_flight);
	CHECK(rx.stats.no_buffer_count == 1U);
	CHECK(fake_count_op(FAKE_OP_INVALIDATE) == 1U);
	CHECK(fake_events[fake_find_op(FAKE_OP_INVALIDATE, 0U)].address ==
	      (uintptr_t)rx_buffer1);
	CHECK(fake_events[fake_find_op(FAKE_OP_INVALIDATE, 0U)].length == 128U);

	ret = awg_eth_rx_acquire(&rx, &frame1);
	CHECK(ret == 0);
	CHECK(frame1.data == rx_buffer1);
	CHECK(frame1.length == 65U);
	CHECK(frame1.buffer_index == 1U);

	/* Releasing the first lease immediately rearms that buffer. */
	fake_trace_reset();
	ret = awg_eth_rx_release(&rx, frame0.buffer_index);
	CHECK(ret == 0);
	CHECK(rx.in_flight);
	CHECK(rx.active_index == 0U);
	CHECK(fake_events[0].op == FAKE_OP_INVALIDATE);
	CHECK(fake_events[0].address == (uintptr_t)rx_buffer0);
	CHECK(fake_events[0].length == sizeof(rx_buffer0));
	CHECK(fake_find_op(FAKE_OP_DMAC_START, 0U) != SIZE_MAX);

	/* EOT without a partial report is dropped; no frame length is guessed. */
	fake_dmac_complete(&fake_rx_dmac, false, 0U, 0U);
	awg_eth_rx_irq(&rx);
	fake_trace_reset();
	ret = awg_eth_rx_service(&rx);
	CHECK(ret == 1);
	CHECK(rx.stats.frames_dropped == 1U);
	CHECK(rx.stats.invalid_length_count == 1U);
	CHECK(fake_events[0].op == FAKE_OP_DMAC_READ);
	CHECK(fake_events[0].reg == AXI_DMAC_REG_TRANSFER_DONE);
	CHECK(fake_count_op(FAKE_OP_DMAC_READ) == 3U);
	CHECK(fake_events[1].reg == AXI_DMAC_REG_FLAGS);
	CHECK(fake_events[2].reg == AXI_DMAC_REG_FLAGS);

	ret = awg_eth_rx_release(&rx, frame1.buffer_index);
	CHECK(ret == 0);

	/* A reported length beyond the destination buffer is also dropped and
	 * never invalidated as though it were a valid completed frame. */
	fake_dmac_complete(&fake_rx_dmac, true,
			   (uint32_t)config.buffer_size + 1U, 1U);
	awg_eth_rx_irq(&rx);
	fake_trace_reset();
	invalidates_before = fake_count_op(FAKE_OP_INVALIDATE);
	ret = awg_eth_rx_service(&rx);
	CHECK(ret == 1);
	CHECK(rx.stats.frames_dropped == 2U);
	CHECK(rx.stats.invalid_length_count == 2U);
	CHECK(fake_count_op(FAKE_OP_INVALIDATE) - invalidates_before == 1U);
	CHECK(fake_events[fake_find_op(FAKE_OP_INVALIDATE, 0U)].length ==
	      config.buffer_size);
	CHECK(rx.stats.frames_completed == 2U);
	CHECK(rx.stats.irq_count == 4U);

	awg_eth_rx_stop(&rx);
	CHECK(fake_rx_dmac.stop_count == 1U);
	CHECK(!rx.in_flight);
}

_Alignas(64) static uint8_t tx_buffer0[128];
_Alignas(64) static uint8_t tx_buffer1[128];

static void test_tx_padding_queue_and_completion_chain(void)
{
	struct awg_eth_tx_config config;
	struct awg_eth_tx tx;
	uint8_t short_frame[42];
	uint8_t long_frame[80];
	size_t flush_index;
	size_t start_index;
	size_t index;
	int32_t ret;

	for (index = 0U; index < ARRAY_SIZE(short_frame); index++)
		short_frame[index] = (uint8_t)(0x20U + index);
	for (index = 0U; index < ARRAY_SIZE(long_frame); index++)
		long_frame[index] = (uint8_t)(0x80U + index);
	memset(tx_buffer0, 0xA5, sizeof(tx_buffer0));
	memset(tx_buffer1, 0xA5, sizeof(tx_buffer1));
	fake_dmac_reset(&fake_tx_dmac, DMA_MEM_TO_DEV);

	memset(&config, 0, sizeof(config));
	config.dmac = &fake_tx_dmac.dmac;
	config.buffer[0] = tx_buffer0;
	config.buffer[1] = tx_buffer1;
	config.dma_address[0] = 0x2000U;
	config.dma_address[1] = 0x2080U;
	config.buffer_size = sizeof(tx_buffer0);
	config.min_frame_size = AWG_ETH_TX_MIN_CLIENT_FRAME_SIZE;
	config.cache_line_size = 64U;
	config.flush = fake_cache_flush;
	config.cache_ctx = &tx;

	ret = awg_eth_tx_init(&tx, &config);
	CHECK(ret == 0);
	CHECK(awg_eth_tx_available(&tx) == 2U);
	fake_trace_reset();
	ret = awg_eth_tx_submit(&tx, short_frame, sizeof(short_frame));
	CHECK(ret == 0);
	CHECK(tx.in_flight);
	CHECK(tx.active_index == 0U);
	CHECK(tx.stats.frames_submitted == 1U);
	CHECK(tx.stats.padded_frames == 1U);
	CHECK(awg_eth_tx_available(&tx) == 1U);
	CHECK(fake_tx_dmac.start_count == 1U);
	CHECK(fake_tx_dmac.starts[0].src_addr == config.dma_address[0]);
	CHECK(fake_tx_dmac.starts[0].size == AWG_ETH_TX_MIN_CLIENT_FRAME_SIZE);
	CHECK(memcmp(tx_buffer0, short_frame, sizeof(short_frame)) == 0);
	for (index = sizeof(short_frame);
	     index < AWG_ETH_TX_MIN_CLIENT_FRAME_SIZE; index++)
		CHECK(tx_buffer0[index] == 0U);
	CHECK(tx_buffer0[AWG_ETH_TX_MIN_CLIENT_FRAME_SIZE] == 0xA5U);
	flush_index = fake_find_op(FAKE_OP_FLUSH, 0U);
	start_index = fake_find_op(FAKE_OP_DMAC_START, 0U);
	CHECK(flush_index != SIZE_MAX);
	CHECK(start_index != SIZE_MAX);
	CHECK(flush_index < start_index);
	CHECK(fake_events[flush_index].address == (uintptr_t)tx_buffer0);
	CHECK(fake_events[flush_index].length == 64U);

	/* A second frame occupies the spare buffer but cannot start concurrently. */
	fake_trace_reset();
	ret = awg_eth_tx_submit(&tx, long_frame, sizeof(long_frame));
	CHECK(ret == 0);
	CHECK(tx.in_flight);
	CHECK(tx.active_index == 0U);
	CHECK(tx.queued_mask == (uint8_t)(1U << 1));
	CHECK(fake_tx_dmac.start_count == 1U);
	CHECK(fake_count_op(FAKE_OP_FLUSH) == 0U);
	CHECK(awg_eth_tx_available(&tx) == 0U);
	CHECK(memcmp(tx_buffer1, long_frame, sizeof(long_frame)) == 0);

	ret = awg_eth_tx_submit(&tx, short_frame, sizeof(short_frame));
	CHECK(ret == -ENOSPC);
	CHECK(tx.stats.frames_rejected == 1U);
	CHECK(fake_tx_dmac.start_count == 1U);

	/* Completing frame zero starts queued frame one, with its cache flush
	 * ordered immediately before the second DMAC submission. */
	fake_dmac_complete(&fake_tx_dmac, false, 0U, 0U);
	awg_eth_tx_irq(&tx);
	CHECK(tx.completion_pending);
	fake_trace_reset();
	ret = awg_eth_tx_service(&tx);
	CHECK(ret == 1);
	CHECK(tx.stats.frames_completed == 1U);
	CHECK(tx.in_flight);
	CHECK(tx.active_index == 1U);
	CHECK(tx.queued_mask == 0U);
	CHECK(fake_tx_dmac.start_count == 2U);
	CHECK(fake_tx_dmac.starts[1].src_addr == config.dma_address[1]);
	CHECK(fake_tx_dmac.starts[1].size == sizeof(long_frame));
	flush_index = fake_find_op(FAKE_OP_FLUSH, 0U);
	start_index = fake_find_op(FAKE_OP_DMAC_START, 0U);
	CHECK(flush_index != SIZE_MAX);
	CHECK(start_index != SIZE_MAX);
	CHECK(flush_index < start_index);
	CHECK(fake_events[flush_index].address == (uintptr_t)tx_buffer1);
	CHECK(fake_events[flush_index].length == sizeof(tx_buffer1));

	fake_dmac_complete(&fake_tx_dmac, false, 0U, 0U);
	awg_eth_tx_irq(&tx);
	ret = awg_eth_tx_service(&tx);
	CHECK(ret == 1);
	CHECK(!tx.in_flight);
	CHECK(tx.stats.frames_completed == 2U);
	CHECK(tx.stats.frames_submitted == 2U);
	CHECK(tx.stats.irq_count == 2U);
	CHECK(awg_eth_tx_available(&tx) == 2U);

	awg_eth_tx_stop(&tx);
	CHECK(fake_tx_dmac.stop_count == 1U);
}

int main(void)
{
	test_mac_bring_up_and_status();
	test_rx_ping_pong_and_partial_reports();
	test_tx_padding_queue_and_completion_chain();

	if (test_failures) {
		fprintf(stderr, "awg_eth_io_test: %u/%u checks failed\n",
			test_failures, test_checks);
		return 1;
	}

	printf("awg_eth_io_test: %u checks passed\n", test_checks);
	return 0;
}
