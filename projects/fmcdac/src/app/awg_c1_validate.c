#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "awg_c1_validate.h"

#define AWG_C1_OP_WAIT          0x01U
#define AWG_C1_OP_FIRE          0x02U
#define AWG_C1_OP_LINEAR        0x03U
#define AWG_C1_OP_LINEAR_CONT   0x04U
#define AWG_C1_OP_REPEAT_BEGIN  0x05U
#define AWG_C1_OP_REPEAT_END    0x06U
#define AWG_C1_CRC_POLY         UINT64_C(0x42F0E1EBA9EA3693)

struct awg_c1_summary {
	bool has_event;
	uint64_t event_count;
	uint64_t duration;
	uint64_t first_offset;
	uint64_t last_offset;
	uint64_t minimum_gap;
};

struct awg_c1_parser {
	const uint8_t *records;
	uint32_t count;
	uint32_t maximum_depth;
	awg_c1_preflight_report_t *report;
};

static uint16_t awg_c1_get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t awg_c1_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t awg_c1_get_le64(const uint8_t *p)
{
	return (uint64_t)awg_c1_get_le32(p) |
	       ((uint64_t)awg_c1_get_le32(p + 4U) << 32);
}

static int64_t awg_c1_get_le64_signed(const uint8_t *p)
{
	uint64_t raw = awg_c1_get_le64(p);

	if (raw <= (uint64_t)INT64_MAX)
		return (int64_t)raw;
	return -1 - (int64_t)(UINT64_MAX - raw);
}

static bool awg_c1_zero(const uint8_t *p, size_t length)
{
	size_t index;

	for (index = 0U; index < length; index++) {
		if (p[index] != 0U)
			return false;
	}
	return true;
}

static bool awg_c1_add_u64(uint64_t left, uint64_t right, uint64_t *sum)
{
	if (right > UINT64_MAX - left)
		return false;
	*sum = left + right;
	return true;
}

static bool awg_c1_mul_u64(uint64_t left, uint64_t right,
			   uint64_t *product)
{
	if (left != 0U && right > UINT64_MAX / left)
		return false;
	*product = left * right;
	return true;
}

static uint64_t awg_c1_min_u64(uint64_t left, uint64_t right)
{
	return left < right ? left : right;
}

static int awg_c1_fail(struct awg_c1_parser *parser, uint32_t error,
		       uint32_t command_index,
		       awg_event_validation_code_t event_error)
{
	if (parser && parser->report) {
		parser->report->error_mask = error;
		parser->report->command_index = command_index;
		parser->report->event_error = event_error;
	}
	return error == AWG_C1_ERROR_RANGE ? -ERANGE : -EINVAL;
}

uint64_t awg_c1_crc64_ecma182(const uint8_t *data, size_t length,
				      uint64_t initial)
{
	uint64_t crc = initial;
	size_t index;
	uint8_t bit;

	if (!data && length != 0U)
		return 0U;
	for (index = 0U; index < length; index++) {
		crc ^= (uint64_t)data[index] << 56;
		for (bit = 0U; bit < 8U; bit++)
			crc = (crc & (UINT64_C(1) << 63)) != 0U ?
			      (crc << 1) ^ AWG_C1_CRC_POLY : crc << 1;
	}
	return crc;
}

static int awg_c1_summary_append(struct awg_c1_parser *parser,
		struct awg_c1_summary *total,
		const struct awg_c1_summary *next, uint32_t index)
{
	uint64_t shifted_first;
	uint64_t shifted_last;
	uint64_t boundary_gap;
	uint64_t combined;
	if (next->has_event) {
		if (!awg_c1_add_u64(total->duration, next->first_offset,
				    &shifted_first) ||
		    !awg_c1_add_u64(total->duration, next->last_offset,
				    &shifted_last))
			return awg_c1_fail(parser, AWG_C1_ERROR_RANGE, index,
				AWG_EVENT_VALIDATE_OK);
		if (!total->has_event)
			total->first_offset = shifted_first;
		else {
			boundary_gap = shifted_first - total->last_offset;
			total->minimum_gap = awg_c1_min_u64(
				total->minimum_gap, boundary_gap);
		}
		total->last_offset = shifted_last;
		total->minimum_gap = awg_c1_min_u64(total->minimum_gap,
			next->minimum_gap);
		total->has_event = true;
	}
	if (!awg_c1_add_u64(total->duration, next->duration, &combined))
		return awg_c1_fail(parser, AWG_C1_ERROR_RANGE, index,
			AWG_EVENT_VALIDATE_OK);
	total->duration = combined;
	if (!awg_c1_add_u64(total->event_count, next->event_count, &combined))
		return awg_c1_fail(parser, AWG_C1_ERROR_RANGE, index,
			AWG_EVENT_VALIDATE_OK);
	total->event_count = combined;
	return 0;
}

static int awg_c1_validate_explicit_event(struct awg_c1_parser *parser,
		const uint8_t *record, uint32_t index, uint16_t flags,
		uint16_t channel, const uint8_t *payload)
{
	awg_event_validation_state_t state;
	awg_event_validation_report_t report;
	awg_event_v1_t event;
	int ret;

	memset(&event, 0, sizeof(event));
	event.channel = channel;
	event.flags = flags;
	event.payload.word0 = awg_c1_get_le32(payload);
	event.payload.word1 = awg_c1_get_le32(payload + 4U);
	event.payload.word2 = awg_c1_get_le32(payload + 8U);
	event.payload.word3 = awg_c1_get_le32(payload + 12U);
	(void)record;
	awg_event_validation_state_init(&state);
	ret = awg_event_v1_validate_batch(&state, &event, 1U, false, true,
		&report);
	if (ret)
		return awg_c1_fail(parser, AWG_C1_ERROR_RANGE, index,
			report.code);
	return 0;
}

static bool awg_c1_linear_endpoint_valid(uint64_t start, int64_t step,
		uint32_t count, uint64_t maximum)
{
	uint64_t magnitude;
	uint64_t distance;

	if (count <= 1U)
		return start <= maximum;
	magnitude = step < 0 ? (uint64_t)(-(step + 1)) + 1U : (uint64_t)step;
	if (!awg_c1_mul_u64(magnitude, (uint64_t)count - 1U, &distance))
		return false;
	if (step < 0)
		return distance <= start;
	return distance <= maximum - start;
}

static bool awg_c1_linear_step_valid(int64_t step, uint64_t maximum)
{
	return step >= -(int64_t)maximum && step <= (int64_t)maximum;
}

static int awg_c1_parse_sequence(struct awg_c1_parser *parser,
		uint32_t *position, uint32_t depth, bool expect_end,
		struct awg_c1_summary *summary)
{
	uint32_t index;

	memset(summary, 0, sizeof(*summary));
	summary->minimum_gap = UINT64_MAX;
	while (*position < parser->count) {
		const uint8_t *record = parser->records +
			(size_t)(*position) * AWG_C1_COMMAND_BYTES;
		uint8_t opcode = record[0];
		uint8_t command_flags = record[1];
		uint16_t channel = awg_c1_get_le16(record + 2U);
		struct awg_c1_summary operation;
		int ret;

		index = *position;
		if (command_flags != 0U)
			return awg_c1_fail(parser, AWG_C1_ERROR_BAD_FLAGS, index,
				AWG_EVENT_VALIDATE_OK);
		memset(&operation, 0, sizeof(operation));
		operation.minimum_gap = UINT64_MAX;

		if (opcode == AWG_C1_OP_REPEAT_END) {
			if (channel != 0U || !awg_c1_zero(record + 4U, 28U))
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_RESERVED, index,
					AWG_EVENT_VALIDATE_OK);
			if (!expect_end)
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_STRUCTURE, index,
					AWG_EVENT_VALIDATE_OK);
			(*position)++;
			return 0;
		}

		if (opcode == AWG_C1_OP_WAIT) {
			if (channel != 0U || !awg_c1_zero(record + 12U, 20U))
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_RESERVED, index,
					AWG_EVENT_VALIDATE_OK);
			operation.duration = awg_c1_get_le64(record + 4U);
			(*position)++;
		} else if (opcode == AWG_C1_OP_FIRE) {
			uint16_t flags = awg_c1_get_le16(record + 4U);

			if (awg_c1_get_le16(record + 6U) != 0U ||
			    !awg_c1_zero(record + 28U, 4U))
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_RESERVED, index,
					AWG_EVENT_VALIDATE_OK);
			if ((flags & (uint16_t)~AWG_EVENT_FLAG_PHASE_REINIT) != 0U)
				return awg_c1_fail(parser, AWG_C1_ERROR_BAD_FLAGS,
					index, AWG_EVENT_VALIDATE_FLAGS);
			ret = awg_c1_validate_explicit_event(parser, record, index,
				flags, channel, record + 12U);
			if (ret)
				return ret;
			operation.has_event = true;
			operation.event_count = 1U;
			operation.duration = awg_c1_get_le32(record + 8U);
			operation.first_offset = 0U;
			operation.last_offset = 0U;
			(*position)++;
		} else if (opcode == AWG_C1_OP_LINEAR) {
			const uint8_t *continuation;
			uint16_t flags = awg_c1_get_le16(record + 4U);
			uint32_t count = awg_c1_get_le32(record + 8U);
			uint32_t dwell = awg_c1_get_le32(record + 12U);
			uint16_t asf = awg_c1_get_le16(record + 16U);
			uint32_t pow = awg_c1_get_le32(record + 20U);
			uint32_t ftw = awg_c1_get_le32(record + 24U);
			int64_t asf_step;
			int64_t pow_step;
			int64_t ftw_step;
			uint8_t payload[16] = { 0 };

			if (awg_c1_get_le16(record + 6U) != 0U ||
			    awg_c1_get_le16(record + 18U) != 0U ||
			    !awg_c1_zero(record + 28U, 4U))
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_RESERVED, index,
					AWG_EVENT_VALIDATE_OK);
			if ((flags & (uint16_t)~AWG_EVENT_FLAG_PHASE_REINIT) != 0U)
				return awg_c1_fail(parser, AWG_C1_ERROR_BAD_FLAGS,
					index, AWG_EVENT_VALIDATE_FLAGS);
			if (index + 1U >= parser->count)
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_STRUCTURE, index,
					AWG_EVENT_VALIDATE_OK);
			continuation = record + AWG_C1_COMMAND_BYTES;
			if (continuation[0] != AWG_C1_OP_LINEAR_CONT ||
			    continuation[1] != 0U ||
			    awg_c1_get_le16(continuation + 2U) != 0U ||
			    !awg_c1_zero(continuation + 28U, 4U))
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_STRUCTURE, index,
					AWG_EVENT_VALIDATE_OK);
			asf_step = awg_c1_get_le64_signed(continuation + 4U);
			pow_step = awg_c1_get_le64_signed(continuation + 12U);
			ftw_step = awg_c1_get_le64_signed(continuation + 20U);
			if (count == 0U || dwell == 0U ||
			    !awg_c1_linear_step_valid(asf_step, UINT16_MAX) ||
			    !awg_c1_linear_step_valid(pow_step, UINT32_MAX) ||
			    !awg_c1_linear_step_valid(ftw_step, UINT32_MAX) ||
			    !awg_c1_linear_endpoint_valid(asf, asf_step, count,
				UINT16_MAX) ||
			    !awg_c1_linear_endpoint_valid(pow, pow_step, count,
				UINT32_MAX) ||
			    !awg_c1_linear_endpoint_valid(ftw, ftw_step, count,
				UINT32_MAX))
				return awg_c1_fail(parser, AWG_C1_ERROR_RANGE,
					index, AWG_EVENT_VALIDATE_OK);
			payload[0] = (uint8_t)asf;
			payload[1] = (uint8_t)(asf >> 8);
			payload[2] = (uint8_t)pow;
			payload[3] = (uint8_t)(pow >> 8);
			payload[4] = (uint8_t)(pow >> 16);
			payload[5] = (uint8_t)(pow >> 24);
			payload[6] = (uint8_t)ftw;
			payload[7] = (uint8_t)(ftw >> 8);
			payload[8] = (uint8_t)(ftw >> 16);
			payload[9] = (uint8_t)(ftw >> 24);
			ret = awg_c1_validate_explicit_event(parser, record, index,
				flags, channel, payload);
			if (ret)
				return ret;
			operation.has_event = true;
			operation.event_count = count;
			if (!awg_c1_mul_u64(count, dwell,
					&operation.duration) ||
			    !awg_c1_mul_u64((uint64_t)count - 1U, dwell,
					&operation.last_offset))
				return awg_c1_fail(parser, AWG_C1_ERROR_RANGE,
					index, AWG_EVENT_VALIDATE_OK);
			operation.first_offset = 0U;
			if (count > 1U)
				operation.minimum_gap = dwell;
			*position += 2U;
		} else if (opcode == AWG_C1_OP_LINEAR_CONT) {
			return awg_c1_fail(parser, AWG_C1_ERROR_BAD_STRUCTURE,
				index, AWG_EVENT_VALIDATE_OK);
		} else if (opcode == AWG_C1_OP_REPEAT_BEGIN) {
			struct awg_c1_summary body;
			uint32_t repeat = awg_c1_get_le32(record + 4U);
			uint64_t repeated_duration;
			uint64_t repeated_count;
			uint64_t repeated_prefix;
			uint64_t boundary_gap;

			if (channel != 0U || !awg_c1_zero(record + 8U, 24U))
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_RESERVED, index,
					AWG_EVENT_VALIDATE_OK);
			if (depth >= AWG_C1_MAX_REPEAT_DEPTH)
				return awg_c1_fail(parser,
					AWG_C1_ERROR_BAD_STRUCTURE, index,
					AWG_EVENT_VALIDATE_OK);
			if (parser->maximum_depth < depth + 1U)
				parser->maximum_depth = depth + 1U;
			(*position)++;
			ret = awg_c1_parse_sequence(parser, position, depth + 1U,
				true, &body);
			if (ret)
				return ret;
			if (!awg_c1_mul_u64(body.duration, repeat,
					&repeated_duration) ||
			    !awg_c1_mul_u64(body.event_count, repeat,
					&repeated_count))
				return awg_c1_fail(parser, AWG_C1_ERROR_RANGE,
					index, AWG_EVENT_VALIDATE_OK);
			operation.duration = repeated_duration;
			operation.event_count = repeated_count;
			operation.has_event = body.has_event && repeat != 0U;
			if (operation.has_event) {
				uint64_t boundary_tail;

				operation.first_offset = body.first_offset;
				if (!awg_c1_mul_u64(body.duration,
						(uint64_t)repeat - 1U,
						&repeated_prefix) ||
				    !awg_c1_add_u64(repeated_prefix,
						body.last_offset,
						&operation.last_offset))
					return awg_c1_fail(parser,
						AWG_C1_ERROR_RANGE, index,
						AWG_EVENT_VALIDATE_OK);
				operation.minimum_gap = body.minimum_gap;
				if (repeat > 1U) {
					boundary_tail = body.duration -
						body.last_offset;
					if (!awg_c1_add_u64(boundary_tail,
							body.first_offset,
							&boundary_gap))
						return awg_c1_fail(parser,
							AWG_C1_ERROR_RANGE, index,
							AWG_EVENT_VALIDATE_OK);
					operation.minimum_gap = awg_c1_min_u64(
						operation.minimum_gap, boundary_gap);
				}
			}
		} else {
			return awg_c1_fail(parser, AWG_C1_ERROR_BAD_OPCODE,
				index, AWG_EVENT_VALIDATE_OK);
		}

		ret = awg_c1_summary_append(parser, summary, &operation, index);
		if (ret)
			return ret;
	}

	if (expect_end)
		return awg_c1_fail(parser, AWG_C1_ERROR_BAD_STRUCTURE,
			parser->count, AWG_EVENT_VALIDATE_OK);
	return 0;
}

int awg_c1_preflight(const void *blob, size_t length,
		     awg_c1_preflight_result_t *result,
		     awg_c1_preflight_report_t *report)
{
	const uint8_t *data = blob;
	struct awg_c1_parser parser;
	struct awg_c1_summary summary;
	uint32_t command_count;
	uint32_t declared_depth;
	uint32_t flags;
	uint32_t position = 0U;
	uint64_t command_bytes;
	uint64_t declared_events;
	uint64_t checksum;
	uint64_t calculated_crc;
	uint64_t start_timestamp;
	uint64_t final_cursor;
	uint8_t zero_crc[8] = { 0 };
	int ret;

	if (result)
		memset(result, 0, sizeof(*result));
	if (report)
		memset(report, 0, sizeof(*report));
	if (!data)
		return -EINVAL;
	if (length < AWG_C1_HEADER_BYTES ||
	    (length % AWG_C1_COMMAND_BYTES) != 0U) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_SIZE;
		return -EMSGSIZE;
	}
	if (awg_c1_get_le32(data) != AWG_C1_MAGIC_U32) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_HEADER;
		return -EINVAL;
	}
	if (awg_c1_get_le16(data + 4U) != AWG_C1_MAJOR ||
	    awg_c1_get_le16(data + 6U) != AWG_C1_MINOR) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_VERSION;
		return -EPROTONOSUPPORT;
	}
	if (awg_c1_get_le16(data + 8U) != AWG_C1_HEADER_BYTES ||
	    awg_c1_get_le16(data + 10U) != AWG_C1_COMMAND_BYTES) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_SIZE;
		return -EMSGSIZE;
	}
	flags = awg_c1_get_le32(data + 12U);
	if ((flags & ~AWG_C1_FLAG_EOF) != 0U) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_FLAGS;
		return -EINVAL;
	}
	start_timestamp = awg_c1_get_le64(data + 16U);
	command_count = awg_c1_get_le32(data + 24U);
	declared_depth = awg_c1_get_le32(data + 28U);
	declared_events = awg_c1_get_le64(data + 32U);
	command_bytes = awg_c1_get_le64(data + 40U);
	checksum = awg_c1_get_le64(data + 48U);
	if (!awg_c1_zero(data + 56U, 8U)) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_RESERVED;
		return -EINVAL;
	}
	if (declared_depth > AWG_C1_MAX_REPEAT_DEPTH) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_STRUCTURE;
		return -EINVAL;
	}
	if (command_count > AWG_C1_MAX_COMMAND_RECORDS ||
	    command_bytes != (uint64_t)command_count * AWG_C1_COMMAND_BYTES ||
	    command_bytes + AWG_C1_HEADER_BYTES != length) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_SIZE;
		return -EMSGSIZE;
	}

	calculated_crc = awg_c1_crc64_ecma182(data, 48U, 0U);
	calculated_crc = awg_c1_crc64_ecma182(zero_crc, sizeof(zero_crc),
		calculated_crc);
	calculated_crc = awg_c1_crc64_ecma182(data + 56U, length - 56U,
		calculated_crc);
	if (calculated_crc != checksum) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_CRC;
		return -EBADMSG;
	}

	memset(&parser, 0, sizeof(parser));
	parser.records = data + AWG_C1_HEADER_BYTES;
	parser.count = command_count;
	parser.report = report;
	ret = awg_c1_parse_sequence(&parser, &position, 0U, false, &summary);
	if (ret)
		return ret;
	if (position != command_count || declared_depth != parser.maximum_depth) {
		if (report)
			report->error_mask = AWG_C1_ERROR_BAD_STRUCTURE;
		return -EINVAL;
	}
	if (declared_events != summary.event_count) {
		if (report)
			report->error_mask = AWG_C1_ERROR_COUNT_MISMATCH;
		return -EINVAL;
	}
	if ((flags & AWG_C1_FLAG_EOF) != 0U && !summary.has_event) {
		if (report)
			report->error_mask = AWG_C1_ERROR_RANGE;
		return -EINVAL;
	}
	if (summary.has_event &&
	    summary.minimum_gap < AWG_EVENT_V1_MIN_SPACING_TICKS) {
		if (report) {
			report->error_mask = AWG_C1_ERROR_RANGE;
			report->event_error = AWG_EVENT_VALIDATE_SPACING;
		}
		return -ERANGE;
	}
	if (!awg_c1_add_u64(start_timestamp, summary.duration,
			    &final_cursor)) {
		if (report)
			report->error_mask = AWG_C1_ERROR_RANGE;
		return -ERANGE;
	}
	(void)final_cursor;

	if (result) {
		result->command_count = command_count;
		result->max_repeat_depth = parser.maximum_depth;
		result->expanded_event_count = summary.event_count;
		result->crc64 = checksum;
		result->duration_ticks = summary.duration;
		result->flags = flags;
	}
	return 0;
}
