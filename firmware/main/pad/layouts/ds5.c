#include "layout.h"

/**
 * DualSense 与 DualSense Edge（0x0CE6 / 0x0DF2）：有线报 0x01、蓝牙报 0x31，两者
 * 只差两字节前缀，位序与 DS4 相同（共用 pad_ps_btn_map），Edge 的背键在第三字节
 * 的高两位、左右 Fn 键不映射（它们兼作配置档修饰键）。
 */
static const pad_layout_t s_rows[] = {
    {
        /* 有线（64 字节）：第 8 字节低四位是方向键帽子开关、高四位是面键，第 9 字节
         * 是肩键、Create/Options 与摇杆按下，第 10 字节是 PS、触摸板按下与静音键。
         * 偏移由蓝牙那行的实测值减去两字节前缀换算，抓包核对前作初值；触摸点
         * （每点 4 字节）与电量字节尚未核对，本轮不登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 8,
        .buttons_bytes = 3,
        .hat_off = 8,
        .trigger_off = {5, 6},
        .stick_off = {1, 2, 3, 4},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 16,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 输出报告 0x02（50 字节）：b1/b2 是两个 valid_flag（0x01 震动、
         * 0x04 灯条、0x10 玩家灯），b3/b4 是右小马达与左大马达，b46 是玩家灯
         * 掩码、b47-b49 是灯条 RGB。偏移按公开实现（Linux hid-playstation.c）
         * 换算，未实机核对。 */
        .out = {
            .report_id = 0x02,
            .len = 50,
            .presets = {{1, 0x01}, {2, 0x14}},
            .rumble_off = {4, 3},
            .rumble_max = {255, 255},
            .led_mask_off = 46,
            .led_rgb_off = 47,
            .led_style = PAD_LED_LIGHTBAR,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
        },
    },
    {
        /* 蓝牙（0x31）：比 DS4 蓝牙的 0x11 整体后移一位。偏移为 DualSense Edge
         * 实测抓包：静止帧第 9 字节读作 0x08（帽子开关松开）、四轴落在死区内、
         * 第 17-22 字节的角速度接近 0 而加速度有一轴约 1 g。触摸点与电量字节
         * 尚未核对，本轮不登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x31,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 9,
        .buttons_bytes = 3,
        .hat_off = 9,
        .trigger_off = {6, 7},
        .stick_off = {2, 3, 4, 5},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 17,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 蓝牙形态报告 0x31 比有线多一字节前缀，其余字段整体后移一位。 */
        .out = {
            .report_id = 0x31,
            .len = 78,
            .presets = {{2, 0x01}, {3, 0x14}},
            .rumble_off = {5, 4},
            .rumble_max = {255, 255},
            .led_mask_off = 47,
            .led_rgb_off = 48,
            .led_style = PAD_LED_LIGHTBAR,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
        },
    },
};

const pad_layout_module_t pad_layout_module_ds5 = {
    .name = "ds5",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
