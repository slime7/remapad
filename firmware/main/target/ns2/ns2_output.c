#include "ns2_output.h"

#include <string.h>

#include "ns2_nfc.h"
#include "ns2_report.h"

static struct {
    ns2_output_sink_t sink;
    ns2_feedback_fn feedback_fn;
    void *feedback_user;

    uint8_t counter09;
    uint32_t counter05;

    uint8_t battery_level;
    uint16_t battery_mv;
    bool charging;
    bool external_power;

    uint8_t motion_mode; /* ns2_motion_mode_t：0x09 运动块占位方式 */
    bool rumble_enabled; /* 主机开启了触觉特性 */

    /* 耳机状态字节（0x09 偏移 0x0D，同时决定 0x05 的插入位）：派生值来自
     * 输入设备的 3.5mm 状态，覆盖值供对照（串口 headset 命令）。 */
    bool headset_override_on;
    uint8_t headset_override;
    uint8_t headset_derived;
} s_out;

void ns2_output_set_sink(const ns2_output_sink_t *sink)
{
    if (sink != NULL) {
        s_out.sink = *sink;
    } else {
        memset(&s_out.sink, 0, sizeof(s_out.sink));
    }
}

void ns2_output_set_feedback_listener(ns2_feedback_fn fn, void *user)
{
    s_out.feedback_fn = fn;
    s_out.feedback_user = user;
}

void ns2_output_send(const ns2_controller_state_t *state)
{
    if (s_out.sink.session_count == NULL || s_out.sink.session_info == NULL ||
        s_out.sink.send_report == NULL) {
        return;
    }
    /* 电池字段以 ns2_output_set_battery 的最新值为准（输入源可能不带电池）。 */
    ns2_controller_state_t merged = *state;
    merged.motion_mode = s_out.motion_mode;
    merged.headset_state = ns2_output_headset_byte();
    if (s_out.battery_level != 0 || s_out.battery_mv != 0) {
        merged.battery_level = s_out.battery_level;
        merged.battery_mv = s_out.battery_mv;
        merged.charging = s_out.charging;
        merged.external_power = s_out.external_power;
    }
    const size_t sessions = s_out.sink.session_count(s_out.sink.user);
    for (size_t i = 0; i < sessions; i++) {
        uint8_t identity = NS2_ID_PRO;
        uint8_t format = NS2_REPORT_ID_09;
        if (!s_out.sink.session_info(i, &identity, &format, s_out.sink.user)) {
            continue;
        }
        /* 通用报告（0x05，USB 模式）与专用报告（0x09，蓝牙）两种报文体，
         * 长度都是 63 字节，按会话格式选编码器。 */
        uint8_t report[NS2_INPUT_09_LEN];
        if (format == NS2_REPORT_ID_05) {
            ns2_encode_input_05(report, &merged, s_out.counter05++);
        } else {
            ns2_encode_input_09(report, &merged, s_out.counter09++);
        }
        s_out.sink.send_report(i, format, report, sizeof(report), s_out.sink.user);
    }
}

void ns2_output_set_battery(uint8_t level, uint16_t voltage_mv, bool charging, bool external)
{
    s_out.battery_level = level;
    s_out.battery_mv = voltage_mv;
    s_out.charging = charging;
    s_out.external_power = external;
}

void ns2_output_set_motion_mode(uint8_t mode)
{
    s_out.motion_mode = mode;
}

void ns2_output_set_rumble_enabled(bool enabled)
{
    s_out.rumble_enabled = enabled;
}

uint8_t ns2_output_motion_mode(void)
{
    return s_out.motion_mode;
}

void ns2_output_set_headset_derived(uint8_t value)
{
    s_out.headset_derived = value;
}

uint8_t ns2_output_headset_derived(void)
{
    return s_out.headset_derived;
}

void ns2_output_set_headset_override(bool enabled, uint8_t value)
{
    s_out.headset_override_on = enabled;
    s_out.headset_override = value;
}

bool ns2_output_headset_override(uint8_t *out_value)
{
    if (out_value != NULL) {
        *out_value = s_out.headset_override;
    }
    return s_out.headset_override_on;
}

uint8_t ns2_output_headset_byte(void)
{
    return s_out.headset_override_on ? s_out.headset_override : s_out.headset_derived;
}

bool ns2_output_send_raw(const pad_state_t *pad)
{
    if (pad == NULL || pad->raw_len == 0 || s_out.sink.session_count == NULL ||
        s_out.sink.session_info == NULL || s_out.sink.send_report == NULL) {
        return false;
    }
    const uint8_t report_id = pad->raw_report_id;
    if (report_id != NS2_REPORT_ID_05 && report_id != NS2_REPORT_ID_09) {
        return false;
    }
    const size_t body_len = report_id == NS2_REPORT_ID_09 ? NS2_INPUT_09_LEN : NS2_INPUT_05_LEN;
    if (pad->raw_len != body_len + 1u) {
        return false;
    }
    uint8_t body[NS2_INPUT_09_LEN];
    size_t delivered = 0;
    const size_t sessions = s_out.sink.session_count(s_out.sink.user);
    for (size_t i = 0; i < sessions; i++) {
        uint8_t identity = NS2_ID_PRO;
        uint8_t format = NS2_REPORT_ID_09;
        if (!s_out.sink.session_info(i, &identity, &format, s_out.sink.user)) {
            continue;
        }
        /* 会话身份只有 Pro 一种，透传只按报告格式对账：同代手柄的报文体与目标
         * 语言一致时原样转发，格式对不上就回落解析重编码。 */
        if (format != report_id) {
            continue;
        }
        memcpy(body, &pad->raw[1], body_len);
        if (report_id == NS2_REPORT_ID_09) {
            body[NS2_09_OFF_STATUS] = s_out.rumble_enabled ? 0x38 : 0x30;
            body[NS2_09_OFF_NFC] = ns2_output_nfc_state();
            /* 耳机状态与编码路径同源：PC 手柄的 3.5mm 状态或串口覆盖值。 */
            body[NS2_09_OFF_HEADSET] = ns2_output_headset_byte();
        }
        s_out.sink.send_report(i, report_id, body, body_len, s_out.sink.user);
        delivered++;
    }
    return delivered > 0;
}

uint8_t ns2_output_nfc_state(void)
{
    /* NFC 状态字节由 ns2_nfc 的标签模拟状态机决定（开轮询且预置镜像 0x01）。 */
    return ns2_nfc_report_state();
}

void ns2_output_emit_rumble(const ns2_rumble_event_t *event)
{
    if (s_out.feedback_fn != NULL && event != NULL) {
        s_out.feedback_fn(NS2_FEEDBACK_RUMBLE, event, s_out.feedback_user);
    }
}

/** 「在震」的载波电平上限（8 位刻度）：静止包是零振幅的频率字段（音圈静置
 *  频率），天然不算在震；万一主机真的用极低振幅维持 LRA 通路，这种电平在
 *  任何马达上都感知不到，归一强度不超过它的参数包也不算在震。 */
#define NS2_RUMBLE_CARRIER_MAX 2u

/** 一个时序子帧的位串（5 字节小端）：低频频率 bit0-8、低频振幅 bit10-19、高频频率 bit20-28、
 *  使能 bit31、高频振幅 bit32-39；位布局与频率刻度见 docs/controller-switch2.md 的输出报告一节。 */
#define NS2_KEY_LF_FREQ_SHIFT 0u
#define NS2_KEY_LF_FREQ_MASK 0x1FFu
#define NS2_KEY_LF_AMP_SHIFT 10u
#define NS2_KEY_LF_AMP_MASK 0x3FFu
#define NS2_KEY_HF_FREQ_SHIFT 20u
#define NS2_KEY_HF_FREQ_MASK 0x1FFu
#define NS2_KEY_HF_AMP_SHIFT 0u
#define NS2_KEY_HF_AMP_MASK 0xFFu

static uint64_t key_word(const uint8_t *p)
{
    uint64_t v = 0;
    for (size_t i = 0; i < 5; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/** 2^(1/2^n) × 65536（n=1..7）：把频率码按八度逐位展开成定点乘数。
 *  首项 92682 超出 16 位，元素必须落在 uint32_t 上。 */
static const uint32_t s_freq_oct_frac[7] = {92682, 77935, 71461, 68442, 66965, 66229, 65878};

/** 频率码落地 Hz：字段是 9 位 log2 刻度，f = 10×2^(码/128)——与 Joy-Con
 *  一代 log2(f/10)×32 同一条曲线、4 倍细分（BlueRetro 的驱动常量 0x100/
 *  0x180 在这条曲线上正好是整八度 40/80Hz，SDL 缺省码 0x112/0x187 落在
 *  经典的 44/83Hz 低高对）。码 0 按「未声明」返回 0，消费侧回落缺省值；
 *  9 位码的满量程是 159Hz，恰好盖住音圈的低频有效区。 */
static uint16_t key_freq_hz(uint16_t code)
{
    if (code == 0) {
        return 0;
    }
    if (code > NS2_KEY_LF_FREQ_MASK) {
        code = NS2_KEY_LF_FREQ_MASK;
    }
    uint64_t f = (uint64_t)(10u << (code >> 7)) << 16;
    const uint16_t frac = code & 0x7Fu;
    for (size_t i = 0; i < 7; i++) {
        if ((frac & (0x40u >> i)) != 0) {
            f = ((f * s_freq_oct_frac[i]) + 0x8000u) >> 16;
        }
    }
    return (uint16_t)((f + 0x8000u) >> 16);
}

void ns2_rumble_band_strengths(const uint8_t raw[16], uint8_t *lf, uint8_t *hf)
{
    if (lf != NULL) {
        *lf = 0;
    }
    if (hf != NULL) {
        *hf = 0;
    }
    if (raw == NULL) {
        return;
    }
    /* 参数包：字节 0 是状态字，其后 3 个时序子帧各 5 字节。低频振幅逐帧取
     * 三帧最大（10 位右移两位压到 8 位刻度），高频振幅本身是 8 位字节、
     * 原样取最大，与私有的 0-255 强度对齐。 */
    uint8_t best_lf = 0;
    uint8_t best_hf = 0;
    for (size_t g = 0; g < 3; g++) {
        const uint64_t v = key_word(&raw[1 + g * 5]);
        const uint8_t group_lf =
            (uint8_t)(((v >> NS2_KEY_LF_AMP_SHIFT) & NS2_KEY_LF_AMP_MASK) >> 2);
        const uint8_t group_hf =
            (uint8_t)((v >> 32) & NS2_KEY_HF_AMP_MASK);
        if (group_lf > best_lf) {
            best_lf = group_lf;
        }
        if (group_hf > best_hf) {
            best_hf = group_hf;
        }
    }
    if (lf != NULL) {
        *lf = best_lf;
    }
    if (hf != NULL) {
        *hf = best_hf;
    }
}

uint8_t ns2_rumble_strength(const uint8_t raw[16])
{
    uint8_t lf = 0;
    uint8_t hf = 0;
    ns2_rumble_band_strengths(raw, &lf, &hf);
    return hf > lf ? hf : lf;
}

void ns2_rumble_band_frequencies(const uint8_t raw[16], uint16_t *lf_hz, uint16_t *hf_hz)
{
    if (lf_hz != NULL) {
        *lf_hz = 0;
    }
    if (hf_hz != NULL) {
        *hf_hz = 0;
    }
    if (raw == NULL) {
        return;
    }
    /* 频率字段与振幅同处一个 5 字节子帧：低频在位 0-8、高频在位 20-28，
     * 解出 Hz 后逐帧取最大。 */
    uint16_t best_lf = 0;
    uint16_t best_hf = 0;
    for (size_t g = 0; g < 3; g++) {
        const uint64_t v = key_word(&raw[1 + g * 5]);
        const uint16_t group_lf =
            key_freq_hz((uint16_t)((v >> NS2_KEY_LF_FREQ_SHIFT) & NS2_KEY_LF_FREQ_MASK));
        const uint16_t group_hf =
            key_freq_hz((uint16_t)((v >> NS2_KEY_HF_FREQ_SHIFT) & NS2_KEY_HF_FREQ_MASK));
        if (group_lf > best_lf) {
            best_lf = group_lf;
        }
        if (group_hf > best_hf) {
            best_hf = group_hf;
        }
    }
    if (lf_hz != NULL) {
        *lf_hz = best_lf;
    }
    if (hf_hz != NULL) {
        *hf_hz = best_hf;
    }
}

size_t ns2_rumble_keys(const uint8_t raw[16], ns2_rumble_key_t keys[PAD_RUMBLE_KEY_COUNT])
{
    if (keys == NULL) {
        return 0;
    }
    const size_t count = 3;
    for (size_t g = 0; g < count; g++) {
        const uint8_t *p = raw != NULL ? &raw[1 + g * 5] : NULL;
        ns2_rumble_key_t *key = &keys[g];
        if (p == NULL) {
            key->lf_freq = 0;
            key->lf_amp = 0;
            key->hf_freq = 0;
            key->hf_amp = 0;
            continue;
        }
        const uint64_t v = key_word(p);
        key->hf_freq = key_freq_hz((uint16_t)((v >> NS2_KEY_HF_FREQ_SHIFT) & NS2_KEY_HF_FREQ_MASK));
        /* 高频振幅是 8 位字节，左移两位抬到 10 位刻度，与低频振幅共用下游
         * 的「压回 8 位」路径。 */
        key->hf_amp = (uint16_t)(((v >> 32) & NS2_KEY_HF_AMP_MASK) << 2);
        key->lf_freq = key_freq_hz((uint16_t)((v >> NS2_KEY_LF_FREQ_SHIFT) & NS2_KEY_LF_FREQ_MASK));
        key->lf_amp = (uint16_t)((v >> NS2_KEY_LF_AMP_SHIFT) & NS2_KEY_LF_AMP_MASK);
    }
    if (raw != NULL) {
        /* 状态字 bit4-5 是有效子帧数（载波包为 1、游戏包为 3）；0 表示
         * 未声明，按满 3 帧处理。 */
        const size_t declared = (raw[0] >> 4) & 0x3u;
        if (declared != 0 && declared < count) {
            return declared;
        }
    }
    return count;
}

bool ns2_rumble_parse(const uint8_t *data, size_t len, ns2_rumble_event_t *out)
{
    if (data == NULL || out == NULL) {
        return false;
    }
    /* 32 字节 = BLE 形态（此长度不带 Report ID）；
     * 33 字节及以上 = 多带 1 字节 Report ID/占位前缀的形态。 */
    const size_t body = len >= 1 + 32 ? 1 : 0;
    if (len < body + 32) {
        return false;
    }
    memcpy(out->raw, &data[body], sizeof(out->raw));
    /* LRA 状态字 bit6 = 启用标志。游戏里主机以接近输入
     * 上报的频率持续刷零幅度保活包，参数包的低有效位也常在抖：判据因此是使能且归一强度
     * 高过载波电平，否则会把主机根本没在震的状态写给输入手柄。 */
    out->left_on = (out->raw[0] & 0x40) != 0 &&
                   ns2_rumble_strength(out->raw) > NS2_RUMBLE_CARRIER_MAX;
    out->right_on = (out->raw[16] & 0x40) != 0 &&
                    ns2_rumble_strength(&out->raw[16]) > NS2_RUMBLE_CARRIER_MAX;
    return true;
}

void ns2_output_emit_player_led(uint8_t led_mask)
{
    if (s_out.feedback_fn != NULL) {
        s_out.feedback_fn(NS2_FEEDBACK_PLAYER_LED, &led_mask, s_out.feedback_user);
    }
}

void ns2_output_emit_haptic_sample(uint8_t sample_id)
{
    if (s_out.feedback_fn != NULL) {
        s_out.feedback_fn(NS2_FEEDBACK_HAPTIC_SAMPLE, &sample_id, s_out.feedback_user);
    }
}

bool ns2_haptic_sample_parse(const uint8_t *frame, size_t len, uint8_t *sample)
{
    if (frame == NULL || sample == NULL || len < 4 || frame[0] != 0x0A) {
        return false;
    }
    /* 命令帧 = 头 4 字节
     * `0A 91 01 02` + 值段，采样 ID 在值段第 5 字节（帧内偏移 8），「查找
     * 手柄」页以约 16-18 Hz 重发 0x02、收尾发 0x00。子命令非 0x02 时帧内
     * 不带值段，采样 ID 就是子命令本身。 */
    *sample = frame[3] == 0x02 && len >= 9 ? frame[8] : frame[3];
    return true;
}
