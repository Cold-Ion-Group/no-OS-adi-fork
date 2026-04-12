#ifndef AWG_SCHED_H
#define AWG_SCHED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload format version 1, packed as four 32-bit words. */
typedef struct {
	uint32_t word0;
	uint32_t word1;
	uint32_t word2;
	uint32_t word3;
} awg_payload_v1_t;

/* Event format version 1. */
typedef struct {
	uint32_t timestamp_ticks;
	uint16_t channel;
	uint16_t flags;
	awg_payload_v1_t payload;
} awg_event_v1_t;

/* Static scheduler configuration. */
typedef struct {
	uint32_t base_addr;
	uint32_t max_events;
	uint32_t tick_hz;
	uint32_t done_timeout_ms;
} awg_sched_cfg_t;

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
	AWG_EVTVAL_ERR_PHASE_WIDTH
} awg_evtval_err_t;

/* Configurable validation rules. */
typedef struct {
	uint32_t min_delta_ticks;
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
	bool armed;
	bool running;
	bool done;
	bool error;
	uint32_t loaded_events;
	uint32_t current_event;
	uint32_t hw_status_word;
} awg_sched_status_t;

int awg_sched_reset(void);
int awg_sched_config(const awg_sched_cfg_t *cfg);
int awg_sched_load_events(const awg_event_v1_t *events, uint32_t count);
int awg_sched_verify_events(const awg_event_v1_t *events, uint32_t count);
int awg_sched_validate_events(const awg_event_v1_t *events, uint32_t count,
			      const awg_sched_validation_rules_t *rules,
			      awg_sched_validation_report_t *report);
void awg_sched_validation_rules_default(awg_sched_validation_rules_t *rules);
int awg_sched_arm(void);
int awg_sched_start(void);
int awg_sched_stop(void);
int awg_sched_get_status(awg_sched_status_t *status);
int awg_sched_wait_done(uint32_t timeout_ms, awg_sched_status_t *final_status);

#ifdef __cplusplus
}
#endif

#endif /* AWG_SCHED_H */
