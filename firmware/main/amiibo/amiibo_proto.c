#include "amiibo_proto.h"

#include <string.h>

void amiibo_upload_init(amiibo_upload_t *up)
{
    memset(up, 0, sizeof(*up));
    up->state = AMIIBO_STATE_IDLE;
}

bool amiibo_upload_parse_begin(const uint8_t *payload, size_t len, char *name, uint32_t *size)
{
    if (payload == NULL || len < 1 + 4) {
        return false;
    }
    const size_t name_len = payload[0];
    if (name_len == 0 || name_len > AMIIBO_NAME_MAX || len < 1 + name_len + 4) {
        return false;
    }
    memcpy(name, &payload[1], name_len);
    name[name_len] = 0;
    const size_t size_off = 1 + name_len;
    *size = (uint32_t)payload[size_off] | ((uint32_t)payload[size_off + 1] << 8) |
            ((uint32_t)payload[size_off + 2] << 16) | ((uint32_t)payload[size_off + 3] << 24);
    return *size == NS2_NFC_TAG_SIZE || *size == NS2_NFC_IMAGE_MAX;
}

amiibo_upload_result_t amiibo_upload_begin(amiibo_upload_t *up, const uint8_t *payload,
                                           size_t len, int64_t now_us)
{
    amiibo_upload_result_t refused = {
        .state = AMIIBO_STATE_FAILED,
        .code = AMIIBO_CODE_BAD_HEADER,
        .received = 0,
        .slot = -1,
        .reply = true,
        .finished = false,
    };
    if (amiibo_upload_busy(up)) {
        refused.code = AMIIBO_CODE_BUSY;
        return refused;
    }
    char name[AMIIBO_NAME_MAX + 1];
    uint32_t size = 0;
    if (!amiibo_upload_parse_begin(payload, len, name, &size)) {
        return refused;
    }
    up->state = AMIIBO_STATE_RECEIVING;
    up->expected = size;
    up->received = 0;
    memcpy(up->name, name, sizeof(name));
    up->last_rx_us = now_us;
    const amiibo_upload_result_t accepted = {
        .state = AMIIBO_STATE_RECEIVING,
        .code = AMIIBO_CODE_OK,
        .received = 0,
        .slot = -1,
        .reply = true,
        .finished = false,
    };
    return accepted;
}

amiibo_upload_result_t amiibo_upload_data(amiibo_upload_t *up, const uint8_t *payload,
                                          size_t len, int64_t now_us)
{
    amiibo_upload_result_t result = {
        .state = up->state,
        .code = AMIIBO_CODE_OK,
        .received = up->received,
        .slot = -1,
        .reply = true,
        .finished = false,
    };
    if (up->state != AMIIBO_STATE_RECEIVING || len < 2) {
        result.state = AMIIBO_STATE_FAILED;
        result.code = AMIIBO_CODE_OFFSET_ERROR;
        return result;
    }
    const uint16_t offset = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
    const size_t data_len = len - 2;
    if (data_len == 0 || data_len > AMIIBO_DATA_MAX_LEN ||
        offset + data_len > up->expected || offset > up->received) {
        result.code = AMIIBO_CODE_OFFSET_ERROR;
        return result;
    }
    if (offset + data_len <= up->received) {
        /* PC 端一次突发三帧、按 ACK 续传：ACK 在共享串口上被日志挤掉时整段
         * 重发，已收部分按重复处理（不推进、回当前 received）。 */
        return result;
    }
    memcpy(&up->buf[offset], &payload[2], data_len);
    up->received = (uint32_t)(offset + data_len);
    up->last_rx_us = now_us;
    result.received = up->received;
    return result;
}

amiibo_upload_result_t amiibo_upload_end(amiibo_upload_t *up, int64_t now_us,
                                         amiibo_store_fn store, void *user)
{
    (void)now_us;
    amiibo_upload_result_t result = {
        .state = up->state,
        .code = AMIIBO_CODE_OK,
        .received = up->received,
        .slot = -1,
        .reply = true,
        .finished = false,
    };
    if (up->state != AMIIBO_STATE_RECEIVING || up->received != up->expected) {
        result.state = AMIIBO_STATE_FAILED;
        result.code = AMIIBO_CODE_SIZE_MISMATCH;
        up->state = AMIIBO_STATE_IDLE;
        return result;
    }
    if (store == NULL) {
        result.state = AMIIBO_STATE_FAILED;
        result.code = AMIIBO_CODE_STORE_ERROR;
        up->state = AMIIBO_STATE_IDLE;
        return result;
    }
    const int slot = store(up->name, up->buf, up->expected, user);
    if (slot < 0) {
        result.state = AMIIBO_STATE_FAILED;
        result.code = AMIIBO_CODE_STORE_ERROR;
        up->state = AMIIBO_STATE_IDLE;
        return result;
    }
    up->state = AMIIBO_STATE_IDLE;
    result.state = AMIIBO_STATE_DONE;
    result.slot = slot;
    result.finished = true;
    return result;
}

amiibo_upload_result_t amiibo_upload_tick(amiibo_upload_t *up, int64_t now_us)
{
    amiibo_upload_result_t result = {
        .state = up->state,
        .code = AMIIBO_CODE_OK,
        .received = up->received,
        .slot = -1,
        .reply = false,
        .finished = false,
    };
    if (up->state != AMIIBO_STATE_RECEIVING ||
        now_us - up->last_rx_us <= AMIIBO_SESSION_TIMEOUT_US) {
        return result;
    }
    result.state = AMIIBO_STATE_FAILED;
    result.code = AMIIBO_CODE_TIMEOUT;
    result.reply = true;
    up->state = AMIIBO_STATE_IDLE;
    return result;
}

size_t amiibo_upload_encode_ack(const amiibo_upload_result_t *result, uint8_t *out, size_t cap)
{
    if (cap < AMIIBO_ACK_PAYLOAD_LEN) {
        return 0;
    }
    out[0] = (uint8_t)result->state;
    out[1] = (uint8_t)result->code;
    out[2] = (uint8_t)(result->received & 0xFFu);
    out[3] = (uint8_t)((result->received >> 8) & 0xFFu);
    out[4] = (uint8_t)((result->received >> 16) & 0xFFu);
    out[5] = (uint8_t)((result->received >> 24) & 0xFFu);
    out[6] = (uint8_t)(result->slot < 0 ? 0xFFu : (unsigned)result->slot);
    return AMIIBO_ACK_PAYLOAD_LEN;
}

bool amiibo_upload_busy(const amiibo_upload_t *up)
{
    return up->state == AMIIBO_STATE_RECEIVING;
}
