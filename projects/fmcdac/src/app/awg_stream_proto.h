#ifndef AWG_STREAM_PROTO_H
#define AWG_STREAM_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "awg_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_STREAM_PROTO_MAGIC                 0x53415747U
#define AWG_STREAM_PROTO_HEADER_BYTES          12U
#define AWG_STREAM_PROTO_CRC_BYTES             4U
#define AWG_STREAM_PROTO_ACK_BYTES             28U

#ifndef AWG_STREAM_PROTO_MAX_FRAME_EVENTS
#define AWG_STREAM_PROTO_MAX_FRAME_EVENTS      128U
#endif

#define AWG_STREAM_PROTO_FLAG_OPEN             (1U << 0)
#define AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF   (1U << 1)
#define AWG_STREAM_PROTO_SUPPORTED_FLAGS       \
	(AWG_STREAM_PROTO_FLAG_OPEN | AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF)

/* ACK status is a bit mask.  A zero value means that the frame was accepted. */
#define AWG_STREAM_PROTO_ACK_OK                0U
#define AWG_STREAM_PROTO_ACK_BAD_ARG           (1U << 0)
#define AWG_STREAM_PROTO_ACK_BAD_MAGIC         (1U << 1)
#define AWG_STREAM_PROTO_ACK_BAD_LENGTH        (1U << 2)
#define AWG_STREAM_PROTO_ACK_BAD_CRC           (1U << 3)
#define AWG_STREAM_PROTO_ACK_DISABLED          (1U << 4)
#define AWG_STREAM_PROTO_ACK_OPEN_FAILED       (1U << 5)
#define AWG_STREAM_PROTO_ACK_RING_FULL         (1U << 6)
#define AWG_STREAM_PROTO_ACK_OVERFLOW          (1U << 7)
#define AWG_STREAM_PROTO_ACK_SCHED_ERROR       (1U << 8)
#define AWG_STREAM_PROTO_ACK_CLOSE_FAILED      (1U << 9)
#define AWG_STREAM_PROTO_ACK_BAD_SEQUENCE      (1U << 10)
#define AWG_STREAM_PROTO_ACK_BAD_FLAGS         (1U << 11)
#define AWG_STREAM_PROTO_ACK_BAD_SESSION       (1U << 12)
#define AWG_STREAM_PROTO_ACK_DMA_ERROR         (1U << 13)
#define AWG_STREAM_PROTO_ACK_BAD_EVENT         (1U << 14)
#define AWG_STREAM_PROTO_ACK_BAD_VERSION       (1U << 15)
#define AWG_STREAM_PROTO_ACK_BAD_KIND          (1U << 16)
#define AWG_STREAM_PROTO_ACK_C1_DISABLED       (1U << 17)

/* Compatibility name used by the UART transport. */
#define AWG_STREAM_PROTO_ACK_DDR_FULL          AWG_STREAM_PROTO_ACK_RING_FULL

typedef struct {
	uint32_t magic;
	uint32_t seq_acked;
	uint32_t ddr_free_events;
	uint32_t status;
	uint32_t stream_free_events;
	uint32_t stream_stalls;
	uint32_t irq_status;
} awg_stream_proto_ack_t;

typedef struct {
	bool active;
	bool closed;
	bool have_last_ack;
	bool have_last_event;
	uint32_t next_seq;
	uint32_t last_seq;
	uint32_t last_frame_crc;
	uint64_t last_timestamp;
	awg_event_validation_state_t event_validation;
	awg_stream_proto_ack_t last_ack;
} awg_stream_proto_session_t;

/*
 * GWAS/2 is the production UDP protocol.  The original format above remains
 * the UART/diagnostic compatibility ABI and is intentionally not overloaded
 * with descriptor records.
 *
 * Wire header (little endian): <IBBHIIHHI>, followed by N 32-byte records and
 * an IEEE CRC32 over the header and records.  OPEN is a CONTROL frame carrying
 * exactly one SHA-256 record.  A zero-record CONTROL frame closes the stream.
 */
#define AWG_STREAM_PROTO_V2_VERSION             2U
#define AWG_STREAM_PROTO_V2_KIND_CONTROL        0U
#define AWG_STREAM_PROTO_V2_KIND_EVENTS         1U
#define AWG_STREAM_PROTO_V2_KIND_C1             2U
#define AWG_STREAM_PROTO_V2_HEADER_BYTES        24U
#define AWG_STREAM_PROTO_V2_ACK_BYTES           40U
#define AWG_STREAM_PROTO_V2_RECORD_BYTES        32U
#define AWG_STREAM_PROTO_V2_PROGRAM_HASH_BYTES  32U

#ifndef AWG_STREAM_PROTO_V2_MAX_FRAME_RECORDS
#define AWG_STREAM_PROTO_V2_MAX_FRAME_RECORDS   128U
#endif

typedef struct {
	uint32_t magic;
	uint8_t version;
	uint8_t reserved;
	uint16_t header_bytes;
	uint32_t session_id;
	uint32_t seq_acked;
	uint32_t status;
	uint32_t free_records;
	uint32_t scheduler_status;
	uint32_t stream_free_records;
	uint32_t stream_stalls;
	uint32_t irq_status;
} awg_stream_proto_v2_ack_t;

typedef int (*awg_stream_proto_v2_prepare_fn)(void *ctx);
typedef int (*awg_stream_proto_v2_select_kind_fn)(void *ctx,
		uint8_t payload_kind);
typedef int (*awg_stream_proto_v2_finalize_kind_fn)(void *ctx,
		uint8_t payload_kind);

typedef struct {
	awg_stream_proto_v2_prepare_fn prepare;
	awg_stream_proto_v2_select_kind_fn select_kind;
	awg_stream_proto_v2_finalize_kind_fn finalize_kind;
	void *ctx;
} awg_stream_proto_v2_ops_t;

typedef struct {
	bool active;
	bool closed;
	bool have_last_ack;
	bool kind_selected;
	bool have_pending_event;
	bool have_last_timestamp;
	bool c1_preflight_complete;
	uint8_t payload_kind;
	uint32_t session_id;
	uint32_t next_seq;
	uint32_t last_seq;
	uint32_t last_frame_crc;
	uint64_t last_timestamp;
	awg_event_validation_state_t event_validation;
	uint8_t program_sha256[AWG_STREAM_PROTO_V2_PROGRAM_HASH_BYTES];
	awg_event_v1_t pending_event;
	awg_stream_proto_v2_ack_t last_ack;
} awg_stream_proto_v2_session_t;

void awg_stream_proto_session_init(awg_stream_proto_session_t *session);
void awg_stream_proto_reset_default_session(void);
uint32_t awg_stream_proto_crc32_ieee(const uint8_t *data, size_t len);
int awg_stream_proto_handle_frame_ctx(awg_stream_proto_session_t *session,
				      const uint8_t *frame, size_t len,
				      const awg_sched_stream_cfg_t *open_cfg,
				      awg_stream_proto_ack_t *ack);
int awg_stream_proto_handle_frame(const uint8_t *frame, size_t len,
				  const awg_sched_stream_cfg_t *open_cfg,
				  awg_stream_proto_ack_t *ack);
void awg_stream_proto_ack_to_le(const awg_stream_proto_ack_t *ack,
				uint8_t out[AWG_STREAM_PROTO_ACK_BYTES]);

void awg_stream_proto_v2_session_init(
	awg_stream_proto_v2_session_t *session);
int awg_stream_proto_v2_handle_frame(
	awg_stream_proto_v2_session_t *session,
	const uint8_t *frame, size_t len,
	const awg_sched_stream_cfg_t *open_cfg,
	const awg_stream_proto_v2_ops_t *ops,
	awg_stream_proto_v2_ack_t *ack);
void awg_stream_proto_v2_ack_to_le(
	const awg_stream_proto_v2_ack_t *ack,
	uint8_t out[AWG_STREAM_PROTO_V2_ACK_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* AWG_STREAM_PROTO_H */
