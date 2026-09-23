#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "input_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA 升级会话（ESP 侧）：接住 input_link 分派来的 OTA 帧，把镜像写进非运行应用分区，
 * 校验通过后切启动分区并重启；协议与聚合逻辑在 ota_proto，这里只做队列、flash 写入与回滚健康门槛。
 * flash 写入必须在内部 RAM 栈上执行：本模块任务、聚合缓冲与帧队列都固定在内部 RAM。
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

/** 当前升级阶段与已收百分比（UI 底栏进度条用）：
 *  phase 取 0 idle、1 receiving、2 verifying、3 rebooting、4 failed，与界面属性同值。 */
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
