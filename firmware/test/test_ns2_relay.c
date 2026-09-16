/**
 * NS2 目标的运动字段与同代透传：0x05 的 IMU 字段、0x09 的实验运动块，以及
 * 「设备自带语言与目标一致时原样转发报文体」的判定与状态字节重写。
 *
 * 透传的正确性靠这里的字节级断言钉住：按键、摇杆、电量与运动块必须原样
 * 到达主机，只有本机会话决定的状态字节被改写。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_output.h"
#include "ns2_report.h"
#include "ns2_state.h"
#include "ns2_target.h"
#include "pad_state.h"
#include "target.h"

/* --- 输出通道替身：记录最后一帧发出的报告体与调用次数 --- */

static uint8_t s_sent_id;
static uint8_t s_sent[NS2_INPUT_09_LEN];
static size_t s_sent_len;
static size_t s_sent_count;
static size_t s_session_count;
static uint8_t s_session_identity;
static uint8_t s_session_format;

static size_t fake_session_count(void *user)
{
    (void)user;
    return s_session_count;
}

static bool fake_session_info(size_t index, uint8_t *identity, uint8_t *report_format,
                              void *user)
{
    (void)index;
    (void)user;
    *identity = s_session_identity;
    *report_format = s_session_format;
    return true;
}

static void fake_send_report(size_t index, uint8_t report_id, const uint8_t *body, size_t len,
                             void *user)
{
    (void)index;
    (void)user;
    s_sent_id = report_id;
    s_sent_len = len;
    memcpy(s_sent, body, len);
    s_sent_count++;
}

static const ns2_output_sink_t s_fake_sink = {
    .session_count = fake_session_count,
    .session_info = fake_session_info,
    .send_report = fake_send_report,
    .user = NULL,
};

static void reset_sink(size_t sessions, uint8_t identity, uint8_t format)
{
    s_session_count = sessions;
    s_session_identity = identity;
    s_session_format = format;
    s_sent_count = 0;
    s_sent_len = 0;
    s_sent_id = 0;
    memset(s_sent, 0, sizeof(s_sent));
    ns2_output_set_sink(&s_fake_sink);
}

/** NS2 手柄透传样本：报文体首字节是 Report ID，其余是设备自己的数据。 */
static pad_state_t relay_pad_state(void)
{
    pad_state_t pad;
    pad_state_defaults(&pad);
    pad.native_lang = PAD_LANG_NS2;
    pad.native_identity = PAD_IDENTITY_PRO;
    pad.raw_len = (uint8_t)(NS2_INPUT_09_LEN + 1u);
    pad.raw_report_id = NS2_REPORT_ID_09;
    pad.raw[0] = NS2_REPORT_ID_09;
    pad.raw[1] = 0x2A;      /* 计数器：设备自己的值 */
    pad.raw[1 + 0x01] = 0x26; /* 电源字节：只有真手柄才知道的真电量 */
    pad.raw[1 + 0x02] = 0x80; /* 按键位图：第一字节 bit7 = 右摇杆按下 */
    pad.raw[1 + NS2_09_OFF_STATUS] = 0x30;
    pad.raw[1 + NS2_09_OFF_NFC] = 0x00;
    pad.raw[1 + NS2_09_OFF_MOTION] = 0xAB; /* 运动块首字节 */
    return pad;
}

static void motion_fills_report_05_imu_field(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.motion_valid = true;
    state.gyro[0] = 0x0123;
    state.gyro[1] = -2;
    state.accel[2] = 0x4567;
    uint8_t report[NS2_INPUT_05_LEN];
    ns2_encode_input_05(report, &state, 2); /* 时间戳 = 2 × 5ms */

    const uint8_t *imu = &report[NS2_05_OFF_IMU];
    CHECK_EQ(imu[0], 0x10); /* 10000 us 小端 */
    CHECK_EQ(imu[1], 0x27);
    CHECK_EQ(imu[4], (uint8_t)(NS2_05_IMU_TEMP & 0xFF));
    CHECK_EQ(imu[5], (uint8_t)(NS2_05_IMU_TEMP >> 8));
    /* 加速 XYZ 在 6..11，陀螺 XYZ 在 12..17。 */
    CHECK_EQ(imu[6], 0x00);
    CHECK_EQ(imu[7], 0x00);
    CHECK_EQ(imu[10], 0x67);
    CHECK_EQ(imu[11], 0x45);
    CHECK_EQ(imu[12], 0x23);
    CHECK_EQ(imu[13], 0x01);
    CHECK_EQ(imu[14], 0xFE); /* 陀螺 Y = -2 */
    CHECK_EQ(imu[15], 0xFF);

    /* 没有运动数据时整段保持 0（不伪造 IMU）。 */
    state.motion_valid = false;
    ns2_encode_input_05(report, &state, 2);
    for (size_t i = 0; i < NS2_05_IMU_LEN; i++) {
        CHECK_EQ(report[NS2_05_OFF_IMU + i], 0);
    }
}

static void motion_sensor_mode_fills_09_block(void)
{
    ns2_controller_state_t state;
    ns2_state_defaults(&state);
    state.motion_mode = NS2_MOTION_SENSOR;
    state.motion_valid = true;
    state.gyro[1] = 0x0102;
    state.accel[0] = -1;
    uint8_t report[NS2_INPUT_09_LEN];
    ns2_encode_input_09(report, &state, 0);
    CHECK_EQ(report[NS2_09_OFF_MOTION_LEN], NS2_INPUT_09_MOTION_LEN);
    for (uint8_t s = 0; s < NS2_09_MOTION_SAMPLES; s++) {
        const uint8_t *sample = &report[NS2_09_OFF_MOTION + s * NS2_09_MOTION_SAMPLE_LEN];
        CHECK_EQ(sample[0], 0x00);
        CHECK_EQ(sample[2], 0x02);
        CHECK_EQ(sample[3], 0x01);
        CHECK_EQ(sample[6], 0xFF); /* 加速 X = -1 */
        CHECK_EQ(sample[7], 0xFF);
    }
    /* 36 字节样本之后余下的 4 字节保持 0。 */
    for (size_t i = NS2_09_MOTION_SAMPLES * NS2_09_MOTION_SAMPLE_LEN; i < NS2_INPUT_09_MOTION_LEN;
         i++) {
        CHECK_EQ(report[NS2_09_OFF_MOTION + i], 0);
    }

    /* 没有运动数据时实验模式退化成全零块，长度仍是非零（主机要求）。 */
    state.motion_valid = false;
    ns2_encode_input_09(report, &state, 0);
    CHECK_EQ(report[NS2_09_OFF_MOTION_LEN], NS2_INPUT_09_MOTION_LEN);
    CHECK_EQ(report[NS2_09_OFF_MOTION], 0);
}

static void relay_keeps_device_payload_and_rewrites_status(void)
{
    pad_state_t pad = relay_pad_state();
    reset_sink(1, NS2_ID_PRO, NS2_REPORT_ID_09);
    ns2_output_set_rumble_enabled(true);

    CHECK(ns2_output_send_raw(&pad));
    CHECK_EQ(s_sent_count, 1);
    CHECK_EQ(s_sent_id, NS2_REPORT_ID_09);
    CHECK_EQ(s_sent_len, NS2_INPUT_09_LEN);
    CHECK_EQ(s_sent[0x00], 0x2A); /* 计数器原样 */
    CHECK_EQ(s_sent[0x01], 0x26); /* 真电量原样 */
    CHECK_EQ(s_sent[0x02], 0x80); /* 按键原样 */
    CHECK_EQ(s_sent[NS2_09_OFF_STATUS], 0x38); /* 本机开了触觉 → 重写 */
    CHECK_EQ(s_sent[NS2_09_OFF_NFC], 0x00);
    CHECK_EQ(s_sent[NS2_09_OFF_HEADSET], 0x00);
    CHECK_EQ(s_sent[NS2_09_OFF_MOTION], 0xAB); /* 真运动块原样 */

    ns2_output_set_rumble_enabled(false);
    ns2_output_send_raw(&pad);
    CHECK_EQ(s_sent[NS2_09_OFF_STATUS], 0x30);
}

static void relay_requires_matching_format(void)
{
    pad_state_t pad = relay_pad_state();

    /* 设备自带的是 0x07 报文体（JoyCon 2）：目标会话只承载 0x09，不转发。 */
    pad.raw_report_id = 0x07;
    reset_sink(1, NS2_ID_PRO, NS2_REPORT_ID_09);
    CHECK(!ns2_output_send_raw(&pad));
    CHECK_EQ(s_sent_count, 0);
    pad.raw_report_id = NS2_REPORT_ID_09;

    /* 报告格式对不上（主机要 0x05）→ 不转发。 */
    reset_sink(1, NS2_ID_PRO, NS2_REPORT_ID_05);
    CHECK(!ns2_output_send_raw(&pad));
    CHECK_EQ(s_sent_count, 0);

    /* 没有会话（没连主机）→ 不转发。 */
    reset_sink(0, NS2_ID_PRO, NS2_REPORT_ID_09);
    CHECK(!ns2_output_send_raw(&pad));

    /* 载荷长度与报告标识对不上 → 不转发。 */
    reset_sink(1, NS2_ID_PRO, NS2_REPORT_ID_09);
    pad.raw_len = 20;
    CHECK(!ns2_output_send_raw(&pad));
}

static void target_prefers_relay_until_turned_off(void)
{
    target_set(ns2_target_get());
    CHECK_EQ(target_language(), PAD_LANG_NS2);
    target_set_relay(true);

    pad_state_t pad = relay_pad_state();
    pad.buttons = PAD_BTN_CROSS; /* 解析路径才算得出的按键 */
    reset_sink(1, NS2_ID_PRO, NS2_REPORT_ID_09);
    target_send_pad(&pad);
    CHECK_EQ(s_sent_count, 1);
    CHECK_EQ(s_sent[0x02], 0x80); /* 透传：设备自己的按键位图 */

    /* 关掉透传后走解析重编码：按键按位置映射成 NS2 的 B 位。 */
    target_set_relay(false);
    CHECK(!target_relay_enabled());
    target_send_pad(&pad);
    CHECK_EQ(s_sent_count, 2);
    CHECK_EQ(s_sent[0x02], 0x01);

    /* NS1 手柄（自带语言与目标不同）不走透传，直接按解析路径编码。 */
    target_set_relay(true);
    pad_state_t ns1 = pad;
    ns1.native_lang = PAD_LANG_NS1;
    ns1.raw_len = 50;
    ns1.raw_report_id = 0x30;
    target_send_pad(&ns1);
    CHECK_EQ(s_sent_count, 3);
    CHECK_EQ(s_sent[0x02], 0x01);
}

/**
 * 透传路径的耳机状态与编码路径同源：原实现把它写死成 0x00，PC 手柄插着
 * 3.5mm 耳机时主机侧永远显示未插入。两条来源（输入设备派生的 auto 值与
 * 串口 headset 覆盖值）都要进 0x09 的 0x0D。
 */
static void relay_reports_headset_state_from_same_source(void)
{
    pad_state_t pad = relay_pad_state();
    reset_sink(1, NS2_ID_PRO, NS2_REPORT_ID_09);

    ns2_output_set_headset_override(false, 0);
    ns2_output_set_headset_derived(NS2_HEADSET_NONE);
    CHECK(ns2_output_send_raw(&pad));
    CHECK_EQ(s_sent[NS2_09_OFF_HEADSET], NS2_HEADSET_NONE);

    ns2_output_set_headset_derived(NS2_HEADSET_WITH_MIC);
    CHECK(ns2_output_send_raw(&pad));
    CHECK_EQ(s_sent[NS2_09_OFF_HEADSET], NS2_HEADSET_WITH_MIC);

    ns2_output_set_headset_override(true, 0x0D);
    CHECK(ns2_output_send_raw(&pad));
    CHECK_EQ(s_sent[NS2_09_OFF_HEADSET], 0x0D);

    /* 复原：后面的用例从 auto + 未插入开始。 */
    ns2_output_set_headset_override(false, 0);
    ns2_output_set_headset_derived(NS2_HEADSET_NONE);
}

HOST_TEST_SUITE(suite_ns2_relay, "ns2_relay",
                {"运动数据填进 0x05 的 IMU 字段", motion_fills_report_05_imu_field},
                {"实验运动块按样本填 0x09 的运动区", motion_sensor_mode_fills_09_block},
                {"透传保留设备载荷并重写状态字节",
                 relay_keeps_device_payload_and_rewrites_status},
                {"透传要求报告格式对上（0x07 载荷不投给 0x09 会话）",
                 relay_requires_matching_format},
                {"目标优先透传，关闭后回到解析重编码", target_prefers_relay_until_turned_off},
                {"透传路径的耳机状态与编码路径同源",
                 relay_reports_headset_state_from_same_source});
