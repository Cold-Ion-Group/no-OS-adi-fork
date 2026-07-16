#ifndef AWG_EVENT_H
#define AWG_EVENT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define AWG_EVENT_PACKED __attribute__((__packed__))
#else
#define AWG_EVENT_PACKED
#endif

#if defined(__cplusplus)
#define AWG_EVENT_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define AWG_EVENT_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/*
 * Version-one DDS payload carried in event bits [223:96].  The active HDL
 * interprets the payload as:
 *
 *   payload[15:0]                              scale
 *   payload[16 +: DDS_PHASE_DW]                initial phase
 *   payload[16 + DDS_PHASE_DW +: DDS_PHASE_DW] phase increment
 *
 * Unused upper bits must be zero.  DDS_PHASE_DW is an HDL build parameter;
 * the current AWG build uses 32 bits.
 */
typedef union {
	uint32_t words[4];
	struct AWG_EVENT_PACKED {
		uint32_t word0;
		uint32_t word1;
		uint32_t word2;
		uint32_t word3;
	};
} awg_payload_v1_t;

#define AWG_PAYLOAD_V1_BITS                 128U
#define AWG_PAYLOAD_V1_SCALE_BITS           16U
#define AWG_PAYLOAD_V1_MAX_DDS_PHASE_BITS   56U

static inline uint32_t awg_payload_v1_mask(uint8_t width)
{
	if (width >= 32U)
		return UINT32_MAX;
	if (width == 0U)
		return 0U;

	return (UINT32_C(1) << width) - UINT32_C(1);
}

static inline void awg_payload_v1_write_bits(awg_payload_v1_t *payload,
		uint32_t bit_offset, uint8_t width, uint32_t value)
{
	uint32_t bits_left;

	if (!payload || width == 0U || bit_offset >= AWG_PAYLOAD_V1_BITS)
		return;

	bits_left = width;
	if (bits_left > (AWG_PAYLOAD_V1_BITS - bit_offset))
		bits_left = AWG_PAYLOAD_V1_BITS - bit_offset;

	while (bits_left > 0U) {
		uint32_t word_idx = bit_offset / 32U;
		uint32_t bit_in_word = bit_offset % 32U;
		uint32_t chunk = 32U - bit_in_word;
		uint32_t mask;

		if (chunk > bits_left)
			chunk = bits_left;
		mask = (chunk == 32U) ? UINT32_MAX :
		       ((UINT32_C(1) << chunk) - UINT32_C(1));
		payload->words[word_idx] |= (value & mask) << bit_in_word;
		value = (chunk == 32U) ? 0U : (value >> chunk);
		bit_offset += chunk;
		bits_left -= chunk;
	}
}

static inline void awg_payload_v1_set_dds(awg_payload_v1_t *payload,
		uint16_t scale, uint32_t init, uint32_t incr,
		uint8_t dds_phase_dw)
{
	uint32_t phase_mask;

	if (!payload)
		return;

	memset(payload, 0, sizeof(*payload));
	if (dds_phase_dw > AWG_PAYLOAD_V1_MAX_DDS_PHASE_BITS)
		dds_phase_dw = AWG_PAYLOAD_V1_MAX_DDS_PHASE_BITS;

	phase_mask = awg_payload_v1_mask(dds_phase_dw);
	awg_payload_v1_write_bits(payload, 0U, AWG_PAYLOAD_V1_SCALE_BITS,
				  scale);
	awg_payload_v1_write_bits(payload, AWG_PAYLOAD_V1_SCALE_BITS,
				  dds_phase_dw, init & phase_mask);
	awg_payload_v1_write_bits(payload,
				  AWG_PAYLOAD_V1_SCALE_BITS + dds_phase_dw,
				  dds_phase_dw, incr & phase_mask);
}

/*
 * Canonical 256-bit event format used in DDR and on the scheduler AXI-Stream:
 *
 *   [63:0]    timestamp
 *   [79:64]   channel
 *   [95:80]   flags
 *   [223:96]  payload
 *   [255:224] reserved (zero)
 *
 * The MicroBlaze target is little-endian, so the in-memory 32-bit word at
 * byte offset 8 is (flags << 16) | channel.  The scheduler's MMIO staging
 * register deliberately uses the opposite half-word order:
 * (channel << 16) | flags.  Call awg_event_v1_mmio_word2() for MMIO writes;
 * do not copy the canonical DDR word verbatim into EVT_WDATA2.
 */
typedef struct AWG_EVENT_PACKED {
	uint64_t timestamp_ticks;
	uint16_t channel;
	uint16_t flags;
	awg_payload_v1_t payload;
	uint32_t reserved;
} awg_event_v1_t;

#define AWG_EVENT_V1_WORDS  8U
#define AWG_EVENT_V1_BYTES  32U

#define AWG_EVENT_FLAG_PHASE_REINIT UINT16_C(0x0001)
#define AWG_EVENT_FLAG_EOF          UINT16_C(0x0002)
#define AWG_EVENT_FLAG_ALL          (AWG_EVENT_FLAG_PHASE_REINIT | \
				     AWG_EVENT_FLAG_EOF)

static inline uint32_t awg_event_v1_mmio_word2(uint16_t channel,
		uint16_t flags)
{
	return ((uint32_t)channel << 16) | (uint32_t)flags;
}

static inline uint32_t awg_event_v1_dma_word2(uint16_t channel,
		uint16_t flags)
{
	return ((uint32_t)flags << 16) | (uint32_t)channel;
}

AWG_EVENT_STATIC_ASSERT(sizeof(awg_payload_v1_t) == 16U,
			"awg_payload_v1_t size mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_payload_v1_t, word0) == 0U,
			"payload.word0 offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_payload_v1_t, word1) == 4U,
			"payload.word1 offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_payload_v1_t, word2) == 8U,
			"payload.word2 offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_payload_v1_t, word3) == 12U,
			"payload.word3 offset mismatch");
AWG_EVENT_STATIC_ASSERT(sizeof(awg_event_v1_t) == AWG_EVENT_V1_BYTES,
			"awg_event_v1_t size mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_event_v1_t, timestamp_ticks) == 0U,
			"event.timestamp offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_event_v1_t, channel) == 8U,
			"event.channel offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_event_v1_t, flags) == 10U,
			"event.flags offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_event_v1_t, payload) == 12U,
			"event.payload offset mismatch");
AWG_EVENT_STATIC_ASSERT(offsetof(awg_event_v1_t, reserved) == 28U,
			"event.reserved offset mismatch");

#ifdef __cplusplus
}
#endif

#endif /* AWG_EVENT_H */
