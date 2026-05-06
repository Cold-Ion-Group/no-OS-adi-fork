#ifndef AWG_STREAM_PROTO_H
#define AWG_STREAM_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "awg_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWG_STREAM_PROTO_MAGIC              0x53415747U
#define AWG_STREAM_PROTO_FLAG_OPEN          (1U << 0)
#define AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF (1U << 1)

#define AWG_STREAM_PROTO_ACK_OK             0U
#define AWG_STREAM_PROTO_ACK_BAD_ARG        1U
#define AWG_STREAM_PROTO_ACK_BAD_MAGIC      2U
#define AWG_STREAM_PROTO_ACK_BAD_LENGTH     3U
#define AWG_STREAM_PROTO_ACK_BAD_CRC        4U
#define AWG_STREAM_PROTO_ACK_DISABLED       5U
#define AWG_STREAM_PROTO_ACK_OPEN_FAILED    6U
#define AWG_STREAM_PROTO_ACK_DDR_FULL       7U
#define AWG_STREAM_PROTO_ACK_OVERFLOW       8U
#define AWG_STREAM_PROTO_ACK_SCHED_ERROR    9U
#define AWG_STREAM_PROTO_ACK_CLOSE_FAILED   10U

typedef struct {
	uint32_t magic;
	uint32_t seq_acked;
	uint32_t ddr_free_events;
	uint32_t status;
} awg_stream_proto_ack_t;

uint32_t awg_stream_proto_crc32_ieee(const uint8_t *data, size_t len);
int awg_stream_proto_handle_frame(const uint8_t *frame, size_t len,
				  const awg_sched_stream_cfg_t *open_cfg,
				  awg_stream_proto_ack_t *ack);
void awg_stream_proto_ack_to_le(const awg_stream_proto_ack_t *ack,
				uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* AWG_STREAM_PROTO_H */
