#include "layout.h"

/**
 * Xbox 家族（Xbox One S / Series X|S 与精英手柄 2 的蓝牙 HID 报告 0x01）：四轴是 16 位
 * 无符号（中心 0x8000）、扳机是 10 位值、方向键是 1 起算的帽子字节，因此按键、扳机与
 * 摇杆都不与 XInput 形态共用偏移；写回是报告 0x03（四个马达与尾参数）。
 * 偏移与核对状态见 docs/controller-xbox.md。
 */

/**
 * 蓝牙报告的按键位（三个字节）：面键与肩键落在第一字节、功能键落在第二字节、分享键落在
 * 第三字节，接不成 16 位字，因此按位列出。面键按位置语义映射，背键由行单独声明。
 */
static const uint32_t s_bt_btn_map[24] = {
  /* b0：A / B / 保留 / X / Y / 保留 / LB / RB。 */
  PAD_BTN_CROSS,
  PAD_BTN_CIRCLE,
  0,
  PAD_BTN_SQUARE,
  PAD_BTN_TRIANGLE,
  0,
  PAD_BTN_L1,
  PAD_BTN_R1,
  /* b1：保留 / 保留 / View / Menu / 西瓜键 / L3 / R3 / 保留。 */
  0,
  0,
  PAD_BTN_TOUCHPAD,
  PAD_BTN_OPT,
  PAD_BTN_HOME,
  PAD_BTN_L3,
  PAD_BTN_R3,
  0,
  /* b2：分享键（Series 手柄与 5.x 固件的精英手柄）。 */
  PAD_BTN_SHARE,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
};

/** 精英手柄 2 的背键位：P1-P4 依次落 L4 / R4 / L5 / R5（上左、上右、下左、下右）。 */
static const uint32_t s_elite_back_map[8] = {
  PAD_BTN_L4, PAD_BTN_R4, PAD_BTN_L5, PAD_BTN_R5, 0, 0, 0, 0,
};

static const pad_layout_t s_rows[] = {
    {
        /* 精英手柄 2（蓝牙，初版固件 55 字节报文）：背键位在第 33 字节，第 35 字节
         * 非零表示背键已交给手柄内部配置档、位域不再采信。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .pids = {0x0B22},
        .len_min = 55,
        .len_max = 55,
        .buttons_off = 14,
        .buttons_bytes = 3,
        .back_off = 33,
        .back_mode_off = 35,
        .back_map = s_elite_back_map,
        .hat_off = 13,
        .hat_style = PAD_HAT_1UP,
        .trigger_off = {9, 11},
        .trigger_style = PAD_TRIGGER_U10,
        .stick_off = {1, 3, 5, 7},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_BACK_BUTTONS,
        .invert_y = true,
        .btn_map = s_bt_btn_map,
        .out = {
            .report_id = 0x03, .len = 9,
            .presets = {{1, 0x0F}, {6, 0xFF}, {7, 0x00}, {8, 0x01}},
            .rumble_off = {4, 5}, .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF}, .led_style = PAD_LED_NONE,
        },
    },
    {
        /* 精英手柄 2（蓝牙，4.x 固件 39 字节报文）：背键位在第 17 字节。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .pids = {0x0B22},
        .len_min = 39,
        .len_max = 39,
        .buttons_off = 14,
        .buttons_bytes = 3,
        .back_off = 17,
        .back_mode_off = 19,
        .back_map = s_elite_back_map,
        .hat_off = 13,
        .hat_style = PAD_HAT_1UP,
        .trigger_off = {9, 11},
        .trigger_style = PAD_TRIGGER_U10,
        .stick_off = {1, 3, 5, 7},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_BACK_BUTTONS,
        .invert_y = true,
        .btn_map = s_bt_btn_map,
        .out = {
            .report_id = 0x03, .len = 9,
            .presets = {{1, 0x0F}, {6, 0xFF}, {7, 0x00}, {8, 0x01}},
            .rumble_off = {4, 5}, .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF}, .led_style = PAD_LED_NONE,
        },
    },
    {
        /* 精英手柄 2（蓝牙，5.13+ 固件 20 字节报文）：背键位在第 19 字节，
         * 配置档判定字节挪到第 17 字节（两行字段的位置与 39 字节形态互换）。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .pids = {0x0B22},
        .len_min = 20,
        .len_max = 20,
        .buttons_off = 14,
        .buttons_bytes = 3,
        .back_off = 19,
        .back_mode_off = 17,
        .back_map = s_elite_back_map,
        .hat_off = 13,
        .hat_style = PAD_HAT_1UP,
        .trigger_off = {9, 11},
        .trigger_style = PAD_TRIGGER_U10,
        .stick_off = {1, 3, 5, 7},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_BACK_BUTTONS,
        .invert_y = true,
        .btn_map = s_bt_btn_map,
        .out = {
            .report_id = 0x03, .len = 9,
            .presets = {{1, 0x0F}, {6, 0xFF}, {7, 0x00}, {8, 0x01}},
            .rumble_off = {4, 5}, .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF}, .led_style = PAD_LED_NONE,
        },
    },
    {
        /* 通用 Xbox 蓝牙报告（Series 手柄与 5.x 固件的 One S / 精英手柄）：17 字节起
         * 带分享键，四轴、扳机、帽子与按键偏移对所有长度一致。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .len_min = 17,
        .buttons_off = 14,
        .buttons_bytes = 3,
        .hat_off = 13,
        .hat_style = PAD_HAT_1UP,
        .trigger_off = {9, 11},
        .trigger_style = PAD_TRIGGER_U10,
        .stick_off = {1, 3, 5, 7},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_bt_btn_map,
        .out = {
            .report_id = 0x03, .len = 9,
            .presets = {{1, 0x0F}, {6, 0xFF}, {7, 0x00}, {8, 0x01}},
            .rumble_off = {4, 5}, .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF}, .led_style = PAD_LED_NONE,
        },
    },
    {
        /* 初代 Xbox One S 蓝牙（4.x 固件 16 字节报文）：没有分享键，按键只占两个字节；
         * 西瓜键由另一份独立报文上报，本轮不登记。电量同样是独立报文，未登记。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .len_min = 16,
        .len_max = 16,
        .buttons_off = 14,
        .buttons_bytes = 2,
        .hat_off = 13,
        .hat_style = PAD_HAT_1UP,
        .trigger_off = {9, 11},
        .trigger_style = PAD_TRIGGER_U10,
        .stick_off = {1, 3, 5, 7},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_bt_btn_map,
        .out = {
            .report_id = 0x03, .len = 9,
            .presets = {{1, 0x0F}, {6, 0xFF}, {7, 0x00}, {8, 0x01}},
            .rumble_off = {4, 5}, .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF}, .led_style = PAD_LED_NONE,
        },
    },
};

const pad_layout_module_t pad_layout_module_xbox = {
  .name = "xbox",
  .rows = s_rows,
  .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
