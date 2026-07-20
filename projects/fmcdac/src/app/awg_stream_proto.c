#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "awg_sched_regs.h"
#include "awg_stream_proto.h"

static awg_stream_proto_session_t g_awg_stream_session;
static awg_event_v1_t g_awg_stream_events[AWG_STREAM_PROTO_MAX_FRAME_EVENTS];
static awg_event_v1_t
	g_awg_stream_v2_records[AWG_STREAM_PROTO_V2_MAX_FRAME_RECORDS + 1U];

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

static void awg_stream_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
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
	awg_sched_stream_snapshot_t snapshot;

	if (!ack)
		return;

	memset(ack, 0, sizeof(*ack));
	ack->magic = AWG_STREAM_PROTO_MAGIC;
	ack->seq_acked = seq;
	ack->ddr_free_events = awg_sched_stream_ddr_free_events();
	ack->status = status;

	if (awg_sched_stream_get_error_snapshot(&snapshot) == 0) {
		ack->stream_free_events = snapshot.free_space;
		ack->stream_stalls = snapshot.stream_stalls;
		ack->irq_status = snapshot.irq_status;
	}
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
		return AWG_STREAM_PROTO_ACK_RING_FULL;
	}
	if (ret == -EIO)
		return AWG_STREAM_PROTO_ACK_SCHED_ERROR;
	return fallback_status;
}

static int awg_stream_proto_fail(awg_stream_proto_ack_t *ack, uint32_t seq,
				 uint32_t status, int ret)
{
	awg_stream_ack_init(ack, seq, status);
	return ret;
}

static void awg_stream_proto_accept(awg_stream_proto_session_t *session,
				    uint32_t seq,
				    uint32_t frame_crc,
				    const awg_stream_proto_ack_t *ack)
{
	session->last_seq = seq;
	session->last_frame_crc = frame_crc;
	session->next_seq = seq + 1U;
	session->last_ack = *ack;
	session->have_last_ack = true;
}

void awg_stream_proto_session_init(awg_stream_proto_session_t *session)
{
	if (session)
		memset(session, 0, sizeof(*session));
}

void awg_stream_proto_reset_default_session(void)
{
	awg_stream_proto_session_init(&g_awg_stream_session);
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

int awg_stream_proto_handle_frame_ctx(awg_stream_proto_session_t *session,
				      const uint8_t *frame, size_t len,
				      const awg_sched_stream_cfg_t *open_cfg,
				      awg_stream_proto_ack_t *ack)
{
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

	if (!session || !frame || !ack)
		return awg_stream_proto_fail(ack, 0U,
					     AWG_STREAM_PROTO_ACK_BAD_ARG, -EINVAL);

	if (len < (AWG_STREAM_PROTO_HEADER_BYTES + AWG_STREAM_PROTO_CRC_BYTES))
		return awg_stream_proto_fail(ack, 0U,
					     AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EINVAL);

	magic = awg_stream_get_le32(frame);
	seq = awg_stream_get_le32(frame + 4U);
	if (magic != AWG_STREAM_PROTO_MAGIC)
		return awg_stream_proto_fail(ack, seq,
					     AWG_STREAM_PROTO_ACK_BAD_MAGIC, -EINVAL);

	n_events = awg_stream_get_le16(frame + 8U);
	flags = awg_stream_get_le16(frame + 10U);
	if (n_events > AWG_STREAM_PROTO_MAX_FRAME_EVENTS)
		return awg_stream_proto_fail(ack, seq,
					     AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EMSGSIZE);

	expected_len = AWG_STREAM_PROTO_HEADER_BYTES +
		       ((size_t)n_events * sizeof(awg_event_v1_t)) +
		       AWG_STREAM_PROTO_CRC_BYTES;
	if (len != expected_len)
		return awg_stream_proto_fail(ack, seq,
					     AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EINVAL);

	expected_crc = awg_stream_get_le32(frame + len - AWG_STREAM_PROTO_CRC_BYTES);
	actual_crc = awg_stream_proto_crc32_ieee(frame,
						len - AWG_STREAM_PROTO_CRC_BYTES);
	if (expected_crc != actual_crc)
		return awg_stream_proto_fail(ack, seq,
					     AWG_STREAM_PROTO_ACK_BAD_CRC, -EBADMSG);

	if ((flags & ~AWG_STREAM_PROTO_SUPPORTED_FLAGS) != 0U)
		return awg_stream_proto_fail(ack, seq,
					     AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);
	if (((flags & AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF) != 0U) &&
	    n_events == 0U)
		return awg_stream_proto_fail(ack, seq,
					     AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);

	if (session->have_last_ack && seq == session->last_seq &&
	    expected_crc == session->last_frame_crc) {
		*ack = session->last_ack;
		return 0;
	}

	if ((flags & AWG_STREAM_PROTO_FLAG_OPEN) != 0U) {
		if (seq != 0U)
			return awg_stream_proto_fail(ack, seq,
						     AWG_STREAM_PROTO_ACK_BAD_SEQUENCE,
						     -EPROTO);
		if (session->active && !session->closed)
			return awg_stream_proto_fail(ack, seq,
						     AWG_STREAM_PROTO_ACK_BAD_SESSION,
						     -EBUSY);
		if (session->active)
			(void)awg_sched_stream_reset_soft();

		ret = awg_sched_stream_open(open_cfg);
		if (ret != 0)
			return awg_stream_proto_fail(ack, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_OPEN_FAILED), ret);

		session->active = true;
		session->closed = false;
		session->have_last_ack = false;
		session->have_last_event = false;
		session->next_seq = 0U;
	} else {
		if (!session->active || session->closed)
			return awg_stream_proto_fail(ack, seq,
						     AWG_STREAM_PROTO_ACK_BAD_SESSION,
						     -ENOTCONN);
		if (seq != session->next_seq)
			return awg_stream_proto_fail(ack, seq,
						     AWG_STREAM_PROTO_ACK_BAD_SEQUENCE,
						     -EPROTO);
	}

	off = AWG_STREAM_PROTO_HEADER_BYTES;
	for (i = 0U; i < n_events; i++) {
		awg_stream_event_from_le(frame + off, &g_awg_stream_events[i]);
		if ((g_awg_stream_events[i].flags &
		     ~(AWG_SCHED_FLAG_PHASE_REINIT | AWG_SCHED_FLAG_EOF)) != 0U ||
		    (((g_awg_stream_events[i].flags & AWG_SCHED_FLAG_EOF) != 0U) &&
		     ((flags & AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF) == 0U)) ||
		    g_awg_stream_events[i].reserved != 0U ||
		    (session->have_last_event &&
		     g_awg_stream_events[i].timestamp_ticks < session->last_timestamp) ||
		    (i > 0U && g_awg_stream_events[i].timestamp_ticks <
		     g_awg_stream_events[i - 1U].timestamp_ticks))
			return awg_stream_proto_fail(ack, seq,
						     AWG_STREAM_PROTO_ACK_BAD_EVENT,
						     -EINVAL);
		off += sizeof(awg_event_v1_t);
	}

	if ((flags & AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF) != 0U) {
		for (i = 0U; i < n_events; i++)
			g_awg_stream_events[i].flags &= ~AWG_SCHED_FLAG_EOF;
		g_awg_stream_events[n_events - 1U].flags |= AWG_SCHED_FLAG_EOF;
	}

	if (n_events > 0U) {
		ret = awg_sched_stream_push(g_awg_stream_events, n_events);
		if (ret != 0)
			return awg_stream_proto_fail(ack, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_RING_FULL), ret);
		session->last_timestamp = g_awg_stream_events[n_events - 1U].timestamp_ticks;
		session->have_last_event = true;
	}

	if ((flags & AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF) != 0U) {
		/* EOF is already attached to the last supplied event. */
		ret = awg_sched_stream_close(false);
		session->closed = true;
		if (ret != 0) {
			awg_stream_ack_init(ack, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_CLOSE_FAILED));
			awg_stream_proto_accept(session, seq, expected_crc, ack);
			return ret;
		}
	}

	awg_stream_ack_init(ack, seq, AWG_STREAM_PROTO_ACK_OK);
	awg_stream_proto_accept(session, seq, expected_crc, ack);
	return 0;
}

int awg_stream_proto_handle_frame(const uint8_t *frame, size_t len,
				  const awg_sched_stream_cfg_t *open_cfg,
				  awg_stream_proto_ack_t *ack)
{
	return awg_stream_proto_handle_frame_ctx(&g_awg_stream_session, frame, len,
						 open_cfg, ack);
}

void awg_stream_proto_ack_to_le(const awg_stream_proto_ack_t *ack,
				uint8_t out[AWG_STREAM_PROTO_ACK_BYTES])
{
	if (!ack || !out)
		return;

	awg_stream_put_le32(out, ack->magic);
	awg_stream_put_le32(out + 4U, ack->seq_acked);
	awg_stream_put_le32(out + 8U, ack->ddr_free_events);
	awg_stream_put_le32(out + 12U, ack->status);
	awg_stream_put_le32(out + 16U, ack->stream_free_events);
	awg_stream_put_le32(out + 20U, ack->stream_stalls);
	awg_stream_put_le32(out + 24U, ack->irq_status);
}

static void awg_stream_v2_ack_init(awg_stream_proto_v2_ack_t *ack,
		uint32_t session_id, uint32_t seq, uint32_t status)
{
	awg_sched_stream_snapshot_t snapshot;

	if (!ack)
		return;

	memset(ack, 0, sizeof(*ack));
	ack->magic = AWG_STREAM_PROTO_MAGIC;
	ack->version = AWG_STREAM_PROTO_V2_VERSION;
	ack->header_bytes = AWG_STREAM_PROTO_V2_ACK_BYTES;
	ack->session_id = session_id;
	ack->seq_acked = seq;
	ack->status = status;
	ack->free_records = awg_sched_stream_ddr_free_events();

	if (awg_sched_stream_get_error_snapshot(&snapshot) == 0) {
		ack->scheduler_status = snapshot.status;
		ack->stream_free_records = snapshot.free_space;
		ack->stream_stalls = snapshot.stream_stalls;
		ack->irq_status = snapshot.irq_status;
	}
}

static int awg_stream_v2_fail(awg_stream_proto_v2_ack_t *ack,
		uint32_t session_id, uint32_t seq, uint32_t status, int ret)
{
	awg_stream_v2_ack_init(ack, session_id, seq, status);
	return ret;
}

static void awg_stream_v2_accept(awg_stream_proto_v2_session_t *session,
		uint32_t seq, uint32_t frame_crc,
		const awg_stream_proto_v2_ack_t *ack)
{
	session->last_seq = seq;
	session->last_frame_crc = frame_crc;
	session->next_seq = seq + 1U;
	session->last_ack = *ack;
	session->have_last_ack = true;
}

void awg_stream_proto_v2_session_init(
	awg_stream_proto_v2_session_t *session)
{
	if (session)
		memset(session, 0, sizeof(*session));
}

static bool awg_stream_v2_kind_valid(uint8_t kind)
{
	return kind == AWG_STREAM_PROTO_V2_KIND_CONTROL ||
	       kind == AWG_STREAM_PROTO_V2_KIND_EVENTS ||
	       kind == AWG_STREAM_PROTO_V2_KIND_C1;
}

static int awg_stream_v2_select_kind(
	awg_stream_proto_v2_session_t *session, uint8_t kind,
	const awg_sched_stream_cfg_t *open_cfg,
	const awg_stream_proto_v2_ops_t *ops,
	awg_stream_proto_v2_ack_t *ack, uint32_t seq)
{
	int ret;

	if (session->kind_selected) {
		if (session->payload_kind != kind)
			return awg_stream_v2_fail(ack, session->session_id, seq,
				AWG_STREAM_PROTO_ACK_BAD_KIND, -EPROTOTYPE);
		return 0;
	}

	if (kind == AWG_STREAM_PROTO_V2_KIND_C1 &&
	    (!open_cfg->use_dma || !ops || !ops->select_kind))
		return awg_stream_v2_fail(ack, session->session_id, seq,
			AWG_STREAM_PROTO_ACK_C1_DISABLED, -ENOTSUP);

	if (ops && ops->select_kind) {
		ret = ops->select_kind(ops->ctx, kind);
		if (ret)
			return awg_stream_v2_fail(ack, session->session_id, seq,
				ret == -ENOTSUP &&
				kind == AWG_STREAM_PROTO_V2_KIND_C1 ?
				AWG_STREAM_PROTO_ACK_C1_DISABLED :
				AWG_STREAM_PROTO_ACK_DMA_ERROR, ret);
	}

	session->payload_kind = kind;
	session->kind_selected = true;
	return 0;
}

static int awg_stream_v2_push_events(
	awg_stream_proto_v2_session_t *session, const uint8_t *payload,
	uint16_t count)
{
	uint32_t base = session->have_pending_event ? 1U : 0U;
	uint32_t push_count;
	uint32_t i;
	uint64_t previous;
	bool have_previous;
	int ret;

	if (session->have_pending_event)
		g_awg_stream_v2_records[0] = session->pending_event;

	previous = session->last_timestamp;
	have_previous = session->have_last_timestamp;
	for (i = 0U; i < count; i++) {
		awg_event_v1_t *event = &g_awg_stream_v2_records[base + i];

		awg_stream_event_from_le(payload +
			((size_t)i * AWG_STREAM_PROTO_V2_RECORD_BYTES), event);
		if ((event->flags & ~AWG_SCHED_FLAG_PHASE_REINIT) != 0U ||
		    event->reserved != 0U ||
		    (have_previous && event->timestamp_ticks < previous))
			return -EINVAL;
		previous = event->timestamp_ticks;
		have_previous = true;
	}

	/*
	 * Hold exactly one direct event until CONTROL/CLOSE.  This lets EOF be
	 * applied without racing scheduler DMA after the final data-frame ACK.
	 */
	push_count = base + (uint32_t)count - 1U;
	if (push_count > 0U) {
		ret = awg_sched_stream_push(g_awg_stream_v2_records, push_count);
		if (ret)
			return ret;
	}

	session->pending_event = g_awg_stream_v2_records[base + count - 1U];
	session->have_pending_event = true;
	session->last_timestamp = previous;
	session->have_last_timestamp = true;
	return 0;
}

static int awg_stream_v2_push_c1(const uint8_t *payload, uint16_t count)
{
	memcpy(g_awg_stream_v2_records, payload,
	       (size_t)count * AWG_STREAM_PROTO_V2_RECORD_BYTES);
	return awg_sched_stream_push(g_awg_stream_v2_records, count);
}

int awg_stream_proto_v2_handle_frame(
	awg_stream_proto_v2_session_t *session,
	const uint8_t *frame, size_t len,
	const awg_sched_stream_cfg_t *open_cfg,
	const awg_stream_proto_v2_ops_t *ops,
	awg_stream_proto_v2_ack_t *ack)
{
	uint32_t magic;
	uint32_t session_id = 0U;
	uint32_t seq = 0U;
	uint32_t payload_bytes;
	uint32_t expected_crc;
	uint32_t actual_crc;
	uint16_t flags;
	uint16_t count;
	uint16_t header_bytes;
	uint8_t version;
	uint8_t kind;
	size_t expected_len;
	const uint8_t *payload;
	int ret;

	if (!session || !frame || !open_cfg || !ack)
		return awg_stream_v2_fail(ack, 0U, 0U,
			AWG_STREAM_PROTO_ACK_BAD_ARG, -EINVAL);
	if (len < AWG_STREAM_PROTO_V2_HEADER_BYTES +
	    AWG_STREAM_PROTO_CRC_BYTES)
		return awg_stream_v2_fail(ack, 0U, 0U,
			AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EMSGSIZE);

	magic = awg_stream_get_le32(frame);
	version = frame[4U];
	kind = frame[5U];
	flags = awg_stream_get_le16(frame + 6U);
	session_id = awg_stream_get_le32(frame + 8U);
	seq = awg_stream_get_le32(frame + 12U);
	count = awg_stream_get_le16(frame + 16U);
	header_bytes = awg_stream_get_le16(frame + 18U);
	payload_bytes = awg_stream_get_le32(frame + 20U);

	if (magic != AWG_STREAM_PROTO_MAGIC)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_MAGIC, -EINVAL);
	if (version != AWG_STREAM_PROTO_V2_VERSION)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_VERSION, -EPROTONOSUPPORT);
	if (!awg_stream_v2_kind_valid(kind))
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_KIND, -EPROTOTYPE);
	if (header_bytes != AWG_STREAM_PROTO_V2_HEADER_BYTES ||
	    count > AWG_STREAM_PROTO_V2_MAX_FRAME_RECORDS ||
	    payload_bytes != (uint32_t)count *
		AWG_STREAM_PROTO_V2_RECORD_BYTES)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EMSGSIZE);

	expected_len = (size_t)header_bytes + payload_bytes +
		AWG_STREAM_PROTO_CRC_BYTES;
	if (len != expected_len)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EMSGSIZE);
	expected_crc = awg_stream_get_le32(frame + len -
		AWG_STREAM_PROTO_CRC_BYTES);
	actual_crc = awg_stream_proto_crc32_ieee(frame,
		len - AWG_STREAM_PROTO_CRC_BYTES);
	if (expected_crc != actual_crc)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_CRC, -EBADMSG);
	if ((flags & ~AWG_STREAM_PROTO_SUPPORTED_FLAGS) != 0U)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);

	if (session->have_last_ack && session_id == session->session_id &&
	    seq == session->last_seq && expected_crc == session->last_frame_crc) {
		*ack = session->last_ack;
		return 0;
	}

	payload = frame + header_bytes;
	if ((flags & AWG_STREAM_PROTO_FLAG_OPEN) != 0U) {
		if (flags != AWG_STREAM_PROTO_FLAG_OPEN ||
		    kind != AWG_STREAM_PROTO_V2_KIND_CONTROL || count != 1U ||
		    seq != 0U)
			return awg_stream_v2_fail(ack, session_id, seq,
				AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);
		if (session->active && !session->closed)
			return awg_stream_v2_fail(ack, session_id, seq,
				AWG_STREAM_PROTO_ACK_BAD_SESSION, -EBUSY);

		if (ops && ops->prepare) {
			ret = ops->prepare(ops->ctx);
			if (ret)
				return awg_stream_v2_fail(ack, session_id, seq,
					AWG_STREAM_PROTO_ACK_OPEN_FAILED, ret);
		} else if (session->active) {
			ret = awg_sched_stream_reset_soft();
			if (ret)
				return awg_stream_v2_fail(ack, session_id, seq,
					AWG_STREAM_PROTO_ACK_OPEN_FAILED, ret);
		}

		ret = awg_sched_stream_open(open_cfg);
		if (ret)
			return awg_stream_v2_fail(ack, session_id, seq,
				awg_stream_status_from_ret(ret,
					AWG_STREAM_PROTO_ACK_OPEN_FAILED), ret);

		awg_stream_proto_v2_session_init(session);
		session->active = true;
		session->session_id = session_id;
		memcpy(session->program_sha256, payload,
		       AWG_STREAM_PROTO_V2_PROGRAM_HASH_BYTES);
		awg_stream_v2_ack_init(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_OK);
		awg_stream_v2_accept(session, seq, expected_crc, ack);
		return 0;
	}

	if (!session->active || session->closed ||
	    session_id != session->session_id)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_SESSION, -ENOTCONN);
	if (seq != session->next_seq)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_SEQUENCE, -EPROTO);

	if (kind == AWG_STREAM_PROTO_V2_KIND_CONTROL) {
		if (flags != AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF && flags != 0U)
			return awg_stream_v2_fail(ack, session_id, seq,
				AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);
		if (count != 0U || !session->kind_selected)
			return awg_stream_v2_fail(ack, session_id, seq,
				AWG_STREAM_PROTO_ACK_BAD_LENGTH, -EINVAL);
		if (session->payload_kind == AWG_STREAM_PROTO_V2_KIND_C1 &&
		    flags != 0U)
			return awg_stream_v2_fail(ack, session_id, seq,
				AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);

		if (session->payload_kind == AWG_STREAM_PROTO_V2_KIND_EVENTS) {
			if (!session->have_pending_event)
				return awg_stream_v2_fail(ack, session_id, seq,
					AWG_STREAM_PROTO_ACK_BAD_EVENT, -EINVAL);
			if ((flags & AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF) != 0U)
				session->pending_event.flags |= AWG_SCHED_FLAG_EOF;
			ret = awg_sched_stream_push(&session->pending_event, 1U);
			if (ret)
				return awg_stream_v2_fail(ack, session_id, seq,
					awg_stream_status_from_ret(ret,
						AWG_STREAM_PROTO_ACK_RING_FULL), ret);
			session->have_pending_event = false;
		}

		ret = awg_sched_stream_close(false);
		session->closed = true;
		awg_stream_v2_ack_init(ack, session_id, seq,
			ret ? awg_stream_status_from_ret(ret,
				AWG_STREAM_PROTO_ACK_CLOSE_FAILED) :
			AWG_STREAM_PROTO_ACK_OK);
		awg_stream_v2_accept(session, seq, expected_crc, ack);
		return ret;
	}

	if (flags != 0U || count == 0U)
		return awg_stream_v2_fail(ack, session_id, seq,
			AWG_STREAM_PROTO_ACK_BAD_FLAGS, -EINVAL);
	ret = awg_stream_v2_select_kind(session, kind, open_cfg, ops, ack, seq);
	if (ret)
		return ret;

	if (kind == AWG_STREAM_PROTO_V2_KIND_EVENTS)
		ret = awg_stream_v2_push_events(session, payload, count);
	else
		ret = awg_stream_v2_push_c1(payload, count);
	if (ret)
		return awg_stream_v2_fail(ack, session_id, seq,
			ret == -EINVAL ? AWG_STREAM_PROTO_ACK_BAD_EVENT :
			awg_stream_status_from_ret(ret,
				AWG_STREAM_PROTO_ACK_RING_FULL), ret);

	awg_stream_v2_ack_init(ack, session_id, seq, AWG_STREAM_PROTO_ACK_OK);
	awg_stream_v2_accept(session, seq, expected_crc, ack);
	return 0;
}

void awg_stream_proto_v2_ack_to_le(
	const awg_stream_proto_v2_ack_t *ack,
	uint8_t out[AWG_STREAM_PROTO_V2_ACK_BYTES])
{
	if (!ack || !out)
		return;

	awg_stream_put_le32(out, ack->magic);
	out[4U] = ack->version;
	out[5U] = ack->reserved;
	awg_stream_put_le16(out + 6U, ack->header_bytes);
	awg_stream_put_le32(out + 8U, ack->session_id);
	awg_stream_put_le32(out + 12U, ack->seq_acked);
	awg_stream_put_le32(out + 16U, ack->status);
	awg_stream_put_le32(out + 20U, ack->free_records);
	awg_stream_put_le32(out + 24U, ack->scheduler_status);
	awg_stream_put_le32(out + 28U, ack->stream_free_records);
	awg_stream_put_le32(out + 32U, ack->stream_stalls);
	awg_stream_put_le32(out + 36U, ack->irq_status);
}
