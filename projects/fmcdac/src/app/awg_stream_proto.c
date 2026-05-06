#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "awg_sched_regs.h"
#include "awg_stream_proto.h"

#define AWG_STREAM_PROTO_HEADER_BYTES 12U
#define AWG_STREAM_PROTO_CRC_BYTES    4U

static uint16_t awg_stream_get_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t awg_stream_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] |
	       ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void awg_stream_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void awg_stream_event_from_le(const uint8_t *p, awg_event_v1_t *ev)
{
	memset(ev, 0, sizeof(*ev));
	ev->timestamp_ticks = (uint64_t)awg_stream_get_le32(p) |
			      ((uint64_t)awg_stream_get_le32(p + 4U) << 32);
	ev->channel = awg_stream_get_le16(p + 8U);
	ev->flags = awg_stream_get_le16(p + 10U);
	ev->payload.word0 = awg_stream_get_le32(p + 12U);
	ev->payload.word1 = awg_stream_get_le32(p + 16U);
	ev->payload.word2 = awg_stream_get_le32(p + 20U);
	ev->payload.word3 = awg_stream_get_le32(p + 24U);
	ev->reserved = awg_stream_get_le32(p + 28U);
}

static void awg_stream_ack_init(awg_stream_proto_ack_t *ack, uint32_t seq,
				uint32_t status)
{
	if (!ack)
		return;

	ack->magic = AWG_STREAM_PROTO_MAGIC;
	ack->seq_acked = seq;
	ack->ddr_free_events = awg_sched_stream_ddr_free_events();
	ack->status = status;
}

static uint32_t awg_stream_status_from_ret(int ret, uint32_t fallback_status)
{
	awg_sched_stream_snapshot_t snapshot;

	if (ret == 0)
		return AWG_STREAM_PROTO_ACK_OK;

	if (ret == -ENOTSUP)
		return AWG_STREAM_PROTO_ACK_DISABLED;

	if (ret == -EAGAIN) {
		if (awg_sched_stream_get_error_snapshot(&snapshot) == 0 &&
		    (snapshot.stream_ctrl & AWG_SCHED_STREAM_CTRL_OVERFLOW) != 0U)
			return AWG_STREAM_PROTO_ACK_OVERFLOW;

		return AWG_STREAM_PROTO_ACK_DDR_FULL;
	}

	if (ret == -EIO)
		return AWG_STREAM_PROTO_ACK_SCHED_ERROR;

	return fallback_status;
}

uint32_t awg_stream_proto_crc32_ieee(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFU;
	size_t i;
	uint8_t bit;

	if (!data && len > 0U)
		return 0U;

	for (i = 0U; i < len; i++) {
		crc ^= data[i];
		for (bit = 0U; bit < 8U; bit++) {
			if ((crc & 1U) != 0U)
				crc = (crc >> 1) ^ 0xEDB88320U;
			else
				crc >>= 1;
		}
	}

	return ~crc;
}

int awg_stream_proto_handle_frame(const uint8_t *frame, size_t len,
				  const awg_sched_stream_cfg_t *open_cfg,
				  awg_stream_proto_ack_t *ack)
{
	awg_event_v1_t ev;
	uint32_t magic;
	uint32_t seq = 0U;
	uint32_t expected_crc;
	uint32_t actual_crc;
	uint16_t n_events;
	uint16_t flags;
	size_t expected_len;
	size_t off;
	uint32_t i;
	int ret;

	if (!frame || !ack) {
		awg_stream_ack_init(ack, 0U, AWG_STREAM_PROTO_ACK_BAD_ARG);
		return -EINVAL;
	}

	if (len < (AWG_STREAM_PROTO_HEADER_BYTES + AWG_STREAM_PROTO_CRC_BYTES)) {
		awg_stream_ack_init(ack, 0U, AWG_STREAM_PROTO_ACK_BAD_LENGTH);
		return -EINVAL;
	}

	magic = awg_stream_get_le32(frame);
	seq = awg_stream_get_le32(frame + 4U);
	if (magic != AWG_STREAM_PROTO_MAGIC) {
		awg_stream_ack_init(ack, seq, AWG_STREAM_PROTO_ACK_BAD_MAGIC);
		return -EINVAL;
	}

	n_events = awg_stream_get_le16(frame + 8U);
	flags = awg_stream_get_le16(frame + 10U);
	expected_len = AWG_STREAM_PROTO_HEADER_BYTES +
		       ((size_t)n_events * sizeof(awg_event_v1_t)) +
		       AWG_STREAM_PROTO_CRC_BYTES;

	if (len != expected_len) {
		awg_stream_ack_init(ack, seq, AWG_STREAM_PROTO_ACK_BAD_LENGTH);
		return -EINVAL;
	}

	expected_crc = awg_stream_get_le32(frame + len - AWG_STREAM_PROTO_CRC_BYTES);
	actual_crc = awg_stream_proto_crc32_ieee(frame,
						len - AWG_STREAM_PROTO_CRC_BYTES);
	if (expected_crc != actual_crc) {
		awg_stream_ack_init(ack, seq, AWG_STREAM_PROTO_ACK_BAD_CRC);
		return -EINVAL;
	}

	if ((flags & AWG_STREAM_PROTO_FLAG_OPEN) != 0U) {
		ret = awg_sched_stream_open(open_cfg);
		if (ret != 0) {
			awg_stream_ack_init(ack, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_OPEN_FAILED));
			return ret;
		}
	}

	off = AWG_STREAM_PROTO_HEADER_BYTES;
	for (i = 0U; i < n_events; i++) {
		awg_stream_event_from_le(frame + off, &ev);
		ret = awg_sched_stream_push(&ev, 1U);
		if (ret != 0) {
			awg_stream_ack_init(ack, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_DDR_FULL));
			return ret;
		}
		off += sizeof(awg_event_v1_t);
	}

	if ((flags & AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF) != 0U) {
		ret = awg_sched_stream_close(true);
		if (ret != 0) {
			awg_stream_ack_init(ack, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_CLOSE_FAILED));
			return ret;
		}
	}

	awg_stream_ack_init(ack, seq, AWG_STREAM_PROTO_ACK_OK);
	return 0;
}

void awg_stream_proto_ack_to_le(const awg_stream_proto_ack_t *ack,
				uint8_t out[16])
{
	if (!ack || !out)
		return;

	awg_stream_put_le32(out, ack->magic);
	awg_stream_put_le32(out + 4U, ack->seq_acked);
	awg_stream_put_le32(out + 8U, ack->ddr_free_events);
	awg_stream_put_le32(out + 12U, ack->status);
}
