#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "awg_phase_f.h"

#define TEST_EXTENSION_BASE UINT32_C(0x44AE0000)

static uint32_t registers[0x200U / sizeof(uint32_t)];
static uint32_t records_high_reads;
static unsigned checks;
static unsigned failures;

#define CHECK(_condition) do { \
	checks++; \
	if (!(_condition)) { \
		fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #_condition); \
		failures++; \
	} \
} while (0)

int no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *value)
{
	if (base != TEST_EXTENSION_BASE || !value ||
	    offset >= sizeof(registers) || (offset & 3U) != 0U)
		return -EINVAL;

	/* Force one rollover retry through the high-low-high read algorithm. */
	if (offset == AWG_PHASE_F_AWGC_REG_RECORDS_HI) {
		records_high_reads++;
		*value = records_high_reads == 1U ? 1U : 2U;
		return 0;
	}
	*value = registers[offset / sizeof(uint32_t)];
	return 0;
}

int no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t value)
{
	(void)base;
	(void)offset;
	(void)value;
	return -ENOTSUP;
}

static void set_u64(uint32_t low_offset, uint32_t high_offset, uint64_t value)
{
	registers[low_offset / sizeof(uint32_t)] = (uint32_t)value;
	registers[high_offset / sizeof(uint32_t)] = (uint32_t)(value >> 32);
}

int main(void)
{
	struct awg_phase_f phase;
	struct awg_phase_f_streamstatus2 status;
	int32_t ret;

	memset(&phase, 0, sizeof(phase));
	memset(registers, 0, sizeof(registers));
	CHECK(!awg_phase_f_scheduler_release_allowed(NULL));
	CHECK(awg_phase_f_scheduler_release_allowed(&phase));
	phase.selected_payload_kind = AWG_STREAM_PROTO_V2_KIND_C1;
	CHECK(!awg_phase_f_scheduler_release_allowed(&phase));
	phase.protocol.c1_preflight_complete = true;
	CHECK(awg_phase_f_scheduler_release_allowed(&phase));
	phase.config.extension_base = TEST_EXTENSION_BASE;
	phase.selected_payload_kind = AWG_STREAM_PROTO_V2_KIND_C1;

	registers[AWG_PHASE_F_AWGX_REG_ID / 4U] = AWG_PHASE_F_AWGX_ID;
	registers[AWG_PHASE_F_AWGX_REG_VERSION / 4U] =
		AWG_PHASE_F_AWGX_VERSION;
	registers[AWG_PHASE_F_AWGX_REG_CAPS / 4U] = UINT32_C(0x1F);
	registers[AWG_PHASE_F_AWGX_REG_CONTROL / 4U] = 1U;
	registers[AWG_PHASE_F_AWGX_REG_STATUS / 4U] = 3U;
	registers[AWG_PHASE_F_AWGC_REG_ID / 4U] = AWG_PHASE_F_AWGC_ID;
	registers[AWG_PHASE_F_AWGC_REG_VERSION / 4U] =
		AWG_PHASE_F_AWGC_VERSION;
	registers[AWG_PHASE_F_AWGC_REG_CAPS / 4U] = AWG_PHASE_F_AWGC_CAPS;
	registers[AWG_PHASE_F_AWGC_REG_STATUS / 4U] = 7U;
	registers[AWG_PHASE_F_AWGC_REG_ERROR / 4U] = UINT32_C(0x200);
	registers[AWG_PHASE_F_AWGC_REG_RECORDS_LO / 4U] = UINT32_C(0x89ABCDEF);
	set_u64(AWG_PHASE_F_AWGC_REG_EVENTS_LO,
		AWG_PHASE_F_AWGC_REG_EVENTS_HI, UINT64_C(0x100000002));
	set_u64(AWG_PHASE_F_AWGC_REG_BUSY_LO,
		AWG_PHASE_F_AWGC_REG_BUSY_HI, UINT64_C(0x300000004));
	set_u64(AWG_PHASE_F_AWGC_REG_STALL_LO,
		AWG_PHASE_F_AWGC_REG_STALL_HI, UINT64_C(0x500000006));
	set_u64(AWG_PHASE_F_AWGC_REG_OUTPUT_CRC_LO,
		AWG_PHASE_F_AWGC_REG_OUTPUT_CRC_HI,
		UINT64_C(0x1122334455667788));
	set_u64(AWG_PHASE_F_AWGC_REG_DECLARED_LO,
		AWG_PHASE_F_AWGC_REG_DECLARED_HI, UINT64_C(0x700000008));
	registers[AWG_PHASE_F_AWGC_REG_MAX_DEPTH / 4U] = 4U;
	registers[AWG_PHASE_F_AWGC_REG_MAX_COMMANDS / 4U] = 4096U;

	CHECK(awg_phase_f_read_streamstatus2(NULL, &status) == -EINVAL);
	CHECK(awg_phase_f_read_streamstatus2(&phase, &status) == -EINVAL);
	phase.initialized = true;
	ret = awg_phase_f_read_streamstatus2(&phase, &status);
	CHECK(ret == 0);
	CHECK(status.version == AWG_PHASE_F_STREAMSTATUS2_VERSION);
	CHECK(status.selected_payload_kind == AWG_STREAM_PROTO_V2_KIND_C1);
	CHECK(status.c1_preflight_complete);
	CHECK(status.awgx_id == AWG_PHASE_F_AWGX_ID);
	CHECK(status.awgx_version == AWG_PHASE_F_AWGX_VERSION);
	CHECK(status.awgx_caps == UINT32_C(0x1F));
	CHECK(status.awgx_control == 1U);
	CHECK(status.awgx_status == 3U);
	CHECK(status.awgc_id == AWG_PHASE_F_AWGC_ID);
	CHECK(status.awgc_version == AWG_PHASE_F_AWGC_VERSION);
	CHECK(status.awgc_caps == AWG_PHASE_F_AWGC_CAPS);
	CHECK(status.awgc_status == 7U);
	CHECK(status.awgc_error == UINT32_C(0x200));
	CHECK(status.accepted_commands == UINT64_C(0x0000000289ABCDEF));
	CHECK(records_high_reads == 4U);
	CHECK(status.emitted_logical_events == UINT64_C(0x100000002));
	CHECK(status.busy_cycles == UINT64_C(0x300000004));
	CHECK(status.stall_cycles == UINT64_C(0x500000006));
	CHECK(status.logical_output_crc == UINT64_C(0x1122334455667788));
	CHECK(status.declared_logical_events == UINT64_C(0x700000008));
	CHECK(status.maximum_repeat_depth == 4U);
	CHECK(status.maximum_commands == 4096U);

	if (failures != 0U) {
		fprintf(stderr, "%u/%u telemetry checks failed\n", failures, checks);
		return 1;
	}
	printf("awg_phase_f telemetry: %u checks passed\n", checks);
	return 0;
}
