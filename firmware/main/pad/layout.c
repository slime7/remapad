#include "layout.h"

/**
 * 共用按键位图与布局模块清单：各系列的布局行在 pad/layouts/ 下，一族一个文件，
 * 这里只做登记与匹配。加一个系列＝加一个文件，并在 s_modules 里登记。
 */

const uint32_t pad_xbox_btn_map[16] = {
    PAD_BTN_DPAD_UP, PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_LEFT, PAD_BTN_DPAD_RIGHT,
    PAD_BTN_OPT, PAD_BTN_TOUCHPAD, PAD_BTN_L3, PAD_BTN_R3,
    PAD_BTN_L1, PAD_BTN_R1, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_SQUARE, PAD_BTN_TRIANGLE,
};

const uint32_t pad_ps_btn_map[24] = {
    0, 0, 0, 0,
    PAD_BTN_SQUARE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_TRIANGLE,
    PAD_BTN_L1, PAD_BTN_R1, 0, 0,
    PAD_BTN_SHARE, PAD_BTN_OPT, PAD_BTN_L3, PAD_BTN_R3,
    PAD_BTN_HOME, PAD_BTN_TOUCHPAD, PAD_BTN_MUTE, 0,
    0, 0, PAD_BTN_L4, PAD_BTN_R4,
};

extern const pad_layout_module_t pad_layout_module_xbox;
extern const pad_layout_module_t pad_layout_module_ds4;
extern const pad_layout_module_t pad_layout_module_ds5;
extern const pad_layout_module_t pad_layout_module_ds3;

/** 模块顺序即匹配顺序：同一组合多行时取先出现的那个（PS 系按 PID 分行，互不重叠）。 */
static const pad_layout_module_t *const s_modules[] = {
    &pad_layout_module_xbox,
    &pad_layout_module_ds4,
    &pad_layout_module_ds5,
    &pad_layout_module_ds3,
};

/** 未识别型号的兜底布局：按 Xbox 有线解析。 */
static const pad_layout_t s_fallback = {
    .family = PAD_FAMILY_XBOX,
    .conn = PAD_CONN_UNKNOWN,
    .report_id = 0x00,
    .buttons_off = 1,
    .buttons_bytes = 2,
    .hat_off = PAD_OFF_NONE,
    .trigger_off = {3, 4},
    .stick_off = {5, 7, 9, 11},
    .touch_off = PAD_OFF_NONE,
    .motion_off = PAD_OFF_NONE,
    .battery_off = PAD_OFF_NONE,
    .stick_style = PAD_STICK_I16,
    .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
    .invert_y = false,
    .btn_map = pad_xbox_btn_map,
};

/** 该行是否适用于这个 PID；行的 pids 为空表示不按型号过滤。 */
static bool row_pid_match(const pad_layout_t *row, uint16_t pid)
{
    if (row->pids[0] == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(row->pids) / sizeof(row->pids[0]); i++) {
        if (row->pids[i] == pid) {
            return true;
        }
    }
    return false;
}

const pad_layout_t *pad_layout_find(const pad_report_t *report, pad_family_t *family)
{
    *family = report->family;
    if (*family == PAD_FAMILY_UNKNOWN) {
        *family = pad_family_from_ids(report->vid, report->pid);
    }
    for (size_t m = 0; m < sizeof(s_modules) / sizeof(s_modules[0]); m++) {
        const pad_layout_module_t *module = s_modules[m];
        for (size_t r = 0; r < module->row_count; r++) {
            const pad_layout_t *row = &module->rows[r];
            if (row->family != *family || row->report_id != report->report_id) {
                continue;
            }
            /* 行的连接方式为空表示两种连接共用；报告的连接方式为空表示不做过滤。 */
            if (row->conn != PAD_CONN_UNKNOWN && report->conn != PAD_CONN_UNKNOWN &&
                row->conn != report->conn) {
                continue;
            }
            /* 型号不匹配就继续找同一组合下的下一行；报告没带 PID 时不做过滤。 */
            if (report->pid != 0 && !row_pid_match(row, report->pid)) {
                continue;
            }
            return row;
        }
    }
    return NULL;
}

const pad_layout_t *pad_layout_fallback(void)
{
    return &s_fallback;
}

