#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "input_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA 升级会话（ESP 侧）：接住 input_link 分派来的 OTA 帧，把镜像写进非运行
 * 应用分区，`esp_ota_end` 校验通过后把启动分区切过去并重启。协议与聚合逻辑
 * 在 ota_proto，这里只做队列、flash 写入、回滚健康门槛与重启。
 *
 * 通道复用 USB-Serial/JTAG 上的桥接帧，不切 USB mux、不经过 BLE；PC 端工具
 * 见 pc/ota.py。flash 写入必须在内部 RAM 栈上执行（本模块任务由 xTaskCreate
 * 创建，栈来自内部 RAM），聚合缓冲与帧队列同样固定在内部 RAM。
 */

/** 该帧类型是否属于 OTA 接收通道（BEGIN / DATA / END）。 */
bool ota_session_is_frame_type(uint8_t type);

/** 启动升级通道（帧队列 + 接收任务）。须在 input_link_start 之前调用。 */
esp_err_t ota_session_start(void);

/** 接收任务分派：非阻塞拷进队列，队列满就丢弃（PC 端窗口重发补齐）。 */
void ota_session_handle_frame(const input_frame_view_t *frame);

/** UI 首帧提交成功后调用，与开机时长一起构成回滚健康门槛。 */
void ota_session_notify_ui_ready(void);

/** 会话状态短名（串口 CLI status 用）：idle / receiving / verifying / rebooting / failed。 */
const char *ota_session_state_name(void);

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
