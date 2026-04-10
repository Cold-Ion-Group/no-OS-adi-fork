#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "awg_sched.h"
#include "no_os_axi_io.h"
#include "no_os_delay.h"

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
	awg_sched_cfg_t cfg;
	uint32_t loaded_events;
} g_awg_sched;

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

int awg_sched_reset(void)
{
	int ret;

	if (!g_awg_sched.configured)
		return -ENODEV;

	ret = awg_sched_reg_write(AWG_SCHED_REG_CTRL, AWG_SCHED_CTRL_RESET);
	if (ret)
		return ret;

	g_awg_sched.loaded_events = 0;
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
	uint32_t i;

	if (!g_awg_sched.configured)
		return -ENODEV;

	if ((count > 0U) && !events)
		return -EINVAL;

	if (count > g_awg_sched.cfg.max_events)
		return -E2BIG;

	for (i = 1U; i < count; i++) {
		if (events[i].timestamp_ticks < events[i - 1U].timestamp_ticks)
			return -EINVAL;
	}

	return 0;
}

static int awg_sched_write_event(uint32_t idx, const awg_event_v1_t *event)
{
	int ret;
	uint32_t ch_flags;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_ADDR, idx);
	if (ret)
		return ret;

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_TIME, event->timestamp_ticks);
	if (ret)
		return ret;

	ch_flags = (((uint32_t)event->channel) << 16) | (uint32_t)event->flags;
	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_CH_FLAGS, ch_flags);
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

	ret = awg_sched_verify_events(events, count);
	if (ret)
		return ret;

	ret = awg_sched_stop();
	if (ret)
		return ret;

	for (i = 0; i < count; i++) {
		ret = awg_sched_write_event(i, &events[i]);
		if (ret)
			return ret;
	}

	ret = awg_sched_reg_write(AWG_SCHED_REG_EVENT_COUNT, count);
	if (ret)
		return ret;

	g_awg_sched.loaded_events = count;
	return 0;
}

int awg_sched_arm(void)
{
	if (!g_awg_sched.configured)
		return -ENODEV;

	if (!g_awg_sched.loaded_events)
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
