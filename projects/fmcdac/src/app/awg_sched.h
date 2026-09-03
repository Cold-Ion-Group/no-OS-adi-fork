#ifndef AWG_SCHED_H
#define AWG_SCHED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"
#include "awg_event.h"
#include "awg_event_validate.h"
#include "awg_stream_ring.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FMCDAC_AWG_SCHED_STREAM
#define FMCDAC_AWG_SCHED_STREAM 0
#endif

/* Source-compatible scheduler names for the canonical event flag values. */
#define AWG_SCHED_FLAG_PHASE_REINIT AWG_EVENT_FLAG_PHASE_REINIT
#define AWG_SCHED_FLAG_EOF          AWG_EVENT_FLAG_EOF

/*
 * Static scheduler configuration.
 *
 * tick_hz is informational only. The current HDL/software contract does not
 * expose scheduler tick rate via IP_CAPS, so firmware and host must agree on
 * the tick rate explicitly. It is not written to any hardware register.
 *
 * log_fn, if non-NULL, is called instead of xil_printf for all diagnostic
 * output.  This decouples the subsystem from the Xilinx BSP and makes the
 * validation layer testable on the host without a hardware dependency.
 */
typedef struct {
	uint32_t base_addr;
	uint32_t max_events;
	uint32_t tick_hz;
	uint32_t done_timeout_ms;
	void (*log_fn)(const char *fmt, ...);
} awg_sched_cfg_t;

typedef struct {
	void     *staging_buffer;
	uint32_t  staging_capacity;
	uint32_t  low_wmark_events;
	uint32_t  refill_chunk_max;
	uint32_t  poll_interval_us;
	bool      use_dma;
} awg_sched_stream_cfg_t;

typedef struct {
	uint32_t status;
	uint32_t err_reg;
	uint32_t irq_status;
	uint32_t occupancy;
	uint32_t free_space;
	uint32_t stream_ctrl;
	uint32_t stream_pushes;
	uint32_t stream_stalls;
} awg_sched_stream_snapshot_t;

/* Event validation mode for minimum timestamp delta handling. */
typedef enum {
	AWG_SCHED_DELTA_MODE_STRICT = 0,
	AWG_SCHED_DELTA_MODE_ALLOW_ZERO_ON_SAME_CHANNEL = 1
} awg_sched_delta_mode_t;

/* Structured event validation error codes. */
typedef enum {
	AWG_EVTVAL_OK = 0,
	AWG_EVTVAL_ERR_NOT_CONFIGURED,
	AWG_EVTVAL_ERR_NULL_EVENTS,
	AWG_EVTVAL_ERR_TOO_MANY_EVENTS,
	AWG_EVTVAL_ERR_TS_NOT_MONOTONIC,
	AWG_EVTVAL_ERR_TS_DELTA_TOO_SMALL,
	AWG_EVTVAL_ERR_RESERVED_FLAGS,
	AWG_EVTVAL_ERR_CHANNEL_WIDTH,
	AWG_EVTVAL_ERR_TONE_WIDTH,
	AWG_EVTVAL_ERR_FREQ_WIDTH,
	AWG_EVTVAL_ERR_SCALE_WIDTH,
	AWG_EVTVAL_ERR_PHASE_WIDTH,
	AWG_EVTVAL_ERR_REINIT_SPACING
} awg_evtval_err_t;

/* Configurable validation rules. */
typedef struct {
	uint32_t min_delta_ticks;
	uint32_t min_reinit_delta_ticks;
	awg_sched_delta_mode_t delta_mode;
	uint16_t allowed_flags_mask;
	uint16_t channel_mask;
	uint8_t tone_shift;
	uint32_t tone_mask;
	uint8_t freq_shift;
	uint32_t freq_mask;
	uint8_t scale_shift;
	uint32_t scale_mask;
	uint8_t phase_shift;
	uint32_t phase_mask;
} awg_sched_validation_rules_t;

/* Detailed first-failure report for event validation. */
typedef struct {
	awg_evtval_err_t code;
	uint32_t failing_index;
	const char *reason;
	uint32_t observed;
	uint32_t expected_min;
} awg_sched_validation_report_t;

/* Runtime status snapshot. */
typedef struct {
	bool configured;
	bool idle;
	bool armed;
	bool running;
	bool done;
	bool error;
	bool err_code_from_rtl_fallback;
	uint8_t err_code;
	uint32_t loaded_events;
	uint32_t current_event;
	uint64_t time_now;
	uint64_t last_exec;
	uint32_t commit_count;
	uint32_t reinit_count;
	uint32_t reinit_reject_count;
	uint32_t irq_status_latched;
	uint32_t hw_status_word;
} awg_sched_status_t;

/*
 * Retained proof of the most recent scheduler epoch application.  The
 * sequence advances only after TIME_NOW proves that the requested reload was
 * applied; failed attempts retain their before/after evidence without
 * advancing the sequence.
 */
typedef struct {
	uint64_t reload_ticks;
	uint32_t control;
	uint64_t time_before;
	uint64_t time_after;
	uint32_t apply_seq;
	bool applied;
} awg_sched_epoch_result_t;

int awg_sched_config(const awg_sched_cfg_t *cfg);
int awg_sched_read_status(awg_sched_status_t *status);
int awg_sched_get_status(awg_sched_status_t *status);
int awg_sched_read_stream_snapshot(awg_sched_stream_snapshot_t *snapshot);

int awg_sched_soft_reset(void);
int awg_sched_reset(void);
int awg_sched_set_stream_mode(bool enable);
int awg_sched_set_dma_mode(bool enable);
int awg_sched_set_low_wmark(uint32_t events);
int awg_sched_irq_enable(uint32_t mask);
int awg_sched_irq_clear(uint32_t mask);
uint32_t awg_sched_stream_depth(void);
int awg_sched_write_event_mmio(uint32_t event_index,
			       const awg_event_v1_t *event);

int awg_sched_load_events(const awg_event_v1_t *events, uint32_t count);
int awg_sched_verify_events(const awg_event_v1_t *events, uint32_t count);
int awg_sched_validate_events(const awg_event_v1_t *events, uint32_t count,
			      const awg_sched_validation_rules_t *rules,
			      awg_sched_validation_report_t *report);
void awg_sched_validation_rules_default(awg_sched_validation_rules_t *rules);
int awg_sched_arm(void);
int awg_sched_run(void);
int awg_sched_start(void);
int awg_sched_stop(void);
int awg_sched_wait_done(uint32_t timeout_ms, awg_sched_status_t *final_status);
int awg_sched_set_epoch(void);
int awg_sched_get_last_epoch_result(awg_sched_epoch_result_t *result);
void awg_sched_irq_signal(void);
void awg_sched_dump_artifacts(const awg_event_v1_t *events, uint32_t count,
			      const awg_sched_status_t *status);

int awg_sched_stream_open(const awg_sched_stream_cfg_t *cfg);
int awg_sched_stream_push(const awg_event_v1_t *ev, uint32_t n);
int awg_sched_stream_push_final(const awg_event_v1_t *ev, uint32_t n,
				 bool require_event);
int awg_sched_stream_push_opaque(const void *records, uint32_t n);
int awg_sched_stream_drain_step(void);
int awg_sched_stream_close(bool send_eof);
void awg_sched_stream_irq_handler(uint32_t irq_status);
uint32_t awg_sched_stream_ddr_free_events(void);
uint32_t awg_sched_stream_poll_interval_us(void);
int awg_sched_stream_get_error_snapshot(awg_sched_stream_snapshot_t *snapshot);
int awg_sched_stream_reset_soft(void);
awg_stream_ring_t *awg_sched_stream_ring_get(void);
bool awg_sched_stream_dma_mode_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* AWG_SCHED_H */
