#ifndef AWG_SCHED_H
#define AWG_SCHED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef FMCDAC_AWG_SCHED_STREAM
#define FMCDAC_AWG_SCHED_STREAM 0
#endif

/* Compiler-portable packed annotation for scheduler ABI structs. */
#if defined(__GNUC__) || defined(__clang__)
#define AWG_SCHED_PACKED __attribute__((__packed__))
#else
#define AWG_SCHED_PACKED
#endif

/*
 * Payload format version 1 (16 bytes, 4x32b BRAM words):
 *
 * The active HDL contract is bit-packed:
 *   payload[15:0] = scale
 *   payload[16 +: DDS_PHASE_DW] = init
 *   payload[16 + DDS_PHASE_DW +: DDS_PHASE_DW] = incr
 *   remaining upper bits = reserved / must be zero
 *
 * DDS_PHASE_DW is an HDL build parameter. For the current KCU116 AWG image it
 * is 32, so the payload carries 16-bit scale plus 32-bit init and 32-bit incr.
 */
typedef union {
	struct AWG_SCHED_PACKED {
		uint32_t words[4];
	};
	struct AWG_SCHED_PACKED {
		uint32_t word0;
		uint32_t word1;
		uint32_t word2;
		uint32_t word3;
	};
} awg_payload_v1_t;

static inline uint32_t awg_payload_v1_mask(uint8_t width)
{
	if (width >= 32U)
		return 0xFFFFFFFFU;
	if (width == 0U)
		return 0U;
	return (uint32_t)((1UL << width) - 1UL);
}

static inline void awg_payload_v1_write_bits(awg_payload_v1_t *payload,
					     uint32_t bit_offset,
					     uint8_t width,
					     uint32_t value)
{
	uint32_t bits_left;
	uint32_t word_idx;
	uint32_t bit_in_word;
	uint32_t chunk;
	uint32_t mask;
	uint32_t chunk_value;

	if (!payload || width == 0U)
		return;

	bits_left = width;
	while (bits_left > 0U) {
		word_idx = bit_offset / 32U;
		bit_in_word = bit_offset % 32U;
		chunk = 32U - bit_in_word;
		if (chunk > bits_left)
			chunk = bits_left;

		mask = (chunk == 32U) ? 0xFFFFFFFFU : (uint32_t)((1UL << chunk) - 1UL);
		chunk_value = value & mask;
		payload->words[word_idx] |= chunk_value << bit_in_word;

		if (chunk == 32U)
			value = 0U;
		else
			value >>= chunk;
		bit_offset += chunk;
		bits_left -= chunk;
	}
}

static inline void awg_payload_v1_set_dds(awg_payload_v1_t *payload,
					  uint16_t scale,
					  uint32_t init,
					  uint32_t incr,
					  uint8_t dds_phase_dw)
{
	uint32_t phase_mask;

	if (!payload)
		return;

	memset(payload, 0, sizeof(*payload));
	phase_mask = awg_payload_v1_mask(dds_phase_dw);
	init &= phase_mask;
	incr &= phase_mask;

	awg_payload_v1_write_bits(payload, 0U, 16U, scale);
	awg_payload_v1_write_bits(payload, 16U, dds_phase_dw, init);
	awg_payload_v1_write_bits(payload, 16U + dds_phase_dw, dds_phase_dw, incr);
}

/*
 * Event format version 1 (32 bytes = 256 bits):
 *   [63:0]    timestamp_ticks  — 64-bit tick counter matching HDL time_reg
 *   [79:64]   channel          — output channel selector
 *   [95:80]   flags            — event-type flags
 *   [223:96]  payload          — 4 × 32-bit DDS control words
 *   [255:224] reserved         — must be zero; pads event to 256 bits
 *
 * The 7-word write sequence (EVT_WDATA0..6 + EVT_WCTRL) maps as:
 *   WDATA0  timestamp[31:0]
 *   WDATA1  timestamp[63:32]
 *   WDATA2  channel[31:16] | flags[15:0]
 *   WDATA3  payload.word0
 *   WDATA4  payload.word1
 *   WDATA5  payload.word2
 *   WDATA6  payload.word3
 */
typedef struct AWG_SCHED_PACKED {
	uint64_t timestamp_ticks;
	uint16_t channel;
	uint16_t flags;
	awg_payload_v1_t payload;
	uint32_t reserved;
} awg_event_v1_t;

_Static_assert(sizeof(awg_payload_v1_t) == 16U, "awg_payload_v1_t size mismatch");
_Static_assert(offsetof(awg_payload_v1_t, word0) == 0U, "payload.word0 offset mismatch");
_Static_assert(offsetof(awg_payload_v1_t, word1) == 4U, "payload.word1 offset mismatch");
_Static_assert(offsetof(awg_payload_v1_t, word2) == 8U, "payload.word2 offset mismatch");
_Static_assert(offsetof(awg_payload_v1_t, word3) == 12U, "payload.word3 offset mismatch");
_Static_assert(sizeof(awg_event_v1_t) == 32U, "awg_event_v1_t size mismatch");
_Static_assert(offsetof(awg_event_v1_t, timestamp_ticks) == 0U, "event.timestamp offset mismatch");
_Static_assert(offsetof(awg_event_v1_t, channel) == 8U, "event.channel offset mismatch");
_Static_assert(offsetof(awg_event_v1_t, flags) == 10U, "event.flags offset mismatch");
_Static_assert(offsetof(awg_event_v1_t, payload) == 12U, "event.payload offset mismatch");
_Static_assert(offsetof(awg_event_v1_t, reserved) == 28U, "event.reserved offset mismatch");

/* Event flag bit definitions. */
#define AWG_SCHED_FLAG_PHASE_REINIT 0x0001U
#define AWG_SCHED_FLAG_EOF          0x0002U

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
	bool armed;
	bool running;
	bool done;
	bool error;
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
int awg_sched_set_epoch(void);
void awg_sched_irq_signal(void);
void awg_sched_dump_artifacts(const awg_event_v1_t *events, uint32_t count,
			      const awg_sched_status_t *status);

int awg_sched_stream_open(const awg_sched_stream_cfg_t *cfg);
int awg_sched_stream_push(const awg_event_v1_t *ev, uint32_t n);
int awg_sched_stream_drain_step(void);
int awg_sched_stream_close(bool send_eof);
void awg_sched_stream_irq_handler(uint32_t irq_status);
uint32_t awg_sched_stream_ddr_free_events(void);
uint32_t awg_sched_stream_poll_interval_us(void);
int awg_sched_stream_get_error_snapshot(awg_sched_stream_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* AWG_SCHED_H */
