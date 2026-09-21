#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * amiibo 镜像上传的桥接帧会话纯逻辑（不依赖 ESP-IDF，可主机端测试）：把 PC 经桥接帧发来的
 * BEGIN/DATA/END 拼成完整镜像，收齐后经存储回调落库。帧载荷与 ACK 语义见 ABSTRACTIONS 的桥接帧表；
 * 重复的 DATA 帧按幂等处理，ACK 的 received 就是续传起点。
 */

#include "ns2_nfc.h"

#define AMIIBO_NAME_MAX 31u
#define AMIIBO_DATA_MAX_LEN 200u
#define AMIIBO_ACK_PAYLOAD_LEN 7u
/** 接收中多久没有新帧视为链路断开（与 OTA 会话同值）。 */
#define AMIIBO_SESSION_TIMEOUT_US (5 * 1000 * 1000LL)

/** 会话状态（与 PC 端 state 字段一致）。 */
typedef enum {
    AMIIBO_STATE_IDLE = 0,
    AMIIBO_STATE_RECEIVING = 1,
    AMIIBO_STATE_DONE = 2,
    AMIIBO_STATE_FAILED = 3,
} amiibo_state_t;

/** 应答错误码（与 PC 端 code 字段一致，数值对齐 ota_code_t 的同义项）。 */
typedef enum {
    AMIIBO_CODE_OK = 0,
    AMIIBO_CODE_BUSY = 1,
    AMIIBO_CODE_BAD_HEADER = 2,
    AMIIBO_CODE_OFFSET_ERROR = 3,
    AMIIBO_CODE_STORE_ERROR = 4,
    AMIIBO_CODE_SIZE_MISMATCH = 5,
    AMIIBO_CODE_TIMEOUT = 7,
} amiibo_code_t;

/** 一次处理的结论：`reply` 为真时上层把 state/code/received/slot 编码成 ACK 发回。 */
typedef struct {
    amiibo_state_t state;
    amiibo_code_t code;
    uint32_t received;
    /** 存储层返回的槽位号（END 成功时有效，其余 -1）。 */
    int slot;
    bool reply;
    /** END 收齐且已落库。 */
    bool finished;
} amiibo_upload_result_t;

/** 落库回调：name 已截断到 AMIIBO_NAME_MAX；成功返回槽位号（>=0），失败返回负值。 */
typedef int (*amiibo_store_fn)(const char *name, const uint8_t *data, size_t len, void *user);

typedef struct {
    amiibo_state_t state;
    /** BEGIN 声明的镜像大小（540 或 572），END 按它核对收满。 */
    uint32_t expected;
    uint32_t received;
    char name[AMIIBO_NAME_MAX + 1];
    uint8_t buf[NS2_NFC_IMAGE_MAX];
    int64_t last_rx_us;
} amiibo_upload_t;

/** 复位会话（回到空闲）。 */
void amiibo_upload_init(amiibo_upload_t *up);

/** 解析 BEGIN 载荷：name/size 合法返回 true（name 以 NUL 结尾）。 */
bool amiibo_upload_parse_begin(const uint8_t *payload, size_t len, char *name, uint32_t *size);

/** 接受 BEGIN：尺寸必须恰好一份 NTAG215 镜像，名字非空且不超长。 */
amiibo_upload_result_t amiibo_upload_begin(amiibo_upload_t *up, const uint8_t *payload,
                                           size_t len, int64_t now_us);

/** 收一帧数据：偏移必须与已收字节数衔接，否则报 OFFSET_ERROR（received 是续传起点）。 */
amiibo_upload_result_t amiibo_upload_data(amiibo_upload_t *up, const uint8_t *payload,
                                          size_t len, int64_t now_us);

/** 声明数据发完：字节数不符即失败，收齐则调用存储回调并回 DONE。 */
amiibo_upload_result_t amiibo_upload_end(amiibo_upload_t *up, int64_t now_us,
                                         amiibo_store_fn store, void *user);

/** 周期调用：接收中空闲超时即作废会话并回 TIMEOUT。 */
amiibo_upload_result_t amiibo_upload_tick(amiibo_upload_t *up, int64_t now_us);

/** 编码 ACK 载荷：state + code + received + slot。缓冲不足返回 0。 */
size_t amiibo_upload_encode_ack(const amiibo_upload_result_t *result, uint8_t *out, size_t cap);

/** 会话是否占用中。 */
bool amiibo_upload_busy(const amiibo_upload_t *up);

#ifdef __cplusplus
}
#endif
