#ifndef AWG_C1_VALIDATE_H
#define AWG_C1_VALIDATE_H

#include <stddef.h>
#include <stdint.h>

#include "awg_event_validate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_C1_MAGIC_U32                 UINT32_C(0x43475741)
#define AWG_C1_MAJOR                     1U
#define AWG_C1_MINOR                     0U
#define AWG_C1_HEADER_BYTES              64U
#define AWG_C1_COMMAND_BYTES             32U
#define AWG_C1_MAX_COMMAND_RECORDS       4096U
#define AWG_C1_MAX_REPEAT_DEPTH          16U
#define AWG_C1_FLAG_EOF                  (1U << 0)

#define AWG_C1_ERROR_BAD_HEADER          (1U << 0)
#define AWG_C1_ERROR_BAD_VERSION         (1U << 1)
#define AWG_C1_ERROR_BAD_SIZE            (1U << 2)
#define AWG_C1_ERROR_BAD_FLAGS           (1U << 3)
#define AWG_C1_ERROR_BAD_RESERVED        (1U << 4)
#define AWG_C1_ERROR_BAD_CRC             (1U << 5)
#define AWG_C1_ERROR_BAD_OPCODE          (1U << 6)
#define AWG_C1_ERROR_BAD_STRUCTURE       (1U << 7)
#define AWG_C1_ERROR_COUNT_MISMATCH      (1U << 8)
#define AWG_C1_ERROR_RANGE               (1U << 9)

typedef struct {
	uint32_t command_count;
	uint32_t max_repeat_depth;
	uint64_t expanded_event_count;
	uint64_t crc64;
	uint64_t duration_ticks;
	uint32_t flags;
} awg_c1_preflight_result_t;

typedef struct {
	uint32_t error_mask;
	uint32_t command_index;
	awg_event_validation_code_t event_error;
} awg_c1_preflight_report_t;

uint64_t awg_c1_crc64_ecma182(const uint8_t *data, size_t length,
				      uint64_t initial);

/* Validate the complete C1 blob before any record is released to DMA. */
int awg_c1_preflight(const void *blob, size_t length,
		     awg_c1_preflight_result_t *result,
		     awg_c1_preflight_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* AWG_C1_VALIDATE_H */
