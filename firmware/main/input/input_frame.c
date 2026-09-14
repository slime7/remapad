#include "input_frame.h"

#include <stddef.h>
#include <string.h>

uint16_t input_frame_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x8000u) != 0) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

size_t input_frame_encode(uint8_t *out, size_t out_len, uint8_t type, uint8_t slot,
                          uint8_t seq, const uint8_t *payload, size_t payload_len)
{
    if (out == NULL || payload_len > INPUT_FRAME_MAX_PAYLOAD) {
        return 0;
    }
    const size_t total = INPUT_FRAME_HEADER_LEN + payload_len + INPUT_FRAME_CRC_LEN;
    if (out_len < total) {
        return 0;
    }
    out[0] = INPUT_FRAME_SYNC0;
    out[1] = INPUT_FRAME_SYNC1;
    out[2] = INPUT_FRAME_VERSION;
    out[3] = type;
    out[4] = slot;
    out[5] = seq;
    out[6] = (uint8_t)payload_len;
    if (payload_len > 0 && payload != NULL) {
        memcpy(&out[INPUT_FRAME_HEADER_LEN], payload, payload_len);
    }
    const uint16_t crc = input_frame_crc16(out, total - INPUT_FRAME_CRC_LEN);
    out[total - 2] = (uint8_t)(crc & 0xFFu);
    out[total - 1] = (uint8_t)(crc >> 8);
    return total;
}

void input_frame_rx_reset(input_frame_rx_t *rx)
{
    rx->len = 0;
}

static void emit_text(const uint8_t *text, size_t len, input_text_cb_t on_text, void *user)
{
    if (len > 0 && on_text != NULL) {
        on_text(text, len, user);
    }
}

/** 在缓冲里找同步字起点；找不到返回 -1。 */
static ptrdiff_t find_sync(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == INPUT_FRAME_SYNC0 && buf[i + 1] == INPUT_FRAME_SYNC1) {
            return (ptrdiff_t)i;
        }
    }
    return -1;
}

/** 丢掉一个字节（同步字失配时的重新对齐），保留其余字节等待后续处理。 */
static void drop_first_byte(input_frame_rx_t *rx)
{
    memmove(rx->buf, &rx->buf[1], rx->len - 1);
    rx->len -= 1;
}

/**
 * 把可判定的文本吐给上层：除「可能是同步字前半的末字节」外全部交出，
 * 末字节留到下一批数据，避免把跨批次的同步字当成文本。
 */
static void flush_text(input_frame_rx_t *rx, input_text_cb_t on_text, void *user)
{
    size_t flush = rx->len;
    if (flush > 0 && rx->buf[flush - 1] == INPUT_FRAME_SYNC0) {
        flush -= 1;
    }
    if (flush == 0) {
        return;
    }
    emit_text(rx->buf, flush, on_text, user);
    memmove(rx->buf, &rx->buf[flush], rx->len - flush);
    rx->len -= flush;
}

static void process(input_frame_rx_t *rx, input_frame_cb_t on_frame, input_text_cb_t on_text,
                     void *user)
{
    for (;;) {
        const ptrdiff_t sync = find_sync(rx->buf, rx->len);
        if (sync < 0) {
            /* 没有同步字：全是 CLI 文本，仅保留可能是同步字前半的末字节。 */
            flush_text(rx, on_text, user);
            return;
        }
        if (sync > 0) {
            emit_text(rx->buf, (size_t)sync, on_text, user);
            memmove(rx->buf, &rx->buf[sync], rx->len - (size_t)sync);
            rx->len -= (size_t)sync;
        }
        if (rx->len < INPUT_FRAME_HEADER_LEN) {
            return; /* 帧头未收齐 */
        }
        const size_t payload_len = rx->buf[6];
        if (payload_len > INPUT_FRAME_MAX_PAYLOAD) {
            /* 长度越界：这里的同步字是误命中，丢一个字节继续扫描。 */
            drop_first_byte(rx);
            continue;
        }
        const size_t total = INPUT_FRAME_HEADER_LEN + payload_len + INPUT_FRAME_CRC_LEN;
        if (rx->len < total) {
            return; /* 帧体未收齐 */
        }
        const uint16_t crc = input_frame_crc16(rx->buf, total - INPUT_FRAME_CRC_LEN);
        const uint16_t want =
            (uint16_t)(rx->buf[total - 2] | ((uint16_t)rx->buf[total - 1] << 8));
        if (crc != want) {
            /* 校验失败：丢掉同步字首字节重新对齐，不对上层交付半截数据。 */
            drop_first_byte(rx);
            continue;
        }
        const input_frame_view_t view = {
            .version = rx->buf[2],
            .type = rx->buf[3],
            .slot = rx->buf[4],
            .seq = rx->buf[5],
            .payload_len = payload_len,
            .payload = &rx->buf[INPUT_FRAME_HEADER_LEN],
        };
        if (on_frame != NULL) {
            on_frame(&view, user);
        }
        memmove(rx->buf, &rx->buf[total], rx->len - total);
        rx->len -= total;
    }
}

void input_frame_rx_feed(input_frame_rx_t *rx, const uint8_t *data, size_t len,
                         input_frame_cb_t on_frame, input_text_cb_t on_text, void *user)
{
    size_t offset = 0;
    while (offset < len) {
        size_t space = sizeof(rx->buf) - rx->len;
        if (space == 0) {
            /* 缓冲已满：先吐出可判定的文本，再重算剩余空间。 */
            flush_text(rx, on_text, user);
            space = sizeof(rx->buf) - rx->len;
            if (space == 0) {
                /* 极端情形：缓冲里只剩候选同步字，强制前进一个字节。 */
                drop_first_byte(rx);
                space = 1;
            }
        }
        const size_t remaining = len - offset;
        const size_t take = remaining < space ? remaining : space;
        memcpy(&rx->buf[rx->len], &data[offset], take);
        rx->len += take;
        offset += take;
        process(rx, on_frame, on_text, user);
    }
}
