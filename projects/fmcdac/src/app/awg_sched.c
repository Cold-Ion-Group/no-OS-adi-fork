#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "awg_sched.h"
#include "awg_sched_regs.h"
#include "awg_stream_ring.h"
#include "no_os_axi_io.h"
#include "no_os_delay.h"
#include "xil_printf.h"

#if FMCDAC_AWG_SCHED_STREAM
#include "parameters.h"
#endif

#ifndef AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY
#define AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY 0
#endif

static struct {
bool configured;
bool events_validated;
awg_sched_cfg_t cfg;
uint32_t loaded_events;
uint32_t hw_event_depth;
uint32_t hw_stream_depth;
uint8_t  hw_payload_bits;
uint8_t  hw_ts_bits;
bool mode_stream_cfg;
bool dma_mode_cfg;
bool mode_locked;
bool error_fallback_logged;
} g_awg_sched;
static volatile uint32_t g_awg_sched_irq_seq;

#define AWG_SCHED_FLAGS_ALLOWED_MASK      (AWG_SCHED_FLAG_PHASE_REINIT | AWG_SCHED_FLAG_EOF)
#define AWG_SCHED_CHANNEL_MASK_DEFAULT    0x0001U
#define AWG_SCHED_TONE_MASK_DEFAULT       0xFFFFU
#define AWG_SCHED_FREQ_MASK_DEFAULT       0x0000FFFFU
#define AWG_SCHED_SCALE_MASK_DEFAULT      0x0000FFFFU
#define AWG_SCHED_PHASE_MASK_DEFAULT      0x0000FFFFU
#define AWG_SCHED_MIN_REINIT_DELTA_TICKS  8U
/*
 * The HDL epoch reload target is zero; allow up to 8 ticks to account for
 * AXI read latency and scheduler-clock crossing jitter after SYSREF.
 */
#define AWG_SCHED_TIME_EPOCH_NEAR_ZERO    8U
#define AWG_SCHED_EVENT_COUNT_SETTLE_US   10U
#define AWG_SCHED_START_ARM_WAIT_US       1000U
#define AWG_SCHED_START_RUN_WAIT_US       1000U
#define AWG_SCHED_START_RUN_RETRIES       3U
#define AWG_SCHED_IRQ_ARM_MASK            (AWG_SCHED_IRQ_DONE | AWG_SCHED_IRQ_ERROR)
#define AWG_SCHED_STATUS_ERR_CODE_SHIFT   8U
#define AWG_SCHED_STATUS_RTL_ERR_SHIFT    16U
#define AWG_SCHED_STATUS_ERR_CODE_MASK    0xFFU
#define AWG_SCHED_STATUS_STATE_MASK       (AWG_SCHED_STATUS_ARMED | \
					   AWG_SCHED_STATUS_RUNNING | \
					   AWG_SCHED_STATUS_DONE | \
					   AWG_SCHED_STATUS_ERROR)
#define AWG_SCHED_CAPS_EVT_DEPTH_LOG2(caps) (((caps) >> 24) & 0xFFU)
#define AWG_SCHED_CAPS_PAYLOAD_BITS(caps)   (((caps) >> 16) & 0xFFU)
#define AWG_SCHED_CAPS_TS_BITS(caps)        (((caps) >> 8) & 0xFFU)

__attribute__((weak)) void awg_sched_irq_wait_hook(uint32_t wait_ms_left)
{
	(void)wait_ms_left;
}

/*
 * Emit a diagnostic log line.  Uses the caller-supplied log_fn if configured,
 * otherwise falls back to xil_printf.
 */
#define AWG_LOG(fmt, ...) \
do { \
if (g_awg_sched.cfg.log_fn) \
g_awg_sched.cfg.log_fn(fmt, ##__VA_ARGS__); \
else \
xil_printf(fmt, ##__VA_ARGS__); \
} while (0)

static const char *awg_sched_validation_reason(awg_evtval_err_t code)
{
switch (code) {
case AWG_EVTVAL_OK:
return "ok";
case AWG_EVTVAL_ERR_NOT_CONFIGURED:
return "scheduler not configured";
case AWG_EVTVAL_ERR_NULL_EVENTS:
return "events pointer is NULL while count > 0";
case AWG_EVTVAL_ERR_TOO_MANY_EVENTS:
return "event count exceeds hardware depth";
case AWG_EVTVAL_ERR_TS_NOT_MONOTONIC:
return "timestamp is not monotonic nondecreasing";
case AWG_EVTVAL_ERR_TS_DELTA_TOO_SMALL:
return "timestamp delta below Tmin_event";
case AWG_EVTVAL_ERR_RESERVED_FLAGS:
return "reserved/illegal flags are set";
case AWG_EVTVAL_ERR_CHANNEL_WIDTH:
return "channel exceeds configured field width";
case AWG_EVTVAL_ERR_TONE_WIDTH:
return "tone exceeds configured field width";
case AWG_EVTVAL_ERR_FREQ_WIDTH:
return "frequency exceeds configured field width";
case AWG_EVTVAL_ERR_SCALE_WIDTH:
return "scale exceeds configured field width";
case AWG_EVTVAL_ERR_PHASE_WIDTH:
return "phase exceeds configured field width";
case AWG_EVTVAL_ERR_REINIT_SPACING:
return "reinit event spacing is below minimum threshold";
default:
return "unknown validation error";
}
}

static void awg_sched_validation_report_set(awg_sched_validation_report_t *report,
awg_evtval_err_t code, uint32_t idx,
uint32_t observed, uint32_t expected_min)
{
if (!report)
return;

report->code = code;
report->failing_index = idx;
report->reason = awg_sched_validation_reason(code);
report->observed = observed;
report->expected_min = expected_min;
}

void awg_sched_validation_rules_default(awg_sched_validation_rules_t *rules)
{
if (!rules)
return;

memset(rules, 0, sizeof(*rules));
rules->min_delta_ticks = 1U;
rules->min_reinit_delta_ticks = AWG_SCHED_MIN_REINIT_DELTA_TICKS;
rules->delta_mode = AWG_SCHED_DELTA_MODE_STRICT;
rules->allowed_flags_mask = AWG_SCHED_FLAGS_ALLOWED_MASK;
rules->channel_mask = AWG_SCHED_CHANNEL_MASK_DEFAULT;
rules->tone_shift = 0U;
rules->tone_mask = AWG_SCHED_TONE_MASK_DEFAULT;
rules->freq_shift = 16U;
rules->freq_mask = AWG_SCHED_FREQ_MASK_DEFAULT;
rules->scale_shift = 0U;
rules->scale_mask = AWG_SCHED_SCALE_MASK_DEFAULT;
rules->phase_shift = 0U;
rules->phase_mask = AWG_SCHED_PHASE_MASK_DEFAULT;
}

static int awg_sched_reg_write(uint32_t offset, uint32_t val)
{
if (!g_awg_sched.configured)
return -ENODEV;

return no_os_axi_io_write(g_awg_sched.cfg.base_addr, offset, val);
}

static int awg_sched_reg_read(uint32_t offset, uint32_t *val)
{
if (!val)
return -EINVAL;

if (!g_awg_sched.configured)
return -ENODEV;

return no_os_axi_io_read(g_awg_sched.cfg.base_addr, offset, val);
}

static int awg_sched_read_time_now(uint64_t *time_now)
{
	uint32_t time_lo;
	uint32_t time_hi;
	int ret;

	if (!time_now)
		return -EINVAL;

	ret = awg_sched_reg_read(AWG_SCHED_REG_TIME_NOW_LO, &time_lo);
	if (ret)
		return ret;

	ret = awg_sched_reg_read(AWG_SCHED_REG_TIME_NOW_HI, &time_hi);
	if (ret)
		return ret;

	*time_now = ((uint64_t)time_hi << 32) | (uint64_t)time_lo;
	return 0;
}

static uint32_t awg_sched_pack_ch_flags(const awg_event_v1_t *event)
{
	return awg_event_v1_mmio_word2(event->channel, event->flags);
}

#if AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY
static int awg_sched_verify_loaded_events_readback(const awg_event_v1_t *events,
uint32_t count)
{
uint32_t i;
uint32_t wdata0_rb;
uint32_t wdata1_rb;
uint32_t wdata2_rb;
uint32_t wdata3_rb;
uint32_t wdata4_rb;
uint32_t wdata5_rb;
uint32_t wdata6_rb;
int ret;

if (count == 0U)
return 0;

for (i = 0U; i < count; i++) {
const awg_event_v1_t *e = &events[i];
uint32_t exp_wdata0 = (uint32_t)(e->timestamp_ticks);
uint32_t exp_wdata1 = (uint32_t)(e->timestamp_ticks >> 32);
uint32_t exp_wdata2 = awg_sched_pack_ch_flags(e);
uint32_t exp_wdata3 = e->payload.word0;
uint32_t exp_wdata4 = e->payload.word1;
uint32_t exp_wdata5 = e->payload.word2;
uint32_t exp_wdata6 = e->payload.word3;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WADDR, i);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA0, &wdata0_rb);
if (ret)
return ret;
ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA1, &wdata1_rb);
if (ret)
return ret;
ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA2, &wdata2_rb);
if (ret)
return ret;
ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA3, &wdata3_rb);
if (ret)
return ret;
ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA4, &wdata4_rb);
if (ret)
return ret;
ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA5, &wdata5_rb);
if (ret)
return ret;
ret = awg_sched_reg_read(AWG_SCHED_REG_EVT_WDATA6, &wdata6_rb);
if (ret)
return ret;

if ((wdata0_rb != exp_wdata0) ||
    (wdata1_rb != exp_wdata1) ||
    (wdata2_rb != exp_wdata2) ||
    (wdata3_rb != exp_wdata3) ||
    (wdata4_rb != exp_wdata4) ||
    (wdata5_rb != exp_wdata5) ||
    (wdata6_rb != exp_wdata6)) {
AWG_LOG("[AWG-SCHED] readback mismatch idx=%lu "
"exp:{w0=0x%08lX w1=0x%08lX w2=0x%08lX w3=0x%08lX w4=0x%08lX w5=0x%08lX w6=0x%08lX} "
"got:{w0=0x%08lX w1=0x%08lX w2=0x%08lX w3=0x%08lX w4=0x%08lX w5=0x%08lX w6=0x%08lX}\n\r",
(unsigned long)i,
(unsigned long)exp_wdata0,
(unsigned long)exp_wdata1,
(unsigned long)exp_wdata2,
(unsigned long)exp_wdata3,
(unsigned long)exp_wdata4,
(unsigned long)exp_wdata5,
(unsigned long)exp_wdata6,
(unsigned long)wdata0_rb,
(unsigned long)wdata1_rb,
(unsigned long)wdata2_rb,
(unsigned long)wdata3_rb,
(unsigned long)wdata4_rb,
(unsigned long)wdata5_rb,
(unsigned long)wdata6_rb);
return -EIO;
}
}

return 0;
}
#endif /* AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY */

int awg_sched_reset(void)
{
uint32_t status;
uint32_t timeout_ms;
int ret;

if (!g_awg_sched.configured)
return -ENODEV;

AWG_LOG("%s", "[AWG-SCHED] reset step=write_ctrl_reset\n\r");
ret = awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_RESET_SOFT);
if (ret)
return ret;
AWG_LOG("%s", "[AWG-SCHED] reset step=write_ctrl_reset_done\n\r");

/*
 * RESET_SOFT crosses into sched_clk.  EVENT_COUNT writes are accepted only
 * after the AXI status shadow reports idle, so wait before clearing it.
 */
timeout_ms = g_awg_sched.cfg.done_timeout_ms;
if (timeout_ms == 0U)
timeout_ms = 100U;
while (true) {
ret = awg_sched_reg_read(AWG_SCHED_REG_STATUS, &status);
if (ret)
return ret;
if ((status & AWG_SCHED_STATUS_STATE_MASK) == 0U)
break;
if (timeout_ms == 0U)
return -ETIMEDOUT;
no_os_mdelay(1U);
timeout_ms--;
}

g_awg_sched.loaded_events = 0;
g_awg_sched.events_validated = false;
g_awg_sched.mode_locked = false;
g_awg_sched.error_fallback_logged = false;
g_awg_sched_irq_seq = 0U;
AWG_LOG("%s", "[AWG-SCHED] reset step=write_evt_count_zero\n\r");
ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_COUNT, 0U);
if (ret)
return ret;
AWG_LOG("%s", "[AWG-SCHED] reset step=write_evt_count_zero_done\n\r");

/* Clear any AXI-domain IRQ bits left by the final pre-reset snapshot. */
ret = awg_sched_reg_write(AWG_SCHED_REG_IRQ_STATUS, AWG_SCHED_IRQ_ALL);
if (ret)
return ret;

/* Soft reset leaves the AXI mode configuration intact; mirror it locally. */
ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_CTRL, &status);
if (ret)
return ret;
g_awg_sched.mode_stream_cfg =
	(status & AWG_SCHED_STREAM_CTRL_MODE) != 0U;
g_awg_sched.dma_mode_cfg =
	(status & AWG_SCHED_STREAM_CTRL_DMA_MODE) != 0U;
return 0;
}

int awg_sched_soft_reset(void)
{
	return awg_sched_reset();
}

int awg_sched_config(const awg_sched_cfg_t *cfg)
{
uint32_t ip_id;
uint32_t ip_ver;
uint32_t ip_caps;
uint32_t event_depth_log2;
uint32_t stream_depth;
uint32_t major;
int ret;

if (!cfg)
return -EINVAL;

if (!cfg->base_addr || !cfg->max_events)
return -EINVAL;

/*
 * Populate state early so that awg_sched_reg_read() works before
 * configured=true (it checks configured internally).  We set
 * configured=true temporarily, then clear it on any error path.
 */
memset(&g_awg_sched, 0, sizeof(g_awg_sched));
g_awg_sched.cfg = *cfg;
g_awg_sched.configured = true;

/* Verify IP identity before touching any other register. */
AWG_LOG("%s", "[AWG-SCHED] config step=read_ip_id\n\r");
ret = awg_sched_reg_read(AWG_SCHED_REG_IP_ID, &ip_id);
if (ret)
goto fail;
AWG_LOG("[AWG-SCHED] config ip_id=0x%08lX\n\r", (unsigned long)ip_id);

if (ip_id != AWG_SCHED_IP_ID) {
AWG_LOG("[AWG-SCHED] IP_ID mismatch: expected=0x%08lX observed=0x%08lX\n\r",
(unsigned long)AWG_SCHED_IP_ID,
(unsigned long)ip_id);
ret = -ENODEV;
goto fail;
}

AWG_LOG("%s", "[AWG-SCHED] config step=read_ip_version\n\r");
ret = awg_sched_reg_read(AWG_SCHED_REG_IP_VERSION, &ip_ver);
if (ret)
goto fail;
AWG_LOG("[AWG-SCHED] config ip_version=0x%08lX\n\r", (unsigned long)ip_ver);

major = (ip_ver >> 16) & 0xFFFFU;
if (ip_ver != AWG_SCHED_IP_VERSION) {
AWG_LOG("[AWG-SCHED] IP_VERSION mismatch: expected=0x%08lX observed=0x%08lX major=%lu\n\r",
(unsigned long)AWG_SCHED_IP_VERSION,
(unsigned long)ip_ver,
(unsigned long)major);
ret = -ENOTSUP;
goto fail;
}

AWG_LOG("%s", "[AWG-SCHED] config step=read_ip_caps\n\r");
ret = awg_sched_reg_read(AWG_SCHED_REG_IP_CAPS, &ip_caps);
if (ret)
goto fail;
AWG_LOG("[AWG-SCHED] config ip_caps=0x%08lX\n\r", (unsigned long)ip_caps);

event_depth_log2 = AWG_SCHED_CAPS_EVT_DEPTH_LOG2(ip_caps);
if (event_depth_log2 == 0U || event_depth_log2 >= 32U) {
ret = -ENOTSUP;
goto fail;
}

g_awg_sched.hw_event_depth  = 1U << event_depth_log2;
g_awg_sched.hw_payload_bits = (uint8_t)AWG_SCHED_CAPS_PAYLOAD_BITS(ip_caps);
g_awg_sched.hw_ts_bits      = (uint8_t)AWG_SCHED_CAPS_TS_BITS(ip_caps);

if (g_awg_sched.hw_payload_bits != 128U || g_awg_sched.hw_ts_bits != 64U ||
    cfg->max_events > g_awg_sched.hw_event_depth) {
AWG_LOG("[AWG-SCHED] unsupported capabilities depth=%lu payload_bits=%u "
"timestamp_bits=%u requested_max=%lu\n\r",
(unsigned long)g_awg_sched.hw_event_depth,
(unsigned)g_awg_sched.hw_payload_bits,
(unsigned)g_awg_sched.hw_ts_bits,
(unsigned long)cfg->max_events);
ret = -ENOTSUP;
goto fail;
}

ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_DEPTH, &stream_depth);
if (ret)
goto fail;
if (stream_depth == 0U) {
AWG_LOG("%s", "[AWG-SCHED] invalid STREAM_DEPTH=0\n\r");
ret = -ENODEV;
goto fail;
}
g_awg_sched.hw_stream_depth = stream_depth;
AWG_LOG("[AWG-SCHED] config stream_depth=%lu event_depth=%lu\n\r",
(unsigned long)stream_depth,
(unsigned long)g_awg_sched.hw_event_depth);

AWG_LOG("%s", "[AWG-SCHED] config step=reset\n\r");
return awg_sched_reset();

fail:
memset(&g_awg_sched, 0, sizeof(g_awg_sched));
return ret;
}

static int awg_sched_set_mode_bits(bool stream_enable, bool dma_enable)
{
	awg_sched_status_t status;
	uint32_t value = 0U;
	int ret;

	if (!g_awg_sched.configured)
		return -ENODEV;
	if (dma_enable && !stream_enable)
		return -EINVAL;

	ret = awg_sched_get_status(&status);
	if (ret)
		return ret;
	if (g_awg_sched.mode_locked || status.armed || status.running)
		return -EBUSY;

	if (stream_enable)
		value |= AWG_SCHED_STREAM_CTRL_MODE;
	if (dma_enable)
		value |= AWG_SCHED_STREAM_CTRL_DMA_MODE;

	/* Never echo the read-only EOF or W1C OVERFLOW bits into this write. */
	ret = awg_sched_reg_write(AWG_SCHED_REG_STREAM_CTRL, value);
	if (ret)
		return ret;

	g_awg_sched.mode_stream_cfg = stream_enable;
	g_awg_sched.dma_mode_cfg = dma_enable;
	return 0;
}

int awg_sched_set_stream_mode(bool enable)
{
	return awg_sched_set_mode_bits(enable,
				       enable && g_awg_sched.dma_mode_cfg);
}

int awg_sched_set_dma_mode(bool enable)
{
	return awg_sched_set_mode_bits(g_awg_sched.mode_stream_cfg, enable);
}

int awg_sched_set_low_wmark(uint32_t events)
{
	if (!g_awg_sched.configured)
		return -ENODEV;
	if (events > g_awg_sched.hw_stream_depth)
		return -EINVAL;

	return awg_sched_reg_write(AWG_SCHED_REG_LOW_WMARK, events);
}

int awg_sched_irq_enable(uint32_t mask)
{
	if (!g_awg_sched.configured)
		return -ENODEV;

	return awg_sched_reg_write(AWG_SCHED_REG_IRQ_ENABLE,
				   mask & AWG_SCHED_IRQ_ALL);
}

int awg_sched_irq_clear(uint32_t mask)
{
	if (!g_awg_sched.configured)
		return -ENODEV;

	return awg_sched_reg_write(AWG_SCHED_REG_IRQ_STATUS,
				   mask & AWG_SCHED_IRQ_ALL);
}

uint32_t awg_sched_stream_depth(void)
{
	return g_awg_sched.configured ? g_awg_sched.hw_stream_depth : 0U;
}

int awg_sched_verify_events(const awg_event_v1_t *events, uint32_t count)
{
awg_sched_validation_report_t report;
int ret;

ret = awg_sched_validate_events(events, count, NULL, &report);
if (ret != 0)
return ret;

#if AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY
if (g_awg_sched.loaded_events == count) {
ret = awg_sched_verify_loaded_events_readback(events, count);
if (ret != 0)
return ret;
}
#endif

g_awg_sched.events_validated = true;
return 0;
}

int awg_sched_validate_events(const awg_event_v1_t *events, uint32_t count,
      const awg_sched_validation_rules_t *rules,
      awg_sched_validation_report_t *report)
{
awg_sched_validation_rules_t active_rules;
uint64_t last_reinit_ts = 0U;
bool has_last_reinit = false;
uint32_t i;
uint32_t max_events;
uint32_t observed;
uint64_t delta;

awg_sched_validation_report_set(report, AWG_EVTVAL_OK, 0U, 0U, 0U);

if (!g_awg_sched.configured)
goto fail_not_configured;

if ((count > 0U) && !events)
goto fail_null_events;

awg_sched_validation_rules_default(&active_rules);
if (rules)
active_rules = *rules;

max_events = g_awg_sched.cfg.max_events;
if (count > max_events)
goto fail_too_many_events;

for (i = 1U; i < count; i++) {
if (events[i].timestamp_ticks < events[i - 1U].timestamp_ticks) {
awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_TS_NOT_MONOTONIC,
i,
(uint32_t)events[i].timestamp_ticks,
(uint32_t)events[i - 1U].timestamp_ticks);
goto fail;
}
}

	for (i = 0U; i < count; i++) {
		const awg_event_v1_t *event = &events[i];
		uint32_t tone_val = (event->payload.word0 >> active_rules.tone_shift) & 0xFFFFU;
		uint32_t freq_val = (event->payload.word0 >> active_rules.freq_shift) & 0xFFFFU;
		uint32_t scale_val = (event->payload.word1 >> active_rules.scale_shift) & 0xFFFFU;
		uint32_t phase_val = (event->payload.word2 >> active_rules.phase_shift) & 0xFFFFU;

if ((event->flags & ~active_rules.allowed_flags_mask) != 0U) {
awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_RESERVED_FLAGS,
i, event->flags,
active_rules.allowed_flags_mask);
goto fail;
}

if ((event->channel & ~active_rules.channel_mask) != 0U) {
awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_CHANNEL_WIDTH,
i, event->channel,
active_rules.channel_mask);
goto fail;
}

		if ((tone_val & ~active_rules.tone_mask) != 0U) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_TONE_WIDTH, i,
							tone_val,
							active_rules.tone_mask);
			goto fail;
		}

		if ((freq_val & ~active_rules.freq_mask) != 0U) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_FREQ_WIDTH, i,
							freq_val,
							active_rules.freq_mask);
			goto fail;
		}

		if ((scale_val & ~active_rules.scale_mask) != 0U) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_SCALE_WIDTH, i,
							scale_val,
							active_rules.scale_mask);
			goto fail;
		}

		if ((phase_val & ~active_rules.phase_mask) != 0U) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_PHASE_WIDTH, i,
							phase_val,
							active_rules.phase_mask);
			goto fail;
		}

		if ((event->flags & AWG_SCHED_FLAG_PHASE_REINIT) != 0U) {
			if (has_last_reinit) {
				if (event->timestamp_ticks < last_reinit_ts) {
					awg_sched_validation_report_set(report,
									AWG_EVTVAL_ERR_REINIT_SPACING,
									i,
									0U,
									active_rules.min_reinit_delta_ticks);
					goto fail;
				}

				delta = event->timestamp_ticks - last_reinit_ts;
				if (delta < (uint64_t)active_rules.min_reinit_delta_ticks) {
					awg_sched_validation_report_set(report,
									AWG_EVTVAL_ERR_REINIT_SPACING,
									i,
									(uint32_t)delta,
									active_rules.min_reinit_delta_ticks);
					goto fail;
				}
			}

			last_reinit_ts = event->timestamp_ticks;
			has_last_reinit = true;
		}
}

if (count > 1U) {
for (i = 1U; i < count; i++) {
delta = events[i].timestamp_ticks - events[i - 1U].timestamp_ticks;
if (delta < (uint64_t)active_rules.min_delta_ticks) {
if ((active_rules.delta_mode ==
     AWG_SCHED_DELTA_MODE_ALLOW_ZERO_ON_SAME_CHANNEL) &&
    (events[i].channel == events[i - 1U].channel) &&
    (delta == 0U))
continue;

awg_sched_validation_report_set(report,
AWG_EVTVAL_ERR_TS_DELTA_TOO_SMALL,
i, (uint32_t)delta,
active_rules.min_delta_ticks);
goto fail;
}
}
}

return 0;

fail_not_configured:
awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_NOT_CONFIGURED, 0U, 0U, 0U);
AWG_LOG("[AWG-SCHED] validation failed @idx=0 code=%d reason=%s\n\r",
(int)AWG_EVTVAL_ERR_NOT_CONFIGURED,
awg_sched_validation_reason(AWG_EVTVAL_ERR_NOT_CONFIGURED));
return -ENODEV;
fail_null_events:
awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_NULL_EVENTS, 0U, 0U, 0U);
AWG_LOG("[AWG-SCHED] validation failed @idx=0 code=%d reason=%s\n\r",
(int)AWG_EVTVAL_ERR_NULL_EVENTS,
awg_sched_validation_reason(AWG_EVTVAL_ERR_NULL_EVENTS));
return -EINVAL;
fail_too_many_events:
observed = count;
awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_TOO_MANY_EVENTS,
0U, observed, max_events);
AWG_LOG("[AWG-SCHED] validation failed @idx=0 code=%d reason=%s observed=%lu max=%lu\n\r",
(int)AWG_EVTVAL_ERR_TOO_MANY_EVENTS,
awg_sched_validation_reason(AWG_EVTVAL_ERR_TOO_MANY_EVENTS),
(unsigned long)observed, (unsigned long)max_events);
return -E2BIG;
fail:
if (report) {
AWG_LOG("[AWG-SCHED] validation failed @idx=%lu code=%d reason=%s observed=0x%08lX expected_min=0x%08lX\n\r",
(unsigned long)report->failing_index, (int)report->code,
report->reason, (unsigned long)report->observed,
(unsigned long)report->expected_min);
}
return -EINVAL;
}

static int awg_sched_write_one_event(const awg_event_v1_t *e,
				     bool check_stream_overflow)
{
int ret;
uint32_t stream_ctrl;

if (!e)
return -EINVAL;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA0, (uint32_t)(e->timestamp_ticks));
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA1, (uint32_t)(e->timestamp_ticks >> 32));
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA2, awg_sched_pack_ch_flags(e));
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA3, e->payload.word0);
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA4, e->payload.word1);
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA5, e->payload.word2);
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WDATA6, e->payload.word3);
if (ret)
return ret;

ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WCTRL, AWG_SCHED_EVT_WCTRL_PUSH);
if (ret)
return ret;

if (check_stream_overflow) {
ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_CTRL, &stream_ctrl);
if (ret)
return ret;

if ((stream_ctrl & AWG_SCHED_STREAM_CTRL_OVERFLOW) != 0U)
return -EAGAIN;
}

/*
 * Current HDL exposes no AXI-visible event-write ack.  Give the sched_clk
 * domain a short window to capture event_wr_addr_cfg/event_wr_data_cfg after
 * toggling EVT_WCTRL before software overwrites them for the next event.
 */
no_os_udelay(2U);
return 0;
}

int awg_sched_write_event_mmio(uint32_t event_index,
			       const awg_event_v1_t *event)
{
	uint32_t free_space;
	int ret;

	if (!event)
		return -EINVAL;
	if (!g_awg_sched.configured)
		return -ENODEV;

	if (g_awg_sched.mode_stream_cfg) {
		if (g_awg_sched.dma_mode_cfg)
			return -EBUSY;

		/* FREE_SPACE is the scheduler FIFO authority in software mode. */
		ret = awg_sched_reg_read(AWG_SCHED_REG_FREE_SPACE, &free_space);
		if (ret)
			return ret;
		if (free_space > g_awg_sched.hw_stream_depth)
			return -EIO;
		if (free_space == 0U)
			return -EAGAIN;

		return awg_sched_write_one_event(event, true);
	}

	if (event_index >= g_awg_sched.hw_event_depth)
		return -ERANGE;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVT_WADDR, event_index);
	if (ret)
		return ret;

	return awg_sched_write_one_event(event, false);
}

/*
 * Poll STATUS.running until clear (or timeout).  Called after awg_sched_stop()
 * to ensure the hardware has drained before overwriting event memory.
 */
static int awg_sched_wait_stopped(uint32_t timeout_ms)
{
uint32_t raw;
uint32_t ms_left = timeout_ms;
int ret;

while (true) {
ret = awg_sched_reg_read(AWG_SCHED_REG_STATUS, &raw);
if (ret)
return ret;

if (!(raw & AWG_SCHED_STATUS_RUNNING))
return 0;

if (ms_left == 0U)
return -ETIMEDOUT;

no_os_mdelay(1U);
ms_left--;
}
}

int awg_sched_load_events(const awg_event_v1_t *events, uint32_t count)
{
awg_sched_validation_report_t report;
uint32_t i;
int ret;

g_awg_sched.events_validated = false;

/* Validate event data before touching hardware. */
ret = awg_sched_validate_events(events, count, NULL, &report);
if (ret)
return ret;

ret = awg_sched_stop();
if (ret)
return ret;

/* Wait for running to deassert before writing to event memory. */
ret = awg_sched_wait_stopped(g_awg_sched.cfg.done_timeout_ms);
if (ret) {
AWG_LOG("%s", "[AWG-SCHED] load_events: engine still running after stop request\n\r");
return ret;
}

ret = awg_sched_set_stream_mode(false);
if (ret)
return ret;

for (i = 0; i < count; i++) {
ret = awg_sched_write_event_mmio(i, &events[i]);
if (ret)
return ret;
}

ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_COUNT, count);
if (ret)
return ret;

/*
 * HDL captures event_count_s2 exactly at arm_edge in the sched_clk domain.
 * Give the 2-FF CDC path time to observe the new EVT_COUNT value before the
 * next ARM request, otherwise the engine can latch event_count_sched=0 and
 * remain stuck in ARMED when RUN is asserted.
 */
no_os_udelay(AWG_SCHED_EVENT_COUNT_SETTLE_US);

g_awg_sched.loaded_events = count;

#if AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY
ret = awg_sched_verify_loaded_events_readback(events, count);
if (ret)
return ret;
#endif

g_awg_sched.events_validated = true;
return 0;
}

int awg_sched_arm(void)
{
int ret;

if (!g_awg_sched.configured)
return -ENODEV;

if (!g_awg_sched.mode_stream_cfg) {
if (!g_awg_sched.loaded_events || !g_awg_sched.events_validated)
return -EINVAL;
}

if (!g_awg_sched.mode_stream_cfg) {
ret = awg_sched_irq_enable(AWG_SCHED_IRQ_ARM_MASK);
if (ret)
return ret;
}

ret = awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_ARM);
if (!ret)
g_awg_sched.mode_locked = true;
return ret;
}

int awg_sched_run(void)
{
	if (!g_awg_sched.configured)
		return -ENODEV;
	if (!g_awg_sched.mode_locked)
		return -EINVAL;

	return awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_RUN);
}

int awg_sched_start(void)
{
	awg_sched_status_t status;
	uint32_t wait_us;
	uint32_t attempt;
	int ret;

	ret = awg_sched_arm();
	if (ret)
		return ret;

	/*
	 * Do not combine ARM and RUN in the same write, and do not assume one
	 * AXI read round-trip is enough for the sched_clk domain to reach
	 * ENGINE_ARMED.  Wait until armed is observable before strobing RUN.
	 */
	for (wait_us = 0U; wait_us < AWG_SCHED_START_ARM_WAIT_US; wait_us++) {
		ret = awg_sched_get_status(&status);
		if (ret)
			return ret;

		if (status.armed && !status.running)
			break;

		no_os_udelay(1U);
	}

	if (!(status.armed && !status.running)) {
		AWG_LOG("[AWG-SCHED] start failed to observe armed state "
			"status=0x%08lX\n\r",
			(unsigned long)status.hw_status_word);
		return -ETIMEDOUT;
	}

	for (attempt = 0U; attempt < AWG_SCHED_START_RUN_RETRIES; attempt++) {
		ret = awg_sched_run();
		if (ret)
			return ret;

		for (wait_us = 0U; wait_us < AWG_SCHED_START_RUN_WAIT_US; wait_us++) {
			ret = awg_sched_get_status(&status);
			if (ret)
				return ret;

			if (status.running || status.done || status.error)
				return 0;

			no_os_udelay(1U);
		}
	}

	AWG_LOG("[AWG-SCHED] start failed to observe running state "
		"status=0x%08lX current=%lu commit=%lu\n\r",
		(unsigned long)status.hw_status_word,
		(unsigned long)status.current_event,
		(unsigned long)status.commit_count);
	return -ETIMEDOUT;
}

int awg_sched_stop(void)
{
int ret;

if (!g_awg_sched.configured)
return -ENODEV;

ret = awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_STOP);
if (!ret)
g_awg_sched.mode_locked = false;
return ret;
}

int awg_sched_get_status(awg_sched_status_t *status)
{
uint32_t raw_status;
uint32_t cur_event;
uint32_t time_lo;
uint32_t time_hi;
uint32_t last_lo;
uint32_t last_hi;
uint32_t commit_cnt;
uint32_t reinit_cnt;
uint32_t reinit_rej;
uint32_t irq_stat;
uint32_t err_reg;
uint8_t err_code;
bool err_fallback;
int ret;

if (!status)
return -EINVAL;

if (!g_awg_sched.configured)
return -ENODEV;

ret = awg_sched_reg_read(AWG_SCHED_REG_STATUS, &raw_status);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_CUR_EVENT, &cur_event);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_TIME_NOW_LO, &time_lo);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_TIME_NOW_HI, &time_hi);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_LAST_EXEC_LO, &last_lo);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_LAST_EXEC_HI, &last_hi);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_COMMIT_COUNT, &commit_cnt);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_REINIT_COUNT, &reinit_cnt);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_REINIT_REJECT, &reinit_rej);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_IRQ_STATUS, &irq_stat);
if (ret)
return ret;

ret = awg_sched_reg_read(AWG_SCHED_REG_ERR_REG, &err_reg);
if (ret)
return ret;

/*
 * ERR_REG and STATUS[15:8] are the documented ABI.  The current Phase-E RTL
 * assigns a 40-bit concatenation to a 32-bit status snapshot, which places
 * the error byte in STATUS[23:16] and leaves ERR_REG zero.  Use that byte only
 * when ERROR is asserted and both documented locations are empty.
 */
err_code = (uint8_t)(err_reg & AWG_SCHED_STATUS_ERR_CODE_MASK);
if (err_code == AWG_SCHED_ERR_NONE)
err_code = (uint8_t)((raw_status >> AWG_SCHED_STATUS_ERR_CODE_SHIFT) &
			     AWG_SCHED_STATUS_ERR_CODE_MASK);
err_fallback = false;
if ((raw_status & AWG_SCHED_STATUS_ERROR) != 0U &&
    err_code == AWG_SCHED_ERR_NONE) {
uint8_t rtl_err = (uint8_t)((raw_status >> AWG_SCHED_STATUS_RTL_ERR_SHIFT) &
				    AWG_SCHED_STATUS_ERR_CODE_MASK);

if (rtl_err != AWG_SCHED_ERR_NONE) {
err_code = rtl_err;
err_fallback = true;
if (!g_awg_sched.error_fallback_logged) {
AWG_LOG("[AWG-SCHED] applying Phase-E RTL error-code fallback "
"STATUS[23:16]=0x%02X raw=0x%08lX\n\r",
(unsigned)rtl_err, (unsigned long)raw_status);
g_awg_sched.error_fallback_logged = true;
}
}
}

memset(status, 0, sizeof(*status));
status->configured = true;
status->idle = (raw_status & AWG_SCHED_STATUS_STATE_MASK) == 0U;
status->armed   = (raw_status & AWG_SCHED_STATUS_ARMED)   != 0U;
status->running = (raw_status & AWG_SCHED_STATUS_RUNNING) != 0U;
status->done    = (raw_status & AWG_SCHED_STATUS_DONE)    != 0U;
status->error   = (raw_status & AWG_SCHED_STATUS_ERROR)   != 0U;
status->err_code_from_rtl_fallback = err_fallback;
status->err_code = err_code;
status->loaded_events        = g_awg_sched.loaded_events;
status->current_event        = cur_event;
status->time_now             = ((uint64_t)time_hi << 32) | time_lo;
status->last_exec            = ((uint64_t)last_hi << 32) | last_lo;
status->commit_count         = commit_cnt;
status->reinit_count         = reinit_cnt;
status->reinit_reject_count  = reinit_rej;
status->irq_status_latched   = irq_stat;
status->hw_status_word       = raw_status;

return 0;
}

int awg_sched_read_status(awg_sched_status_t *status)
{
	return awg_sched_get_status(status);
}

int awg_sched_read_stream_snapshot(awg_sched_stream_snapshot_t *snapshot)
{
	int ret;

	if (!snapshot)
		return -EINVAL;
	if (!g_awg_sched.configured)
		return -ENODEV;

	memset(snapshot, 0, sizeof(*snapshot));
	ret = awg_sched_reg_read(AWG_SCHED_REG_STATUS, &snapshot->status);
	if (ret)
		return ret;
	ret = awg_sched_reg_read(AWG_SCHED_REG_ERR_REG, &snapshot->err_reg);
	if (ret)
		return ret;
	ret = awg_sched_reg_read(AWG_SCHED_REG_IRQ_STATUS,
				 &snapshot->irq_status);
	if (ret)
		return ret;
	ret = awg_sched_reg_read(AWG_SCHED_REG_OCCUPANCY,
				 &snapshot->occupancy);
	if (ret)
		return ret;
	ret = awg_sched_reg_read(AWG_SCHED_REG_FREE_SPACE,
				 &snapshot->free_space);
	if (ret)
		return ret;
	ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_CTRL,
				 &snapshot->stream_ctrl);
	if (ret)
		return ret;
	ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_PUSHES,
				 &snapshot->stream_pushes);
	if (ret)
		return ret;

	return awg_sched_reg_read(AWG_SCHED_REG_STREAM_STALLS,
				  &snapshot->stream_stalls);
}

int awg_sched_wait_done(uint32_t timeout_ms, awg_sched_status_t *final_status)
{
awg_sched_status_t status;
uint32_t wait_ms;
#if FMCDAC_AWG_SCHED_USE_IRQ
uint32_t irq_status;
uint32_t last_irq_seq;
#endif
int ret;

if (!g_awg_sched.configured)
return -ENODEV;

wait_ms = timeout_ms;
if (wait_ms == 0U)
wait_ms = g_awg_sched.cfg.done_timeout_ms;

#if FMCDAC_AWG_SCHED_USE_IRQ
last_irq_seq = g_awg_sched_irq_seq;
#endif

while (true) {
#if FMCDAC_AWG_SCHED_USE_IRQ
while (g_awg_sched_irq_seq == last_irq_seq) {
if (wait_ms == 0U)
return -ETIMEDOUT;

awg_sched_irq_wait_hook(wait_ms);
no_os_mdelay(1U);
wait_ms--;
}
last_irq_seq = g_awg_sched_irq_seq;
#endif

ret = awg_sched_get_status(&status);
if (ret)
return ret;

/*
 * Some HDL revisions can miss the terminal DONE snapshot because the status
 * toggle is updated in FIRE and again in the immediately following ADVANCE
 * cycle. If commit_count has reached the loaded event count without an error,
 * treat the run as complete even if the done/running bits are stale.
 */
if (!status.error &&
    status.loaded_events > 0U &&
    status.commit_count >= status.loaded_events) {
if (final_status)
*final_status = status;
return 0;
}

if (status.done || status.error) {
#if FMCDAC_AWG_SCHED_USE_IRQ
ret = awg_sched_reg_read(AWG_SCHED_REG_IRQ_STATUS, &irq_status);
if (ret)
return ret;
status.irq_status_latched = irq_status;
ret = awg_sched_reg_write(AWG_SCHED_REG_IRQ_STATUS, irq_status);
if (ret)
return ret;
#endif

if (final_status)
*final_status = status;
return status.error ? -EIO : 0;
}

#if !FMCDAC_AWG_SCHED_USE_IRQ
if (wait_ms == 0U)
return -ETIMEDOUT;

no_os_mdelay(1U);
wait_ms--;
#endif
}
}

int awg_sched_set_epoch(void)
{
	uint64_t time_before;
	uint64_t time_now;
	uint32_t poll_us;
	int ret;

	if (!g_awg_sched.configured)
		return -ENODEV;

	ret = awg_sched_read_time_now(&time_before);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_TIME_RELOAD_LO, 0U);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_TIME_RELOAD_HI, 0U);
	if (ret)
		return ret;

	AWG_LOG("%s", "[AWG-SCHED] set_epoch step=load_now\n\r");
	ret = awg_sched_reg_write(AWG_SCHED_REG_TIME_RELOAD_CTRL,
				  AWG_SCHED_TIME_RELOAD_LOAD_NOW);
	if (ret)
		return ret;

	for (poll_us = 0U; poll_us < 1000U; poll_us++) {
		ret = awg_sched_read_time_now(&time_now);
		if (ret)
			return ret;

		if ((time_now <= AWG_SCHED_TIME_EPOCH_NEAR_ZERO) ||
		    (time_now < time_before)) {
			AWG_LOG("[SCHED-ARTIFACT] set_epoch before=0x%08lX_%08lX now=0x%08lX_%08lX\n\r",
				(unsigned long)(time_before >> 32),
				(unsigned long)time_before,
				(unsigned long)(time_now >> 32),
				(unsigned long)time_now);
			return 0;
		}

		no_os_udelay(1U);
	}

	return -ETIMEDOUT;
}

void awg_sched_irq_signal(void)
{
	uint32_t irq_status;

	g_awg_sched_irq_seq++;

#if FMCDAC_AWG_SCHED_STREAM
	if (g_awg_sched.configured &&
	    awg_sched_reg_read(AWG_SCHED_REG_IRQ_STATUS, &irq_status) == 0)
		awg_sched_stream_irq_handler(irq_status);
#else
	(void)irq_status;
#endif
}

#if FMCDAC_AWG_SCHED_STREAM

#define AWG_STREAM_DEFAULT_POLL_INTERVAL_US 1000U
#define AWG_STREAM_IRQ_ENABLE_MASK \
	(AWG_SCHED_IRQ_DONE | AWG_SCHED_IRQ_ERROR | \
	 AWG_SCHED_IRQ_SPACING_VIOLATION | AWG_SCHED_IRQ_UNDERRUN | \
	 AWG_SCHED_IRQ_LOW_WATERMARK | AWG_SCHED_IRQ_EMPTY_STALL)
#define AWG_STREAM_IRQ_HARD_ERROR_MASK \
	(AWG_SCHED_IRQ_ERROR | AWG_SCHED_IRQ_SPACING_VIOLATION | AWG_SCHED_IRQ_UNDERRUN)

static struct {
	bool open;
	bool producer_closed;
	bool hard_error;
	bool started;
	bool has_last_event;
	awg_stream_ring_t ring;
	awg_event_v1_t last_event;
	uint32_t stream_depth;
	uint32_t low_wmark_events;
	uint32_t refill_chunk_max;
	uint32_t poll_interval_us;
	volatile uint32_t refill_pending;
	volatile uint32_t error_pending;
	volatile uint32_t irq_latched;
	awg_sched_stream_snapshot_t last_error;
} g_awg_stream;

static uint32_t awg_stream_min_u32(uint32_t a, uint32_t b)
{
	return (a < b) ? a : b;
}

static int awg_stream_record_hard_error(int ret_code)
{
	int snapshot_ret;
	int recovery_ret;

	snapshot_ret = awg_sched_read_stream_snapshot(&g_awg_stream.last_error);
	if (snapshot_ret)
		return snapshot_ret;

	g_awg_stream.hard_error = true;
	g_awg_stream.producer_closed = true;
	/* STOP preserves FIFO contents; RESET_SOFT is the required recovery. */
	(void)awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_STOP);
	recovery_ret = awg_sched_reset();
	awg_stream_ring_reset(&g_awg_stream.ring);
	if (recovery_ret)
		return recovery_ret;

	return ret_code;
}

static int awg_stream_check_ip(uint32_t *stream_depth)
{
	uint32_t ip_id;
	uint32_t ip_ver;
	int ret;

	ret = awg_sched_reg_read(AWG_SCHED_REG_IP_ID, &ip_id);
	if (ret)
		return ret;

	if (ip_id != AWG_SCHED_IP_ID)
		return -ENODEV;

	ret = awg_sched_reg_read(AWG_SCHED_REG_IP_VERSION, &ip_ver);
	if (ret)
		return ret;

	if (ip_ver != AWG_SCHED_IP_VERSION)
		return -ENOTSUP;

	ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_DEPTH, stream_depth);
	if (ret)
		return ret;

	if (*stream_depth == 0U)
		return -EINVAL;

	return 0;
}

static int awg_stream_start_if_needed(void)
{
	awg_sched_status_t status;
	uint32_t wait_us;
	uint32_t attempt;
	int ret;

	if (g_awg_stream.started)
		return 0;

	ret = awg_sched_arm();
	if (ret)
		return ret;

	for (wait_us = 0U; wait_us < AWG_SCHED_START_ARM_WAIT_US; wait_us++) {
		ret = awg_sched_get_status(&status);
		if (ret)
			return ret;

		if (status.armed && !status.running)
			break;

		no_os_udelay(1U);
	}

	if (!(status.armed && !status.running))
		return -ETIMEDOUT;

	for (attempt = 0U; attempt < AWG_SCHED_START_RUN_RETRIES; attempt++) {
		ret = awg_sched_run();
		if (ret)
			return ret;

		for (wait_us = 0U; wait_us < AWG_SCHED_START_RUN_WAIT_US; wait_us++) {
			ret = awg_sched_get_status(&status);
			if (ret)
				return ret;

			if (status.running || status.done || status.error) {
				g_awg_stream.started = true;
				return status.error ? -EIO : 0;
			}

			no_os_udelay(1U);
		}
	}

	/*
	 * In stream mode with future timestamps, some HDL revisions remain in an
	 * armed/waiting state until the first event becomes due.  RUN has been
	 * strobed; treat armed-without-error as accepted and let later status
	 * polling surface missed deadlines or hard errors.
	 */
	if (status.armed && !status.error) {
		AWG_LOG("[AWG-STREAM] start accepted pending timestamp status=0x%08lX current=%lu commit=%lu\n\r",
			(unsigned long)status.hw_status_word,
			(unsigned long)status.current_event,
			(unsigned long)status.commit_count);
		g_awg_stream.started = true;
		return 0;
	}

	AWG_LOG("[AWG-STREAM] start failed status=0x%08lX current=%lu commit=%lu\n\r",
		(unsigned long)status.hw_status_word,
		(unsigned long)status.current_event,
		(unsigned long)status.commit_count);
	return -ETIMEDOUT;
}

int awg_sched_stream_open(const awg_sched_stream_cfg_t *cfg)
{
	awg_event_v1_t *ring;
	uint32_t capacity;
	uint32_t stream_depth;
	uint32_t low_wmark;
	uint32_t refill_chunk;
	uint32_t poll_interval;
	int ret;

	if (!cfg)
		return -EINVAL;

	if (!g_awg_sched.configured)
		return -ENODEV;

	ret = awg_stream_check_ip(&stream_depth);
	if (ret)
		return ret;

	if (cfg->staging_buffer) {
		ring = (awg_event_v1_t *)cfg->staging_buffer;
		capacity = cfg->staging_capacity;
		if (capacity == 0U)
			return -EINVAL;
	} else {
		ring = (awg_event_v1_t *)(uintptr_t)AWG_STREAM_DDR_BASEADDR;
		capacity = AWG_STREAM_DDR_SIZE_BYTES / sizeof(awg_event_v1_t);
	}

	if (!ring || capacity == 0U)
		return -EINVAL;

	low_wmark = cfg->low_wmark_events;
	if (low_wmark == 0U) {
		low_wmark = stream_depth / 4U;
		if (low_wmark == 0U)
			low_wmark = 1U;
	}
	if (low_wmark > stream_depth)
		low_wmark = stream_depth;

	refill_chunk = cfg->refill_chunk_max;
	if (refill_chunk == 0U) {
		refill_chunk = awg_stream_min_u32(128U, stream_depth / 4U);
		if (refill_chunk == 0U)
			refill_chunk = 1U;
	}
	if (refill_chunk > stream_depth)
		refill_chunk = stream_depth;

	poll_interval = cfg->poll_interval_us;
	if (poll_interval == 0U)
		poll_interval = AWG_STREAM_DEFAULT_POLL_INTERVAL_US;

	memset(&g_awg_stream, 0, sizeof(g_awg_stream));
	ret = awg_stream_ring_init(&g_awg_stream.ring, ring, capacity,
				   sizeof(awg_event_v1_t));
	if (ret)
		return ret;
	g_awg_stream.open = true;
	g_awg_stream.stream_depth = stream_depth;
	g_awg_stream.low_wmark_events = low_wmark;
	g_awg_stream.refill_chunk_max = refill_chunk;
	g_awg_stream.poll_interval_us = poll_interval;

	ret = awg_sched_reset();
	if (ret)
		goto fail;

	ret = awg_sched_set_mode_bits(true, cfg->use_dma);
	if (ret)
		goto fail;

	/* Clear a previous software-push overflow without disturbing mode bits. */
	ret = awg_sched_reg_write(AWG_SCHED_REG_STREAM_CTRL,
				  AWG_SCHED_STREAM_CTRL_MODE |
				  (cfg->use_dma ?
				   AWG_SCHED_STREAM_CTRL_DMA_MODE : 0U) |
				  AWG_SCHED_STREAM_CTRL_OVERFLOW);
	if (ret)
		goto fail;

	ret = awg_sched_set_low_wmark(low_wmark);
	if (ret)
		goto fail;

	ret = awg_sched_irq_clear(AWG_SCHED_IRQ_ALL);
	if (ret)
		goto fail;

	/* Physical IRQ output is gated by IRQ_ENABLE, not CTRL[8]. */
	ret = awg_sched_irq_enable(AWG_STREAM_IRQ_ENABLE_MASK);
	if (ret)
		goto fail;

	ret = awg_sched_set_epoch();
	if (ret)
		goto fail;

	return 0;

fail:
	memset(&g_awg_stream, 0, sizeof(g_awg_stream));
	return ret;
}

int awg_sched_stream_push(const awg_event_v1_t *ev, uint32_t n)
{
	int ret;

	if (!g_awg_stream.open)
		return -ENODEV;

	if (g_awg_stream.producer_closed)
		return -EPERM;

	if (g_awg_stream.hard_error)
		return -EIO;

	if (n == 0U)
		return awg_sched_stream_drain_step();

	if (!ev)
		return -EINVAL;

	if (awg_stream_ring_free(&g_awg_stream.ring) < n) {
		ret = awg_sched_stream_drain_step();
		if (ret)
			return ret;
	}

	if (awg_stream_ring_free(&g_awg_stream.ring) < n)
		return -EAGAIN;

	ret = awg_stream_ring_push(&g_awg_stream.ring, ev, n);
	if (ret)
		return (ret == -ENOSPC) ? -EAGAIN : ret;

	g_awg_stream.last_event = ev[n - 1U];
	g_awg_stream.has_last_event = true;

	return awg_sched_stream_drain_step();
}

int awg_sched_stream_drain_step(void)
{
	uint32_t raw_status;
	uint32_t irq_status;
	uint32_t stream_ctrl;
	uint32_t free_space;
	uint32_t to_move;
	uint32_t i;
	int ret;

	if (!g_awg_stream.open)
		return -ENODEV;

	if (g_awg_stream.hard_error)
		return -EIO;

	ret = awg_sched_reg_read(AWG_SCHED_REG_STATUS, &raw_status);
	if (ret)
		return ret;

	ret = awg_sched_reg_read(AWG_SCHED_REG_IRQ_STATUS, &irq_status);
	if (ret)
		return ret;

	if (((raw_status & AWG_SCHED_STATUS_ERROR) != 0U) ||
	    ((irq_status & AWG_STREAM_IRQ_HARD_ERROR_MASK) != 0U) ||
	    g_awg_stream.error_pending) {
		return awg_stream_record_hard_error(-EIO);
	}

	ret = awg_sched_reg_read(AWG_SCHED_REG_STREAM_CTRL, &stream_ctrl);
	if (ret)
		return ret;

	if ((stream_ctrl & AWG_SCHED_STREAM_CTRL_OVERFLOW) != 0U)
		return awg_stream_record_hard_error(-EAGAIN);

	ret = awg_sched_reg_read(AWG_SCHED_REG_FREE_SPACE, &free_space);
	if (ret)
		return ret;

	if (free_space > g_awg_stream.stream_depth)
		return awg_stream_record_hard_error(-EIO);

	/* DMA service owns the consumer index and advances it only after EOT. */
	if (g_awg_sched.dma_mode_cfg) {
		g_awg_stream.refill_pending =
			(awg_stream_ring_count(&g_awg_stream.ring) != 0U) ? 1U : 0U;
		return 0;
	}

	if (free_space == 0U ||
	    awg_stream_ring_count(&g_awg_stream.ring) == 0U) {
		g_awg_stream.refill_pending = 0U;
		return 0;
	}

	to_move = awg_stream_min_u32(free_space,
				     awg_stream_ring_count(&g_awg_stream.ring));
	to_move = awg_stream_min_u32(to_move, g_awg_stream.refill_chunk_max);

	for (i = 0U; i < to_move; i++) {
		const awg_event_v1_t *event =
			awg_stream_ring_consumer_const_ptr(&g_awg_stream.ring);

		ret = awg_sched_write_event_mmio(0U, event);
		if (ret == -EAGAIN)
			return awg_stream_record_hard_error(-EAGAIN);
		if (ret)
			return ret;

		ret = awg_stream_ring_consume(&g_awg_stream.ring, 1U);
		if (ret)
			return ret;
	}

	if (awg_stream_ring_count(&g_awg_stream.ring) == 0U ||
	    to_move >= free_space)
		g_awg_stream.refill_pending = 0U;

	if (to_move > 0U) {
		ret = awg_stream_start_if_needed();
		if (ret)
			return ret;
	}

	return 0;
}

int awg_sched_stream_close(bool send_eof)
{
	uint32_t count;
	uint32_t n;

	if (!g_awg_stream.open)
		return -ENODEV;

	if (g_awg_stream.hard_error)
		return -EIO;

	if (send_eof) {
		count = awg_stream_ring_count(&g_awg_stream.ring);
		if (count == 0U) {
			if (g_awg_stream.has_last_event &&
			    ((g_awg_stream.last_event.flags & AWG_SCHED_FLAG_EOF) != 0U))
				goto close_ready;

			/* EOF must belong to a caller-supplied event; never synthesize one. */
			return -EINVAL;
		}

		for (n = 0U; n < count; n++) {
			awg_event_v1_t *event =
				awg_stream_ring_at(&g_awg_stream.ring, n);

			event->flags &= (uint16_t)~AWG_SCHED_FLAG_EOF;
		}
		((awg_event_v1_t *)awg_stream_ring_last(&g_awg_stream.ring))->flags |=
			AWG_SCHED_FLAG_EOF;
		g_awg_stream.last_event =
			*(awg_event_v1_t *)awg_stream_ring_last(&g_awg_stream.ring);
	}

close_ready:
	g_awg_stream.producer_closed = true;
	return awg_sched_stream_drain_step();
}

void awg_sched_stream_irq_handler(uint32_t irq_status)
{
	uint32_t relevant;
	uint32_t ack_mask;

	if (!g_awg_sched.configured || !g_awg_stream.open)
		return;

	relevant = irq_status & AWG_SCHED_IRQ_ALL;
	if (relevant == 0U)
		return;

	g_awg_stream.irq_latched |= relevant;

	if ((relevant & (AWG_SCHED_IRQ_LOW_WATERMARK |
			AWG_SCHED_IRQ_EMPTY_STALL)) != 0U)
		g_awg_stream.refill_pending = 1U;

	if ((relevant & AWG_STREAM_IRQ_HARD_ERROR_MASK) != 0U)
		g_awg_stream.error_pending = 1U;

	ack_mask = relevant & ~AWG_STREAM_IRQ_HARD_ERROR_MASK;
	if (ack_mask != 0U)
		(void)awg_sched_irq_clear(ack_mask);
}

uint32_t awg_sched_stream_ddr_free_events(void)
{
	return g_awg_stream.open ?
	       awg_stream_ring_free(&g_awg_stream.ring) : 0U;
}

awg_stream_ring_t *awg_sched_stream_ring_get(void)
{
	return g_awg_stream.open ? &g_awg_stream.ring : NULL;
}

bool awg_sched_stream_dma_mode_enabled(void)
{
	return g_awg_stream.open && g_awg_sched.dma_mode_cfg;
}

uint32_t awg_sched_stream_poll_interval_us(void)
{
	if (!g_awg_stream.open || g_awg_stream.poll_interval_us == 0U)
		return AWG_STREAM_DEFAULT_POLL_INTERVAL_US;

	return g_awg_stream.poll_interval_us;
}

int awg_sched_stream_get_error_snapshot(awg_sched_stream_snapshot_t *snapshot)
{
	if (!snapshot)
		return -EINVAL;

	if (!g_awg_stream.open)
		return -ENODEV;

	*snapshot = g_awg_stream.last_error;
	return 0;
}

int awg_sched_stream_reset_soft(void)
{
	int ret;

	if (!g_awg_sched.configured)
		return -ENODEV;

	ret = awg_sched_reset();
	if (ret)
		return ret;

	ret = awg_sched_irq_clear(AWG_SCHED_IRQ_ALL);
	if (ret)
		return ret;

	/*
	 * The HDL soft reset flushes the stream FIFO and held/prefetched event.
	 * Clear software state as well so foreground polling cannot refill from
	 * stale DDR ring indices after an operator-triggered reset.
	 */
	memset(&g_awg_stream, 0, sizeof(g_awg_stream));
	return 0;
}

#else

int awg_sched_stream_open(const awg_sched_stream_cfg_t *cfg)
{
	(void)cfg;
	return -ENOTSUP;
}

int awg_sched_stream_push(const awg_event_v1_t *ev, uint32_t n)
{
	(void)ev;
	(void)n;
	return -ENOTSUP;
}

int awg_sched_stream_drain_step(void)
{
	return -ENOTSUP;
}

int awg_sched_stream_close(bool send_eof)
{
	(void)send_eof;
	return -ENOTSUP;
}

void awg_sched_stream_irq_handler(uint32_t irq_status)
{
	(void)irq_status;
}

uint32_t awg_sched_stream_ddr_free_events(void)
{
	return 0U;
}

uint32_t awg_sched_stream_poll_interval_us(void)
{
	return 0U;
}

int awg_sched_stream_get_error_snapshot(awg_sched_stream_snapshot_t *snapshot)
{
	(void)snapshot;
	return -ENOTSUP;
}

int awg_sched_stream_reset_soft(void)
{
	return -ENOTSUP;
}

awg_stream_ring_t *awg_sched_stream_ring_get(void)
{
	return NULL;
}

bool awg_sched_stream_dma_mode_enabled(void)
{
	return false;
}

#endif

void awg_sched_dump_artifacts(const awg_event_v1_t *events, uint32_t count,
      const awg_sched_status_t *status)
{
uint32_t i;

AWG_LOG("[SCHED-ARTIFACT] config base=0x%08lX max_events=%lu tick_hz=%lu timeout_ms=%lu\n\r",
(unsigned long)g_awg_sched.cfg.base_addr,
(unsigned long)g_awg_sched.cfg.max_events,
(unsigned long)g_awg_sched.cfg.tick_hz,
(unsigned long)g_awg_sched.cfg.done_timeout_ms);

if (events) {
for (i = 0U; i < count; i++) {
const awg_event_v1_t *e = &events[i];

AWG_LOG("[SCHED-ARTIFACT] event idx=%lu "
"ts=0x%08lX_%08lX ch=%lu fl=0x%04lX "
"p0=0x%08lX p1=0x%08lX p2=0x%08lX p3=0x%08lX\n\r",
(unsigned long)i,
(unsigned long)(e->timestamp_ticks >> 32),
(unsigned long)(e->timestamp_ticks),
(unsigned long)e->channel,
(unsigned long)e->flags,
(unsigned long)e->payload.word0,
(unsigned long)e->payload.word1,
(unsigned long)e->payload.word2,
(unsigned long)e->payload.word3);
}
}

if (status) {
AWG_LOG("[SCHED-ARTIFACT] status armed=%u running=%u done=%u error=%u "
"err_code=0x%02lX current=%lu loaded=%lu commit=%lu "
"reinit=%lu reinit_reject=%lu irq=0x%08lX\n\r",
(unsigned)status->armed,
(unsigned)status->running,
(unsigned)status->done,
(unsigned)status->error,
(unsigned long)status->err_code,
(unsigned long)status->current_event,
(unsigned long)status->loaded_events,
(unsigned long)status->commit_count,
(unsigned long)status->reinit_count,
(unsigned long)status->reinit_reject_count,
(unsigned long)status->irq_status_latched);

AWG_LOG("[SCHED-ARTIFACT] time_now=0x%08lX_%08lX last_exec=0x%08lX_%08lX\n\r",
(unsigned long)(status->time_now >> 32),
(unsigned long)(status->time_now),
(unsigned long)(status->last_exec >> 32),
(unsigned long)(status->last_exec));
}

#if FMCDAC_AWG_SCHED_STREAM
{
uint32_t stream_depth;
uint32_t low_wmark;
uint32_t stream_ctrl;
uint32_t occupancy;
uint32_t free_space;
uint32_t stream_pushes;
uint32_t stream_stalls;
uint32_t irq_status;
uint32_t err_reg;

if (awg_sched_reg_read(AWG_SCHED_REG_STREAM_DEPTH, &stream_depth) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_LOW_WMARK, &low_wmark) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_STREAM_CTRL, &stream_ctrl) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_OCCUPANCY, &occupancy) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_FREE_SPACE, &free_space) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_STREAM_PUSHES, &stream_pushes) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_STREAM_STALLS, &stream_stalls) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_IRQ_STATUS, &irq_status) == 0 &&
    awg_sched_reg_read(AWG_SCHED_REG_ERR_REG, &err_reg) == 0) {
AWG_LOG("[SCHED-ARTIFACT] stream depth=%lu low_wmark=%lu "
"ctrl=0x%08lX occupancy=%lu free_space=%lu pushes=%lu stalls=%lu "
"irq=0x%08lX err=0x%08lX\n\r",
(unsigned long)stream_depth,
(unsigned long)low_wmark,
(unsigned long)stream_ctrl,
(unsigned long)occupancy,
(unsigned long)free_space,
(unsigned long)stream_pushes,
(unsigned long)stream_stalls,
(unsigned long)irq_status,
(unsigned long)err_reg);
}
}
#endif
}
