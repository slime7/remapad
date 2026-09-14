/**
 * OTA 会话协议（ota_proto.c）：单个位错或序号判断写错，在真机上表现为
 * 「升级卡住」「写坏镜像」或「PC 以为成功而设备其实没换分区」，这里把载荷
 * 布局、序号判定、4 KB 聚合边界、超时与应答字节逐条钉住。
 */
#include "host_test.h"

#include <string.h>

#include "ota_proto.h"

/** 记录每次交付给 flash 的块大小与首末字节，用来钉住聚合边界。 */
typedef struct {
    size_t calls;
    size_t bytes;
    size_t sizes[8];
    uint8_t first;
    uint8_t last;
    ota_code_t fail_with;
    size_t fail_at_call;
} sink_t;

static ota_code_t sink_flush(const uint8_t *data, size_t len, void *user)
{
    sink_t *sink = (sink_t *)user;
    sink->calls++;
    if (sink->calls == 1) {
        sink->first = data[0];
    }
    sink->last = data[len - 1];
    if (sink->calls < sizeof(sink->sizes) / sizeof(sink->sizes[0])) {
        sink->sizes[sink->calls - 1] = len;
    }
    if (sink->fail_at_call != 0 && sink->calls == sink->fail_at_call) {
        return sink->fail_with;
    }
    sink->bytes += len;
    return OTA_CODE_OK;
}

static size_t build_begin(uint8_t *out, uint32_t image_size)
{
    memcpy(out, OTA_BEGIN_MAGIC, OTA_BEGIN_MAGIC_LEN);
    out[OTA_BEGIN_MAGIC_LEN] = (uint8_t)(image_size & 0xFFu);
    out[OTA_BEGIN_MAGIC_LEN + 1] = (uint8_t)((image_size >> 8) & 0xFFu);
    out[OTA_BEGIN_MAGIC_LEN + 2] = (uint8_t)((image_size >> 16) & 0xFFu);
    out[OTA_BEGIN_MAGIC_LEN + 3] = (uint8_t)((image_size >> 24) & 0xFFu);
    return OTA_BEGIN_PAYLOAD_LEN;
}

static size_t build_data(uint8_t *out, uint16_t seq, size_t data_len, uint8_t fill)
{
    out[0] = (uint8_t)(seq & 0xFFu);
    out[1] = (uint8_t)(seq >> 8);
    memset(&out[OTA_DATA_SEQ_LEN], fill, data_len);
    return data_len + OTA_DATA_SEQ_LEN;
}

/**
 * 按 PC 端的节奏喂一块镜像数据（单帧不超过 200 字节，也不超过剩余镜像），
 * 返回这一帧的结论。
 */
static ota_proto_result_t feed_image(ota_proto_t *proto, uint32_t image_size, uint16_t seq,
                                     size_t data_len, sink_t *sink)
{
    uint8_t payload[OTA_DATA_PAYLOAD_MAX];
    REQUIRE(data_len <= OTA_DATA_MAX_LEN);
    REQUIRE(data_len <= image_size);
    const size_t len = build_data(payload, seq, data_len, (uint8_t)seq);
    return ota_proto_data(proto, payload, len, false, (int64_t)seq * 1000, sink_flush, sink);
}

static void begin_validates_magic_and_size(void)
{
    ota_proto_t proto;
    ota_proto_init(&proto);

    uint8_t payload[OTA_BEGIN_PAYLOAD_LEN];
    const size_t len = build_begin(payload, 4096);
    uint32_t size = 0;
    CHECK(ota_proto_parse_begin(payload, len, &size));
    CHECK_EQ(size, 4096);
    /* 长度不足与 magic 不符都拒绝。 */
    CHECK(!ota_proto_parse_begin(payload, OTA_BEGIN_PAYLOAD_LEN - 1, &size));
    const uint8_t bad_magic[OTA_BEGIN_PAYLOAD_LEN] = {0x52, 0x4F, 0x4D, 0x32, 0, 1, 0, 0};
    CHECK(!ota_proto_parse_begin(bad_magic, sizeof(bad_magic), &size));

    /* 尺寸为 0 或超出目标分区容量即拒绝，且不进入接收态。 */
    ota_proto_result_t result = ota_proto_begin(&proto, 0, 4096, 0);
    CHECK_EQ(result.state, OTA_STATE_FAILED);
    CHECK_EQ(result.code, OTA_CODE_BAD_HEADER);
    CHECK(result.reply);
    result = ota_proto_begin(&proto, 4097, 4096, 0);
    CHECK_EQ(result.state, OTA_STATE_FAILED);
    CHECK_EQ(result.code, OTA_CODE_BAD_HEADER);

    /* 等于分区容量的边界值合法。 */
    result = ota_proto_begin(&proto, 4096, 4096, 0);
    CHECK_EQ(result.state, OTA_STATE_RECEIVING);
    CHECK_EQ(result.next_seq, 0);
    CHECK_EQ(result.received, 0);
    CHECK(result.reply);
    CHECK(ota_proto_busy(&proto));
}

static void begin_refuses_second_session(void)
{
    ota_proto_t proto;
    ota_proto_init(&proto);
    CHECK(!ota_proto_busy(&proto));
    REQUIRE(ota_proto_begin(&proto, 1024, 4096, 0).state == OTA_STATE_RECEIVING);

    /* 接收中再收到 BEGIN：回 BUSY，不改动当前会话。 */
    const ota_proto_result_t result = ota_proto_begin(&proto, 2048, 4096, 1000);
    CHECK_EQ(result.state, OTA_STATE_RECEIVING);
    CHECK_EQ(result.code, OTA_CODE_BUSY);
    CHECK(result.reply);
    CHECK_EQ(proto.image_size, 1024);
}

static void data_tracks_sequence_and_resend(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    const uint32_t image_size = 300;
    REQUIRE(ota_proto_begin(&proto, image_size, 1048576, 0).state == OTA_STATE_RECEIVING);

    /* 连续帧：不回应答，只累计字节与期望序号。 */
    ota_proto_result_t result = feed_image(&proto, image_size, 0, 200, &sink);
    CHECK(!result.reply);
    CHECK_EQ(result.received, 200);
    CHECK_EQ(result.next_seq, 1);

    /* 重发已收过的序号：回 SEQ_ERROR 并给出重发起点，字节不再累加。 */
    result = feed_image(&proto, image_size, 0, 200, &sink);
    CHECK(result.reply);
    CHECK_EQ(result.code, OTA_CODE_SEQ_ERROR);
    CHECK_EQ(result.next_seq, 1);
    CHECK_EQ(result.received, 200);

    /* 跳号同样从期望序号续传。 */
    result = feed_image(&proto, image_size, 5, 200, &sink);
    CHECK_EQ(result.code, OTA_CODE_SEQ_ERROR);
    CHECK_EQ(result.next_seq, 1);

    /* 这一帧会把累计字节顶过声明的镜像大小：立刻作废，不再写入。 */
    result = feed_image(&proto, image_size, 1, 200, &sink);
    CHECK_EQ(result.state, OTA_STATE_FAILED);
    CHECK_EQ(result.code, OTA_CODE_SIZE_MISMATCH);
    CHECK(result.reply);
    CHECK_EQ(sink.bytes, 0);
}

static void data_acks_every_window(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    const uint32_t image_size = OTA_ACK_WINDOW * 200u;
    REQUIRE(ota_proto_begin(&proto, image_size, 1048576, 0).state == OTA_STATE_RECEIVING);

    for (uint16_t seq = 0; seq < OTA_ACK_WINDOW - 1u; seq++) {
        const ota_proto_result_t result = feed_image(&proto, image_size, seq, 200, &sink);
        CHECK(!result.reply);
    }
    /* 中间插一次重发请求不重置窗口计数。 */
    const ota_proto_result_t resent = feed_image(&proto, image_size, 0, 200, &sink);
    CHECK_EQ(resent.code, OTA_CODE_SEQ_ERROR);

    const ota_proto_result_t last = feed_image(&proto, image_size, OTA_ACK_WINDOW - 1u, 200, &sink);
    CHECK(last.reply);
    CHECK_EQ(last.code, OTA_CODE_OK);
    CHECK_EQ(last.next_seq, OTA_ACK_WINDOW);
    CHECK_EQ(last.received, image_size);
}

static void window_end_marker_acks_at_once(void)
{
    ota_proto_t proto;
    sink_t sink;
    uint8_t payload[OTA_DATA_PAYLOAD_MAX];
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    REQUIRE(ota_proto_begin(&proto, 200, 1048576, 0).state == OTA_STATE_RECEIVING);

    /* 不足一窗：没有末帧标记就不回应答（PC 等 ACK 才发下一窗）。 */
    size_t len = build_data(payload, 0, 100, 0x11);
    ota_proto_result_t result =
        ota_proto_data(&proto, payload, len, false, 1000, sink_flush, &sink);
    CHECK(!result.reply);
    CHECK_EQ(result.received, 100);

    /* 末帧标记（帧 slot 字段）：收到即应答，末尾的不足一窗不必等超时。 */
    len = build_data(payload, 1, 100, 0x22);
    result = ota_proto_data(&proto, payload, len, true, 2000, sink_flush, &sink);
    CHECK(result.reply);
    CHECK_EQ(result.code, OTA_CODE_OK);
    CHECK_EQ(result.received, 200);
    CHECK_EQ(result.next_seq, 2);
}

static void repeated_duplicates_answer_once(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    REQUIRE(ota_proto_begin(&proto, 600, 1048576, 0).state == OTA_STATE_RECEIVING);
    feed_image(&proto, 600, 0, 200, &sink);

    /* 整窗重发的第一帧回 SEQ_ERROR 给出续传起点，其余重发帧不再刷应答。 */
    ota_proto_result_t result = feed_image(&proto, 600, 0, 200, &sink);
    CHECK(result.reply);
    CHECK_EQ(result.code, OTA_CODE_SEQ_ERROR);
    CHECK_EQ(result.next_seq, 1);
    result = feed_image(&proto, 600, 0, 200, &sink);
    CHECK(!result.reply);
    CHECK_EQ(result.code, OTA_CODE_SEQ_ERROR);

    /* 收下新帧后重置标记，下一次重发仍会得到应答。 */
    result = feed_image(&proto, 600, 1, 200, &sink);
    CHECK(!result.reply);
    result = feed_image(&proto, 600, 1, 200, &sink);
    CHECK(result.reply);
    CHECK_EQ(result.code, OTA_CODE_SEQ_ERROR);
    CHECK_EQ(result.next_seq, 2);
}

static void data_needs_an_open_session(void)
{
    ota_proto_t proto;
    sink_t sink;
    uint8_t payload[OTA_DATA_PAYLOAD_MAX];
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);

    const size_t len = build_data(payload, 0, 8, 0x5A);
    ota_proto_result_t result =
        ota_proto_data(&proto, payload, len, false, 0, sink_flush, &sink);
    CHECK_EQ(result.code, OTA_CODE_BAD_HEADER);
    CHECK(result.reply);
    CHECK_EQ(result.state, OTA_STATE_IDLE);

    result = ota_proto_end(&proto, 0, sink_flush, &sink);
    CHECK_EQ(result.code, OTA_CODE_BAD_HEADER);
    CHECK_EQ(sink.calls, 0);
}

static void chunks_flush_on_4k_boundary_and_at_end(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    const uint32_t image_size = 20u * 200u + 100u;
    REQUIRE(ota_proto_begin(&proto, image_size, 1048576, 0).state == OTA_STATE_RECEIVING);

    for (uint16_t seq = 0; seq < 20u; seq++) {
        feed_image(&proto, image_size, seq, 200, &sink);
    }
    CHECK_EQ(sink.calls, 0); /* 4000 字节还没到一块 */
    const ota_proto_result_t result = feed_image(&proto, image_size, 20u, 100, &sink);
    CHECK_EQ(result.received, image_size);
    CHECK_EQ(sink.calls, 1);
    CHECK_EQ(sink.sizes[0], OTA_CHUNK_LEN);
    CHECK_EQ(sink.bytes, OTA_CHUNK_LEN);
    CHECK_EQ(sink.first, 0x00);

    /* END 把尾块交付出去，字节总数与声明一致。 */
    const ota_proto_result_t done = ota_proto_end(&proto, 99999, sink_flush, &sink);
    CHECK(done.finished);
    CHECK(!done.reply);
    CHECK_EQ(sink.calls, 2);
    CHECK_EQ(sink.sizes[1], 4);
    CHECK_EQ(sink.bytes, image_size);
    CHECK_EQ(sink.last, 0x14);
    CHECK(ota_proto_busy(&proto)); /* 收完等上层校验 */
}

static void end_rejects_size_mismatch(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    REQUIRE(ota_proto_begin(&proto, 400, 1048576, 0).state == OTA_STATE_RECEIVING);
    feed_image(&proto, 400, 0, 200, &sink);

    const ota_proto_result_t result = ota_proto_end(&proto, 1000, sink_flush, &sink);
    CHECK_EQ(result.state, OTA_STATE_FAILED);
    CHECK_EQ(result.code, OTA_CODE_SIZE_MISMATCH);
    CHECK(result.reply);
    CHECK(!result.finished);
    CHECK_EQ(sink.bytes, 0); /* 不满一帧的残留不写 flash */
}

static void flush_failure_aborts_with_its_code(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    sink.fail_with = OTA_CODE_BAD_HEADER;
    sink.fail_at_call = 1;
    ota_proto_init(&proto);
    REQUIRE(ota_proto_begin(&proto, 200, 1048576, 0).state == OTA_STATE_RECEIVING);
    feed_image(&proto, 200, 0, 200, &sink);

    const ota_proto_result_t result = ota_proto_end(&proto, 1000, sink_flush, &sink);
    CHECK_EQ(result.state, OTA_STATE_FAILED);
    CHECK_EQ(result.code, OTA_CODE_BAD_HEADER);
    CHECK(result.reply);
    CHECK_EQ(sink.calls, 1);
}

static void idle_session_times_out(void)
{
    ota_proto_t proto;
    ota_proto_init(&proto);
    REQUIRE(ota_proto_begin(&proto, 800, 1048576, 1000).state == OTA_STATE_RECEIVING);

    CHECK(!ota_proto_tick(&proto, 1000 + OTA_SESSION_TIMEOUT_US).reply);
    const ota_proto_result_t result =
        ota_proto_tick(&proto, 1000 + OTA_SESSION_TIMEOUT_US + 1);
    CHECK(result.reply);
    CHECK_EQ(result.state, OTA_STATE_FAILED);
    CHECK_EQ(result.code, OTA_CODE_TIMEOUT);
    CHECK(!ota_proto_busy(&proto));

    /* 收完之后不再计时（校验与重启期间不能自己作废）。 */
    ota_proto_init(&proto);
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    REQUIRE(ota_proto_begin(&proto, 100, 1048576, 0).state == OTA_STATE_RECEIVING);
    feed_image(&proto, 100, 0, 100, &sink);
    REQUIRE(ota_proto_end(&proto, 0, sink_flush, &sink).finished);
    CHECK(!ota_proto_tick(&proto, 10 * OTA_SESSION_TIMEOUT_US).reply);
}

static void ack_payload_is_a_golden_byte_sequence(void)
{
    const ota_proto_result_t result = {
        .state = OTA_STATE_RECEIVING,
        .code = OTA_CODE_SEQ_ERROR,
        .next_seq = 0x1234,
        .received = 0x01020304u,
        .reply = true,
        .finished = false,
    };
    uint8_t payload[OTA_ACK_PAYLOAD_LEN + OTA_ACK_VERSION_LEN];
    const size_t len = ota_proto_encode_ack(&result, "1.2.3", false, payload, sizeof(payload));
    CHECK_EQ(len, OTA_ACK_PAYLOAD_LEN);
    static const uint8_t expected[OTA_ACK_PAYLOAD_LEN] = {0x01, 0x03, 0x34, 0x12,
                                                          0x04, 0x03, 0x02, 0x01};
    CHECK_BYTES(payload, expected, sizeof(expected));

    /* BEGIN 的应答在末尾追加 16 字节运行版本，不足补 0。 */
    const size_t with_version = ota_proto_encode_ack(&result, "1.2.3", true, payload,
                                                     sizeof(payload));
    CHECK_EQ(with_version, OTA_ACK_PAYLOAD_LEN + OTA_ACK_VERSION_LEN);
    static const uint8_t expected_tail[OTA_ACK_VERSION_LEN] = {'1', '.', '2', '.', '3'};
    CHECK_BYTES(&payload[OTA_ACK_PAYLOAD_LEN], expected_tail, sizeof(expected_tail));

    /* 缓冲不够时拒绝编码，不写半截。 */
    CHECK_EQ(ota_proto_encode_ack(&result, "1.2.3", true, payload, OTA_ACK_PAYLOAD_LEN), 0);
    CHECK_EQ(ota_proto_encode_ack(NULL, "1.2.3", false, payload, sizeof(payload)), 0);
}

static void done_reports_success_once(void)
{
    ota_proto_t proto;
    sink_t sink;
    memset(&sink, 0, sizeof(sink));
    ota_proto_init(&proto);
    REQUIRE(ota_proto_begin(&proto, 100, 1048576, 0).state == OTA_STATE_RECEIVING);
    feed_image(&proto, 100, 0, 100, &sink);
    REQUIRE(ota_proto_end(&proto, 0, sink_flush, &sink).finished);

    const ota_proto_result_t result = ota_proto_done(&proto);
    CHECK_EQ(result.state, OTA_STATE_DONE);
    CHECK_EQ(result.code, OTA_CODE_OK);
    CHECK_EQ(result.received, 100);
    CHECK(result.reply);
    CHECK(ota_proto_busy(&proto));

    /* 上层判定失败时回具体错误码。 */
    const ota_proto_result_t failed = ota_proto_fail(&proto, OTA_CODE_VERIFY_FAILED);
    CHECK_EQ(failed.state, OTA_STATE_FAILED);
    CHECK_EQ(failed.code, OTA_CODE_VERIFY_FAILED);
    CHECK(failed.reply);
    CHECK(!ota_proto_busy(&proto));
}

HOST_TEST_SUITE(suite_ota_proto, "ota_proto",
                {"BEGIN 校验 magic 与镜像尺寸边界", begin_validates_magic_and_size},
                {"接收中再次 BEGIN 只回 BUSY", begin_refuses_second_session},
                {"序号连续、重发与跳号都从期望序号续传",
                 data_tracks_sequence_and_resend},
                {"每收满一个窗口回一次 ACK", data_acks_every_window},
                {"窗口末帧标记立刻应答（末尾不足一窗）", window_end_marker_acks_at_once},
                {"整窗重发只回一次序号错误应答", repeated_duplicates_answer_once},
                {"没有 BEGIN 的数据帧与结束帧被拒绝", data_needs_an_open_session},
                {"4 KB 聚合边界与尾块交付", chunks_flush_on_4k_boundary_and_at_end},
                {"结束帧字节数与声明不符即失败", end_rejects_size_mismatch},
                {"写 flash 失败按原错误码作废会话", flush_failure_aborts_with_its_code},
                {"空闲超时作废会话，收完则不再计时", idle_session_times_out},
                {"ACK 载荷黄金字节与版本尾巴", ack_payload_is_a_golden_byte_sequence},
                {"完成与失败结论各回一次", done_reports_success_once});
