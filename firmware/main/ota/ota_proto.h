#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA 会话的纯逻辑（不依赖 ESP-IDF，可主机端测试）：把桥接帧里的 OTA 载荷拼成镜像字节流，
 * 负责序号判定、窗口应答、4 KB 聚合成块与空闲超时；写 flash 与写启动分区由上层回调承担。
 * 载荷布局见 ABSTRACTIONS 的桥接帧表。
 */

#define OTA_BEGIN_MAGIC "ROM1"
#define OTA_BEGIN_MAGIC_LEN 4u
#define OTA_BEGIN_PAYLOAD_LEN 8u

#define OTA_DATA_SEQ_LEN 2u
#define OTA_DATA_MAX_LEN 200u
#define OTA_DATA_PAYLOAD_MAX (OTA_DATA_SEQ_LEN + OTA_DATA_MAX_LEN)

#define OTA_ACK_PAYLOAD_LEN 8u
#define OTA_ACK_VERSION_LEN 16u

/** 数据帧的 slot 字段：1 表示这一帧是当前窗口的最后一帧（包括最后的不足一窗）。
 *  PC 端据此告诉设备「这一批发完了」，设备收到即回应答；不靠固定帧数或空闲超时，
 *  否则末尾的不足一窗要么永远等不到 ACK，要么要多等一个超时。 */
#define OTA_SLOT_WINDOW_END 1u

/** 每收满这么多数据帧回一次 ACK：PC 收到才发下一窗，构成收发流控。 */
#define OTA_ACK_WINDOW 16u

/** 同一期望序号的重复应答最小间隔：压掉整窗重发时的连串应答，又不至于把丢掉的
 *  那一次永久压制（应答在共享串口上可能被日志挤掉，PC 会按超时重问）。 */
#define OTA_DUPLICATE_REPLY_MIN_INTERVAL_US (50 * 1000LL)

/** 聚合缓冲：攒满一块写一次 flash，避免每个数据帧都开关一次 cache。 */
#define OTA_CHUNK_LEN 4096u

/** 接收中多久没有新帧视为链路断开。 */
#define OTA_SESSION_TIMEOUT_US (5 * 1000 * 1000LL)

/** 会话状态（与 PC 端 state 字段一致）。 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RECEIVING = 1,
    OTA_STATE_DONE = 2,
    OTA_STATE_FAILED = 3,
} ota_state_t;

/** 应答错误码（与 PC 端 code 字段一致）。 */
typedef enum {
    OTA_CODE_OK = 0,
    OTA_CODE_BUSY = 1,
    OTA_CODE_BAD_HEADER = 2,
    OTA_CODE_SEQ_ERROR = 3,
    OTA_CODE_FLASH_ERROR = 4,
    OTA_CODE_SIZE_MISMATCH = 5,
    OTA_CODE_VERIFY_FAILED = 6,
    OTA_CODE_TIMEOUT = 7,
} ota_code_t;

/** 一次处理的结论：`reply` 为真时上层把 state/code/next_seq/received 编码成 ACK 发回。 */
typedef struct {
    ota_state_t state;
    ota_code_t code;
    uint16_t next_seq;
    uint32_t received;
    bool reply;
    /** END 已收齐声明长度且数据全部交付上层，等上层校验镜像。 */
    bool finished;
} ota_proto_result_t;

/** 交付一块镜像数据：成功返回 OTA_CODE_OK，失败返回具体错误码（会话随即作废）。 */
typedef ota_code_t (*ota_flush_fn)(const uint8_t *data, size_t len, void *user);

typedef struct {
    ota_state_t state;
    ota_code_t code;
    uint16_t next_seq;
    uint32_t received;
    uint32_t image_size;
    size_t chunk_len;
    uint16_t accepted_since_ack;
    /** 已经为当前期望序号回过一次序号错误应答。 */
    bool seq_error_reported;
    /** 上次为重复序号回应答的时刻（微秒），用来给重发应答限流。 */
    int64_t seq_error_reply_us;
    bool finished;
    int64_t last_rx_us;
    uint8_t chunk[OTA_CHUNK_LEN];
} ota_proto_t;

/** 复位会话（回到空闲，清聚合缓冲）。 */
void ota_proto_init(ota_proto_t *proto);

/** 解析 BEGIN 载荷：magic 不符或长度不足返回 false。 */
bool ota_proto_parse_begin(const uint8_t *payload, size_t len, uint32_t *image_size);

/**
 * 接受 BEGIN：image_size 是声明的镜像大小，max_image_size 是目标分区容量。
 * 尺寸为 0 或超出分区即拒绝并回 BAD_HEADER，不进入接收态。
 */
ota_proto_result_t ota_proto_begin(ota_proto_t *proto, uint32_t image_size,
                                   uint32_t max_image_size, int64_t now_us);

/**
 * 刷新空闲计时起点：BEGIN 之后的目标分区预擦在应答之前完成，
 * 那段时间不该算进接收窗口，否则大镜像会在 PC 收到应答前就判超时。
 * 会话不在接收态时是空操作。
 */
void ota_proto_note_rx(ota_proto_t *proto, int64_t now_us);

/**
 * 收一块镜像数据：序号不符回 SEQ_ERROR（next_seq 是 PC 的重发起点）。window_end
 * 为真（帧 slot 标记）或已收满一个固定窗口时回 ACK。
 */
ota_proto_result_t ota_proto_data(ota_proto_t *proto, const uint8_t *payload, size_t len,
                                  bool window_end, int64_t now_us, ota_flush_fn flush,
                                  void *user);

/** 声明数据发完：字节数与声明不符即失败，否则把剩余数据交付上层并置 finished。 */
ota_proto_result_t ota_proto_end(ota_proto_t *proto, int64_t now_us, ota_flush_fn flush,
                                 void *user);

/** 周期调用：接收中空闲超时即作废会话并回 TIMEOUT。 */
ota_proto_result_t ota_proto_tick(ota_proto_t *proto, int64_t now_us);

/** 上层判定失败时收尾（写 flash 出错、镜像校验不过等）。 */
ota_proto_result_t ota_proto_fail(ota_proto_t *proto, ota_code_t code);

/** 上层校验通过时收尾。 */
ota_proto_result_t ota_proto_done(ota_proto_t *proto);

/** 当前会话是否占用中（接收或已收完待校验）。 */
bool ota_proto_busy(const ota_proto_t *proto);

/**
 * 编码 ACK 载荷：state + code + next_seq + received；with_version 为真时追加
 * 16 字节运行版本（BEGIN 的应答用）。缓冲不足返回 0。
 */
size_t ota_proto_encode_ack(const ota_proto_result_t *result, const char *version,
                            bool with_version, uint8_t *out, size_t out_len);

#ifdef __cplusplus
}
#endif
