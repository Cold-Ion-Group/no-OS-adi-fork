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

#include "awg_sched.h"
#include "awg_sched_regs.h"

/* -----------------------------------------------------------------------
 * Stub register bank — mirrors writes back on reads.
 * The bank is indexed by (offset / 4); 256 entries covers 0x00..0x3FF.
 * --------------------------------------------------------------------- */

#define STUB_REG_WORDS 256U
#define TEST_IRQ_DONE_AND_ERROR_MASK (AWG_SCHED_IRQ_DONE | AWG_SCHED_IRQ_ERROR)
#define TEST_EVENT_FLAG AWG_SCHED_FLAG_PHASE_REINIT
#define TEST_STATUS_ARMED_LEGACY  (1U << 0)
#define TEST_STATUS_RUNNING_LEGACY (1U << 1)
#define TEST_STATUS_DONE_LEGACY   (1U << 2)
static uint32_t s_stub_regs[STUB_REG_WORDS];
static int s_irq_wait_simulate_done;
static int s_epoch_reload_simulate_done;

int no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t val)
{
uint32_t idx = offset / 4U;

(void)base;
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
}

void awg_sched_irq_wait_hook(uint32_t wait_ms_left)
{
	(void)wait_ms_left;

	if (!s_irq_wait_simulate_done)
		return;

	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = TEST_STATUS_DONE_LEGACY;
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
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_CTRL / 4U],
		  (int)AWG_SCHED_CTRL_ARM);

	/* Simulate hardware acknowledging ARM. */
	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = TEST_STATUS_ARMED_LEGACY;

	ret = awg_sched_start();
	EXPECT_EQ(ret, 0);
	/*
	 * start() must issue RUN as a separate write after an ARM round-trip
	 * (not ARM|RUN in one combined write).
	 */
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_CTRL / 4U],
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

	s_stub_regs[AWG_SCHED_REG_STATUS / 4U] = TEST_STATUS_ARMED_LEGACY;

	ret = awg_sched_start();
	EXPECT_EQ(ret, 0);

	s_irq_wait_simulate_done = 1;
	ret = awg_sched_wait_done(10U, &st);
	EXPECT_EQ(ret, 0);
	EXPECT_EQ((int)st.done, 1);
	EXPECT_EQ((int)st.error, 0);
	EXPECT_EQ((int)st.irq_status_latched, (int)TEST_IRQ_DONE_AND_ERROR_MASK);
	EXPECT_EQ((int)s_stub_regs[AWG_SCHED_REG_IRQ_STATUS / 4U],
		  (int)TEST_IRQ_DONE_AND_ERROR_MASK);
}

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

printf("\n%d passed, %d failed\n", s_pass, s_fail);
return (s_fail > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
