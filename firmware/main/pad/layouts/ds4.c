#include "layout.h"

/**
 * DualShock 4：有线报 0x01、蓝牙报 0x11（比有线多两个前导字节）。位序用 PS 家族
 * 共用的 pad_ps_btn_map：第一字节低四位是方向键帽子开关、高四位是面键，第二字节
 * 是肩键、Create/Options 与摇杆按下，第三字节是 PS、触摸板按下与静音键。
 */
static const pad_layout_t s_rows[] = {
    {
        /* 有线：电量取 status[0]（0x1E），低四位是 0-10 档、bit4 表示充电中。
         * DualSense 有线同样报 0x01，但在扳机之后多一个序号字节，另列一行。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x05C4, 0x09CC},
        .buttons_off = 5,
        .buttons_bytes = 3,
        .hat_off = 5,
        .trigger_off = {8, 9},
        .stick_off = {1, 2, 3, 4},
        .touch_off = 34,
        .motion_off = 13,
        .battery_off = 30,
        .touch_max_x = 1919,
        .touch_max_y = 942,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 输出报告 0x05（32 字节）：b1 是 flags（0x01 震动、0x02 灯条颜色），
         * b4/b5 是右小马达与左大马达强度，b6-b8 是灯条 RGB。偏移取自公开实现
         * （Linux hid-playstation.c），未实机核对。 */
        .out = {
            .report_id = 0x05,
            .len = 32,
            .presets = {{1, 0x03}},
            .rumble_off = {5, 4},
            .rumble_max = {255, 255},
            .led_rgb_off = 6,
            .led_style = PAD_LED_LIGHTBAR,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
        },
    },
    {
        /* 蓝牙：其余偏移整体后移两位，电量在 0x20。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x11,
        .pids = {0x05C4, 0x09CC},
        .buttons_off = 7,
        .buttons_bytes = 3,
        .hat_off = 7,
        .trigger_off = {10, 11},
        .stick_off = {3, 4, 5, 6},
        .touch_off = 36,
        .motion_off = 15,
        .battery_off = 32,
        .touch_max_x = 1919,
        .touch_max_y = 942,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 蓝牙形态：报告 ID 换成 0x11，字段偏移与有线一致（公开实现如此，
         * 未实机核对）。 */
        .out = {
            .report_id = 0x11,
            .len = 78,
            .presets = {{1, 0x03}},
            .rumble_off = {5, 4},
            .rumble_max = {255, 255},
            .led_rgb_off = 6,
            .led_style = PAD_LED_LIGHTBAR,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
        },
    },
};

const pad_layout_module_t pad_layout_module_ds4 = {
    .name = "ds4",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
