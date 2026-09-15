#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 目标手柄事实：不属于手柄输入、来自板卡与主机会话的字段（电池、主机开启
 * 的特性、amiibo 状态）。它们由数据面每周期刷新，目标实现负责折进报文。
 */
typedef struct {
    uint8_t battery_level; /**< 0-9，对齐 NS2 电量档位。 */
    uint16_t battery_mv;
    bool charging;
    bool external_power;
    bool rumble_enabled; /**< 主机开启了触觉/震动特性。 */
    uint8_t nfc_state;   /**< 预置 amiibo 的就绪状态。 */
} pad_target_facts_t;

/**
 * 目标编码器接口（转换段）：把私有格式编码成某个目标手柄家族的报文。
 * 新增目标（例如未来的 NS1）只需实现这个结构并在启动时注册。
 */
typedef struct {
    const char *name;
    /** 目标支持的能力集合（PAD_CAP_*）：不在集合里的私有字段会被丢弃。 */
    uint32_t caps;
    /** 目标自带的报告语言（pad_lang_t）：与输入设备的自带语言一致时，
     *  输入走透传而不是解析重编码。 */
    uint8_t language;
    /** 刷新目标侧事实（可每周期调用，实现需自行判断是否需要更新）。 */
    void (*set_facts)(const pad_target_facts_t *facts);
    /** 用当前私有状态发送一轮输入报告。 */
    void (*send_pad)(const pad_state_t *pad);
    /** 同代透传：原样转发一帧设备报告体；条件不满足或目标不支持时返回 false。 */
    bool (*send_raw)(const pad_state_t *pad);
} pad_target_t;

/** 注册当前目标；传 NULL 表示停用目标（数据面照常采样，不再发送）。 */
void target_set(const pad_target_t *target);

/** 当前目标；未注册时为 NULL。 */
const pad_target_t *target_get(void);

/** 当前目标名；未注册时返回 "none"。 */
const char *target_name(void);

/** 刷新目标侧事实；未注册时忽略。 */
void target_set_facts(const pad_target_facts_t *facts);

/** 发送一轮输入报告；未注册时忽略。 */
void target_send_pad(const pad_state_t *pad);

/** 当前目标的报告语言（pad_lang_t）；未注册时为 PAD_LANG_NONE。 */
uint8_t target_language(void);

/** 同代透传开关（串口 CLI 的 relay 0|1；默认打开）。 */
void target_set_relay(bool enabled);

/** 当前透传开关状态。 */
bool target_relay_enabled(void);

#ifdef __cplusplus
}
#endif
