#include "layout.h"

/**
 * XInput 形态（Xbox 360 报文）：20 字节 HID 报告、首两字节固定 00 14，按键两字节、
 * 扳机两字节、四轴 16 位小端，写回是 8 字节震动包（报文不带 Report ID）。
 * 说这份报文的第三方手柄厂商 VID 各不相同，家族靠本模块的型号表判定；
 * 偏移与核对状态见 docs/controller-xinput.md。
 */

/** 家族识别表：公开实现的 XInput 型号清单里的 VID:PID（厂商 VID 判不出家族）。 */
static const pad_id_t s_ids[] = {
    {0x0079, 0x18D4}, /* GPD Win 2 */
    {0x0351, 0x1000}, {0x0351, 0x2000}, /* CRKD LP */
    {0x03EB, 0xFF01}, {0x03EB, 0xFF02}, /* Wooting One / Two（旧形态） */
    {0x03F0, 0x038D}, {0x03F0, 0x048D}, /* HyperX Clutch */
    {0x044F, 0xB326}, /* Thrustmaster GP XID */
    {0x046D, 0xC21D}, {0x046D, 0xC21E}, {0x046D, 0xC21F}, /* Logitech F310 / F510 / F710 */
    {0x046D, 0xC242}, /* Logitech Chillstream */
    {0x0502, 0x1305}, /* Acer NGR200 */
    {0x056E, 0x2004}, /* Elecom JC-U3613M */
    {0x06A3, 0xF51A}, /* Saitek P3600 */
    {0x0738, 0x4716}, {0x0738, 0x4726}, {0x0738, 0x4736}, /* Mad Catz 系列 */
    {0x0738, 0x4728}, {0x0738, 0xB726}, /* Mad Catz 系列 */
    {0x0B05, 0x1C91}, {0x0B05, 0x1C92}, /* ASUS ROG Raikiri II */
    {0x0DB0, 0x1901}, /* MSI Xbox360 Controller */
    {0x0E6F, 0x0113}, {0x0E6F, 0x0131}, {0x0E6F, 0x0133}, /* PDP / Afterglow */
    {0x0E6F, 0x0147}, {0x0E6F, 0x0201}, {0x0E6F, 0x0213}, /* PDP / Pelican */
    {0x0F0D, 0x000C}, {0x0F0D, 0x000D}, {0x0F0D, 0x0016}, /* Hori 系列 */
    {0x1038, 0x1430}, {0x1038, 0x1431}, /* SteelSeries Stratus Duo */
    {0x11C9, 0x55F0}, /* Nacon GC-100XF */
    {0x11FF, 0x0511}, /* PXN V900 */
    {0x1209, 0x2882}, /* Ardwiino */
    {0x146B, 0x0601}, /* BigBen XBOX 360 Controller */
    {0x1532, 0x0A57}, {0x1532, 0x0A59}, /* Razer Wolverine V3 Pro */
    {0x15E4, 0x3F00}, {0x15E4, 0x3F0A}, /* PowerA Mini Pro Elite / Airflo */
    {0x1689, 0xFD00}, {0x1689, 0xFD01}, {0x1689, 0xFE00}, /* Razer Onza / Sabertooth */
    {0x16D0, 0x1103}, {0x16D0, 0x113C}, {0x16D0, 0x1212}, /* Azeron 系列 */
    {0x17EF, 0x6182}, /* Lenovo Legion Controller */
    {0x1949, 0x041A}, /* Amazon Game Controller */
    {0x1A86, 0xE310}, /* Legion Go S */
    {0x1BAD, 0xF016}, {0x1BAD, 0xF023}, {0x1BAD, 0xF02E}, /* Mad Catz / MLG */
    {0x1BAD, 0xF025}, {0x1BAD, 0xF027}, {0x1BAD, 0xF901}, /* Mad Catz / Gamestop */
    {0x20BC, 0x5134}, {0x20BC, 0x514A}, /* BETOP Xinput Dongle */
    {0x20D6, 0x281F}, /* PowerA Wired Xbox 360 */
    {0x24C6, 0x5300}, {0x24C6, 0x5303}, {0x24C6, 0x530A}, /* PowerA / Xbox Airflo */
    {0x24C6, 0x5500}, {0x24C6, 0x550D}, {0x24C6, 0x5510}, /* Hori 系列 */
    {0x2563, 0x058D}, /* OneXPlayer Gamepad */
    {0x2DC8, 0x3106}, {0x2DC8, 0x3109}, {0x2DC8, 0x310A}, /* 8BitDo Ultimate 系列 */
    {0x2DC8, 0x310B}, {0x2DC8, 0x6001}, /* 8BitDo Ultimate 2 / SN30 Pro */
    {0x31E3, 0x1100}, {0x31E3, 0x1200}, {0x31E3, 0x1210}, /* Wooting One / Two / Lekker */
    {0x3285, 0x0607}, {0x3285, 0x0662}, /* Nacon GC-100 / Revolution5 Pro */
    {0x3537, 0x1004}, {0x3537, 0x100F}, /* GameSir T4 Kaleid / Nova 2 Lite */
    {0x3651, 0x1000}, /* CRKD SG */
    {0x37D7, 0x2501}, /* Flydigi Apex 5 */
    {0x413D, 0x2104}, /* Black Shark Green Ghost */
};

static const pad_layout_t s_rows[] = {
    {
        /* 有线形态：按键在 2-3 字节、扳机 4/5、四轴从第 6 字节起，四轴是 16 位小端、
         * Y 轴设备侧向下为正。方向键、Start/Back 与摇杆按下都在按键位图里。
         * 无线接收器形态是同一段报文体加 4 字节前缀，未登记，留待实机核对。 */
        .family = PAD_FAMILY_XBOX,
        /* 蓝牙形态的第三方手柄（8BitDo 一类）也报同一份报文，因此两种连接共用这一行。 */
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x00,
        .len_min = 20,
        .len_max = 20,
        .buttons_off = 2,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {4, 5},
        .stick_off = {6, 8, 10, 12},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_I16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = pad_xbox_btn_map,
        /* 输出报告 8 字节 00 08 00 <左大马达> <右小马达> 00 00 00：报文不带
         * Report ID，字段偏移从首字节起算。玩家灯是另一份 3 字节报文
         * （01 03 <档位>），一份输出描述装不下，本轮不驱动。 */
        .out = {
            .report_id = 0x00,
            .len = 8,
            .no_report_id = true,
            .presets = {{1, 0x08}},
            .rumble_off = {3, 4},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_style = PAD_LED_NONE,
        },
    },
};

const pad_layout_module_t pad_layout_module_xinput = {
    .name = "xinput",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
    .ids = s_ids,
    .id_count = sizeof(s_ids) / sizeof(s_ids[0]),
};
