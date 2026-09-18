#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "haptic_synth.h"
#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * USB host 输入源（接收段）：手柄插在板卡 Type-C 上时，host 栈收到原始报告
 * 后同样组成 pad_report_t 交给 pad/ 的家族布局表——与桥接路径共用同一份解析
 * 与映射，本模块只负责搬运与设备标识。
 */

/** 注册 USB 输入源（dp_plane_start 里调用，与桥接源并列）。 */
void usb_input_register(void);

/** 当前是否已枚举到可用手柄。 */
bool usb_input_attached(void);

/** 已接入设备的标识（VID/PID 与连接方式）；未接入返回 false。 */
bool usb_input_device_ids(uint16_t *vid, uint16_t *pid, pad_conn_t *conn);

/** 设备描述（家族 / VID:PID / 报告长度），未接入时为 "none"。 */
const char *usb_input_device_desc(void);

/** 已接收的报告数与已写回的反馈报告数（诊断）。 */
uint32_t usb_input_report_count(void);
uint32_t usb_input_output_count(void);

/** 把编码好的输出报告交给 host 任务写进 OUT 端点；无设备或已在写时忽略。 */
void usb_input_send_output(const uint8_t *report, size_t len);

/** 音频触觉流是否在跑（USB 直插 DS5 时震动改走音频通道，HID 震动让位）。 */
bool usb_input_audio_haptics(void);

/** 更新音频触觉合成参数（usb_audio 的转发口，数据面调用）。 */
void usb_input_haptic(const haptic_synth_params_t *params);

/* --- 由 usb_host.c 的 host 任务调用 --- */

/** 设备接入/断开：重置报告快照与描述。 */
void usb_input_note_device(bool attached, uint16_t vid, uint16_t pid);

/** 收到一帧原始报告（含 Report ID 的首字节）。 */
void usb_input_submit_report(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
