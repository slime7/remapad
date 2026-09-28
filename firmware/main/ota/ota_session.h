#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA 升级会话核心（ESP 侧，传输无关）：驱动 ota_proto 把镜像写进非运行应用分区，
 * 校验通过后切启动分区并重启；帧的来源、ACK 的去向与进度广播经 ota_session_port_t
 * 注入，不认识任何具体传输与业务模块（本工程的桥接帧装配在 ota_link，换固件复用时
 * 只重写适配层）。flash 写入必须在内部 RAM 栈上执行：本模块任务、聚合缓冲与帧队列
 * 都固定在内部 RAM。
 */

/** 会话阶段，与界面 ota 属性同值。 */
typedef enum {
  OTA_SESSION_PHASE_IDLE = 0,
  OTA_SESSION_PHASE_RECEIVING = 1,
  OTA_SESSION_PHASE_VERIFYING = 2,
  OTA_SESSION_PHASE_REBOOTING = 3,
  OTA_SESSION_PHASE_FAILED = 4,
} ota_session_phase_t;

/** 帧来源翻译出的会话消息：传输帧型到这里的映射由适配层负责。 */
typedef enum {
  OTA_SESSION_MSG_BEGIN = 0,
  OTA_SESSION_MSG_DATA = 1,
  OTA_SESSION_MSG_END = 2,
} ota_session_msg_t;

/** 传输与业务出口：核心只经这份回调表触达外部世界，全部在接收任务上下文调用。 */
typedef struct {
  /** 回一帧 ACK：载荷已按协议编码（BEGIN 的应答末尾带 16 字节运行版本），
   *  发送策略（等待/重试/丢弃）由适配层决定。 */
  esp_err_t (*send_ack)(const uint8_t *payload, size_t len, void *user);
  /** 阶段/进度广播，可为 NULL；接收侧已按 1% 粒度限频，回调里不要再做重活。 */
  void (*on_progress)(ota_session_phase_t phase, uint32_t received, uint32_t total, void *user);
  /** 两个回调共用的上下文，原样透传。 */
  void *user;
} ota_session_port_t;

/** 启动升级会话（帧队列 + 接收任务）。须在帧来源开始投递之前调用。 */
esp_err_t ota_session_start(const ota_session_port_t *port);

/** 接收任务分派：非阻塞拷进队列，队列满就丢弃（发送端按窗口应答重发补齐）。 */
void ota_session_handle_frame(ota_session_msg_t msg, bool window_end, const uint8_t *payload, size_t len);

/** 应用就绪信号：与开机满 30 秒一起构成回滚健康门槛；就绪含义由装配方决定
 *  （本工程 = 核心服务启动完成，有屏与无屏构建同一判据，不看画面首帧）。 */
void ota_session_notify_ready(void);

/** 会话状态短名（串口 CLI status 用）：idle / receiving / verifying / rebooting / failed。 */
const char *ota_session_state_name(void);

/** 当前升级阶段与已收百分比（UI 底栏进度条用），取值与 ota_session_phase_t 同值。 */
void ota_session_progress(int *phase, int *percent);

/** 运行镜像的版本号（与 UI 系统页、OTA 应答同一来源）。 */
const char *ota_session_running_version(void);

/** 运行镜像所在分区标签（ota_0 / ota_1）。 */
const char *ota_session_running_partition(void);

/** 运行镜像是否处于「待验证」状态（回滚保护下升级后的首次启动）。 */
bool ota_session_pending_verify(void);

/** 手动回滚到上一个可用镜像并重启（仅待验证状态有效，串口 CLI rollback 用）。 */
esp_err_t ota_session_rollback_and_reboot(void);

#ifdef __cplusplus
}
#endif
