/**
 * 测试套件注册表：新增一个 test_*.c 时在这里加两行（extern + 表项）。
 * 用例数量与顺序不会影响其他文件，各套件相互独立。
 */
#include "host_test.h"

extern const host_test_suite_t suite_ns2_report;
extern const host_test_suite_t suite_ns2_serial;
extern const host_test_suite_t suite_ns2_frames;
extern const host_test_suite_t suite_render_accel;
extern const host_test_suite_t suite_dp_source;

static const host_test_suite_t *const s_suites[] = {
    &suite_ns2_report,
    &suite_ns2_serial,
    &suite_ns2_frames,
    &suite_render_accel,
    &suite_dp_source,
    NULL,
};

const host_test_suite_t *const *host_test_all_suites(void)
{
    return s_suites;
}

