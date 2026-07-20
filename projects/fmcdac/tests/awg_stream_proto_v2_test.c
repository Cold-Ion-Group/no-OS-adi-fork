#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "awg_stream_proto.h"

#define TEST_RECORDS 8U
#define TEST_FRAME_BYTES (AWG_STREAM_PROTO_V2_HEADER_BYTES + \
	(TEST_RECORDS * AWG_STREAM_PROTO_V2_RECORD_BYTES) + \
	AWG_STREAM_PROTO_CRC_BYTES)

struct sched_stub {
	uint32_t open_calls;
	uint32_t reset_calls;
	uint32_t push_calls;
	uint32_t close_calls;
	uint32_t pushed_count;
	int push_ret;
	awg_event_v1_t pushed[TEST_RECORDS];
	awg_sched_stream_snapshot_t snapshot;
};

struct route_stub {
	uint32_t prepare_calls;
	uint32_t select_calls;
	uint8_t selected_kind;
	int prepare_ret;
	int select_ret;
};

static struct sched_stub g_sched;
static unsigned int g_checks;
static unsigned int g_failures;

#define CHECK(condition) do { \
	g_checks++; \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
			__func__, __LINE__, #condition); \
		g_failures++; \
		return; \
	} \
} while (0)

int awg_sched_stream_open(const awg_sched_stream_cfg_t *cfg)
{
	g_sched.open_calls++;
	return cfg ? 0 : -EINVAL;
}

int awg_sched_stream_push(const awg_event_v1_t *events, uint32_t count)
{
	if (g_sched.push_ret)
		return g_sched.push_ret;
	if ((!events && count) || g_sched.pushed_count + count > TEST_RECORDS)
		return -ENOSPC;
	memcpy(&g_sched.pushed[g_sched.pushed_count], events,
	       (size_t)count * sizeof(*events));
	g_sched.pushed_count += count;
	g_sched.push_calls++;
	return 0;
}

int awg_sched_stream_close(bool send_eof)
{
	if (send_eof)
		return -EINVAL;
	g_sched.close_calls++;
	return 0;
}

uint32_t awg_sched_stream_ddr_free_events(void)
{
	return 512U - g_sched.pushed_count;
}

int awg_sched_stream_get_error_snapshot(awg_sched_stream_snapshot_t *snapshot)
{
	if (!snapshot)
		return -EINVAL;
	*snapshot = g_sched.snapshot;
	return 0;
}

int awg_sched_stream_reset_soft(void)
{
	g_sched.reset_calls++;
	return 0;
}

static int route_prepare(void *ctx)
{
	struct route_stub *route = ctx;
	route->prepare_calls++;
	return route->prepare_ret;
}

static int route_select(void *ctx, uint8_t kind)
{
	struct route_stub *route = ctx;
	route->select_calls++;
	route->selected_kind = kind;
	return route->select_ret;
}

static void put_le16(uint8_t *data, uint16_t value)
{
	data[0] = (uint8_t)value;
	data[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)value;
	data[1] = (uint8_t)(value >> 8);
	data[2] = (uint8_t)(value >> 16);
	data[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *data, uint64_t value)
{
	put_le32(data, (uint32_t)value);
	put_le32(data + 4U, (uint32_t)(value >> 32));
}

static size_t build_frame(uint8_t *frame, uint32_t session_id,
		uint32_t seq, uint8_t kind, uint16_t flags,
		const uint8_t *records, uint16_t count)
{
	size_t payload_bytes = (size_t)count * AWG_STREAM_PROTO_V2_RECORD_BYTES;
	size_t crc_offset = AWG_STREAM_PROTO_V2_HEADER_BYTES + payload_bytes;

	put_le32(frame, AWG_STREAM_PROTO_MAGIC);
	frame[4U] = AWG_STREAM_PROTO_V2_VERSION;
	frame[5U] = kind;
	put_le16(frame + 6U, flags);
	put_le32(frame + 8U, session_id);
	put_le32(frame + 12U, seq);
	put_le16(frame + 16U, count);
	put_le16(frame + 18U, AWG_STREAM_PROTO_V2_HEADER_BYTES);
	put_le32(frame + 20U, (uint32_t)payload_bytes);
	if (payload_bytes)
		memcpy(frame + AWG_STREAM_PROTO_V2_HEADER_BYTES, records,
		       payload_bytes);
	put_le32(frame + crc_offset,
		 awg_stream_proto_crc32_ieee(frame, crc_offset));
	return crc_offset + AWG_STREAM_PROTO_CRC_BYTES;
}

static void event_record(uint8_t record[AWG_STREAM_PROTO_V2_RECORD_BYTES],
		uint64_t timestamp, uint16_t flags, uint32_t seed)
{
	memset(record, 0, AWG_STREAM_PROTO_V2_RECORD_BYTES);
	put_le64(record, timestamp);
	put_le16(record + 8U, 1U);
	put_le16(record + 10U, flags);
	put_le32(record + 12U, seed);
}

static awg_sched_stream_cfg_t stream_config(void)
{
	awg_sched_stream_cfg_t config;

	memset(&config, 0, sizeof(config));
	config.staging_capacity = 512U;
	config.use_dma = true;
	return config;
}

static void fixture_reset(struct route_stub *route,
		awg_stream_proto_v2_session_t *session)
{
	memset(&g_sched, 0, sizeof(g_sched));
	g_sched.snapshot.status = UINT32_C(0x01020304);
	g_sched.snapshot.free_space = 511U;
	g_sched.snapshot.stream_stalls = UINT32_C(0x11121314);
	g_sched.snapshot.irq_status = UINT32_C(0x21222324);
	memset(route, 0, sizeof(*route));
	awg_stream_proto_v2_session_init(session);
}

static void open_session(awg_stream_proto_v2_session_t *session,
		struct route_stub *route, uint32_t session_id,
		uint8_t frame[TEST_FRAME_BYTES], const uint8_t digest[32])
{
	const awg_stream_proto_v2_ops_t ops = {
		.prepare = route_prepare,
		.select_kind = route_select,
		.ctx = route,
	};
	awg_stream_proto_v2_ack_t ack;
	awg_sched_stream_cfg_t config = stream_config();
	size_t length;

	length = build_frame(frame, session_id, 0U,
		AWG_STREAM_PROTO_V2_KIND_CONTROL,
		AWG_STREAM_PROTO_FLAG_OPEN, digest, 1U);
	CHECK(awg_stream_proto_v2_handle_frame(session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(ack.status == 0U);
}

static void test_open_identity_and_duplicate(void)
{
	awg_stream_proto_v2_session_t session;
	awg_stream_proto_v2_ack_t ack;
	awg_stream_proto_v2_ack_t first_ack;
	awg_sched_stream_cfg_t config = stream_config();
	struct route_stub route;
	const awg_stream_proto_v2_ops_t ops = {
		.prepare = route_prepare,
		.select_kind = route_select,
		.ctx = &route,
	};
	uint8_t digest[32];
	uint8_t frame[TEST_FRAME_BYTES];
	size_t length;
	uint32_t index;

	fixture_reset(&route, &session);
	for (index = 0U; index < sizeof(digest); index++)
		digest[index] = (uint8_t)index;
	length = build_frame(frame, UINT32_C(0x11223344), 0U,
		AWG_STREAM_PROTO_V2_KIND_CONTROL,
		AWG_STREAM_PROTO_FLAG_OPEN, digest, 1U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(session.active && !session.closed);
	CHECK(session.session_id == UINT32_C(0x11223344));
	CHECK(memcmp(session.program_sha256, digest, sizeof(digest)) == 0);
	CHECK(route.prepare_calls == 1U && g_sched.open_calls == 1U);
	CHECK(ack.scheduler_status == g_sched.snapshot.status);
	first_ack = ack;

	memset(&ack, 0, sizeof(ack));
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(memcmp(&ack, &first_ack, sizeof(ack)) == 0);
	CHECK(route.prepare_calls == 1U && g_sched.open_calls == 1U);
}

static void test_direct_tail_eof_and_idempotency(void)
{
	awg_stream_proto_v2_session_t session;
	awg_stream_proto_v2_ack_t ack;
	awg_sched_stream_cfg_t config = stream_config();
	struct route_stub route;
	const awg_stream_proto_v2_ops_t ops = {
		.prepare = route_prepare,
		.select_kind = route_select,
		.ctx = &route,
	};
	uint8_t digest[32] = { 0 };
	uint8_t records[2U * AWG_STREAM_PROTO_V2_RECORD_BYTES];
	uint8_t frame[TEST_FRAME_BYTES];
	size_t length;

	fixture_reset(&route, &session);
	open_session(&session, &route, 7U, frame, digest);
	event_record(records, 100U, 0U, 1U);
	event_record(records + AWG_STREAM_PROTO_V2_RECORD_BYTES, 200U,
		AWG_SCHED_FLAG_PHASE_REINIT, 2U);
	length = build_frame(frame, 7U, 1U,
		AWG_STREAM_PROTO_V2_KIND_EVENTS, 0U, records, 2U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(route.select_calls == 1U &&
	      route.selected_kind == AWG_STREAM_PROTO_V2_KIND_EVENTS);
	CHECK(g_sched.pushed_count == 1U);
	CHECK(g_sched.pushed[0].timestamp_ticks == 100U);
	CHECK(session.have_pending_event &&
	      session.pending_event.timestamp_ticks == 200U);

	/* The same accepted data frame is ACKed without a second push. */
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(g_sched.pushed_count == 1U && route.select_calls == 1U);

	length = build_frame(frame, 7U, 2U,
		AWG_STREAM_PROTO_V2_KIND_CONTROL,
		AWG_STREAM_PROTO_FLAG_CLOSE_WITH_EOF, NULL, 0U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(session.closed && !session.have_pending_event);
	CHECK(g_sched.pushed_count == 2U && g_sched.close_calls == 1U);
	CHECK((g_sched.pushed[0].flags & AWG_SCHED_FLAG_EOF) == 0U);
	CHECK((g_sched.pushed[1].flags & AWG_SCHED_FLAG_EOF) != 0U);

	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(g_sched.pushed_count == 2U && g_sched.close_calls == 1U);
}

static void test_c1_records_are_opaque_and_close_does_not_add_eof(void)
{
	awg_stream_proto_v2_session_t session;
	awg_stream_proto_v2_ack_t ack;
	awg_sched_stream_cfg_t config = stream_config();
	struct route_stub route;
	const awg_stream_proto_v2_ops_t ops = {
		.prepare = route_prepare,
		.select_kind = route_select,
		.ctx = &route,
	};
	uint8_t digest[32] = { 0x5aU };
	uint8_t records[2U * AWG_STREAM_PROTO_V2_RECORD_BYTES];
	uint8_t frame[TEST_FRAME_BYTES];
	size_t length;
	uint32_t index;

	fixture_reset(&route, &session);
	open_session(&session, &route, 9U, frame, digest);
	for (index = 0U; index < sizeof(records); index++)
		records[index] = (uint8_t)(index ^ 0xa5U);
	length = build_frame(frame, 9U, 1U,
		AWG_STREAM_PROTO_V2_KIND_C1, 0U, records, 2U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(route.selected_kind == AWG_STREAM_PROTO_V2_KIND_C1);
	CHECK(g_sched.pushed_count == 2U);
	CHECK(memcmp(g_sched.pushed, records, sizeof(records)) == 0);
	CHECK(!session.have_pending_event);

	length = build_frame(frame, 9U, 2U,
		AWG_STREAM_PROTO_V2_KIND_CONTROL, 0U, NULL, 0U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(g_sched.pushed_count == 2U && g_sched.close_calls == 1U);
}

static void test_rejection_and_retry_paths(void)
{
	awg_stream_proto_v2_session_t session;
	awg_stream_proto_v2_ack_t ack;
	awg_sched_stream_cfg_t config = stream_config();
	struct route_stub route;
	const awg_stream_proto_v2_ops_t ops = {
		.prepare = route_prepare,
		.select_kind = route_select,
		.ctx = &route,
	};
	uint8_t digest[32] = { 0 };
	uint8_t records[2U * AWG_STREAM_PROTO_V2_RECORD_BYTES];
	uint8_t frame[TEST_FRAME_BYTES];
	size_t length;

	fixture_reset(&route, &session);
	open_session(&session, &route, 11U, frame, digest);
	event_record(records, 1U, 0U, 1U);
	event_record(records + AWG_STREAM_PROTO_V2_RECORD_BYTES, 2U, 0U, 2U);

	length = build_frame(frame, 11U, 2U,
		AWG_STREAM_PROTO_V2_KIND_EVENTS, 0U, records, 2U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == -EPROTO);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_SEQUENCE);

	length = build_frame(frame, 11U, 1U,
		AWG_STREAM_PROTO_V2_KIND_EVENTS, 0U, records, 2U);
	g_sched.push_ret = -EAGAIN;
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == -EAGAIN);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_RING_FULL);
	CHECK(session.next_seq == 1U && g_sched.pushed_count == 0U);
	g_sched.push_ret = 0;
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == 0);
	CHECK(session.next_seq == 2U && g_sched.pushed_count == 1U);

	length = build_frame(frame, 11U, 2U,
		AWG_STREAM_PROTO_V2_KIND_C1, 0U, records, 1U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == -EPROTOTYPE);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_BAD_KIND);

	fixture_reset(&route, &session);
	open_session(&session, &route, 12U, frame, digest);
	route.select_ret = -ENOTSUP;
	length = build_frame(frame, 12U, 1U,
		AWG_STREAM_PROTO_V2_KIND_C1, 0U, records, 1U);
	CHECK(awg_stream_proto_v2_handle_frame(&session, frame, length,
		&config, &ops, &ack) == -ENOTSUP);
	CHECK(ack.status == AWG_STREAM_PROTO_ACK_C1_DISABLED);
	CHECK(session.next_seq == 1U && g_sched.pushed_count == 0U);
}

static void test_ack_wire_layout(void)
{
	static const uint8_t expected[AWG_STREAM_PROTO_V2_ACK_BYTES] = {
		0x47, 0x57, 0x41, 0x53, 0x02, 0x00, 0x28, 0x00,
		0x04, 0x03, 0x02, 0x01, 0x14, 0x13, 0x12, 0x11,
		0x24, 0x23, 0x22, 0x21, 0x34, 0x33, 0x32, 0x31,
		0x44, 0x43, 0x42, 0x41, 0x54, 0x53, 0x52, 0x51,
		0x64, 0x63, 0x62, 0x61, 0x74, 0x73, 0x72, 0x71,
	};
	awg_stream_proto_v2_ack_t ack = {
		.magic = AWG_STREAM_PROTO_MAGIC,
		.version = AWG_STREAM_PROTO_V2_VERSION,
		.header_bytes = AWG_STREAM_PROTO_V2_ACK_BYTES,
		.session_id = UINT32_C(0x01020304),
		.seq_acked = UINT32_C(0x11121314),
		.status = UINT32_C(0x21222324),
		.free_records = UINT32_C(0x31323334),
		.scheduler_status = UINT32_C(0x41424344),
		.stream_free_records = UINT32_C(0x51525354),
		.stream_stalls = UINT32_C(0x61626364),
		.irq_status = UINT32_C(0x71727374),
	};
	uint8_t wire[AWG_STREAM_PROTO_V2_ACK_BYTES];

	awg_stream_proto_v2_ack_to_le(&ack, wire);
	CHECK(memcmp(wire, expected, sizeof(expected)) == 0);
}

int main(void)
{
	test_open_identity_and_duplicate();
	test_direct_tail_eof_and_idempotency();
	test_c1_records_are_opaque_and_close_does_not_add_eof();
	test_rejection_and_retry_paths();
	test_ack_wire_layout();

	if (g_failures) {
		fprintf(stderr, "awg_stream_proto_v2_test: %u/%u checks failed\n",
			g_failures, g_checks);
		return 1;
	}
	printf("awg_stream_proto_v2_test: %u checks passed\n", g_checks);
	return 0;
}
