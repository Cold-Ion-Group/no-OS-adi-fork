#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "awg_sched.h"
#include "no_os_axi_io.h"
#include "no_os_delay.h"
#include "xil_printf.h"

/* Control/status register map (local to this implementation). */
#define AWG_SCHED_REG_CTRL            0x0000U
#define AWG_SCHED_REG_STATUS          0x0004U
#define AWG_SCHED_REG_EVENT_COUNT     0x0008U
#define AWG_SCHED_REG_CUR_EVENT       0x000CU
#define AWG_SCHED_REG_ERROR           0x0010U
#define AWG_SCHED_REG_EVENT_ADDR      0x0014U
#define AWG_SCHED_REG_EVENT_TIME      0x0018U
#define AWG_SCHED_REG_EVENT_CH_FLAGS  0x001CU
#define AWG_SCHED_REG_EVENT_PAYLOAD0  0x0020U
#define AWG_SCHED_REG_EVENT_PAYLOAD1  0x0024U
#define AWG_SCHED_REG_EVENT_PAYLOAD2  0x0028U
#define AWG_SCHED_REG_EVENT_PAYLOAD3  0x002CU
#define AWG_SCHED_REG_EVENT_COMMIT    0x0030U

#define AWG_SCHED_CTRL_RESET          (1U << 0)
#define AWG_SCHED_CTRL_ARM            (1U << 1)
#define AWG_SCHED_CTRL_START          (1U << 2)
#define AWG_SCHED_CTRL_STOP           (1U << 3)

#define AWG_SCHED_STATUS_ARMED        (1U << 0)
#define AWG_SCHED_STATUS_RUNNING      (1U << 1)
#define AWG_SCHED_STATUS_DONE         (1U << 2)
#define AWG_SCHED_STATUS_ERROR        (1U << 3)

static struct {
	bool configured;
	bool events_validated;
	awg_sched_cfg_t cfg;
	uint32_t loaded_events;
} g_awg_sched;

#define AWG_SCHED_FLAGS_ALLOWED_MASK      0x0001U
#define AWG_SCHED_CHANNEL_MASK_DEFAULT    0x0001U
#define AWG_SCHED_TONE_MASK_DEFAULT       0x0003U
#define AWG_SCHED_FREQ_MASK_DEFAULT       0x0000FFFFU
#define AWG_SCHED_SCALE_MASK_DEFAULT      0x0000FFFFU
#define AWG_SCHED_PHASE_MASK_DEFAULT      0x0000FFFFU
#ifndef AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY
#define AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY 1
#endif

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

static bool awg_sched_check_field_width(uint32_t raw, uint32_t mask, uint8_t shift)
{
	if (mask == 0U)
		return true;

	return ((raw >> shift) & ~mask) == 0U;
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

static uint32_t awg_sched_pack_ch_flags(const awg_event_v1_t *event)
{
	return (((uint32_t)event->channel) << 16) | (uint32_t)event->flags;
}

static int awg_sched_verify_loaded_events_readback(const awg_event_v1_t *events, uint32_t count)
{
	uint32_t i;
	uint32_t time_rb;
	uint32_t ch_flags_rb;
	uint32_t payload0_rb;
	uint32_t payload1_rb;
	uint32_t payload2_rb;
	uint32_t payload3_rb;
	int ret;

	if (count == 0U)
		return 0;

	for (i = 0U; i < count; i++) {
		const awg_event_v1_t *event = &events[i];
		uint32_t expected_ch_flags = awg_sched_pack_ch_flags(event);

		ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_ADDR, i);
		if (ret)
			return ret;

		ret = awg_sched_reg_read(AWG_SCHED_REG_EVENT_TIME, &time_rb);
		if (ret)
			return ret;
		ret = awg_sched_reg_read(AWG_SCHED_REG_EVENT_CH_FLAGS, &ch_flags_rb);
		if (ret)
			return ret;
		ret = awg_sched_reg_read(AWG_SCHED_REG_EVENT_PAYLOAD0, &payload0_rb);
		if (ret)
			return ret;
		ret = awg_sched_reg_read(AWG_SCHED_REG_EVENT_PAYLOAD1, &payload1_rb);
		if (ret)
			return ret;
		ret = awg_sched_reg_read(AWG_SCHED_REG_EVENT_PAYLOAD2, &payload2_rb);
		if (ret)
			return ret;
		ret = awg_sched_reg_read(AWG_SCHED_REG_EVENT_PAYLOAD3, &payload3_rb);
		if (ret)
			return ret;

		if ((time_rb != event->timestamp_ticks) ||
		    (ch_flags_rb != expected_ch_flags) ||
		    (payload0_rb != event->payload.word0) ||
		    (payload1_rb != event->payload.word1) ||
		    (payload2_rb != event->payload.word2) ||
		    (payload3_rb != event->payload.word3)) {
			xil_printf("[AWG-SCHED] readback mismatch idx=%lu "
				   "exp:{t=0x%08lX cf=0x%08lX p0=0x%08lX p1=0x%08lX p2=0x%08lX p3=0x%08lX} "
				   "got:{t=0x%08lX cf=0x%08lX p0=0x%08lX p1=0x%08lX p2=0x%08lX p3=0x%08lX}\n\r",
				   (unsigned long)i,
				   (unsigned long)event->timestamp_ticks,
				   (unsigned long)expected_ch_flags,
				   (unsigned long)event->payload.word0,
				   (unsigned long)event->payload.word1,
				   (unsigned long)event->payload.word2,
				   (unsigned long)event->payload.word3,
				   (unsigned long)time_rb,
				   (unsigned long)ch_flags_rb,
				   (unsigned long)payload0_rb,
				   (unsigned long)payload1_rb,
				   (unsigned long)payload2_rb,
				   (unsigned long)payload3_rb);
			return -EIO;
		}
	}

	return 0;
}

int awg_sched_reset(void)
{
	int ret;

	if (!g_awg_sched.configured)
		return -ENODEV;

	ret = awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_RESET);
	if (ret)
		return ret;

	g_awg_sched.loaded_events = 0;
	g_awg_sched.events_validated = false;
	return awg_sched_reg_write(AWG_SCHED_REG_EVENT_COUNT, 0U);
}

int awg_sched_config(const awg_sched_cfg_t *cfg)
{
	if (!cfg)
		return -EINVAL;

	if (!cfg->base_addr || !cfg->max_events)
		return -EINVAL;

	memset(&g_awg_sched, 0, sizeof(g_awg_sched));
	g_awg_sched.cfg = *cfg;
	g_awg_sched.configured = true;

	return awg_sched_reset();
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
	uint32_t i;
	uint32_t max_events;
	uint32_t observed;
	uint32_t delta;

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
							i, events[i].timestamp_ticks,
							events[i - 1U].timestamp_ticks);
			goto fail;
		}
	}

	for (i = 0U; i < count; i++) {
		const awg_event_v1_t *event = &events[i];

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

		if (!awg_sched_check_field_width(event->payload.word0, active_rules.tone_mask,
						 active_rules.tone_shift)) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_TONE_WIDTH, i,
							event->payload.word0,
							active_rules.tone_mask);
			goto fail;
		}

		if (!awg_sched_check_field_width(event->payload.word0, active_rules.freq_mask,
						 active_rules.freq_shift)) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_FREQ_WIDTH, i,
							event->payload.word0,
							active_rules.freq_mask);
			goto fail;
		}

		if (!awg_sched_check_field_width(event->payload.word1, active_rules.scale_mask,
						 active_rules.scale_shift)) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_SCALE_WIDTH, i,
							event->payload.word1,
							active_rules.scale_mask);
			goto fail;
		}

		if (!awg_sched_check_field_width(event->payload.word2, active_rules.phase_mask,
						 active_rules.phase_shift)) {
			awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_PHASE_WIDTH, i,
							event->payload.word2,
							active_rules.phase_mask);
			goto fail;
		}
	}

	if (count > 1U) {
		for (i = 1U; i < count; i++) {
			delta = events[i].timestamp_ticks - events[i - 1U].timestamp_ticks;
			if (delta < active_rules.min_delta_ticks) {
				if ((active_rules.delta_mode ==
				     AWG_SCHED_DELTA_MODE_ALLOW_ZERO_ON_SAME_CHANNEL) &&
				    (events[i].channel == events[i - 1U].channel) &&
				    (delta == 0U))
					continue;

				awg_sched_validation_report_set(report,
							AWG_EVTVAL_ERR_TS_DELTA_TOO_SMALL,
							i, delta, active_rules.min_delta_ticks);
				goto fail;
			}
		}
	}

	return 0;

fail_not_configured:
	awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_NOT_CONFIGURED, 0U, 0U, 0U);
	xil_printf("[AWG-SCHED] validation failed @idx=0 code=%d reason=%s\n\r",
		   (int)AWG_EVTVAL_ERR_NOT_CONFIGURED,
		   awg_sched_validation_reason(AWG_EVTVAL_ERR_NOT_CONFIGURED));
	return -ENODEV;
fail_null_events:
	awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_NULL_EVENTS, 0U, 0U, 0U);
	xil_printf("[AWG-SCHED] validation failed @idx=0 code=%d reason=%s\n\r",
		   (int)AWG_EVTVAL_ERR_NULL_EVENTS,
		   awg_sched_validation_reason(AWG_EVTVAL_ERR_NULL_EVENTS));
	return -EINVAL;
fail_too_many_events:
	observed = count;
	awg_sched_validation_report_set(report, AWG_EVTVAL_ERR_TOO_MANY_EVENTS,
					0U, observed, max_events);
	xil_printf("[AWG-SCHED] validation failed @idx=0 code=%d reason=%s observed=%lu max=%lu\n\r",
		   (int)AWG_EVTVAL_ERR_TOO_MANY_EVENTS,
		   awg_sched_validation_reason(AWG_EVTVAL_ERR_TOO_MANY_EVENTS),
		   (unsigned long)observed, (unsigned long)max_events);
	return -E2BIG;
fail:
	if (report) {
		xil_printf("[AWG-SCHED] validation failed @idx=%lu code=%d reason=%s observed=0x%08lX expected_min=0x%08lX\n\r",
			   (unsigned long)report->failing_index, (int)report->code,
			   report->reason, (unsigned long)report->observed,
			   (unsigned long)report->expected_min);
	}
	return -EINVAL;
}

static int awg_sched_write_event(uint32_t idx, const awg_event_v1_t *event)
{
	int ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_ADDR, idx);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_TIME, event->timestamp_ticks);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_CH_FLAGS, awg_sched_pack_ch_flags(event));
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_PAYLOAD0, event->payload.word0);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_PAYLOAD1, event->payload.word1);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_PAYLOAD2, event->payload.word2);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_PAYLOAD3, event->payload.word3);
	if (ret)
		return ret;

	return awg_sched_reg_write(AWG_SCHED_REG_EVENT_COMMIT, 1U);
}

int awg_sched_load_events(const awg_event_v1_t *events, uint32_t count)
{
	int ret;
	uint32_t i;

	g_awg_sched.events_validated = false;
	ret = awg_sched_verify_events(events, count);
	if (ret)
		return ret;

	ret = awg_sched_stop();
	if (ret) {
		g_awg_sched.events_validated = false;
		return ret;
	}

	for (i = 0; i < count; i++) {
		ret = awg_sched_write_event(i, &events[i]);
		if (ret) {
			g_awg_sched.events_validated = false;
			return ret;
		}
	}

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_COUNT, count);
	if (ret) {
		g_awg_sched.events_validated = false;
		return ret;
	}

	g_awg_sched.loaded_events = count;

#if AWG_SCHED_ENABLE_LOAD_READBACK_VERIFY
	ret = awg_sched_verify_loaded_events_readback(events, count);
	if (ret) {
		g_awg_sched.events_validated = false;
		return ret;
	}
#endif

	return 0;
}

int awg_sched_arm(void)
{
	if (!g_awg_sched.configured)
		return -ENODEV;

	if (!g_awg_sched.loaded_events)
		return -EINVAL;

	if (!g_awg_sched.events_validated)
		return -EINVAL;

	return awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_ARM);
}

int awg_sched_start(void)
{
	int ret;

	ret = awg_sched_arm();
	if (ret)
		return ret;

	return awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_START);
}

int awg_sched_stop(void)
{
	if (!g_awg_sched.configured)
		return -ENODEV;

	return awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_STOP);
}

int awg_sched_get_status(awg_sched_status_t *status)
{
	int ret;
	uint32_t raw_status;
	uint32_t cur_event;

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

	memset(status, 0, sizeof(*status));
	status->configured = true;
	status->armed = (raw_status & AWG_SCHED_STATUS_ARMED) != 0U;
	status->running = (raw_status & AWG_SCHED_STATUS_RUNNING) != 0U;
	status->done = (raw_status & AWG_SCHED_STATUS_DONE) != 0U;
	status->error = (raw_status & AWG_SCHED_STATUS_ERROR) != 0U;
	status->loaded_events = g_awg_sched.loaded_events;
	status->current_event = cur_event;
	status->hw_status_word = raw_status;

	return 0;
}

int awg_sched_wait_done(uint32_t timeout_ms, awg_sched_status_t *final_status)
{
	awg_sched_status_t status;
	uint32_t wait_ms;
	int ret;

	if (!g_awg_sched.configured)
		return -ENODEV;

	wait_ms = timeout_ms;
	if (wait_ms == 0U)
		wait_ms = g_awg_sched.cfg.done_timeout_ms;

	while (true) {
		ret = awg_sched_get_status(&status);
		if (ret)
			return ret;

		if (status.done || status.error) {
			if (final_status)
				*final_status = status;
			return status.error ? -EIO : 0;
		}

		if (wait_ms == 0U)
			return -ETIMEDOUT;

		no_os_mdelay(1U);
		wait_ms--;
	}
}
