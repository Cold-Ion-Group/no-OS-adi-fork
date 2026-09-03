#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "awg_event_validate.h"

#ifndef EVENT_VECTOR_PATH
#define EVENT_VECTOR_PATH "../../../../rfsoc-bench/contracts/event_v1_validation_vectors.tsv"
#endif

#define MAX_VECTOR_EVENTS 8U

static unsigned int checks;
static unsigned int failures;

#define CHECK(expr) do { \
	checks++; \
	if (!(expr)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
			__func__, __LINE__, #expr); \
		failures++; \
	} \
} while (0)

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
	return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4U) << 32);
}

static int hex_nibble(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static int decode_record(const char *hex, awg_event_v1_t *event)
{
	uint8_t raw[AWG_EVENT_V1_BYTES];
	size_t index;

	if (!hex || !event || strlen(hex) != AWG_EVENT_V1_BYTES * 2U)
		return -1;
	for (index = 0U; index < sizeof(raw); index++) {
		int high = hex_nibble(hex[index * 2U]);
		int low = hex_nibble(hex[index * 2U + 1U]);

		if (high < 0 || low < 0)
			return -1;
		raw[index] = (uint8_t)((high << 4) | low);
	}
	memset(event, 0, sizeof(*event));
	event->timestamp_ticks = get_le64(raw);
	event->channel = get_le16(raw + 8U);
	event->flags = get_le16(raw + 10U);
	event->payload.word0 = get_le32(raw + 12U);
	event->payload.word1 = get_le32(raw + 16U);
	event->payload.word2 = get_le32(raw + 20U);
	event->payload.word3 = get_le32(raw + 24U);
	event->reserved = get_le32(raw + 28U);
	return 0;
}

static void run_golden_vectors(void)
{
	awg_event_v1_t events[MAX_VECTOR_EVENTS];
	awg_event_validation_state_t state;
	awg_event_validation_report_t report;
	char line[2048];
	FILE *input = fopen(EVENT_VECTOR_PATH, "r");
	unsigned int vectors = 0U;

	CHECK(input != NULL);
	if (!input)
		return;
	while (fgets(line, sizeof(line), input)) {
		char *fields[6];
		char *cursor = line;
		char *separator;
		char *record_cursor;
		uint32_t count = 0U;
		unsigned int field;
		int expected;
		int expected_index;
		int ret;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		for (field = 0U; field < 5U; field++) {
			fields[field] = cursor;
			separator = strchr(cursor, '|');
			CHECK(separator != NULL);
			if (!separator)
				break;
			*separator = '\0';
			cursor = separator + 1U;
		}
		if (field != 5U)
			continue;
		fields[5] = cursor;
		fields[5][strcspn(fields[5], "\r\n")] = '\0';

		if (strcmp(fields[3], "-") != 0) {
			record_cursor = fields[3];
			while (record_cursor && *record_cursor) {
				separator = strchr(record_cursor, ',');
				if (separator)
					*separator = '\0';
				CHECK(count < MAX_VECTOR_EVENTS);
				CHECK(decode_record(record_cursor, &events[count]) == 0);
				count++;
				record_cursor = separator ? separator + 1U : NULL;
			}
		}
		expected = atoi(fields[4]);
		expected_index = atoi(fields[5]);
		awg_event_validation_state_init(&state);
		ret = awg_event_v1_validate_batch(&state,
			count ? events : NULL, count, atoi(fields[1]) != 0,
			atoi(fields[2]) != 0, &report);
		if (expected == AWG_EVENT_VALIDATE_OK) {
			CHECK(ret == 0);
			CHECK(state.accepted_events == count);
		} else {
			CHECK(ret != 0);
			CHECK(report.code == (awg_event_validation_code_t)expected);
			CHECK(report.failing_index == (uint32_t)expected_index);
			CHECK(state.accepted_events == 0U);
		}
		vectors++;
	}
	CHECK(vectors == 12U);
	CHECK(fclose(input) == 0);
}

static awg_event_v1_t event(uint64_t timestamp, uint16_t flags)
{
	awg_event_v1_t value;

	memset(&value, 0, sizeof(value));
	value.timestamp_ticks = timestamp;
	value.flags = flags;
	return value;
}

static void run_incremental_boundary_checks(void)
{
	awg_event_validation_state_t state;
	awg_event_validation_report_t report;
	awg_event_v1_t first = event(100U, 0U);
	awg_event_v1_t invalid[2] = { event(108U, 0U), event(115U, 0U) };
	awg_event_v1_t retry = event(108U, 0U);
	awg_event_v1_t final = event(116U, AWG_EVENT_FLAG_EOF);
	awg_event_v1_t after = event(124U, 0U);

	awg_event_validation_state_init(&state);
	CHECK(awg_event_v1_validate_batch(&state, &first, 1U, false, false,
		&report) == 0);
	CHECK(awg_event_v1_validate_batch(&state, invalid, 2U, false, false,
		&report) != 0);
	CHECK(report.code == AWG_EVENT_VALIDATE_SPACING);
	CHECK(state.accepted_events == 1U && state.previous_timestamp == 100U);
	CHECK(awg_event_v1_validate_batch(&state, &retry, 1U, false, false,
		&report) == 0);
	CHECK(awg_event_v1_validate_batch(&state, &final, 1U, true, true,
		&report) == 0);
	CHECK(state.accepted_events == 3U && state.eof_seen);
	CHECK(awg_event_v1_validate_batch(&state, &after, 1U, false, false,
		&report) != 0);
	CHECK(report.code == AWG_EVENT_VALIDATE_AFTER_EOF);
}

int main(void)
{
	run_golden_vectors();
	run_incremental_boundary_checks();
	if (failures) {
		fprintf(stderr, "awg_event_validation_test: %u/%u checks failed\n",
			failures, checks);
		return 1;
	}
	printf("awg_event_validation_test: %u checks passed\n", checks);
	return 0;
}
