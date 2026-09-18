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

/** PS 家族位图（DS4/DS5 各形态共用，DS3 另表）：DS4 的 SHARE 与 DS5 的 Create
 *  是左侧小键，与 Xbox 的 View、DS3 的 Select 同位，按位置语义归一为减号位；
 *  触摸板按下作为中央额外键归一为截图位——跨家族一致，减号的使用频率高于
 *  截图，交给每只手柄都有的物理小键。 */
const uint32_t pad_ps_btn_map[24] = {
    0, 0, 0, 0,
    PAD_BTN_SQUARE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_TRIANGLE,
    PAD_BTN_L1, PAD_BTN_R1, 0, 0,
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_L3, PAD_BTN_R3,
    PAD_BTN_HOME, PAD_BTN_SHARE, PAD_BTN_MUTE, 0,
    0, 0, PAD_BTN_L4, PAD_BTN_R4,
};

extern const pad_layout_module_t pad_layout_module_xbox;
extern const pad_layout_module_t pad_layout_module_ds4;
extern const pad_layout_module_t pad_layout_module_ds5;
extern const pad_layout_module_t pad_layout_module_ds3;
extern const pad_layout_module_t pad_layout_module_ns;

/** 模块顺序即匹配顺序：同一组合多行时取先出现的那个（PS 系按 PID 分行，互不重叠）。 */
static const pad_layout_module_t *const s_modules[] = {
    &pad_layout_module_xbox,
    &pad_layout_module_ds4,
    &pad_layout_module_ds5,
    &pad_layout_module_ds3,
    &pad_layout_module_ns,
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

/**
 * 行是否适用于给定的设备标识。report_id 传 -1 表示不比对报告标识（反馈
 * 方向没有报告帧）；pid 为 0 表示报告没带型号，不做过滤。
 */
static bool row_match(const pad_layout_t *row, pad_family_t family, pad_conn_t conn,
                      int report_id, uint16_t pid)
{
    if (row->family != family) {
        return false;
    }
    if (report_id >= 0 && row->report_id != (uint8_t)report_id) {
        return false;
    }
    /* 行的连接方式为空表示两种连接共用；查询的连接方式为空表示不做过滤。 */
    if (row->conn != PAD_CONN_UNKNOWN && conn != PAD_CONN_UNKNOWN && row->conn != conn) {
        return false;
    }
    return pid == 0 || row_pid_match(row, pid);
}

static const pad_layout_t *find_row(pad_family_t family, pad_conn_t conn, int report_id,
                                    uint16_t pid)
{
    for (size_t m = 0; m < sizeof(s_modules) / sizeof(s_modules[0]); m++) {
        const pad_layout_module_t *module = s_modules[m];
        for (size_t r = 0; r < module->row_count; r++) {
            const pad_layout_t *row = &module->rows[r];
            if (row_match(row, family, conn, report_id, pid)) {
                return row;
            }
        }
    }
    return NULL;
}

const pad_layout_t *pad_layout_find(const pad_report_t *report, pad_family_t *family)
{
    *family = report->family;
    if (*family == PAD_FAMILY_UNKNOWN) {
        *family = pad_family_from_ids(report->vid, report->pid);
    }
    return find_row(*family, report->conn, report->report_id, report->pid);
}

const pad_layout_t *pad_layout_find_by_ids(uint16_t vid, uint16_t pid, pad_conn_t conn,
                                           pad_family_t *family)
{
    const pad_family_t found = pad_family_from_ids(vid, pid);
    if (family != NULL) {
        *family = found;
    }
    return find_row(found, conn, -1, pid);
}

const pad_layout_t *pad_layout_fallback(void)
{
    return &s_fallback;
}
