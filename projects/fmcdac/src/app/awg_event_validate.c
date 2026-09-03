#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "awg_event_validate.h"

static void awg_event_validation_report_set(
		awg_event_validation_report_t *report,
		awg_event_validation_code_t code, uint32_t index,
		uint64_t observed, uint64_t expected)
{
	if (!report)
		return;

	report->code = code;
	report->failing_index = index;
	report->observed = observed;
	report->expected = expected;
	report->reason = awg_event_validation_reason(code);
}

void awg_event_validation_state_init(awg_event_validation_state_t *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

const char *awg_event_validation_reason(awg_event_validation_code_t code)
{
	switch (code) {
	case AWG_EVENT_VALIDATE_OK:
		return "ok";
	case AWG_EVENT_VALIDATE_NULL:
		return "events pointer is NULL while count is non-zero";
	case AWG_EVENT_VALIDATE_CHANNEL:
		return "event channel is outside active channels 0-1";
	case AWG_EVENT_VALIDATE_FLAGS:
		return "event contains a reserved flag";
	case AWG_EVENT_VALIDATE_RESERVED:
		return "event reserved word is non-zero";
	case AWG_EVENT_VALIDATE_PAYLOAD_RESERVED:
		return "event payload bits 127:80 are non-zero";
	case AWG_EVENT_VALIDATE_TIMESTAMP_ORDER:
		return "event timestamp moves backward";
	case AWG_EVENT_VALIDATE_SPACING:
		return "adjacent event timestamps are fewer than 8 ticks apart";
	case AWG_EVENT_VALIDATE_EOF_NOT_FINAL:
		return "EOF is allowed only on the final logical event";
	case AWG_EVENT_VALIDATE_AFTER_EOF:
		return "event follows an accepted EOF";
	case AWG_EVENT_VALIDATE_EMPTY_FINAL:
		return "final batch must contain a logical event";
	default:
		return "unknown event validation error";
	}
}

int awg_event_v1_validate_batch(awg_event_validation_state_t *state,
		const awg_event_v1_t *events, uint32_t count,
		bool final_batch, bool require_event_on_final,
		awg_event_validation_report_t *report)
{
	awg_event_validation_state_t next;
	uint32_t index;

	awg_event_validation_report_set(report, AWG_EVENT_VALIDATE_OK, 0U, 0U,
		0U);
	if (!state)
		return -EINVAL;
	if (count != 0U && !events) {
		awg_event_validation_report_set(report, AWG_EVENT_VALIDATE_NULL, 0U,
			0U, 0U);
		return -EINVAL;
	}
	if (state->eof_seen && count != 0U) {
		awg_event_validation_report_set(report, AWG_EVENT_VALIDATE_AFTER_EOF,
			0U, 0U, 0U);
		return -EPERM;
	}
	if (final_batch && require_event_on_final && count == 0U &&
	    !state->have_previous) {
		awg_event_validation_report_set(report,
			AWG_EVENT_VALIDATE_EMPTY_FINAL, 0U, 0U, 1U);
		return -EINVAL;
	}

	next = *state;
	for (index = 0U; index < count; index++) {
		const awg_event_v1_t *event = &events[index];
		uint64_t delta;
		bool has_eof =
			(event->flags & AWG_EVENT_FLAG_EOF) != 0U;

		if (event->channel >= AWG_EVENT_V1_ACTIVE_CHANNELS) {
			awg_event_validation_report_set(report,
				AWG_EVENT_VALIDATE_CHANNEL, index, event->channel,
				AWG_EVENT_V1_ACTIVE_CHANNELS - 1U);
			return -ERANGE;
		}
		if ((event->flags & (uint16_t)~AWG_EVENT_FLAG_ALL) != 0U) {
			awg_event_validation_report_set(report,
				AWG_EVENT_VALIDATE_FLAGS, index, event->flags,
				AWG_EVENT_FLAG_ALL);
			return -EINVAL;
		}
		if (event->reserved != 0U) {
			awg_event_validation_report_set(report,
				AWG_EVENT_VALIDATE_RESERVED, index,
				event->reserved, 0U);
			return -EINVAL;
		}
		/* DDS payload bits [79:0] are scale/init/increment. */
		if ((event->payload.word2 & UINT32_C(0xFFFF0000)) != 0U ||
		    event->payload.word3 != 0U) {
			awg_event_validation_report_set(report,
				AWG_EVENT_VALIDATE_PAYLOAD_RESERVED, index,
				((uint64_t)event->payload.word3 << 16) |
				(event->payload.word2 >> 16), 0U);
			return -EINVAL;
		}
		if (next.have_previous) {
			if (event->timestamp_ticks < next.previous_timestamp) {
				awg_event_validation_report_set(report,
					AWG_EVENT_VALIDATE_TIMESTAMP_ORDER, index,
					event->timestamp_ticks,
					next.previous_timestamp);
				return -EINVAL;
			}
			delta = event->timestamp_ticks - next.previous_timestamp;
			if (delta < AWG_EVENT_V1_MIN_SPACING_TICKS) {
				awg_event_validation_report_set(report,
					AWG_EVENT_VALIDATE_SPACING, index, delta,
					AWG_EVENT_V1_MIN_SPACING_TICKS);
				return -ERANGE;
			}
		}
		if (has_eof && (!final_batch || index + 1U != count)) {
			awg_event_validation_report_set(report,
				AWG_EVENT_VALIDATE_EOF_NOT_FINAL, index,
				index, count ? count - 1U : 0U);
			return -EINVAL;
		}

		next.have_previous = true;
		next.previous_timestamp = event->timestamp_ticks;
		next.accepted_events++;
		next.eof_seen = has_eof;
	}

	*state = next;
	return 0;
}
