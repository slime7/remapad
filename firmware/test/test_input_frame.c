/**
 * 桥接帧协议（input_frame.c）：CRC 或失步重同步写错，表现为「手柄偶尔失灵」
 * 或「命令行里冒出乱码」，两种现象在真机上都不好复现；这里用字节流样本把
 * 编码、校验、重新对齐与跨批次分帧逐条钉住。
 */
#include "host_test.h"

#include <string.h>

#include "input_frame.h"

typedef struct {
    size_t frames;
    uint8_t types[4];
    uint8_t seqs[4];
    uint8_t slots[4];
    size_t lens[4];
    uint8_t payload[4][INPUT_FRAME_MAX_PAYLOAD];
    uint8_t text[128];
    size_t text_len;
} capture_t;

static void capture_frame(const input_frame_view_t *frame, void *user)
{
    capture_t *cap = (capture_t *)user;
    if (cap->frames >= 4) {
        return;
    }
    const size_t i = cap->frames++;
    cap->types[i] = frame->type;
    cap->seqs[i] = frame->seq;
    cap->slots[i] = frame->slot;
    cap->lens[i] = frame->payload_len;
    memcpy(cap->payload[i], frame->payload, frame->payload_len);
}

static void capture_text(const uint8_t *text, size_t len, void *user)
{
    capture_t *cap = (capture_t *)user;
    const size_t room = sizeof(cap->text) - cap->text_len;
    const size_t take = len < room ? len : room;
    memcpy(&cap->text[cap->text_len], text, take);
    cap->text_len += take;
}

static void feed(capture_t *cap, input_frame_rx_t *rx, const uint8_t *data, size_t len)
{
    input_frame_rx_feed(rx, data, len, capture_frame, capture_text, cap);
}

static size_t build_report_frame(uint8_t *out, size_t out_len, uint8_t seq, uint8_t first)
{
    const uint8_t payload[4] = {first, 0x02, 0x03, 0x04};
    return input_frame_encode(out, out_len, INPUT_FRAME_TYPE_REPORT, 0, seq, payload,
                              sizeof(payload));
}

static void crc_known_vector_and_golden_frame(void)
{
    const uint8_t check[] = "123456789";
    CHECK_EQ(input_frame_crc16(check, sizeof(check) - 1), 0x29B1);

    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = build_report_frame(frame, sizeof(frame), 7, 0x01);
    CHECK_EQ(len, 13);
    static const uint8_t expected[13] = {0xA5, 0x5A, 0x01, 0x10, 0x00, 0x07, 0x04,
                                         0x01, 0x02, 0x03, 0x04, 0x4F, 0x96};
    CHECK_BYTES(frame, expected, sizeof(expected));
}

static void encode_rejects_invalid_arguments(void)
{
    uint8_t frame[INPUT_FRAME_MAX_LEN + 8];
    uint8_t payload[INPUT_FRAME_MAX_PAYLOAD + 1] = {0};
    CHECK_EQ(input_frame_encode(frame, sizeof(frame), INPUT_FRAME_TYPE_REPORT, 0, 0, payload,
                                sizeof(payload)),
             0);
    /* 输出缓冲不足同样拒绝，不留半截帧。 */
    CHECK_EQ(input_frame_encode(frame, 8, INPUT_FRAME_TYPE_REPORT, 0, 0, payload, 4), 0);
    CHECK_EQ(input_frame_encode(NULL, sizeof(frame), INPUT_FRAME_TYPE_REPORT, 0, 0, payload, 4),
             0);
    /* 载荷长度为 0 的帧（断开）合法。 */
    CHECK_EQ(input_frame_encode(frame, sizeof(frame), INPUT_FRAME_TYPE_DETACH, 0, 3, NULL, 0),
             INPUT_FRAME_HEADER_LEN + INPUT_FRAME_CRC_LEN);
}

static void demux_text_and_frames(void)
{
    capture_t cap;
    input_frame_rx_t rx;
    memset(&cap, 0, sizeof(cap));
    input_frame_rx_reset(&rx);

    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = build_report_frame(frame, sizeof(frame), 9, 0xAA);
    uint8_t stream[64];
    size_t used = 0;
    const char *text1 = "status\r";
    memcpy(&stream[used], text1, strlen(text1));
    used += strlen(text1);
    memcpy(&stream[used], frame, len);
    used += len;
    const char *text2 = "help\r";
    memcpy(&stream[used], text2, strlen(text2));
    used += strlen(text2);

    feed(&cap, &rx, stream, used);
    CHECK_EQ(cap.frames, 1);
    CHECK_EQ(cap.types[0], INPUT_FRAME_TYPE_REPORT);
    CHECK_EQ(cap.seqs[0], 9);
    CHECK_EQ(cap.lens[0], 4);
    CHECK_EQ(cap.payload[0][0], 0xAA);
    CHECK_EQ(cap.text_len, strlen("status\rhelp\r"));
    CHECK_BYTES(cap.text, "status\rhelp\r", strlen("status\rhelp\r"));
}

static void frame_split_across_feeds(void)
{
    capture_t cap;
    input_frame_rx_t rx;
    memset(&cap, 0, sizeof(cap));
    input_frame_rx_reset(&rx);

    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = build_report_frame(frame, sizeof(frame), 0x42, 0x11);
    for (size_t i = 0; i < len; i++) {
        feed(&cap, &rx, &frame[i], 1);
    }
    CHECK_EQ(cap.frames, 1);
    CHECK_EQ(cap.seqs[0], 0x42);
    CHECK_EQ(cap.lens[0], 4);

    /* 少最后一个字节时不算一帧；补上才交付。 */
    memset(&cap, 0, sizeof(cap));
    input_frame_rx_reset(&rx);
    feed(&cap, &rx, frame, len - 1);
    CHECK_EQ(cap.frames, 0);
    feed(&cap, &rx, &frame[len - 1], 1);
    CHECK_EQ(cap.frames, 1);
}

static void resync_after_garbage_and_false_sync(void)
{
    capture_t cap;
    input_frame_rx_t rx;
    memset(&cap, 0, sizeof(cap));
    input_frame_rx_reset(&rx);

    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = build_report_frame(frame, sizeof(frame), 1, 0x55);

    /* 普通噪声按文本吐出。 */
    const uint8_t noise[2] = {0x41, 0x42};
    feed(&cap, &rx, noise, sizeof(noise));

    /* 假同步字 + 越界长度：必须重新对齐，不能读越界。 */
    const uint8_t fake[7] = {0xA5, 0x5A, 0x01, 0x10, 0x00, 0x02, 0xFF};
    feed(&cap, &rx, fake, sizeof(fake));
    feed(&cap, &rx, frame, len);
    CHECK_EQ(cap.frames, 1);
    CHECK_EQ(cap.seqs[0], 1);
    CHECK_EQ(cap.payload[0][0], 0x55);
}

static void bad_crc_is_dropped_and_stream_recovers(void)
{
    capture_t cap;
    input_frame_rx_t rx;
    memset(&cap, 0, sizeof(cap));
    input_frame_rx_reset(&rx);

    uint8_t broken[INPUT_FRAME_MAX_LEN];
    const size_t len = build_report_frame(broken, sizeof(broken), 3, 0x66);
    broken[INPUT_FRAME_HEADER_LEN] ^= 0xFFu; /* 破坏载荷，CRC 不再匹配 */
    feed(&cap, &rx, broken, len);
    CHECK_EQ(cap.frames, 0);

    uint8_t good[INPUT_FRAME_MAX_LEN];
    const size_t good_len = build_report_frame(good, sizeof(good), 4, 0x77);
    feed(&cap, &rx, good, good_len);
    CHECK_EQ(cap.frames, 1);
    CHECK_EQ(cap.seqs[0], 4);
    CHECK_EQ(cap.payload[0][0], 0x77);
}

static void empty_detach_frame_round_trip(void)
{
    capture_t cap;
    input_frame_rx_t rx;
    memset(&cap, 0, sizeof(cap));
    input_frame_rx_reset(&rx);

    uint8_t frame[INPUT_FRAME_MAX_LEN];
    const size_t len = input_frame_encode(frame, sizeof(frame), INPUT_FRAME_TYPE_DETACH, 0, 5,
                                          NULL, 0);
    feed(&cap, &rx, frame, len);
    CHECK_EQ(cap.frames, 1);
    CHECK_EQ(cap.types[0], INPUT_FRAME_TYPE_DETACH);
    CHECK_EQ(cap.lens[0], 0);
    CHECK_EQ(cap.seqs[0], 5);
}

HOST_TEST_SUITE(suite_input_frame, "input_frame",
                {"CRC 已知向量与黄金帧字节", crc_known_vector_and_golden_frame},
                {"编码拒绝越界载荷、缓冲不足与空指针", encode_rejects_invalid_arguments},
                {"文本与帧混流按顺序分流", demux_text_and_frames},
                {"跨批次分帧与补全末字节", frame_split_across_feeds},
                {"噪声与假同步字之后仍能恢复", resync_after_garbage_and_false_sync},
                {"CRC 不符的帧被丢弃且后续帧照常",
                 bad_crc_is_dropped_and_stream_recovers},
                {"零载荷断开帧往返", empty_detach_frame_round_trip});
