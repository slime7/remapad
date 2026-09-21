#include "layout.h"

/**
 * Nintendo 家族：Switch 一代手柄（0x30 / 0x3F）与 Switch 2 手柄（0x05 / 0x09
 * 报文体），以及伪装成 NS 布局的第三方手柄。同一份文件里登记两代，是因为
 * 二者共用键位语义，只是报告格式不同；行里的 native_lang 决定能否把它们
 * 原样转发给同代目标（NS2 手柄 → NS2 主机走透传，NS1 手柄走解析重编码）。
 *
 * 偏移与键位按公开资料（Nintendo Switch 与 Switch 2 的 HID 报告描述）整理，
 * 本轮没有实机可核对；USB 形态的报文体
 * 首字节是 Report ID，因此各行偏移都比报文体自身的偏移大 1。
 */

/** NS1（0x30 / 0x3F）按键位：右半边、功能键、左半边各一字节。 */
static const uint32_t s_ns1_btn_map[24] = {
    /* b0：Y / X / B / A / SR / SL / R / ZR。面键按位置取值。 */
    PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, 0, 0, PAD_BTN_R1, 0,
    /* b1：Minus / Plus / RStick / LStick / Home / Capture / 保留 / 充电握把。 */
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_R3, PAD_BTN_L3, PAD_BTN_HOME, PAD_BTN_SHARE, 0, 0,
    /* b2：Down / Up / Right / Left / SR / SL / L / ZL。 */
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_UP, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, 0, 0,
    PAD_BTN_L1, 0,
};

/**
 * NS2（0x09 报文体）按键位：b0 右半边、b1 左半边、b2 系统与背键。
 * ZL / ZR 是数字位，私有格式的扳机是模拟量，本轮不映射（透传路径不受影响，
 * 透传会把整份报文体原样交给主机）。
 */
static const uint32_t s_ns2_09_btn_map[24] = {
    /* b0：B / A / Y / X / R / ZR / Plus / RStick。 */
    PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_R1, 0,
    PAD_BTN_OPT, PAD_BTN_R3,
    /* b1：Down / Right / Left / Up / L / ZL / Minus / LStick。 */
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, PAD_BTN_DPAD_UP, PAD_BTN_L1, 0,
    PAD_BTN_TOUCHPAD, PAD_BTN_L3,
    /* b2：Home / Capture / GR / GL / C / 保留。 */
    PAD_BTN_HOME, PAD_BTN_SHARE, PAD_BTN_R4, PAD_BTN_L4, PAD_BTN_MUTE, 0, 0, 0,
};

/** NS2（0x05 报文体）按键位：四字节，第三字节还带左半边的 SL/SR 与 C 键。 */
static const uint32_t s_ns2_05_btn_map[32] = {
    /* b0：Y / X / B / A / 右SL / 右SR / R / ZR。 */
    PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, 0, 0, PAD_BTN_R1, 0,
    /* b1：Minus / Plus / RStick / LStick / Home / Capture / C / 保留。 */
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_R3, PAD_BTN_L3, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_MUTE, 0,
    /* b2：Down / Up / Right / Left / 左SR / 左SL / L / ZL。 */
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_UP, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, 0, 0,
    PAD_BTN_L1, 0,
    /* b3：GR / GL / 其余保留。 */
    PAD_BTN_R4, PAD_BTN_L4, 0, 0, 0, 0, 0, 0,
};

/**
 * NS1 的震动走输出报告 0x10：左右各 4 字节，前两字节是低频段的固定头
 * （0x00 0x01）、后两字节是高频段与低频段的振幅（0x40 为满量程）。私有的
 * 0-255 强度按 0x40 缩放后写进振幅字节，强度为 0 时振幅为 0 即静音。
 * 该编码取自公开实现，未实机核对；NS1 的玩家灯与控制灯只在本机使用，主机
 * 不下发，本轮不映射。
 */
#define PAD_NS1_RUMBLE_MAX 0x40

static const pad_layout_t s_rows[] = {
    {
        /* Switch 一代 Pro Controller（0x30）：有线与蓝牙的报文体一致，字段
         * 偏移相同，因此一行覆盖两种连接。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x30,
        .pids = {0x2009},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {6, 7, 8, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 13,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns1_btn_map,
        .motion = {.samples = 3, .stride = 12},
        .out = {
            .report_id = 0x10,
            .len = 9,
            .presets = {{1, 0x00}, {2, 0x01}, {3, PAD_NS1_RUMBLE_MAX},
                        {5, 0x00}, {6, 0x01}, {7, PAD_NS1_RUMBLE_MAX}},
            .rumble_off = {4, 8},
            .rumble_max = {PAD_NS1_RUMBLE_MAX, PAD_NS1_RUMBLE_MAX},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS1,
        .native_identity = PAD_IDENTITY_PRO,
    },
    {
        /* Switch 一代 Joy-Con (L)（0x3F）：基础字段与 0x30 相同，后面多出
         * 红外与磁力计区段。国产伪装手柄多数也报 0x3F。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x3F,
        .pids = {0x2006},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {6, 7, 8, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 13,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns1_btn_map,
        .motion = {.samples = 3, .stride = 12},
        .out = {
            .report_id = 0x10,
            .len = 9,
            .presets = {{1, 0x00}, {2, 0x01}, {3, PAD_NS1_RUMBLE_MAX},
                        {5, 0x00}, {6, 0x01}, {7, PAD_NS1_RUMBLE_MAX}},
            .rumble_off = {4, 8},
            .rumble_max = {PAD_NS1_RUMBLE_MAX, PAD_NS1_RUMBLE_MAX},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS1,
        .native_identity = PAD_IDENTITY_JOYCON_L,
    },
    {
        /* Switch 一代 Joy-Con (R)（0x3F，右手柄独立的 PID）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x3F,
        .pids = {0x2007},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {6, 7, 8, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 13,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns1_btn_map,
        .motion = {.samples = 3, .stride = 12},
        .out = {
            .report_id = 0x10,
            .len = 9,
            .presets = {{1, 0x00}, {2, 0x01}, {3, PAD_NS1_RUMBLE_MAX},
                        {5, 0x00}, {6, 0x01}, {7, PAD_NS1_RUMBLE_MAX}},
            .rumble_off = {4, 8},
            .rumble_max = {PAD_NS1_RUMBLE_MAX, PAD_NS1_RUMBLE_MAX},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS1,
        .native_identity = PAD_IDENTITY_JOYCON_R,
    },
    {
        /* Switch 2 Pro Controller（0x09 报文体）：按键 3 字节、摇杆 12 位
         * 打包、电量在 0x01。运动块在 0x0F（40 字节），公开资料里仍是未解析
         * 的打包格式，因此不登记——真机的运动数据靠透传原样送达主机。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x09,
        .pids = {0x2069},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS2,
        .caps = PAD_CAP_BATTERY | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_09_btn_map,
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
            /* 同代透传：主机下发的 32 字节 LRA 参数包按 USB 形态原样写回。 */
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_PRO,
    },
    {
        /* Switch 2 Joy-Con (L)（0x09 报文体）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x09,
        .pids = {0x2067},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS2,
        .caps = PAD_CAP_BATTERY | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_09_btn_map,
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_L,
    },
    {
        /* Switch 2 Joy-Con (R)（0x09 报文体）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x09,
        .pids = {0x2066},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS2,
        .caps = PAD_CAP_BATTERY | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_09_btn_map,
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_R,
    },
    {
        /* Switch 2 Pro Controller 的 0x05 报文体（主机切到通用报告时）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x05,
        .pids = {0x2069},
        .buttons_off = 5,
        .buttons_bytes = 4,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {11, 11, 14, 14},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 43,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U12,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_05_btn_map,
        .motion = {.samples = 1, .stride = 12},
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_PRO,
    },
    {
        /* Switch 2 Joy-Con (L) 的 0x05 报文体。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x05,
        .pids = {0x2067},
        .buttons_off = 5,
        .buttons_bytes = 4,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {11, 11, 14, 14},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 43,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U12,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_05_btn_map,
        .motion = {.samples = 1, .stride = 12},
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_L,
    },
    {
        /* Switch 2 Joy-Con (R) 的 0x05 报文体。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x05,
        .pids = {0x2066},
        .buttons_off = 5,
        .buttons_bytes = 4,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .stick_off = {11, 11, 14, 14},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 43,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U12,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_05_btn_map,
        .motion = {.samples = 1, .stride = 12},
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_R,
    },
};

const pad_layout_module_t pad_layout_module_ns = {
    .name = "ns",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
