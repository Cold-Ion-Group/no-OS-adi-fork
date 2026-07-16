#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "awg_stream_proto.h"

#define TEST_MAX_EVENTS 4U
#define TEST_FRAME_BYTES (AWG_STREAM_PROTO_HEADER_BYTES + \
	(TEST_MAX_EVENTS * AWG_EVENT_V1_BYTES) + AWG_STREAM_PROTO_CRC_BYTES)

typedef struct {
	uint32_t open_calls;
	uint32_t reset_calls;
	uint32_t push_calls;
	uint32_t close_calls;
	uint32_t attempted_push_events;
	uint32_t pushed_events;
	bool last_close_send_eof;
	int open_ret;
	int reset_ret;
	int push_ret;
	int close_ret;
	int snapshot_ret;
	uint32_t ddr_free_events;
	awg_sched_stream_cfg_t open_cfg;
	awg_sched_stream_snapshot_t snapshot;
	awg_event_v1_t pushed[TEST_MAX_EVENTS];
} sched_stub_t;

static sched_stub_t g_sched;

static void sched_stub_reset(void)
{
	memset(&g_sched, 0, sizeof(g_sched));
	g_sched.ddr_free_events = UINT32_C(0x01020304);
	g_sched.snapshot.free_space = UINT32_C(0x11121314);
	g_sched.snapshot.stream_stalls = UINT32_C(0x21222324);
	g_sched.snapshot.irq_status = UINT32_C(0x31323334);
}

int awg_sched_stream_open(const awg_sched_stream_cfg_t *cfg)
{
	g_sched.open_calls++;
	if (cfg)
		g_sched.open_cfg = *cfg;
	return g_sched.open_ret;
}

int awg_sched_stream_push(const awg_event_v1_t *events, uint32_t count)
{
	g_sched.push_calls++;
	g_sched.attempted_push_events += count;
	if (g_sched.push_ret != 0)
		return g_sched.push_ret;
	if (!events || count > TEST_MAX_EVENTS)
		return -EINVAL;

	memcpy(g_sched.pushed, events, count * sizeof(*events));
	g_sched.pushed_events = count;
	return 0;
}

int awg_sched_stream_close(bool send_eof)
{
	g_sched.close_calls++;
	g_sched.last_close_send_eof = send_eof;
	return g_sched.close_ret;
}

uint32_t awg_sched_stream_ddr_free_events(void)
{
	return g_sched.ddr_free_events;
}

int awg_sched_stream_get_error_snapshot(awg_sched_stream_snapshot_t *snapshot)
{
	if (g_sched.snapshot_ret != 0)
		return g_sched.snapshot_ret;
	if (!snapshot)
		return -EINVAL;

	*snapshot = g_sched.snapshot;
	return 0;
}

int awg_sched_stream_reset_soft(void)
{
	g_sched.reset_calls++;
	return g_sched.reset_ret;
}

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

static void event_to_le(uint8_t out[AWG_EVENT_V1_BYTES],
			const awg_event_v1_t *event)
{
	put_le64(out, event->timestamp_ticks);
	put_le16(out + 8U, event->channel);
	put_le16(out + 10U, event->flags);
	put_le32(out + 12U, event->payload.word0);
	put_le32(out + 16U, event->payload.word1);
	put_le32(out + 20U, event->payload.word2);
	put_le32(out + 24U, event->payload.word3);
	put_le32(out + 28U, event->reserved);
}

static awg_event_v1_t make_event(uint64_t timestamp, uint16_t channel,
				 uint16_t flags, uint32_t seed)
{
	awg_event_v1_t event;

	memset(&event, 0, sizeof(event));
	event.timestamp_ticks = timestamp;
	event.channel = channel;
	event.flags = flags;
	event.payload.word0 = seed;
	event.payload.word1 = seed + 1U;
	event.payload.word2 = seed + 2U;
	event.payload.word3 = seed + 3U;
	return event;
}

static size_t build_frame(uint8_t frame[TEST_FRAME_BYTES], uint32_t seq,
			  uint16_t flags, const awg_event_v1_t *events,
			  uint16_t count)
{
	size_t crc_offset;
	uint16_t i;

	if (count > TEST_MAX_EVENTS || (count > 0U && !events))
		return 0U;

	put_le32(frame, AWG_STREAM_PROTO_MAGIC);
	put_le32(frame + 4U, seq);
	put_le16(frame + 8U, count);
	put_le16(frame + 10U, flags);
	for (i = 0U; i < count; i++)
		event_to_le(frame + AWG_STREAM_PROTO_HEADER_BYTES +
			    ((size_t)i * AWG_EVENT_V1_BYTES), &events[i]);

	crc_offset = AWG_STREAM_PROTO_HEADER_BYTES +
		     ((size_t)count * AWG_EVENT_V1_BYTES);
	put_le32(frame + crc_offset,
		 awg_stream_proto_crc32_ieee(frame, crc_offset));
	return crc_offset + AWG_STREAM_PROTO_CRC_BYTES;
}

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
			__func__, __LINE__, #condition); \
		return false; \
	} \
} while (0)

static awg_sched_stream_cfg_t test_open_cfg(void)
{
	awg_sched_stream_cfg_t cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.staging_capacity = 64U;
	cfg.low_wmark_events = 8U;
	cfg.refill_chunk_max = 16U;
	cfg.poll_interval_us = 100U;
	return cfg;
}

static bool test_crc_golden_vector(void)
{
	static const uint8_t input[] = "123456789";

	CHECK(awg_stream_proto_crc32_ieee(input, sizeof(input) - 1U) ==
	      UINT32_C(0xCBF43926));
	CHECK(awg_stream_proto_crc32_ieee(NULL, 0U) == 0U);
	return true;
}

static bool test_open_next_and_duplicate(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_stream_proto_ack_t first_ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	awg_event_v1_t event = make_event(100U, 2U, 0U, UINT32_C(0x1000));
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_OK);
	CHECK(ack.seq_acked == 0U);
	CHECK(ack.ddr_free_events == g_sched.ddr_free_events);
	CHECK(ack.stream_free_events == g_sched.snapshot.free_space);
	CHECK(ack.stream_stalls == g_sched.snapshot.stream_stalls);
	CHECK(ack.irq_status == g_sched.snapshot.irq_status);
	CHECK(session.active && !session.closed && session.next_seq == 1U);
	CHECK(g_sched.open_calls == 1U);
	CHECK(g_sched.push_calls == 1U && g_sched.pushed_events == 1U);
	CHECK(g_sched.pushed[0].timestamp_ticks == event.timestamp_ticks);
	first_ack = ack;

	/* A duplicate of the most recently accepted frame returns its cached ACK. */
	memset(&ack, 0, sizeof(ack));
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(memcmp(&ack, &first_ack, sizeof(ack)) == 0);
	CHECK(g_sched.open_calls == 1U);
	CHECK(g_sched.push_calls == 1U);

	event = make_event(101U, 2U, 0U, UINT32_C(0x2000));
	len = build_frame(frame, 1U, 0U, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_OK);
	CHECK(session.last_seq == 1U && session.next_seq == 2U);
	CHECK(g_sched.push_calls == 2U);
	return true;
}

static bool test_gap_and_out_of_order_sequence(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	awg_event_v1_t event = make_event(200U, 0U, 0U, 1U);
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);

	len = build_frame(frame, 2U, 0U, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -EPROTO);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_SEQUENCE);
	CHECK(g_sched.push_calls == 0U);

	len = build_frame(frame, 1U, 0U, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(g_sched.push_calls == 1U);

	/* Sequence zero is now stale rather than the most recent duplicate. */
	len = build_frame(frame, 0U, 0U, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -EPROTO);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_SEQUENCE);
	CHECK(g_sched.push_calls == 1U);
	return true;
}

static bool test_unsupported_flags(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U,
			  AWG_STREAM_PROTO_FLAG_OPEN | UINT16_C(0x8000),
			  NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -EINVAL);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_FLAGS);
	CHECK(g_sched.open_calls == 0U);
	return true;
}

static bool test_bad_crc(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, NULL, 0U);
	frame[len - 1U] ^= UINT8_C(0x80);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -EBADMSG);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_CRC);
	CHECK(g_sched.open_calls == 0U);
	return true;
}

static bool test_empty_close_with_eof_rejected(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	len = build_frame(frame, 1U, AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF,
			  NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -EINVAL);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_FLAGS);
	CHECK(g_sched.push_calls == 0U && g_sched.close_calls == 0U);
	return true;
}

static bool test_eof_on_final_event_and_post_close(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	awg_event_v1_t events[2];
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);

	/* Input EOF bits are normalized: only the frame's final event keeps EOF. */
	events[0] = make_event(300U, 1U,
			       AWG_SCHED_FLAG_PHASE_REINIT | AWG_SCHED_FLAG_EOF, 10U);
	events[1] = make_event(301U, 1U, 0U, 20U);
	len = build_frame(frame, 1U, AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF,
			  events, 2U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(g_sched.push_calls == 1U && g_sched.pushed_events == 2U);
	CHECK((g_sched.pushed[0].flags & AWG_SCHED_FLAG_EOF) == 0U);
	CHECK((g_sched.pushed[0].flags & AWG_SCHED_FLAG_PHASE_REINIT) != 0U);
	CHECK((g_sched.pushed[1].flags & AWG_SCHED_FLAG_EOF) != 0U);
	CHECK(g_sched.close_calls == 1U && !g_sched.last_close_send_eof);
	CHECK(session.closed);

	events[0] = make_event(302U, 1U, 0U, 30U);
	len = build_frame(frame, 2U, 0U, events, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -ENOTCONN);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_SESSION);
	CHECK(g_sched.push_calls == 1U);
	return true;
}

static bool test_new_open_resets_closed_session(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	awg_event_v1_t event = make_event(400U, 3U, 0U, 40U);
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	len = build_frame(frame, 1U, AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF,
			  &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(session.closed);

	/* A fresh OPEN is sequence zero and resets the prior closed stream. */
	event = make_event(1U, 3U, 0U, 50U);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_OK);
	CHECK(g_sched.reset_calls == 1U);
	CHECK(g_sched.open_calls == 2U);
	CHECK(!session.closed && session.next_seq == 1U);
	CHECK(g_sched.pushed[0].timestamp_ticks == 1U);
	return true;
}

static bool test_ring_full_status_is_distinct(void)
{
	awg_stream_proto_session_t session;
	awg_stream_proto_ack_t ack;
	awg_sched_stream_cfg_t cfg = test_open_cfg();
	awg_event_v1_t event = make_event(500U, 0U, 0U, 60U);
	uint8_t frame[TEST_FRAME_BYTES];
	size_t len;

	sched_stub_reset();
	awg_stream_proto_session_init(&session);
	len = build_frame(frame, 0U, AWG_STREAM_PROTO_FLAG_OPEN, NULL, 0U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == 0);

	g_sched.push_ret = -EAGAIN;
	g_sched.snapshot.stream_ctrl = 0U;
	len = build_frame(frame, 1U, 0U, &event, 1U);
	CHECK(awg_stream_proto_handle_frame_ctx(&session, frame, len, &cfg,
					       &ack) == -EAGAIN);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_RING_FULL);
	CHECK(ack.status != AWG_STREAM_PROTO_ACK_OVERFLOW);
	CHECK(session.next_seq == 1U);
	return true;
}

static bool test_ack_serialization_is_28_byte_little_endian(void)
{
	static const uint8_t expected[AWG_STREAM_PROTO_ACK_BYTES] = {
		0x44, 0x33, 0x22, 0x11,
		0x88, 0x77, 0x66, 0x55,
		0xcc, 0xbb, 0xaa, 0x99,
		0x00, 0xff, 0xee, 0xdd,
		0x04, 0x03, 0x02, 0x01,
		0x14, 0x13, 0x12, 0x11,
		0x24, 0x23, 0x22, 0x21,
	};
	awg_stream_proto_ack_t ack = {
		.magic = UINT32_C(0x11223344),
		.seq_acked = UINT32_C(0x55667788),
		.ddr_free_events = UINT32_C(0x99aabbcc),
		.status = UINT32_C(0xddeeff00),
		.stream_free_events = UINT32_C(0x01020304),
		.stream_stalls = UINT32_C(0x11121314),
		.irq_status = UINT32_C(0x21222324),
	};
	uint8_t wire[AWG_STREAM_PROTO_ACK_BYTES];

	CHECK(AWG_STREAM_PROTO_ACK_BYTES == 28U);
	memset(wire, 0xa5, sizeof(wire));
	awg_stream_proto_ack_to_le(&ack, wire);
	CHECK(memcmp(wire, expected, sizeof(expected)) == 0);
	return true;
}

typedef bool (*test_fn_t)(void);

typedef struct {
	const char *name;
	test_fn_t fn;
} test_case_t;

int main(void)
{
	static const test_case_t tests[] = {
		{ "crc golden vector", test_crc_golden_vector },
		{ "OPEN, next sequence, duplicate", test_open_next_and_duplicate },
		{ "gap and out-of-order sequence", test_gap_and_out_of_order_sequence },
		{ "unsupported flags", test_unsupported_flags },
		{ "bad CRC", test_bad_crc },
		{ "empty CLOSE_WITH_EOF", test_empty_close_with_eof_rejected },
		{ "EOF final event and post-close", test_eof_on_final_event_and_post_close },
		{ "new OPEN reset", test_new_open_resets_closed_session },
		{ "ring-full status", test_ring_full_status_is_distinct },
		{ "28-byte ACK serialization", test_ack_serialization_is_28_byte_little_endian },
	};
	size_t i;
	unsigned int passed = 0U;

	for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); i++) {
		if (tests[i].fn()) {
			printf("PASS: %s\n", tests[i].name);
			passed++;
		} else {
			printf("FAIL: %s\n", tests[i].name);
		}
	}

	printf("%u/%u protocol tests passed\n", passed,
	       (unsigned int)(sizeof(tests) / sizeof(tests[0])));
	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
