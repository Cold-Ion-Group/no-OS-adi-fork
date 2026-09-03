#ifndef AWG_EVENT_VALIDATE_H
#define AWG_EVENT_VALIDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "awg_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Release-A event-v1 policy.  These are properties of the active KCU116
 * build, not changes to the frozen AWGS v1 register/event ABI.
 */
#define AWG_EVENT_V1_ACTIVE_CHANNELS          2U
#define AWG_EVENT_V1_MIN_SPACING_TICKS        8U
#define AWG_EVENT_V1_DDS_ACCUMULATOR_BITS     32U
#define AWG_EVENT_V1_CORDIC_ANGLE_BITS        16U
#define AWG_EVENT_V1_PAYLOAD_USED_BITS        80U

typedef enum {
	AWG_EVENT_VALIDATE_OK = 0,
	AWG_EVENT_VALIDATE_NULL,
	AWG_EVENT_VALIDATE_CHANNEL,
	AWG_EVENT_VALIDATE_FLAGS,
	AWG_EVENT_VALIDATE_RESERVED,
	AWG_EVENT_VALIDATE_PAYLOAD_RESERVED,
	AWG_EVENT_VALIDATE_TIMESTAMP_ORDER,
	AWG_EVENT_VALIDATE_SPACING,
	AWG_EVENT_VALIDATE_EOF_NOT_FINAL,
	AWG_EVENT_VALIDATE_AFTER_EOF,
	AWG_EVENT_VALIDATE_EMPTY_FINAL
} awg_event_validation_code_t;

typedef struct {
	bool have_previous;
	bool eof_seen;
	uint64_t previous_timestamp;
	uint64_t accepted_events;
} awg_event_validation_state_t;

typedef struct {
	awg_event_validation_code_t code;
	uint32_t failing_index;
	uint64_t observed;
	uint64_t expected;
	const char *reason;
} awg_event_validation_report_t;

void awg_event_validation_state_init(awg_event_validation_state_t *state);
const char *awg_event_validation_reason(awg_event_validation_code_t code);

/*
 * Validate a batch transactionally.  State is changed only when the complete
 * batch is valid, so a rejected/full frame can be retried without poisoning
 * the cross-frame timestamp history.  An EOF event is legal only as the last
 * event of a batch submitted with final_batch=true.
 */
int awg_event_v1_validate_batch(awg_event_validation_state_t *state,
		const awg_event_v1_t *events, uint32_t count,
		bool final_batch, bool require_event_on_final,
		awg_event_validation_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* AWG_EVENT_VALIDATE_H */
