/**
 * 测试套件注册表：新增一个 test_*.c 时在这里加两行（extern + 表项）。
 * 用例数量与顺序不会影响其他文件，各套件相互独立。
 */
#include "host_test.h"

extern const host_test_suite_t suite_ns2_report;
extern const host_test_suite_t suite_ns2_adv;
extern const host_test_suite_t suite_ns2_serial;
extern const host_test_suite_t suite_ns2_frames;
extern const host_test_suite_t suite_ns2_upgrade;
extern const host_test_suite_t suite_ns2_identity;
extern const host_test_suite_t suite_render_accel;
extern const host_test_suite_t suite_render_damage;
extern const host_test_suite_t suite_dp_source;
extern const host_test_suite_t suite_dp_ui;
extern const host_test_suite_t suite_dp_capture;
extern const host_test_suite_t suite_pad_device;
extern const host_test_suite_t suite_ds_behavior;
extern const host_test_suite_t suite_pad_ns;
extern const host_test_suite_t suite_pad_feedback;
extern const host_test_suite_t suite_ns2_relay;
extern const host_test_suite_t suite_input_frame;
extern const host_test_suite_t suite_ota_proto;
extern const host_test_suite_t suite_ns2_nfc;
extern const host_test_suite_t suite_amiibo_proto;
extern const host_test_suite_t suite_target_ns2;
extern const host_test_suite_t suite_battery;
extern const host_test_suite_t suite_usb_audio;
extern const host_test_suite_t suite_haptic_synth;

static const host_test_suite_t *const s_suites[] = {
    &suite_ns2_report,
    &suite_ns2_adv,
    &suite_ns2_serial,
    &suite_ns2_frames,
    &suite_ns2_upgrade,
    &suite_ns2_identity,
    &suite_render_accel,
    &suite_render_damage,
    &suite_dp_source,
    &suite_dp_ui,
    &suite_dp_capture,
    &suite_pad_device,
    &suite_ds_behavior,
    &suite_pad_ns,
    &suite_pad_feedback,
    &suite_ns2_relay,
    &suite_input_frame,
    &suite_ota_proto,
    &suite_ns2_nfc,
    &suite_amiibo_proto,
    &suite_target_ns2,
    &suite_battery,
    &suite_usb_audio,
    &suite_haptic_synth,
    NULL,
};

const host_test_suite_t *const *host_test_all_suites(void)
{
    return s_suites;
}
