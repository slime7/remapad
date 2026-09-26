#include "layout.h"

/** DualSense 与 DualSense Edge（0x0CE6 / 0x0DF2）：有线报 0x01、蓝牙报 0x31，位序与 DS4 相同；
 *  Edge 的背键在按键位图第三字节高两位，左右 Fn 键不映射（兼作配置档修饰键）。
 *  运动字段的原始刻度与 DS4 相同（加速 8192 计数/g、陀螺 16 计数每 °/s，标称）。
 *  字段偏移的来源与核对状态见 docs/controller-ps.md。 */
static const pad_layout_t s_rows[] = {
    {
        /* 有线（64 字节）：偏移由蓝牙值减去三字节前缀换算，核对前作初值；
         * 电量与耳机状态字节本轮不登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 8,
        .buttons_bytes = 3,
        .hat_off = 8,
        .trigger_off = {5, 6},
        .stick_off = {1, 2, 3, 4},
        .touch_off = 33,
        .motion_off = 16,
        .battery_off = PAD_OFF_NONE,
        .touch_max_x = 1919,
        .touch_max_y = 1079,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12, .accel_per_g = 8192, .gyro_per_dps_x1000 = 16000},
        /* 输出报告 0x02（48 字节）：b1/b2 是两个 valid_flag，b3/b4 是右小马达与左大马达，
         * b6 是喇叭音量，b44 是玩家灯、b45-b47 是灯条 RGB。灯条不驱动（valid_flag1 只置玩家灯位，
         * 颜色留给 PC 侧管理），玩家号落四颗白灯；喇叭音量逐报钉在 PS5 缺省档 100。 */
        .out = {
            .report_id = 0x02,
            .len = 48,
            /* 内置喇叭要显式路由：输出路径位段置手柄喇叭（0x30）、前级 +6dB，
             * 否则送进去的发声段全被丢掉。 */
            .presets = {{1, 0xA3}, {2, 0x90}, {6, 100}, {8, 0x30}, {38, 0x02}},
            /* 音频触觉让位期间的写回（板载合成接手音圈）：马达字节恒零、
             * valid_flag0 = 0xA1（带兼容震动、不带音频触觉选择，即交还音圈的那次切换）；
             * 音频路由与音量档照旧保留。 */
            .quiet_presets = {{1, 0xA1}, {2, 0x90}, {6, 100}, {8, 0x30}, {38, 0x02}},
            .rumble_off = {4, 3},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_mask_off = 44,
            .led_rgb_off = PAD_OFF_NONE,
            .led_style = PAD_LED_PLAYER_MASK,
            .audio_haptic = 1,
            /* HD 触觉波形映射（NS 波形 → 本设备 PCM）：USB 承载是 4 声道 48kHz 16-bit PCM
             * （频道 3/4 音圈、1/2 小喇叭），amp_peak 取 30000；规则细节见 docs/controller-ps.md。 */
            .hd = {.ops = 3, .rate_hz = 48000, .amp_peak = 30000, .cycle_ms = 15,
                   .lf_min_hz = 20, .lf_max_hz = 500, .lf_default_hz = 80,
                   .hf_min_hz = 20, .hf_max_hz = 500, .hf_default_hz = 135,
                   .pulse_hz = 135, .beep_hz = 500, .gain_num = 4, .gain_den = 1},
            .led_mask_map = {0x04, 0x0A, 0x15, 0x1B},
        },
    },
    {
        /* 蓝牙（0x31）：偏移按 DualSense Edge 核对，触摸点按公共段布局登记（未核对）；
         * 第 54 字节是电量、第 55 字节是耳机状态。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x31,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 9,
        .buttons_bytes = 3,
        .hat_off = 9,
        .trigger_off = {6, 7},
        .stick_off = {2, 3, 4, 5},
        .touch_off = 34,
        .motion_off = 17,
        .battery_off = 54,
        .touch_max_x = 1919,
        .touch_max_y = 1079,
        .headset_off = 55,
        .headset_style = PAD_HEADSET_PS,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12, .accel_per_g = 8192, .gyro_per_dps_x1000 = 16000},
        /* 蓝牙形态报告 0x31（78 字节）：b1 是序号/标签字节（seq_off 交给编码器递增）、
         * b2 是固定魔数 0x10、公共段从 b3 起，末 4 字节是 CRC32；b46 是玩家灯、
         * b47-b49 是灯条 RGB（同有线行不驱动）。 */
        .out = {
            .report_id = 0x31,
            .len = 78,
            /* 与有线行一致：保住喇叭音量档并对内置喇叭做路由，0x36 私有流的喇叭块靠它出声。 */
            .presets = {{2, 0x10}, {3, 0xA3}, {4, 0x90}, {8, 100}, {10, 0x30}, {40, 0x02}},
            /* 让位期间的写回（PC 侧私有流接手音圈）：valid_flag0 = 0xA1（同有线行），
             * 音频路由与音量档照旧保留。 */
            .quiet_presets = {{2, 0x10}, {3, 0xA1}, {4, 0x90}, {8, 100}, {10, 0x30}, {40, 0x02}},
            .rumble_off = {6, 5},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_mask_off = 46,
            .led_rgb_off = PAD_OFF_NONE,
            .led_style = PAD_LED_PLAYER_MASK,
            /* 蓝牙私有触觉流（0x32 / 0x36）：3000Hz 2ch 8-bit，PC 侧按 FEEDBACK 的子帧哑渲染；
             * audio_haptic 置位后 0x31 的马达字节让位，同一对音圈不双驱动。 */
            .audio_haptic = 1,
            .hd = {.ops = 3, .rate_hz = 3000, .amp_peak = 127, .cycle_ms = 15,
                   .lf_min_hz = 20, .lf_max_hz = 500, .lf_default_hz = 80,
                   .hf_min_hz = 20, .hf_max_hz = 500, .hf_default_hz = 135,
                   .pulse_hz = 135, .beep_hz = 500, .gain_num = 4, .gain_den = 1},
            .frame = PAD_OUT_FRAME_PS_BT,
            .seq_off = 1,
            .led_mask_map = {0x04, 0x0A, 0x15, 0x1B},
        },
    },
};

const pad_layout_module_t pad_layout_module_ds5 = {
  .name = "ds5",
  .rows = s_rows,
  .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
