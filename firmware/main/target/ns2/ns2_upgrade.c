#include "ns2_upgrade.h"

#include <string.h>

void ns2_upgrade_reset(ns2_upgrade_t *up)
{
    memset(up, 0, sizeof(*up));
    up->min_record = SIZE_MAX;
}

size_t ns2_upgrade_frame_body(const ns2_upgrade_t *up)
{
    if (up->frame_len < NS2_FRAME_HEADER_LEN) {
        return 0;
    }
    return ((size_t)up->frame[4] << 8) | (size_t)up->frame[5];
}

/** 记录流留样：只保留最近 NS2_UPGRADE_TAIL_CAP 字节。 */
static void tail_push(ns2_upgrade_t *up, const uint8_t *data, size_t len)
{
    if (len >= NS2_UPGRADE_TAIL_CAP) {
        memcpy(up->tail, &data[len - NS2_UPGRADE_TAIL_CAP], NS2_UPGRADE_TAIL_CAP);
        up->tail_len = NS2_UPGRADE_TAIL_CAP;
        return;
    }
    if (up->tail_len + len > NS2_UPGRADE_TAIL_CAP) {
        const size_t drop = up->tail_len + len - NS2_UPGRADE_TAIL_CAP;
        memmove(up->tail, &up->tail[drop], up->tail_len - drop);
        up->tail_len -= drop;
    }
    memcpy(&up->tail[up->tail_len], data, len);
    up->tail_len += len;
}

/** 装配缓冲是否已凑齐一帧：帧头 8 字节 + 帧头声明的体长。 */
static ns2_upgrade_event_t check_frame(ns2_upgrade_t *up)
{
    if (up->truncated || up->frame_len < NS2_FRAME_HEADER_LEN) {
        return NS2_UPGRADE_NONE;
    }
    const size_t total = NS2_FRAME_HEADER_LEN + ns2_upgrade_frame_body(up);
    if (total > NS2_UPGRADE_FRAME_CAP) {
        up->truncated = true;
        return NS2_UPGRADE_NONE;
    }
    if (up->frame_len < total) {
        return NS2_UPGRADE_NONE;
    }
    up->frames++;
    if (up->sample_len == 0) {
        const size_t n = up->frame_len < NS2_UPGRADE_SAMPLE_CAP ? up->frame_len
                                                                : NS2_UPGRADE_SAMPLE_CAP;
        memcpy(up->sample, up->frame, n);
        up->sample_len = n;
    }
    up->frame_ready = true;
    return NS2_UPGRADE_FRAME;
}

ns2_upgrade_event_t ns2_upgrade_feed(ns2_upgrade_t *up, const uint8_t *data, size_t len)
{
    up->records++;
    up->bytes += (uint32_t)len;
    if (len < up->min_record) {
        up->min_record = len;
    }
    if (len > up->max_record) {
        up->max_record = len;
    }
    tail_push(up, data, len);
    if (data == NULL || len < NS2_UPGRADE_RECORD_HEADER_LEN) {
        return NS2_UPGRADE_MALFORMED;
    }
    /* 上一帧已经交出去：这一条记录起算新的一帧，不管它的类型字段是什么
     * （每帧都以帧首记录开头，缺首记录时不能把两帧拼在一起）。 */
    if (up->frame_ready) {
        up->frame_ready = false;
        up->frame_len = 0;
        up->truncated = false;
    }
    if (data[0] == NS2_UPGRADE_RECORD_FIRST) {
        up->frame_len = 0;
        up->truncated = false;
    }
    const size_t claimed = (size_t)data[2] | ((size_t)data[3] << 8);
    size_t payload_len = len - NS2_UPGRADE_RECORD_HEADER_LEN;
    if (payload_len > claimed) {
        payload_len = claimed;
    }
    if (payload_len > 0) {
        const size_t room = NS2_UPGRADE_FRAME_CAP - up->frame_len;
        size_t n = payload_len;
        if (n > room) {
            n = room;
            up->truncated = true;
        }
        memcpy(&up->frame[up->frame_len], &data[NS2_UPGRADE_RECORD_HEADER_LEN], n);
        up->frame_len += n;
    }
    return check_frame(up);
}
