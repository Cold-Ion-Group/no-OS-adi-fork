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
	awg_stream_proto_ack_t last_ack;
} awg_stream_proto_session_t;

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

#ifdef __cplusplus
}
#endif

#endif /* AWG_STREAM_PROTO_H */
