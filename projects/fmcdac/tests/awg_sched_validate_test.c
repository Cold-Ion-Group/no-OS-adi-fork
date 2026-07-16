/*
 * awg_sched_validate_test.c — host-side unit tests for awg_sched validation.
 *
 * Build:
 *   make -C projects/fmcdac/tests
 *
 * Run:
 *   make -C projects/fmcdac/tests run
 *
 * Every AWG_EVTVAL_ERR_* path in awg_sched_validate_events() is exercised.
 * Hardware register I/O is replaced by a stub that mirrors writes back on
 * read; the IP_ID / IP_VERSION / IP_CAPS registers are pre-populated so
 * awg_sched_config() succeeds without a real peripheral.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

#include "awg_sched.h"
#include "awg_sched_regs.h"

/* -----------------------------------------------------------------------
 * Stub register bank — mirrors writes back on reads.
 * The bank is indexed by (offset / 4); 256 entries covers 0x00..0x3FF.
 * --------------------------------------------------------------------- */

#define STUB_REG_WORDS 256U
#define TEST_IRQ_DONE_AND_ERROR_MASK (AWG_SCHED_IRQ_DONE | AWG_SCHED_IRQ_ERROR)
#define TEST_EVENT_FLAG AWG_SCHED_FLAG_PHASE_REINIT
static uint32_t s_stub_regs[STUB_REG_WORDS];
static uint32_t s_last_ctrl_write;
static int s_irq_wait_simulate_done;
static int s_epoch_reload_simulate_done;
/*
 * When non-zero, writes of ARM/RUN to CTRL automatically update the STATUS
 * stub register to simulate hardware state machine acknowledgment.  Only set
 * during streaming tests that exercise awg_stream_start_if_needed(); the
 * legacy preload tests manage STATUS manually and must leave this at zero.
 */
static int s_stub_simulate_arm_ack;

int no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t val)
{
	uint32_t idx = offset / 4U;

	(void)base;

	/*
	 * Simulate W1C register behavior for STREAM_CTRL and IRQ_STATUS so
	 * streaming tests observe the same semantics as real hardware.
	 *
	 * STREAM_CTRL layout:
	 *   [0] MODE     — normal RW
	 *   [1] OVERFLOW — W1C (write 1 clears the sticky bit)
	 *   [2] EOF_SEEN — RO (preserved; hardware sets, software cannot clear)
	 *   [3] DMA_MODE — normal RW, captured with MODE at ARM
	 *
	 * IRQ_STATUS — fully W1C; writing 1 to any bit clears that bit.
	 */
	if (offset == AWG_SCHED_REG_STREAM_CTRL) {
		if (idx < STUB_REG_WORDS) {
			uint32_t cur         = s_stub_regs[idx];
			uint32_t new_mode    = val &
				(AWG_SCHED_STREAM_CTRL_MODE |
				 AWG_SCHED_STREAM_CTRL_DMA_MODE);
			uint32_t ovfl_clr    = val & AWG_SCHED_STREAM_CTRL_OVERFLOW;
			uint32_t eof_seen    = cur & AWG_SCHED_STREAM_CTRL_EOF_SEEN;
			s_stub_regs[idx]     = new_mode | eof_seen |
			                       ((cur & AWG_SCHED_STREAM_CTRL_OVERFLOW) &
			                        ~ovfl_clr);
		}
		return 0;
	}

	if (offset == AWG_SCHED_REG_IRQ_STATUS) {
		/* Only the implemented low six IRQ bits are W1C. */
		if (idx < STUB_REG_WORDS)
			s_stub_regs[idx] &= ~(val & AWG_SCHED_IRQ_ALL);
		return 0;
	}

	if (offset == AWG_SCHED_REG_CTRL) {
		uint32_t depth = s_stub_regs[AWG_SCHED_REG_STREAM_DEPTH / 4U];
		uint32_t stream_ctrl =
			s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U];

		s_last_ctrl_write = val;
		/* CTRL contains write-only pulses and therefore never reads back. */
		s_stub_regs[idx] = 0U;
		if ((val & AWG_SCHED_CTRL_RESET_SOFT) != 0U) {
			s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = 0U;
			s_stub_regs[AWG_SCHED_REG_OCCUPANCY / 4U] = 0U;
			s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U] = depth;
			s_stub_regs[AWG_SCHED_REG_STREAM_PUSHES / 4U] = 0U;
			s_stub_regs[AWG_SCHED_REG_STREAM_STALLS / 4U] = 0U;
			s_stub_regs[AWG_SCHED_REG_COMMIT_COUNT / 4U] = 0U;
			s_stub_regs[AWG_SCHED_REG_REINIT_COUNT / 4U] = 0U;
			s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U] =
				stream_ctrl & (AWG_SCHED_STREAM_CTRL_MODE |
					       AWG_SCHED_STREAM_CTRL_DMA_MODE);
		}
		if (s_stub_simulate_arm_ack && (val & AWG_SCHED_CTRL_ARM) != 0U)
			s_stub_regs[AWG_SCHED_REG_STATUS / 4U] =
				AWG_SCHED_STATUS_ARMED;
		if (s_stub_simulate_arm_ack && (val & AWG_SCHED_CTRL_RUN) != 0U)
			s_stub_regs[AWG_SCHED_REG_STATUS / 4U] =
				AWG_SCHED_STATUS_RUNNING;
		if ((val & AWG_SCHED_CTRL_STOP) != 0U)
			s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = 0U;
		return 0;
	}

	if (offset == AWG_SCHED_REG_EVT_WCTRL) {
		uint32_t ctrl = s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U];
		uint32_t free_space = s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U];

		s_stub_regs[idx] = val;
		if ((val & AWG_SCHED_EVT_WCTRL_PUSH) != 0U &&
		    (ctrl & AWG_SCHED_STREAM_CTRL_MODE) != 0U &&
		    (ctrl & AWG_SCHED_STREAM_CTRL_DMA_MODE) == 0U) {
			if (free_space != 0U) {
				s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U]--;
				s_stub_regs[AWG_SCHED_REG_OCCUPANCY / 4U]++;
				s_stub_regs[AWG_SCHED_REG_STREAM_PUSHES / 4U]++;
			} else {
				s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U] |=
					AWG_SCHED_STREAM_CTRL_OVERFLOW;
			}
		}
		return 0;
	}

	if (idx < STUB_REG_WORDS)
		s_stub_regs[idx] = val;

	return 0;
}

int no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *val)
{
uint32_t idx = offset / 4U;

(void)base;
if (idx < STUB_REG_WORDS) {
*val = s_stub_regs[idx];
return 0;
}
return -1;
}

void no_os_mdelay(uint32_t ms)
{
(void)ms;

if (s_epoch_reload_simulate_done) {
s_stub_regs[AWG_SCHED_REG_TIME_NOW_LO / 4U] = 0U;
s_epoch_reload_simulate_done = 0;
}
}

void no_os_udelay(uint32_t us)
{
	(void)us;

	if (s_epoch_reload_simulate_done) {
		s_stub_regs[AWG_SCHED_REG_TIME_NOW_LO / 4U] = 0U;
		s_epoch_reload_simulate_done = 0;
	}
}

void awg_sched_irq_wait_hook(uint32_t wait_ms_left)
{
	(void)wait_ms_left;

	if (!s_irq_wait_simulate_done)
		return;

	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = AWG_SCHED_STATUS_DONE;
	s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] = TEST_IRQ_DONE_AND_ERROR_MASK;
	awg_sched_irq_signal();
	s_irq_wait_simulate_done = 0;
}

/* -----------------------------------------------------------------------
 * Log capture — stored so tests can inspect output if needed.
 * --------------------------------------------------------------------- */

static char s_log_buf[4096];
static int  s_log_len;

static void test_log_fn(const char *fmt, ...)
{
va_list ap;
int n;

va_start(ap, fmt);
n = vsnprintf(s_log_buf + s_log_len,
      sizeof(s_log_buf) - (size_t)s_log_len, fmt, ap);
va_end(ap);
if (n > 0)
s_log_len += n;
}

static void log_reset(void)
{
s_log_len = 0;
s_log_buf[0] = '\0';
}

/* -----------------------------------------------------------------------
 * Helpers — populate IP identity registers and call awg_sched_config().
 * --------------------------------------------------------------------- */

/*
 * Pre-populate the stub register bank with the values awg_sched_config()
 * expects, then call awg_sched_config().  This exercises the same code
 * path as real hardware without requiring a physical peripheral.
 *
 * IP_CAPS is set to: depth_log2=6 (64 events), payload=128b, ts=64b.
 */
static int stub_config(uint32_t max_events)
{
awg_sched_cfg_t cfg;

memset(s_stub_regs, 0, sizeof(s_stub_regs));
s_last_ctrl_write = 0U;
s_irq_wait_simulate_done = 0;
s_epoch_reload_simulate_done = 0;

/* IP identity registers (byte offsets / 4 = word index). */
s_stub_regs[AWG_SCHED_REG_IP_ID      / 4U] = AWG_SCHED_IP_ID;
s_stub_regs[AWG_SCHED_REG_IP_VERSION / 4U] =
AWG_SCHED_IP_VERSION;
/* caps: depth_log2=6 → hw_event_depth=64; payload=128b; ts=64b */
s_stub_regs[AWG_SCHED_REG_IP_CAPS    / 4U] =
(6U   << 24) |  /* log2(depth)    */
(128U << 16) |  /* payload bits   */
(64U  <<  8);   /* timestamp bits */
s_stub_regs[AWG_SCHED_REG_STREAM_DEPTH / 4U] = 64U;
s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U] = 64U;

memset(&cfg, 0, sizeof(cfg));
cfg.base_addr       = 0x44AA0000U;
cfg.max_events      = max_events;
cfg.tick_hz         = 1000000U;
cfg.done_timeout_ms = 2000U;
cfg.log_fn          = test_log_fn;

return awg_sched_config(&cfg);
}

/* -----------------------------------------------------------------------
 * Test framework
 * --------------------------------------------------------------------- */

static int s_pass;
static int s_fail;

#define EXPECT_EQ(a, b) \
do { \
if ((a) != (b)) { \
printf("  FAIL %s:%d: expected %d got %d\n", \
       __FILE__, __LINE__, (int)(b), (int)(a)); \
s_fail++; \
} else { \
s_pass++; \
} \
} while (0)

#define EXPECT_NE(a, b) \
do { \
if ((a) == (b)) { \
printf("  FAIL %s:%d: expected value != %d\n", \
       __FILE__, __LINE__, (int)(b)); \
s_fail++; \
} else { \
s_pass++; \
} \
} while (0)

#define EXPECT_TRUE(cond) \
do { \
if (!(cond)) { \
printf("  FAIL %s:%d: expected true: %s\n", \
       __FILE__, __LINE__, #cond); \
s_fail++; \
} else { \
s_pass++; \
} \
} while (0)

static void test_begin(const char *name)
{
printf("[TEST] %s\n", name);
log_reset();
}

/* -----------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------- */

static void test_struct_sizes(void)
{
test_begin("Struct size / offset ABI guards");
EXPECT_EQ((int)sizeof(awg_payload_v1_t), 16);
EXPECT_EQ((int)sizeof(awg_event_v1_t),   32);
EXPECT_EQ((int)offsetof(awg_event_v1_t, timestamp_ticks), 0);
EXPECT_EQ((int)offsetof(awg_event_v1_t, channel),         8);
EXPECT_EQ((int)offsetof(awg_event_v1_t, flags),           10);
EXPECT_EQ((int)offsetof(awg_event_v1_t, payload),         12);
EXPECT_EQ((int)offsetof(awg_event_v1_t, reserved),        28);
}

static void test_config_ok(void)
{
test_begin("awg_sched_config: succeeds with correct IP identity");
EXPECT_EQ(stub_config(64), 0);
}

static void test_config_bad_ip_id(void)
{
awg_sched_cfg_t cfg;
int ret;

test_begin("awg_sched_config: fails on IP_ID mismatch");
memset(s_stub_regs, 0, sizeof(s_stub_regs));
s_stub_regs[AWG_SCHED_REG_IP_ID / 4U] = 0xDEADBEEFU;  /* wrong */

memset(&cfg, 0, sizeof(cfg));
cfg.base_addr  = 0x44AA0000U;
cfg.max_events = 64U;
cfg.log_fn     = test_log_fn;
log_reset();

ret = awg_sched_config(&cfg);
EXPECT_NE(ret, 0);
}

static void test_ok_empty(void)
{
awg_sched_validation_report_t r;
int ret;

test_begin("AWG_EVTVAL_OK: empty event list");
EXPECT_EQ(stub_config(64), 0);
ret = awg_sched_validate_events(NULL, 0, NULL, &r);
EXPECT_EQ(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_OK);
}

static void test_ok_single(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
int ret;

test_begin("AWG_EVTVAL_OK: single valid event");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.channel = 0U;
ev.flags = TEST_EVENT_FLAG;
ret = awg_sched_validate_events(&ev, 1, NULL, &r);
EXPECT_EQ(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_OK);
}

static void test_not_configured(void)
{
awg_sched_cfg_t cfg;
awg_sched_validation_report_t r;
awg_event_v1_t ev;
int ret;

test_begin("AWG_EVTVAL_ERR_NOT_CONFIGURED");
/*
 * Force unconfigured state: populate stub registers with a wrong IP_ID
 * so that awg_sched_config() reaches the fail: label which zeroes
 * g_awg_sched (including configured=false).
 */
memset(s_stub_regs, 0, sizeof(s_stub_regs));  /* IP_ID=0 → mismatch */
memset(&cfg, 0, sizeof(cfg));
cfg.base_addr  = 0x44AA0000U;
cfg.max_events = 64U;
cfg.log_fn     = test_log_fn;
(void)awg_sched_config(&cfg);  /* expected to fail */

memset(&ev, 0, sizeof(ev));
ret = awg_sched_validate_events(&ev, 1, NULL, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_NOT_CONFIGURED);
}

static void test_null_events(void)
{
awg_sched_validation_report_t r;
int ret;

test_begin("AWG_EVTVAL_ERR_NULL_EVENTS");
EXPECT_EQ(stub_config(64), 0);
ret = awg_sched_validate_events(NULL, 3, NULL, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_NULL_EVENTS);
}

static void test_too_many(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev[5];
int ret;

test_begin("AWG_EVTVAL_ERR_TOO_MANY_EVENTS");
EXPECT_EQ(stub_config(4), 0);
memset(ev, 0, sizeof(ev));
ev[0].timestamp_ticks = 100U;  ev[0].flags = TEST_EVENT_FLAG;
ev[1].timestamp_ticks = 200U;  ev[1].flags = TEST_EVENT_FLAG;
ev[2].timestamp_ticks = 300U;  ev[2].flags = TEST_EVENT_FLAG;
ev[3].timestamp_ticks = 400U;  ev[3].flags = TEST_EVENT_FLAG;
ev[4].timestamp_ticks = 500U;  ev[4].flags = TEST_EVENT_FLAG;
ret = awg_sched_validate_events(ev, 5, NULL, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_TOO_MANY_EVENTS);
}

static void test_ts_not_monotonic(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev[3];
int ret;

test_begin("AWG_EVTVAL_ERR_TS_NOT_MONOTONIC");
EXPECT_EQ(stub_config(64), 0);
memset(ev, 0, sizeof(ev));
ev[0].timestamp_ticks = 1000U;  ev[0].flags = TEST_EVENT_FLAG;
ev[1].timestamp_ticks = 2000U;  ev[1].flags = TEST_EVENT_FLAG;
ev[2].timestamp_ticks = 500U;   ev[2].flags = TEST_EVENT_FLAG;  /* backward */
ret = awg_sched_validate_events(ev, 3, NULL, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_TS_NOT_MONOTONIC);
EXPECT_EQ((unsigned)r.failing_index, 2U);
}

static void test_ts_delta_too_small(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev[2];
awg_sched_validation_rules_t rules;
int ret;

test_begin("AWG_EVTVAL_ERR_TS_DELTA_TOO_SMALL");
EXPECT_EQ(stub_config(64), 0);
memset(ev, 0, sizeof(ev));
ev[0].timestamp_ticks = 1000U;  ev[0].flags = TEST_EVENT_FLAG;
ev[1].timestamp_ticks = 1000U;  ev[1].flags = TEST_EVENT_FLAG;  /* delta = 0 */

awg_sched_validation_rules_default(&rules);
rules.min_delta_ticks = 1U;
rules.delta_mode = AWG_SCHED_DELTA_MODE_STRICT;
rules.min_reinit_delta_ticks = 0U;

ret = awg_sched_validate_events(ev, 2, &rules, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_TS_DELTA_TOO_SMALL);
}

static void test_delta_allow_zero_same_channel(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev[2];
awg_sched_validation_rules_t rules;
int ret;

test_begin("DELTA_MODE_ALLOW_ZERO_ON_SAME_CHANNEL: passes");
EXPECT_EQ(stub_config(64), 0);
memset(ev, 0, sizeof(ev));
ev[0].timestamp_ticks = 1000U;  ev[0].channel = 0U;  ev[0].flags = TEST_EVENT_FLAG;
ev[1].timestamp_ticks = 1000U;  ev[1].channel = 0U;  ev[1].flags = TEST_EVENT_FLAG;

awg_sched_validation_rules_default(&rules);
rules.min_delta_ticks = 1U;
rules.delta_mode = AWG_SCHED_DELTA_MODE_ALLOW_ZERO_ON_SAME_CHANNEL;
rules.min_reinit_delta_ticks = 0U;

ret = awg_sched_validate_events(ev, 2, &rules, &r);
EXPECT_EQ(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_OK);
}

static void test_reinit_spacing(void)
{
	awg_sched_validation_report_t r;
	awg_event_v1_t ev[2];
	awg_sched_validation_rules_t rules;
	int ret;

	test_begin("AWG_EVTVAL_ERR_REINIT_SPACING");
	EXPECT_EQ(stub_config(64), 0);
	memset(ev, 0, sizeof(ev));
	ev[0].timestamp_ticks = 1000U;
	ev[0].channel = 0U;
	ev[0].flags = TEST_EVENT_FLAG;
	ev[1].timestamp_ticks = 1004U;
	ev[1].channel = 0U;
	ev[1].flags = TEST_EVENT_FLAG;

	awg_sched_validation_rules_default(&rules);
	rules.min_reinit_delta_ticks = 8U;

	ret = awg_sched_validate_events(ev, 2, &rules, &r);
	EXPECT_NE(ret, 0);
	EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_REINIT_SPACING);
}

static void test_reinit_spacing_ok(void)
{
	awg_sched_validation_report_t r;
	awg_event_v1_t ev[2];
	awg_sched_validation_rules_t rules;
	int ret;

	test_begin("AWG_EVTVAL_REINIT_SPACING: pass case");
	EXPECT_EQ(stub_config(64), 0);
	memset(ev, 0, sizeof(ev));
	ev[0].timestamp_ticks = 1000U;
	ev[0].channel = 0U;
	ev[0].flags = TEST_EVENT_FLAG;
	ev[1].timestamp_ticks = 1016U;
	ev[1].channel = 0U;
	ev[1].flags = TEST_EVENT_FLAG;

	awg_sched_validation_rules_default(&rules);
	rules.min_reinit_delta_ticks = 8U;

	ret = awg_sched_validate_events(ev, 2, &rules, &r);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_OK);
}

static void test_set_epoch(void)
{
	int ret;

	test_begin("set_epoch writes reload registers");
	EXPECT_EQ(stub_config(64), 0);

	s_stub_regs[AWG_SCHED_REG_TIME_NOW_LO / 4U] = 100U;
	s_epoch_reload_simulate_done = 1;

	ret = awg_sched_set_epoch();
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_TIME_RELOAD_LO / 4U], 0);
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_TIME_RELOAD_HI / 4U], 0);
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_TIME_RELOAD_CTRL / 4U],
		  (int)AWG_SCHED_TIME_RELOAD_LOAD_NOW);
	EXPECT_TRUE(strstr(s_log_buf, "[SCHED-ARTIFACT] set_epoch") != NULL);
}

static void test_reserved_flags(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
int ret;

test_begin("AWG_EVTVAL_ERR_RESERVED_FLAGS");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.flags = 0x00FFU;  /* bits above allowed mask 0x0001 */
ret = awg_sched_validate_events(&ev, 1, NULL, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_RESERVED_FLAGS);
}

static void test_channel_width(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
int ret;

test_begin("AWG_EVTVAL_ERR_CHANNEL_WIDTH");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.flags = TEST_EVENT_FLAG;
ev.channel  = 0x0002U;  /* default channel_mask=0x0001 → bit1 illegal */
ret = awg_sched_validate_events(&ev, 1, NULL, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_CHANNEL_WIDTH);
}

static void test_tone_width(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
awg_sched_validation_rules_t rules;
int ret;

test_begin("AWG_EVTVAL_ERR_TONE_WIDTH");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.flags = TEST_EVENT_FLAG;
/* tone occupies word0[1:0] (mask=0x3, shift=0); set bit2 */
ev.payload.word0 = 0x00000004U;

awg_sched_validation_rules_default(&rules);
rules.tone_mask = 0x3U;
rules.freq_mask = 0U;  /* disable freq check so only tone triggers */

ret = awg_sched_validate_events(&ev, 1, &rules, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_TONE_WIDTH);
}

static void test_freq_width(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
awg_sched_validation_rules_t rules;
int ret;

test_begin("AWG_EVTVAL_ERR_FREQ_WIDTH");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.flags = TEST_EVENT_FLAG;
/*
 * Use an 8-bit freq_mask so we can actually exceed it.  freq occupies
 * word0[31:16]; with freq_mask=0x00FF, freq_shift=16, a value of 0x0100
 * in the upper half exceeds the mask:
 *   (0x01000000 >> 16) = 0x100; 0x100 & ~0xFF = 0x100 != 0 → fail.
 */
ev.payload.word0 = 0x01000000U;

awg_sched_validation_rules_default(&rules);
rules.tone_mask = 0U;
rules.freq_mask = 0x00FFU;  /* 8-bit frequency field for this test */

ret = awg_sched_validate_events(&ev, 1, &rules, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_FREQ_WIDTH);
}

static void test_scale_width(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
awg_sched_validation_rules_t rules;
int ret;

test_begin("AWG_EVTVAL_ERR_SCALE_WIDTH");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.flags = TEST_EVENT_FLAG;
/* scale occupies word1[15:0]; use a narrow mask and exceed it. */
ev.payload.word1 = 0x00000100U;

awg_sched_validation_rules_default(&rules);
rules.tone_mask = 0U;
rules.freq_mask = 0U;
rules.scale_mask = 0x00FFU;

ret = awg_sched_validate_events(&ev, 1, &rules, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_SCALE_WIDTH);
}

static void test_phase_width(void)
{
awg_sched_validation_report_t r;
awg_event_v1_t ev;
awg_sched_validation_rules_t rules;
int ret;

test_begin("AWG_EVTVAL_ERR_PHASE_WIDTH");
EXPECT_EQ(stub_config(64), 0);
memset(&ev, 0, sizeof(ev));
ev.timestamp_ticks = 1000U;
ev.flags = TEST_EVENT_FLAG;
/* phase occupies word2[15:0]; use a narrow mask and exceed it. */
ev.payload.word2 = 0x00000100U;

awg_sched_validation_rules_default(&rules);
rules.tone_mask  = 0U;
rules.freq_mask  = 0U;
rules.scale_mask = 0U;
rules.phase_mask = 0x00FFU;

ret = awg_sched_validate_events(&ev, 1, &rules, &r);
EXPECT_NE(ret, 0);
EXPECT_EQ((int)r.code, (int)AWG_EVTVAL_ERR_PHASE_WIDTH);
}

static void test_fake_hw_progression(void)
{
	awg_event_v1_t ev;
	awg_sched_status_t st;
	int ret;

	test_begin("fake-hw arm/run/done progression");
	EXPECT_EQ(stub_config(64), 0);

	memset(&ev, 0, sizeof(ev));
	ev.timestamp_ticks = 1000U;
	ev.flags = TEST_EVENT_FLAG;
	ev.payload.word0 = 0x00010000U;

	ret = awg_sched_load_events(&ev, 1U);
	EXPECT_EQ(ret, 0);

	ret = awg_sched_arm();
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)s_last_ctrl_write,
		  (int)AWG_SCHED_CTRL_ARM);

	/* Simulate hardware acknowledging ARM. */
	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = AWG_SCHED_STATUS_ARMED;
	s_stub_simulate_arm_ack = 1;

	ret = awg_sched_start();
	s_stub_simulate_arm_ack = 0;
	EXPECT_EQ(ret, 0);
	/*
	 * start() must issue RUN as a separate write after an ARM round-trip
	 * (not ARM|RUN in one combined write).
	 */
	EXPECT_EQ((int)s_last_ctrl_write,
		  (int)AWG_SCHED_CTRL_RUN);

	/* Simulate completion and verify polled wait path. */
	s_irq_wait_simulate_done = 1;
	ret = awg_sched_wait_done(1U, &st);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)st.done, 1);
	EXPECT_EQ((int)st.error, 0);
}

static void test_irq_wait_done(void)
{
	awg_event_v1_t ev;
	awg_sched_status_t st;
	int ret;

	test_begin("irq wait_done progression");
	EXPECT_EQ(stub_config(64), 0);

	memset(&ev, 0, sizeof(ev));
	ev.timestamp_ticks = 1000U;
	ev.flags = TEST_EVENT_FLAG;
	ev.payload.word0 = 0x00010000U;

	ret = awg_sched_load_events(&ev, 1U);
	EXPECT_EQ(ret, 0);

	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = AWG_SCHED_STATUS_ARMED;
	s_stub_simulate_arm_ack = 1;

	ret = awg_sched_start();
	s_stub_simulate_arm_ack = 0;
	EXPECT_EQ(ret, 0);

	s_irq_wait_simulate_done = 1;
	ret = awg_sched_wait_done(10U, &st);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)st.done, 1);
	EXPECT_EQ((int)st.error, 0);
	EXPECT_EQ((int)st.irq_status_latched, (int)TEST_IRQ_DONE_AND_ERROR_MASK);
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U], 0);
}

static void test_rtl_error_code_fallback(void)
{
	awg_sched_status_t status;
	int ret;

	test_begin("status error code: documented field then Phase-E RTL fallback");
	EXPECT_EQ(stub_config(64), 0);

	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] =
		AWG_SCHED_STATUS_ERROR | (0x5AU << 16);
	s_stub_regs[AWG_SCHED_REG_ERR_REG / 4U] = 0U;
	ret = awg_sched_get_status(&status);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)status.error, 1);
	EXPECT_EQ((int)status.err_code, 0x5A);
	EXPECT_EQ((int)status.err_code_from_rtl_fallback, 1);

	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] =
		AWG_SCHED_STATUS_ERROR | (0x33U << 8);
	s_stub_regs[AWG_SCHED_REG_ERR_REG / 4U] = 0U;
	ret = awg_sched_get_status(&status);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)status.err_code, 0x33);
	EXPECT_EQ((int)status.err_code_from_rtl_fallback, 0);
}

/* -----------------------------------------------------------------------
 * Streaming tests
 * --------------------------------------------------------------------- */

#if FMCDAC_AWG_SCHED_STREAM

/*
 * TEST_STREAM_DEPTH  — value written to the STREAM_DEPTH stub register.
 *                      Must be >= 1 and <= TEST_STREAM_RING_EVENTS.
 * TEST_STREAM_RING_EVENTS — staging ring capacity passed to stream_open.
 *                           Must be >= TEST_STREAM_DEPTH.
 */
#define TEST_STREAM_DEPTH       4U
#define TEST_STREAM_RING_EVENTS 8U

static awg_event_v1_t s_stream_ring[TEST_STREAM_RING_EVENTS];

/*
 * Configure the scheduler for streaming tests: call stub_config() then
 * additionally populate STREAM_DEPTH and reset the arm-ack simulation flag.
 */
static int stub_stream_config(void)
{
	int ret = stub_config(64U);

	if (ret)
		return ret;
	s_stub_regs[AWG_SCHED_REG_STREAM_DEPTH / 4U] = TEST_STREAM_DEPTH;
	s_stub_simulate_arm_ack = 0;
	return 0;
}

/*
 * test_stream_open_registers — open/close round-trip verifying that
 * stream_open() correctly initialises STREAM_CTRL (MODE set, OVERFLOW
 * cleared via W1C), LOW_WMARK, and IRQ_ENABLE.  After close(), push()
 * must return -EPERM (producer closed, stream still logically open).
 */
static void test_stream_open_registers(void)
{
	awg_sched_stream_cfg_t scfg;
	awg_event_v1_t ev;
	int ret;

	test_begin("stream open: registers initialised, close seals producer");
	EXPECT_EQ(stub_stream_config(), 0);

	memset(&scfg, 0, sizeof(scfg));
	scfg.staging_buffer    = s_stream_ring;
	scfg.staging_capacity  = TEST_STREAM_RING_EVENTS;
	scfg.low_wmark_events  = TEST_STREAM_DEPTH;

	ret = awg_sched_stream_open(&scfg);
	EXPECT_EQ(ret, 0);

	/* STREAM_CTRL must have MODE set; OVERFLOW must be clear (W1C). */
	EXPECT_TRUE((s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U] &
		     AWG_SCHED_STREAM_CTRL_MODE) != 0U);
	EXPECT_EQ((int)(s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U] &
			AWG_SCHED_STREAM_CTRL_OVERFLOW), 0);

	/* LOW_WMARK must be set to the requested value. */
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_LOW_WMARK / 4U],
		  (int)TEST_STREAM_DEPTH);

	/* IRQ_ENABLE must be non-zero. */
	EXPECT_TRUE(s_stub_regs[AWG_SCHED_REG_IRQ_ENABLE / 4U] != 0U);

	ret = awg_sched_stream_close(false);
	EXPECT_EQ(ret, 0);

	/* After close(), the producer is sealed: push must return -EPERM. */
	memset(&ev, 0, sizeof(ev));
	ev.timestamp_ticks = 100U;
	EXPECT_EQ(awg_sched_stream_push(&ev, 1U), -EPERM);
}

/*
 * test_stream_drain_steady — push a batch of events and verify they are
 * fully drained from the DDR ring to the HW FIFO in a single drain_step()
 * call (achieved by setting refill_chunk_max >= n and FREE_SPACE >= n).
 */
static void test_stream_drain_steady(void)
{
	awg_sched_stream_cfg_t scfg;
	awg_event_v1_t evs[TEST_STREAM_DEPTH];
	uint32_t i;
	int ret;

	test_begin("stream drain: batch drains completely in one drain_step");
	EXPECT_EQ(stub_stream_config(), 0);

	/* Pre-condition: HW FIFO has room for all events. */
	s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U] = TEST_STREAM_DEPTH;
	/* Enable ARM/RUN acknowledgment so start_if_needed() succeeds. */
	s_stub_simulate_arm_ack = 1;

	memset(&scfg, 0, sizeof(scfg));
	scfg.staging_buffer    = s_stream_ring;
	scfg.staging_capacity  = TEST_STREAM_RING_EVENTS;
	scfg.refill_chunk_max  = TEST_STREAM_RING_EVENTS; /* drain everything */

	ret = awg_sched_stream_open(&scfg);
	EXPECT_EQ(ret, 0);

	memset(evs, 0, sizeof(evs));
	for (i = 0U; i < TEST_STREAM_DEPTH; i++)
		evs[i].timestamp_ticks = 1000U + i * 100U;
	evs[0].flags = AWG_SCHED_FLAG_PHASE_REINIT;

	ret = awg_sched_stream_push(evs, TEST_STREAM_DEPTH);
	EXPECT_EQ(ret, 0);

	/* All events must have been forwarded: DDR ring should be empty. */
	EXPECT_EQ((int)awg_sched_stream_ddr_free_events(),
		  (int)TEST_STREAM_RING_EVENTS);

	/* EVT_WCTRL must carry the PUSH strobe from the last event write. */
	EXPECT_TRUE((s_stub_regs[AWG_SCHED_REG_EVT_WCTRL / 4U] &
		     AWG_SCHED_EVT_WCTRL_PUSH) != 0U);

	s_stub_simulate_arm_ack = 0;
}

/*
 * test_stream_ring_full_eagain — fill the DDR ring to capacity while HW
 * FIFO free-space is zero (drain_step cannot move events).  A subsequent
 * push must return -EAGAIN rather than silently overwriting ring entries.
 */
static void test_stream_ring_full_eagain(void)
{
	awg_sched_stream_cfg_t scfg;
	awg_event_v1_t ev;
	uint32_t i;
	int ret;

	test_begin("stream push: ring full returns -EAGAIN");
	EXPECT_EQ(stub_stream_config(), 0);

	memset(&scfg, 0, sizeof(scfg));
	scfg.staging_buffer   = s_stream_ring;
	scfg.staging_capacity = TEST_STREAM_RING_EVENTS;

	ret = awg_sched_stream_open(&scfg);
	EXPECT_EQ(ret, 0);
	/* stream_open soft-resets the FIFO, so force backpressure afterward. */
	s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U] = 0U;

	memset(&ev, 0, sizeof(ev));

	/* Fill the ring to capacity — each push must succeed. */
	for (i = 0U; i < TEST_STREAM_RING_EVENTS; i++) {
		ev.timestamp_ticks = 1000U + i * 100U;
		ret = awg_sched_stream_push(&ev, 1U);
		EXPECT_EQ(ret, 0);
	}

	/* One more push into a full ring must be refused. */
	ev.timestamp_ticks = 1000U + TEST_STREAM_RING_EVENTS * 100U;
	ret = awg_sched_stream_push(&ev, 1U);
	EXPECT_EQ(ret, -EAGAIN);
}

/*
 * test_stream_hw_overflow_hard_error — simulate the HW asserting the W1C
 * OVERFLOW bit in STREAM_CTRL after the stream was opened.  The next
 * drain_step() must record a hard error and return -EAGAIN; subsequent
 * push() calls must return -EIO (hard error is sticky).
 */
static void test_stream_hw_overflow_hard_error(void)
{
	awg_sched_stream_cfg_t scfg;
	awg_event_v1_t ev;
	int ret;

	test_begin("stream drain: HW OVERFLOW sets sticky hard error");
	EXPECT_EQ(stub_stream_config(), 0);

	memset(&scfg, 0, sizeof(scfg));
	scfg.staging_buffer   = s_stream_ring;
	scfg.staging_capacity = TEST_STREAM_RING_EVENTS;

	ret = awg_sched_stream_open(&scfg);
	EXPECT_EQ(ret, 0);

	/* Simulate hardware asserting OVERFLOW after open cleared it. */
	s_stub_regs[AWG_SCHED_REG_STREAM_CTRL / 4U] |=
		AWG_SCHED_STREAM_CTRL_OVERFLOW;

	ret = awg_sched_stream_drain_step();
	EXPECT_EQ(ret, -EAGAIN);

	/* Hard-error recovery soft-resets and seals the producer. */
	memset(&ev, 0, sizeof(ev));
	ev.timestamp_ticks = 1000U;
	ret = awg_sched_stream_push(&ev, 1U);
	EXPECT_EQ(ret, -EPERM);
}

/*
 * test_stream_irq_refill_trigger — verify that IRQ_LOW_WATERMARK and
 * IRQ_EMPTY_STALL are treated as refill triggers: the IRQ handler must
 * acknowledge (W1C-clear) the non-hard-error IRQ bits and leave the
 * hard-error bits (UNDERRUN, SPACING_VIOLATION, ERROR) untouched.
 */
static void test_stream_irq_refill_trigger(void)
{
	awg_sched_stream_cfg_t scfg;
	int ret;

	test_begin("stream IRQ: LOW_WATERMARK / EMPTY_STALL acked, ring drains");
	EXPECT_EQ(stub_stream_config(), 0);
	s_stub_regs[AWG_SCHED_REG_FREE_SPACE / 4U] = TEST_STREAM_DEPTH;

	memset(&scfg, 0, sizeof(scfg));
	scfg.staging_buffer   = s_stream_ring;
	scfg.staging_capacity = TEST_STREAM_RING_EVENTS;

	ret = awg_sched_stream_open(&scfg);
	EXPECT_EQ(ret, 0);

	/* --- LOW_WATERMARK IRQ --- */
	s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] = AWG_SCHED_IRQ_LOW_WATERMARK;
	awg_sched_stream_irq_handler(AWG_SCHED_IRQ_LOW_WATERMARK);

	/* Handler must have W1C-acked LOW_WATERMARK (non-hard-error). */
	EXPECT_EQ((int)(s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] &
			AWG_SCHED_IRQ_LOW_WATERMARK), 0);

	/* With empty ring, drain returns 0 cleanly (no start_if_needed). */
	ret = awg_sched_stream_drain_step();
	EXPECT_EQ(ret, 0);

	/* --- EMPTY_STALL IRQ --- */
	s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] = AWG_SCHED_IRQ_EMPTY_STALL;
	awg_sched_stream_irq_handler(AWG_SCHED_IRQ_EMPTY_STALL);

	/* Handler must have W1C-acked EMPTY_STALL. */
	EXPECT_EQ((int)(s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] &
			AWG_SCHED_IRQ_EMPTY_STALL), 0);

	/*
	 * Hard-error IRQ bits must NOT be acked by the handler (they need to
	 * be seen by drain_step()).  Set UNDERRUN and verify it is preserved.
	 */
	s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] = AWG_SCHED_IRQ_UNDERRUN;
	awg_sched_stream_irq_handler(AWG_SCHED_IRQ_UNDERRUN);
	EXPECT_TRUE((s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U] &
		     AWG_SCHED_IRQ_UNDERRUN) != 0U);
}

#endif /* FMCDAC_AWG_SCHED_STREAM */

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */

int main(void)
{
test_struct_sizes();
test_config_ok();
test_config_bad_ip_id();
test_ok_empty();
test_ok_single();
test_not_configured();
test_null_events();
test_too_many();
test_ts_not_monotonic();
test_ts_delta_too_small();
test_delta_allow_zero_same_channel();
test_reinit_spacing();
test_reinit_spacing_ok();
test_reserved_flags();
test_channel_width();
test_tone_width();
test_freq_width();
test_scale_width();
test_phase_width();
test_set_epoch();
test_fake_hw_progression();
test_irq_wait_done();
test_rtl_error_code_fallback();

#if FMCDAC_AWG_SCHED_STREAM
test_stream_open_registers();
test_stream_drain_steady();
test_stream_ring_full_eagain();
test_stream_hw_overflow_hard_error();
test_stream_irq_refill_trigger();
#endif

printf("\n%d passed, %d failed\n", s_pass, s_fail);
return (s_fail > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
