/*
 * Host-side unit tests for the Phase-F scheduler DMA refill service.
 *
 * The production refill and ring implementations are linked directly.  Only
 * the scheduler, AXI-DMAC, cache, and MMIO boundaries are faked here.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "awg_sched.h"
#include "awg_sched_dma.h"
#include "awg_sched_regs.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define TEST_STORAGE_EVENTS 32U
#define TEST_CACHE_LINE 64U
#define TEST_SCHED_BASE 0x44AA0000U
#define TEST_TRANSFER_HISTORY 8U
#define TEST_TRACE_DEPTH 16U

enum test_trace_op {
	TEST_TRACE_FLUSH = 1,
	TEST_TRACE_TRANSFER_START
};

struct test_fake {
	uint32_t free_space;
	uint32_t irq_pending;
	awg_sched_status_t scheduler_status;

	int axi_read_ret;
	int free_space_read_ret;
	int transfer_start_ret;
	int scheduler_status_ret;
	int scheduler_arm_ret;
	int scheduler_start_ret;
	int scheduler_reset_ret;

	uint32_t axi_read_calls;
	uint32_t free_space_read_calls;
	uint32_t transfer_start_calls;
	uint32_t transfer_stop_calls;
	uint32_t dmac_isr_calls;
	uint32_t scheduler_status_calls;
	uint32_t scheduler_arm_api_calls;
	uint32_t scheduler_start_api_calls;
	uint32_t scheduler_arm_strobes;
	uint32_t scheduler_run_strobes;
	uint32_t scheduler_reset_calls;

	uintptr_t flush_address;
	size_t flush_length;
	uint32_t flush_calls;

	struct axi_dma_transfer transfers[TEST_TRANSFER_HISTORY];
	enum test_trace_op trace[TEST_TRACE_DEPTH];
	uint32_t trace_count;
};

static struct test_fake g_fake;
static struct axi_dmac g_dmac;
static awg_stream_ring_t g_ring;
static struct awg_sched_dma g_refill;
static _Alignas(TEST_CACHE_LINE) uint8_t
	g_storage[TEST_STORAGE_EVENTS * AWG_SCHED_DMA_EVENT_BYTES];

static int g_passed;
static int g_failed;

static void test_begin(const char *name)
{
	printf("\n[TEST] %s\n", name);
}

static void test_expect_true(bool condition, const char *expression,
			     const char *file, int line)
{
	if (condition) {
		g_passed++;
		return;
	}

	g_failed++;
	printf("  FAIL %s:%d: %s\n", file, line, expression);
}

static void test_expect_u64(uint64_t actual, uint64_t expected,
			    const char *actual_expression,
			    const char *expected_expression,
			    const char *file, int line)
{
	if (actual == expected) {
		g_passed++;
		return;
	}

	g_failed++;
	printf("  FAIL %s:%d: %s=%llu, expected %s=%llu\n", file, line,
	       actual_expression, (unsigned long long)actual,
	       expected_expression, (unsigned long long)expected);
}

static void test_expect_i64(int64_t actual, int64_t expected,
			    const char *actual_expression,
			    const char *expected_expression,
			    const char *file, int line)
{
	if (actual == expected) {
		g_passed++;
		return;
	}

	g_failed++;
	printf("  FAIL %s:%d: %s=%lld, expected %s=%lld\n", file, line,
	       actual_expression, (long long)actual,
	       expected_expression, (long long)expected);
}

#define EXPECT_TRUE(expression) \
	test_expect_true((expression), #expression, __FILE__, __LINE__)
#define EXPECT_EQ_U(actual, expected) \
	test_expect_u64((uint64_t)(actual), (uint64_t)(expected), #actual, \
			#expected, __FILE__, __LINE__)
#define EXPECT_EQ_I(actual, expected) \
	test_expect_i64((int64_t)(actual), (int64_t)(expected), #actual, \
			#expected, __FILE__, __LINE__)

static void test_trace(enum test_trace_op operation)
{
	if (g_fake.trace_count < ARRAY_SIZE(g_fake.trace))
		g_fake.trace[g_fake.trace_count++] = operation;
}

static void test_cache_flush(void *ctx, uintptr_t address, size_t length)
{
	(void)ctx;
	g_fake.flush_calls++;
	g_fake.flush_address = address;
	g_fake.flush_length = length;
	test_trace(TEST_TRACE_FLUSH);
}

/* -------------------------------------------------------------------------
 * Production dependency fakes
 * ---------------------------------------------------------------------- */

int no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *value)
{
	if (!value)
		return -EINVAL;
	if (base != TEST_SCHED_BASE || offset != AWG_SCHED_REG_FREE_SPACE)
		return -EINVAL;

	g_fake.free_space_read_calls++;
	if (g_fake.free_space_read_ret)
		return g_fake.free_space_read_ret;

	*value = g_fake.free_space;
	return 0;
}

int32_t axi_dmac_read(struct axi_dmac *dmac, uint32_t reg_addr,
		      uint32_t *reg_data)
{
	if (dmac != &g_dmac || !reg_data ||
	    reg_addr != AXI_DMAC_REG_IRQ_PENDING)
		return -EINVAL;

	g_fake.axi_read_calls++;
	if (g_fake.axi_read_ret)
		return g_fake.axi_read_ret;

	*reg_data = g_fake.irq_pending;
	return 0;
}

void axi_dmac_mem_to_dev_isr(void *instance)
{
	struct axi_dmac *dmac = instance;

	g_fake.dmac_isr_calls++;
	if (dmac != &g_dmac)
		return;

	if ((g_fake.irq_pending & AXI_DMAC_IRQ_EOT) != 0U)
		dmac->transfer.transfer_done = true;
	g_fake.irq_pending = 0U;
}

int32_t axi_dmac_transfer_start(struct axi_dmac *dmac,
				struct axi_dma_transfer *transfer)
{
	uint32_t index = g_fake.transfer_start_calls;

	g_fake.transfer_start_calls++;
	test_trace(TEST_TRACE_TRANSFER_START);
	if (index < ARRAY_SIZE(g_fake.transfers) && transfer)
		g_fake.transfers[index] = *transfer;

	if (dmac != &g_dmac || !transfer)
		return -EINVAL;
	if (g_fake.transfer_start_ret)
		return g_fake.transfer_start_ret;

	dmac->transfer.size = transfer->size;
	dmac->transfer.cyclic = transfer->cyclic;
	dmac->transfer.src_addr = transfer->src_addr;
	dmac->transfer.dest_addr = transfer->dest_addr;
	dmac->transfer.transfer_done = false;
	return 0;
}

void axi_dmac_transfer_stop(struct axi_dmac *dmac)
{
	if (dmac == &g_dmac)
		g_fake.transfer_stop_calls++;
}

int awg_sched_get_status(awg_sched_status_t *status)
{
	g_fake.scheduler_status_calls++;
	if (g_fake.scheduler_status_ret)
		return g_fake.scheduler_status_ret;
	if (!status)
		return -EINVAL;

	*status = g_fake.scheduler_status;
	return 0;
}

int awg_sched_arm(void)
{
	g_fake.scheduler_arm_api_calls++;
	g_fake.scheduler_arm_strobes++;
	return g_fake.scheduler_arm_ret;
}

int awg_sched_start(void)
{
	/*
	 * The real public start operation includes ARM, waits for ARMED, and then
	 * strobes RUN.  Model that contract so callers cannot hide a redundant
	 * explicit awg_sched_arm() behind a shallow host stub.
	 */
	g_fake.scheduler_start_api_calls++;
	g_fake.scheduler_arm_strobes++;
	if (g_fake.scheduler_start_ret)
		return g_fake.scheduler_start_ret;
	g_fake.scheduler_run_strobes++;
	return 0;
}

int awg_sched_stream_reset_soft(void)
{
	g_fake.scheduler_reset_calls++;
	return g_fake.scheduler_reset_ret;
}

/* -------------------------------------------------------------------------
 * Fixture helpers
 * ---------------------------------------------------------------------- */

static int test_fixture_init(uint32_t capacity, uint32_t read_index,
			     uint32_t count, uint32_t free_space,
			     uint32_t configured_max_events,
			     uint32_t driver_max_events)
{
	struct awg_sched_dma_config config;
	int ret;

	memset(&g_fake, 0, sizeof(g_fake));
	memset(&g_dmac, 0, sizeof(g_dmac));
	memset(&g_ring, 0, sizeof(g_ring));
	memset(&g_refill, 0, sizeof(g_refill));
	memset(g_storage, 0xA5, sizeof(g_storage));

	if (capacity > TEST_STORAGE_EVENTS || read_index >= capacity ||
	    count > capacity || driver_max_events == 0U)
		return -EINVAL;

	ret = awg_stream_ring_init(&g_ring, g_storage, capacity,
				   AWG_SCHED_DMA_EVENT_BYTES);
	if (ret)
		return ret;

	/* Move an empty ring to the requested consumer index, then fill it. */
	if (read_index != 0U) {
		ret = awg_stream_ring_produce(&g_ring, read_index);
		if (ret)
			return ret;
		ret = awg_stream_ring_consume(&g_ring, read_index);
		if (ret)
			return ret;
	}
	ret = awg_stream_ring_produce(&g_ring, count);
	if (ret)
		return ret;

	g_fake.free_space = free_space;
	g_dmac.direction = DMA_MEM_TO_DEV;
	g_dmac.max_length =
		driver_max_events * AWG_SCHED_DMA_EVENT_BYTES - 1U;

	memset(&config, 0, sizeof(config));
	config.dmac = &g_dmac;
	config.ring = &g_ring;
	config.scheduler_base = TEST_SCHED_BASE;
	config.max_events = configured_max_events;
	config.cache_line_size = TEST_CACHE_LINE;
	config.cache_flush = test_cache_flush;

	return awg_sched_dma_init(&g_refill, &config);
}

static uintptr_t test_align_down(uintptr_t value, size_t alignment)
{
	return value & ~((uintptr_t)alignment - 1U);
}

static uintptr_t test_align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1U) & ~((uintptr_t)alignment - 1U);
}

static void test_expect_batch(uint32_t capacity, uint32_t read_index,
			      uint32_t available, uint32_t free_space,
			      uint32_t configured_max,
			      uint32_t driver_max, uint32_t expected)
{
	const void *source;
	uint32_t original_read_index;
	int ret;

	ret = test_fixture_init(capacity, read_index, available, free_space,
				configured_max, driver_max);
	EXPECT_EQ_I(ret, 0);
	if (ret)
		return;

	source = awg_stream_ring_consumer_const_ptr(&g_ring);
	original_read_index = g_ring.read_index;
	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 1U);
	EXPECT_TRUE(awg_sched_dma_in_flight(&g_refill));
	EXPECT_EQ_U(g_refill.pending_events, expected);
	EXPECT_EQ_U(g_fake.transfers[0].size,
		    expected * AWG_SCHED_DMA_EVENT_BYTES);
	EXPECT_EQ_U(g_fake.transfers[0].src_addr,
		    (uint32_t)(uintptr_t)source);

	/* Submission reserves nothing: the consumer advances only after EOT. */
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), available);
	EXPECT_EQ_U(g_ring.read_index, original_read_index);
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

static void test_batch_is_minimum_of_all_limits(void)
{
	test_begin("batch = min(available, FREE_SPACE, contiguous, config, driver)");

	/* Available events are limiting. */
	test_expect_batch(16U, 0U, 3U, 12U, 10U, 10U, 3U);
	/* Scheduler FREE_SPACE is limiting. */
	test_expect_batch(16U, 0U, 10U, 4U, 10U, 10U, 4U);
	/* Only two events remain before the physical ring end. */
	test_expect_batch(16U, 14U, 10U, 10U, 10U, 10U, 2U);
	/* The configured service cap is limiting. */
	test_expect_batch(16U, 0U, 10U, 10U, 3U, 10U, 3U);
	/* AXI-DMAC max_length permits only two complete event records. */
	test_expect_batch(16U, 0U, 10U, 10U, 10U, 2U, 2U);
}

static void test_cache_flush_alignment_and_ordering(void)
{
	const void *source;
	uintptr_t expected_start;
	uintptr_t expected_end;
	int ret;

	test_begin("cache range is aligned and flushed before DMA submission");
	EXPECT_EQ_U((uintptr_t)g_storage & (TEST_CACHE_LINE - 1U), 0U);
	EXPECT_EQ_I(test_fixture_init(8U, 1U, 3U, 3U, 3U, 3U), 0);
	source = awg_stream_ring_consumer_const_ptr(&g_ring);
	expected_start = test_align_down((uintptr_t)source, TEST_CACHE_LINE);
	expected_end = test_align_up((uintptr_t)source +
				     3U * AWG_SCHED_DMA_EVENT_BYTES,
				     TEST_CACHE_LINE);

	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.flush_calls, 1U);
	EXPECT_EQ_U(g_fake.flush_address, expected_start);
	EXPECT_EQ_U(g_fake.flush_length, expected_end - expected_start);
	EXPECT_EQ_U(g_fake.trace_count, 2U);
	EXPECT_EQ_U(g_fake.trace[0], TEST_TRACE_FLUSH);
	EXPECT_EQ_U(g_fake.trace[1], TEST_TRACE_TRANSFER_START);
}

static void test_one_in_flight_and_consume_only_after_eot(void)
{
	uint32_t initial_read_index;
	int ret;

	test_begin("one transfer in flight; ring commits only after EOT");
	EXPECT_EQ_I(test_fixture_init(8U, 0U, 6U, 6U, 4U, 8U), 0);
	initial_read_index = g_ring.read_index;

	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 1U);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 6U);

	/* No IRQ: servicing again must neither submit nor consume. */
	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 1U);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 6U);
	EXPECT_EQ_U(g_ring.read_index, initial_read_index);

	/* The ISR records completion, but foreground service owns the commit. */
	g_fake.irq_pending = AXI_DMAC_IRQ_EOT;
	awg_sched_dma_irq(&g_refill);
	EXPECT_TRUE(g_refill.completion_pending);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 6U);

	/* Prevent an immediate second transfer after the completed commit. */
	g_fake.free_space = 0U;
	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 2U);
	EXPECT_EQ_U(g_ring.read_index, 4U);
	EXPECT_TRUE(!awg_sched_dma_in_flight(&g_refill));
	EXPECT_TRUE(!g_refill.completion_pending);
	EXPECT_EQ_U(g_refill.stats.transfers_completed, 1U);
	EXPECT_EQ_U(g_refill.stats.events_completed, 4U);

	/* awg_sched_start() is itself the single ARM -> RUN operation. */
	EXPECT_EQ_U(g_fake.scheduler_start_api_calls, 1U);
	EXPECT_EQ_U(g_fake.scheduler_arm_strobes, 1U);
	EXPECT_EQ_U(g_fake.scheduler_run_strobes, 1U);
	EXPECT_TRUE(g_refill.scheduler_started);
}

static void test_wrap_is_split_at_ring_end(void)
{
	int ret;

	test_begin("wrapped ring is submitted as two contiguous DMA transfers");
	EXPECT_EQ_I(test_fixture_init(8U, 6U, 6U, 8U, 8U, 8U), 0);

	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 1U);
	EXPECT_EQ_U(g_fake.transfers[0].size,
		    2U * AWG_SCHED_DMA_EVENT_BYTES);
	EXPECT_EQ_U(g_fake.transfers[0].src_addr,
		    (uint32_t)(uintptr_t)&g_storage[6U * AWG_SCHED_DMA_EVENT_BYTES]);

	/* Polling sees EOT, commits the tail extent, and submits the head. */
	g_fake.irq_pending = AXI_DMAC_IRQ_EOT;
	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 2U);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 4U);
	EXPECT_EQ_U(g_ring.read_index, 0U);
	EXPECT_EQ_U(g_fake.transfers[1].size,
		    4U * AWG_SCHED_DMA_EVENT_BYTES);
	EXPECT_EQ_U(g_fake.transfers[1].src_addr,
		    (uint32_t)(uintptr_t)&g_storage[0]);
	EXPECT_TRUE(awg_sched_dma_in_flight(&g_refill));

	/* The second EOT consumes the head without restarting the scheduler. */
	g_fake.irq_pending = AXI_DMAC_IRQ_EOT;
	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 0U);
	EXPECT_EQ_U(g_refill.stats.transfers_completed, 2U);
	EXPECT_EQ_U(g_refill.stats.events_completed, 6U);
	EXPECT_EQ_U(g_fake.scheduler_start_api_calls, 1U);
}

static void test_transfer_start_error_does_not_advance_ring(void)
{
	uint32_t initial_read_index;
	int ret;

	test_begin("DMA start failure leaves consumer state untouched");
	EXPECT_EQ_I(test_fixture_init(8U, 2U, 5U, 5U, 4U, 8U), 0);
	initial_read_index = g_ring.read_index;
	g_fake.transfer_start_ret = -EBUSY;

	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, -EBUSY);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 1U);
	EXPECT_EQ_U(g_fake.flush_calls, 1U);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 5U);
	EXPECT_EQ_U(g_ring.read_index, initial_read_index);
	EXPECT_TRUE(!awg_sched_dma_in_flight(&g_refill));
	EXPECT_EQ_U(g_refill.pending_events, 0U);
	EXPECT_EQ_U(g_refill.stats.transfers_started, 0U);
	EXPECT_EQ_U(g_refill.stats.events_submitted, 0U);
	EXPECT_EQ_U(g_refill.stats.dma_errors, 1U);
}

static void test_low_watermark_requests_foreground_service(void)
{
	test_begin("low-watermark request sets foreground service flag");
	EXPECT_EQ_I(test_fixture_init(8U, 0U, 0U, 8U, 4U, 8U), 0);
	g_refill.service_pending = false;

	awg_sched_dma_request_service(&g_refill);
	EXPECT_TRUE(g_refill.service_pending);
	EXPECT_EQ_U(g_refill.stats.low_watermark_requests, 1U);

	awg_sched_dma_request_service(&g_refill);
	EXPECT_EQ_U(g_refill.stats.low_watermark_requests, 2U);
}

static void test_scheduler_error_blocks_submission(void)
{
	int ret;

	test_begin("scheduler hard error blocks DMA submission");
	EXPECT_EQ_I(test_fixture_init(8U, 0U, 4U, 4U, 4U, 8U), 0);
	g_fake.scheduler_status.error = true;

	ret = awg_sched_dma_service(&g_refill);
	EXPECT_EQ_I(ret, -EIO);
	EXPECT_EQ_U(g_fake.transfer_start_calls, 0U);
	EXPECT_EQ_U(g_fake.flush_calls, 0U);
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 4U);
	EXPECT_EQ_U(g_refill.stats.scheduler_errors, 1U);
}

static void test_abort_stops_dma_and_soft_resets_scheduler(void)
{
	int ret;

	test_begin("abort clears refill state and soft-resets scheduler");
	EXPECT_EQ_I(test_fixture_init(8U, 0U, 5U, 5U, 3U, 8U), 0);
	EXPECT_EQ_I(awg_sched_dma_service(&g_refill), 0);
	EXPECT_TRUE(awg_sched_dma_in_flight(&g_refill));
	g_refill.completion_pending = true;
	g_refill.service_pending = true;
	g_refill.scheduler_started = true;

	ret = awg_sched_dma_abort(&g_refill);
	EXPECT_EQ_I(ret, 0);
	EXPECT_EQ_U(g_fake.transfer_stop_calls, 1U);
	EXPECT_EQ_U(g_fake.scheduler_reset_calls, 1U);
	EXPECT_TRUE(!awg_sched_dma_in_flight(&g_refill));
	EXPECT_EQ_U(g_refill.pending_events, 0U);
	EXPECT_TRUE(!g_refill.completion_pending);
	EXPECT_TRUE(!g_refill.service_pending);
	EXPECT_TRUE(!g_refill.scheduler_started);

	/* Abort does not falsely claim that queued DDR events were delivered. */
	EXPECT_EQ_U(awg_stream_ring_count(&g_ring), 5U);
}

int main(void)
{
	test_batch_is_minimum_of_all_limits();
	test_cache_flush_alignment_and_ordering();
	test_one_in_flight_and_consume_only_after_eot();
	test_wrap_is_split_at_ring_end();
	test_transfer_start_error_does_not_advance_ring();
	test_low_watermark_requests_foreground_service();
	test_scheduler_error_blocks_submission();
	test_abort_stops_dma_and_soft_resets_scheduler();

	printf("\n%d passed, %d failed\n", g_passed, g_failed);
	return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
