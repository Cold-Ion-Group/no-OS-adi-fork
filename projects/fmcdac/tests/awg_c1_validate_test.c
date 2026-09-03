#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "awg_c1_validate.h"

#define MAX_COMMANDS 8U
#define BLOB_BYTES (AWG_C1_HEADER_BYTES + MAX_COMMANDS * AWG_C1_COMMAND_BYTES)

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

static void put_le16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *p, uint64_t value)
{
	put_le32(p, (uint32_t)value);
	put_le32(p + 4U, (uint32_t)(value >> 32));
}

static size_t init_blob(uint8_t blob[BLOB_BYTES], uint32_t commands,
		uint32_t depth, uint64_t events, uint32_t flags)
{
	size_t length = AWG_C1_HEADER_BYTES +
		(size_t)commands * AWG_C1_COMMAND_BYTES;

	memset(blob, 0, BLOB_BYTES);
	put_le32(blob, AWG_C1_MAGIC_U32);
	put_le16(blob + 4U, AWG_C1_MAJOR);
	put_le16(blob + 6U, AWG_C1_MINOR);
	put_le16(blob + 8U, AWG_C1_HEADER_BYTES);
	put_le16(blob + 10U, AWG_C1_COMMAND_BYTES);
	put_le32(blob + 12U, flags);
	put_le64(blob + 16U, 100U);
	put_le32(blob + 24U, commands);
	put_le32(blob + 28U, depth);
	put_le64(blob + 32U, events);
	put_le64(blob + 40U, (uint64_t)commands * AWG_C1_COMMAND_BYTES);
	return length;
}

static void seal_blob(uint8_t *blob, size_t length)
{
	uint64_t crc;

	put_le64(blob + 48U, 0U);
	crc = awg_c1_crc64_ecma182(blob, length, 0U);
	put_le64(blob + 48U, crc);
}

static uint8_t *command(uint8_t *blob, uint32_t index)
{
	return blob + AWG_C1_HEADER_BYTES +
		(size_t)index * AWG_C1_COMMAND_BYTES;
}

static void make_fire(uint8_t *record, uint16_t channel,
		uint32_t advance)
{
	memset(record, 0, AWG_C1_COMMAND_BYTES);
	record[0] = 0x02U;
	put_le16(record + 2U, channel);
	put_le32(record + 8U, advance);
}

static void test_crc_check_value(void)
{
	static const uint8_t input[] = "123456789";

	CHECK(awg_c1_crc64_ecma182(input, sizeof(input) - 1U, 0U) ==
	      UINT64_C(0x6C40DF5F0B497347));
}

static void test_valid_fire_and_boundaries(void)
{
	uint8_t blob[BLOB_BYTES];
	awg_c1_preflight_result_t result;
	awg_c1_preflight_report_t report;
	size_t length = init_blob(blob, 1U, 0U, 1U, AWG_C1_FLAG_EOF);

	make_fire(command(blob, 0U), 0U, 8U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, &result, &report) == 0);
	CHECK(result.command_count == 1U && result.expanded_event_count == 1U);
	CHECK(result.duration_ticks == 8U && result.flags == AWG_C1_FLAG_EOF);

	length = init_blob(blob, 2U, 0U, 2U, AWG_C1_FLAG_EOF);
	make_fire(command(blob, 0U), 0U, 8U);
	make_fire(command(blob, 1U), 1U, 0U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, &result, &report) == 0);

	put_le32(command(blob, 0U) + 8U, 7U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, &result, &report) == -ERANGE);
	CHECK(report.error_mask == AWG_C1_ERROR_RANGE);
	CHECK(report.event_error == AWG_EVENT_VALIDATE_SPACING);
}

static void test_active_event_shape_rejections(void)
{
	uint8_t blob[BLOB_BYTES];
	awg_c1_preflight_report_t report;
	size_t length = init_blob(blob, 1U, 0U, 1U, 0U);

	make_fire(command(blob, 0U), 2U, 8U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -ERANGE);
	CHECK(report.event_error == AWG_EVENT_VALIDATE_CHANNEL);

	make_fire(command(blob, 0U), 0U, 8U);
	command(blob, 0U)[22U] = 1U; /* payload bit 80 */
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -ERANGE);
	CHECK(report.event_error == AWG_EVENT_VALIDATE_PAYLOAD_RESERVED);
}

static void test_repeat_boundary_and_large_count_are_analytical(void)
{
	uint8_t blob[BLOB_BYTES];
	awg_c1_preflight_result_t result;
	awg_c1_preflight_report_t report;
	size_t length = init_blob(blob, 3U, 1U, 2U, AWG_C1_FLAG_EOF);

	command(blob, 0U)[0] = 0x05U;
	put_le32(command(blob, 0U) + 4U, 2U);
	make_fire(command(blob, 1U), 0U, 7U);
	command(blob, 2U)[0] = 0x06U;
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, &result, &report) == -ERANGE);
	CHECK(report.event_error == AWG_EVENT_VALIDATE_SPACING);

	put_le32(command(blob, 0U) + 4U, UINT32_MAX);
	put_le32(command(blob, 1U) + 8U, 8U);
	put_le64(blob + 32U, UINT32_MAX);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, &result, &report) == 0);
	CHECK(result.expanded_event_count == UINT32_MAX);
	CHECK(result.duration_ticks == (uint64_t)UINT32_MAX * 8U);
}

static void test_header_crc_structure_and_count_fail_closed(void)
{
	uint8_t blob[BLOB_BYTES];
	awg_c1_preflight_report_t report;
	size_t length = init_blob(blob, 1U, 0U, 1U, AWG_C1_FLAG_EOF);

	make_fire(command(blob, 0U), 0U, 8U);
	seal_blob(blob, length);
	blob[60U] = 1U;
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -EINVAL);
	CHECK(report.error_mask == AWG_C1_ERROR_BAD_RESERVED);

	blob[60U] = 0U;
	seal_blob(blob, length);
	blob[length - 1U] ^= 1U;
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -EBADMSG);
	CHECK(report.error_mask == AWG_C1_ERROR_BAD_CRC);

	blob[length - 1U] ^= 1U;
	put_le64(blob + 32U, 2U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -EINVAL);
	CHECK(report.error_mask == AWG_C1_ERROR_COUNT_MISMATCH);

	put_le64(blob + 32U, 1U);
	put_le32(blob + 28U, AWG_C1_MAX_REPEAT_DEPTH + 1U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -EINVAL);
	CHECK(report.error_mask == AWG_C1_ERROR_BAD_STRUCTURE);

	put_le32(blob + 28U, 0U);
	put_le64(blob + 16U, UINT64_MAX);
	make_fire(command(blob, 0U), 0U, 1U);
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -ERANGE);
	CHECK(report.error_mask == AWG_C1_ERROR_RANGE);
}

static void test_unused_linear_step_still_obeys_field_range(void)
{
	uint8_t blob[BLOB_BYTES];
	awg_c1_preflight_report_t report;
	uint8_t *linear;
	uint8_t *continuation;
	size_t length = init_blob(blob, 2U, 0U, 1U, AWG_C1_FLAG_EOF);

	linear = command(blob, 0U);
	continuation = command(blob, 1U);
	linear[0] = 0x03U;
	put_le32(linear + 8U, 1U);
	put_le32(linear + 12U, 8U);
	continuation[0] = 0x04U;
	put_le64(continuation + 4U, UINT64_C(0x0000000000010000));
	seal_blob(blob, length);
	CHECK(awg_c1_preflight(blob, length, NULL, &report) == -ERANGE);
	CHECK(report.error_mask == AWG_C1_ERROR_RANGE);
}

int main(void)
{
	test_crc_check_value();
	test_valid_fire_and_boundaries();
	test_active_event_shape_rejections();
	test_repeat_boundary_and_large_count_are_analytical();
	test_header_crc_structure_and_count_fail_closed();
	test_unused_linear_step_still_obeys_field_range();
	if (failures) {
		fprintf(stderr, "awg_c1_validate_test: %u/%u checks failed\n",
			failures, checks);
		return 1;
	}
	printf("awg_c1_validate_test: %u checks passed\n", checks);
	return 0;
}
