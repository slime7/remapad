/**
 * NFC 命令通路（Command 0x01）的主机端用例：主机在游戏里读一张 amiibo 时
 * 走「开轮询 → 取卡信息 → 分块取镜像 → （可选）写存档 → 关轮询」，这些用例
 * 把每一步的应答字节钉住（布局来自 ndeadly/switch2_controller_research 的样本）。
 */
#include "host_test.h"

#include <string.h>

#include "ns2_frames.h"
#include "ns2_nfc.h"

/** 一份带已知 UID 的 NTAG215 镜像：UID 04 8A 6D 2A B7 5D 80（样本卡），
 *  BCC0/BCC1 按 NTAG 规则自洽，其余字节按页号填花样。 */
static uint8_t s_image[NS2_NFC_TAG_SIZE];

static void fill_image(void)
{
    const uint8_t uid[7] = {0x04, 0x8A, 0x6D, 0x2A, 0xB7, 0x5D, 0x80};
    memset(s_image, 0, sizeof(s_image));
    memcpy(&s_image[0], uid, 3);
    s_image[3] = (uint8_t)(0x88u ^ uid[0] ^ uid[1] ^ uid[2]);
    memcpy(&s_image[4], &uid[3], 3);
    s_image[7] = (uint8_t)(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]);
    s_image[8] = uid[6];
    for (size_t i = 9; i < sizeof(s_image); i++) {
        s_image[i] = (uint8_t)i;
    }
}

/** 构造一条 Command 0x01 指令帧（BLE 形态）交给状态机，返回应答长度。 */
static size_t run_cmd(uint8_t subcmd, const uint8_t *body, size_t body_len, uint8_t *resp,
                      size_t cap)
{
    uint8_t req[NS2_FRAME_HEADER_LEN + 240];
    memset(req, 0, sizeof(req));
    req[0] = 0x01;
    req[1] = NS2_FRAME_DIR_REQUEST;
    req[2] = NS2_FRAME_TRANSPORT_BLE;
    req[3] = subcmd;
    req[5] = (uint8_t)body_len;
    if (body_len > 0 && body_len <= sizeof(req) - NS2_FRAME_HEADER_LEN) {
        memcpy(&req[8], body, body_len);
    }
    return ns2_nfc_on_command(req, NS2_FRAME_HEADER_LEN + body_len, subcmd, resp, cap);
}

/** 0x05 的 63 字节应答体：前缀（状态 + 卡类型 + UID 长度）+ 镜像 UID + 补零。 */
static void build_tag_info_body(uint8_t *out)
{
    memset(out, 0, NS2_NFC_TAG_INFO_BODY_LEN);
    out[0] = 0x09;
    out[4] = 0x01;
    out[5] = 0x01;
    out[6] = 0x02;
    out[8] = 0x07;
    memcpy(&out[9], s_image, 3);      /* UID0-2 */
    memcpy(&out[12], &s_image[4], 3); /* UID3-5 */
    out[15] = s_image[8];             /* UID6 */
}

/** 主机开轮询且选了 amiibo，输入报告的 NFC 状态字节报「卡片在场 0x09」
 *  （与 0x05 体首字节同源）；场开无卡 0x01，关轮询归零。 */
static void test_nfc_state_follows_polling(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);

    CHECK_EQ(ns2_nfc_report_state(), 0x00);
    ns2_nfc_set_polling(true);
    CHECK_EQ(ns2_nfc_report_state(), 0x09);
    ns2_nfc_set_polling(false);
    CHECK_EQ(ns2_nfc_report_state(), 0x00);

    ns2_nfc_stage(NULL, 0);
    ns2_nfc_set_polling(true);
    CHECK_EQ(ns2_nfc_report_state(), 0x01);
}

/** 主机取卡信息（0x01/0x05）拿到与样本同构的状态体，UID 取自镜像页 0-2。 */
static void test_tag_info_carries_uid(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    ns2_nfc_set_polling(true);

    uint8_t resp[NS2_FRAME_HEADER_LEN + NS2_NFC_TAG_INFO_BODY_LEN];
    const size_t len = run_cmd(0x05, NULL, 0, resp, sizeof(resp));
    CHECK_EQ(len, sizeof(resp));
    uint8_t golden[NS2_NFC_TAG_INFO_BODY_LEN];
    build_tag_info_body(golden);
    CHECK_BYTES(&resp[NS2_FRAME_HEADER_LEN], golden, NS2_NFC_TAG_INFO_BODY_LEN);
}

/** 没有预置镜像时主机取卡信息是全零体（感应区里没有卡）。 */
static void test_tag_info_empty_without_image(void)
{
    ns2_nfc_reset();
    ns2_nfc_set_polling(true);

    uint8_t resp[NS2_FRAME_HEADER_LEN + NS2_NFC_TAG_INFO_BODY_LEN];
    const size_t len = run_cmd(0x05, NULL, 0, resp, sizeof(resp));
    CHECK_EQ(len, sizeof(resp));
    const uint8_t zeros[NS2_NFC_TAG_INFO_BODY_LEN] = {0};
    CHECK_BYTES(&resp[NS2_FRAME_HEADER_LEN], zeros, NS2_NFC_TAG_INFO_BODY_LEN);
}

/** 读缓冲区 = [60 字节读卡结果头][540 字节标签镜像] 共 600 字节，按 70 字节
 *  分块，应答体带数据长度，最后一块只剩 40 字节；缓冲偏移 60-70 是 UID 块
 *  （主机校验点）。头区置零模式（amiibo hdr 0）验证分块拼装本身。 */
static void test_read_buffer_chunks(void)
{
    ns2_nfc_reset();
    ns2_nfc_set_header_mode(0);
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    ns2_nfc_set_polling(true);

    uint8_t assembled[NS2_NFC_BUFFER_TOTAL];
    size_t total = 0;
    for (uint16_t off = 0; off < NS2_NFC_BUFFER_TOTAL; off += NS2_NFC_READ_CHUNK_MAX) {
        const uint8_t body[2] = {(uint8_t)(off & 0xFFu), (uint8_t)(off >> 8)};
        uint8_t resp[NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX];
        const size_t len = run_cmd(0x15, body, sizeof(body), resp, sizeof(resp));
        CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x00);
        const size_t chunk = len - NS2_FRAME_HEADER_LEN - 3;
        CHECK_EQ(resp[NS2_FRAME_HEADER_LEN + 1], (uint8_t)(chunk & 0xFFu));
        CHECK_EQ(resp[NS2_FRAME_HEADER_LEN + 2], (uint8_t)(chunk >> 8));
        CHECK_EQ(chunk, total + NS2_NFC_READ_CHUNK_MAX <= NS2_NFC_BUFFER_TOTAL
                            ? NS2_NFC_READ_CHUNK_MAX : NS2_NFC_BUFFER_TOTAL - total);
        memcpy(&assembled[total], &resp[NS2_FRAME_HEADER_LEN + 3], chunk);
        total += chunk;
    }
    CHECK_EQ(total, NS2_NFC_BUFFER_TOTAL);
    /* 零模式下头区全零，UID 块落在缓冲偏移 60-70（主机校验点）。 */
    const uint8_t zeros[NS2_NFC_BUFFER_HEADER] = {0};
    CHECK_BYTES(assembled, zeros, NS2_NFC_BUFFER_HEADER);
    CHECK_BYTES(&assembled[NS2_NFC_BUFFER_HEADER], s_image, NS2_NFC_TAG_SIZE);
}

/** 默认（模板模式）下头区按 Switch 1 MCU 读卡响应的结构填充：前缀、UID、
 *  厂商签名（572 字节 dump 尾部）与 3B 3C 77 78 86 尾串各就各位，标签数据
 *  从缓冲偏移 60 接续。 */
static void test_read_buffer_header_template(void)
{
    ns2_nfc_reset();
    ns2_nfc_set_header_mode(1);
    fill_image();
    uint8_t dump[NS2_NFC_IMAGE_MAX];
    memcpy(dump, s_image, NS2_NFC_TAG_SIZE);
    for (size_t i = 0; i < NS2_NFC_SIG_SIZE; i++) {
        dump[NS2_NFC_TAG_SIZE + i] = (uint8_t)(0xC0 + i); /* 签名花样。 */
    }
    REQUIRE(ns2_nfc_stage(dump, sizeof(dump)) == ESP_OK);
    ns2_nfc_set_polling(true);

    uint8_t assembled[NS2_NFC_BUFFER_TOTAL];
    size_t total = 0;
    for (uint16_t off = 0; off < NS2_NFC_BUFFER_TOTAL; off += NS2_NFC_READ_CHUNK_MAX) {
        const uint8_t body[2] = {(uint8_t)(off & 0xFFu), (uint8_t)(off >> 8)};
        uint8_t resp[NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX];
        const size_t len = run_cmd(0x15, body, sizeof(body), resp, sizeof(resp));
        const size_t chunk = len - NS2_FRAME_HEADER_LEN - 3;
        memcpy(&assembled[total], &resp[NS2_FRAME_HEADER_LEN + 3], chunk);
        total += chunk;
    }

    CHECK_EQ(assembled[0], 0x31);
    CHECK_EQ(assembled[1], 0x02);
    CHECK_EQ(assembled[2], 0x00);
    CHECK_EQ(assembled[3], 0x00);
    CHECK_EQ(assembled[4], 0x00);
    CHECK_EQ(assembled[5], 0x01);
    CHECK_EQ(assembled[6], 0x02);
    CHECK_EQ(assembled[7], 0x00);
    CHECK_EQ(assembled[8], 0x07);
    /* UID（镜像页 0-2 的字段）与 0x05 卡信息同源。 */
    const uint8_t uid[7] = {0x04, 0x8A, 0x6D, 0x2A, 0xB7, 0x5D, 0x80};
    CHECK_BYTES(&assembled[9], uid, sizeof(uid));
    for (size_t i = 16; i < 20; i++) {
        CHECK_EQ(assembled[i], 0x00);
    }
    CHECK_BYTES(&assembled[20], &dump[NS2_NFC_TAG_SIZE], NS2_NFC_SIG_SIZE);
    CHECK_EQ(assembled[52], 0x00);
    CHECK_EQ(assembled[53], 0x3B);
    CHECK_EQ(assembled[54], 0x3C);
    CHECK_EQ(assembled[55], 0x77);
    CHECK_EQ(assembled[56], 0x78);
    CHECK_EQ(assembled[57], 0x86);
    CHECK_EQ(assembled[58], 0x00);
    CHECK_EQ(assembled[59], 0x00);
    CHECK_BYTES(&assembled[NS2_NFC_BUFFER_HEADER], s_image, NS2_NFC_TAG_SIZE);

    /* 540 字节上传（不带签名）时结构头仍在、签名区全零。 */
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    const uint8_t body[2] = {0x00, 0x00};
    uint8_t resp[NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX];
    const size_t len0 = run_cmd(0x15, body, sizeof(body), resp, sizeof(resp));
    REQUIRE(len0 > NS2_FRAME_HEADER_LEN + 3);
    const uint8_t *chunk0 = &resp[NS2_FRAME_HEADER_LEN + 3];
    CHECK_EQ(chunk0[0], 0x31);
    const uint8_t sig_zeros[NS2_NFC_SIG_SIZE] = {0};
    CHECK_BYTES(&chunk0[20], sig_zeros, NS2_NFC_SIG_SIZE);
}

/** 读取偏移越过缓冲区末端（600 字节）时不回数据（长度字段为 0）。 */
static void test_read_buffer_beyond_end(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);

    uint8_t resp[NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX];
    const uint8_t beyond[2] = {0x58, 0x02}; /* 0x0258 = 600 = 缓冲区总长 */
    const size_t len = run_cmd(0x15, beyond, sizeof(beyond), resp, sizeof(resp));
    CHECK_EQ(len, NS2_FRAME_HEADER_LEN + 3);
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN + 1], 0x00);
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN + 2], 0x00);
}

/** 主机写入的存档（0x14 装载）要在写卡指令（0x01/0x08）之后才出现在镜像里。 */
static uint8_t s_sink_image[NS2_NFC_TAG_SIZE];
static size_t s_sink_len;
static bool s_sink_ok;
static int s_sink_calls;

static bool sink_capture(const uint8_t *data, size_t len, void *user)
{
    (void)user;
    s_sink_calls++;
    if (len > sizeof(s_sink_image)) {
        return false;
    }
    memcpy(s_sink_image, data, len);
    s_sink_len = len;
    s_sink_ok = true;
    return true;
}

/** 主机写卡（0x14）首块带 `d0 07` 操作描述符（样本 0x14）：描述符
 *  17 字节剥掉、标签数据从页 4（镜像偏移 16）接续落位，后续块按序续写，
 *  提交后 UID/CC 只读页（页 0-3）保持原值。 */
static void test_write_descriptor_stream(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    s_sink_calls = 0;
    ns2_nfc_set_write_sink(sink_capture, NULL);
    ns2_nfc_set_polling(true);

    /* 首块：描述符（d0 07 + UID 占位 + 操作参数）+ 8 字节页数据。 */
    uint8_t first[4 + 17 + 8] = {0};
    first[2] = 17 + 8; /* len(u16 LE) */
    first[4] = 0xD0;
    first[5] = 0x07;
    for (size_t i = 0; i < 15; i++) {
        first[6 + i] = (uint8_t)(0x10 + i); /* 描述符其余字节（UID + 参数）。 */
    }
    const uint8_t pages_a[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
    memcpy(&first[4 + 17], pages_a, sizeof(pages_a));
    uint8_t resp[NS2_FRAME_HEADER_LEN + 8];
    CHECK_EQ(run_cmd(0x14, first, sizeof(first), resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);

    /* 次块：不带描述符，流式续写（偏移字段任意，按游标接续）。 */
    uint8_t next[4 + 4] = {0x46, 0x00, 0x04, 0x00, 0xB1, 0xB2, 0xB3, 0xB4};
    CHECK_EQ(run_cmd(0x14, next, sizeof(next), resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);

    CHECK_EQ(run_cmd(0x08, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(s_sink_calls, 1);
    CHECK_EQ(s_sink_len, NS2_NFC_TAG_SIZE);
    /* 页 4 起是被写入的数据，描述符没有落进镜像，页 0-3 保持原值。 */
    const uint8_t pages_b[4] = {0xB1, 0xB2, 0xB3, 0xB4};
    CHECK_BYTES(&s_sink_image[16], pages_a, sizeof(pages_a));
    CHECK_BYTES(&s_sink_image[16 + 8], pages_b, sizeof(pages_b));
    CHECK_BYTES(s_sink_image, s_image, 16);
    CHECK_BYTES(&s_sink_image[28], &s_image[28], 32);
}

static void test_write_commits_on_write_command(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    s_sink_calls = 0;
    ns2_nfc_set_write_sink(sink_capture, NULL);

    ns2_nfc_set_polling(true);
    const uint8_t load[4 + 4] = {0x64, 0x00, 0x04, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t resp[NS2_FRAME_HEADER_LEN + 8];
    CHECK_EQ(run_cmd(0x14, load, sizeof(load), resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);

    /* 提交前读到的是旧数据。 */
    uint8_t before[4];
    CHECK_EQ(ns2_nfc_read(0x64, before, sizeof(before)), sizeof(before));
    CHECK_BYTES(before, &s_image[0x64], sizeof(before));

    CHECK_EQ(run_cmd(0x08, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(s_sink_calls, 1);
    CHECK(s_sink_ok);
    CHECK_EQ(s_sink_len, NS2_NFC_TAG_SIZE);

    /* 提交后镜像带上写入的存档，写回入口收到完整镜像。 */
    uint8_t after[4];
    CHECK_EQ(ns2_nfc_read(0x64, after, sizeof(after)), sizeof(after));
    const uint8_t written[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    CHECK_BYTES(after, written, sizeof(after));
    CHECK_EQ(s_sink_image[0x64], 0xAA);
    CHECK_EQ(s_sink_image[0], s_image[0]);
    CHECK_EQ(s_sink_image[0x68], s_image[0x68]); /* 写入范围之外的页保持原值。 */
}

/** 写卡失败（存储层拒绝）不拦住主机流程：镜像照常更新，结果记日志。 */
static bool sink_reject(const uint8_t *data, size_t len, void *user)
{
    (void)data;
    (void)len;
    (void)user;
    s_sink_calls++;
    return false;
}

static void test_write_sink_failure_is_not_fatal(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    s_sink_calls = 0;
    ns2_nfc_set_write_sink(sink_reject, NULL);

    ns2_nfc_set_polling(true);
    const uint8_t load[4 + 1] = {0x10, 0x00, 0x01, 0x00, 0x5A};
    uint8_t resp[NS2_FRAME_HEADER_LEN + 8];
    CHECK_EQ(run_cmd(0x14, load, sizeof(load), resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(run_cmd(0x08, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(s_sink_calls, 1);

    uint8_t after[1];
    CHECK_EQ(ns2_nfc_read(0x10, after, sizeof(after)), sizeof(after));
    CHECK_EQ(after[0], 0x5A);
}

/** 0x0C 查询 NFC 控制器状态返回固定原值（主机初始化/reconnect 期会问这条）。 */
static void test_nfc_status_body(void)
{
    ns2_nfc_reset();
    const uint8_t golden[4] = NS2_NFC_STATUS_BODY;
    uint8_t resp[NS2_FRAME_HEADER_LEN + sizeof(golden)];
    const size_t len = run_cmd(0x0C, NULL, 0, resp, sizeof(resp));
    CHECK_EQ(len, sizeof(resp));
    CHECK_BYTES(&resp[NS2_FRAME_HEADER_LEN], golden, sizeof(golden));
}

/** 0x06 触发读卡后状态走 0x14（读取中）→ 0x15（数据就绪，读卡耗时后）：
 *  输入报告状态字节与 0x05 体首字节同源；0x03 重新开轮询复位回卡片在场。 */
static void test_read_status_progression_14_to_15(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    ns2_nfc_set_polling(true);

    uint8_t resp[NS2_FRAME_HEADER_LEN + NS2_NFC_TAG_INFO_BODY_LEN];
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x09); /* 卡片在场 */

    CHECK_EQ(run_cmd(0x06, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(ns2_nfc_report_state(), 0x14);
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x14); /* 读取中（未满读卡耗时） */

    /* 读卡耗时过后按数据就绪报告（桩时钟每次调用 +1ms，多调几次推过去）。 */
    for (int i = 0; i < 60; i++) {
        (void)ns2_nfc_report_state();
    }
    CHECK_EQ(ns2_nfc_report_state(), 0x15);
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x15);

    /* 主机抽块期间状态保持在就绪不变（抽块不被打断）。 */
    const uint8_t body[2] = {0x00, 0x00};
    uint8_t chunk[NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX];
    run_cmd(0x15, body, sizeof(body), chunk, sizeof(chunk));
    run_cmd(0x15, body, sizeof(body), chunk, sizeof(chunk));
    CHECK_EQ(ns2_nfc_report_state(), 0x15);

    /* 拉到缓冲区末端（偏移 600，长度 0）即读取结束，状态报「读取结束」值，
     * 0x05 体首字节与报告字节同源。 */
    const uint8_t eof[2] = {0x58, 0x02};
    run_cmd(0x15, eof, sizeof(eof), chunk, sizeof(chunk));
    CHECK_EQ(ns2_nfc_report_state(), 0x00);
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x00);

    /* 0x03 重新开轮询复位回卡片在场。 */
    const uint8_t poll_args[5] = {0x00, 0xE8, 0x03, 0x2C, 0x01};
    run_cmd(0x03, poll_args, sizeof(poll_args), resp, sizeof(resp));
    CHECK_EQ(ns2_nfc_report_state(), 0x09);
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x09);
}

/** 0x03 重新开轮询把读卡流程复位：状态字节回到卡片入场。 */
static void test_poll_start_resets_read_stage(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    ns2_nfc_set_polling(true);

    const uint8_t poll_args[5] = {0x00, 0xE8, 0x03, 0x2C, 0x01};
    uint8_t resp[NS2_FRAME_HEADER_LEN + 8];
    CHECK_EQ(run_cmd(0x06, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(ns2_nfc_report_state(), 0x14);
    CHECK_EQ(run_cmd(0x03, poll_args, sizeof(poll_args), resp, sizeof(resp)),
             NS2_FRAME_HEADER_LEN);
    CHECK_EQ(ns2_nfc_report_state(), 0x09);
    CHECK_EQ(run_cmd(0x06, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(ns2_nfc_report_state(), 0x14);
}

/** 串口对账用的手动钉值入口：钉住后报告值与 0x05 体首字节恒定，传 0 回自动。 */
static void test_report_stage_manual_override(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    ns2_nfc_set_polling(true);

    ns2_nfc_set_report_stage(0x15);
    CHECK_EQ(ns2_nfc_report_state(), 0x15);
    uint8_t resp[NS2_FRAME_HEADER_LEN + NS2_NFC_TAG_INFO_BODY_LEN];
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x15);
    ns2_nfc_set_report_stage(0);
    CHECK_EQ(ns2_nfc_report_state(), 0x09);
    CHECK_EQ(run_cmd(0x05, NULL, 0, resp, sizeof(resp)), sizeof(resp));
    CHECK_EQ(resp[NS2_FRAME_HEADER_LEN], 0x09);
}

/** 应答帧头的 Status/ACK 字节按子命令取固定值：0x0C/0x15 是 10/78（蓝牙
 *  亦同），其余已见子命令是 00/F8；未知子命令沿用通用帧头。 */
static void test_response_ack_pairs_follow_captures(void)
{
    uint8_t status = 0;
    uint8_t ack = 0;
    CHECK(ns2_nfc_response_ack(0x0C, &status, &ack));
    CHECK_EQ(status, 0x10);
    CHECK_EQ(ack, 0x78);
    CHECK(ns2_nfc_response_ack(0x15, &status, &ack));
    CHECK_EQ(status, 0x10);
    CHECK_EQ(ack, 0x78);

    const uint8_t bare[] = {0x03, 0x04, 0x05, 0x06, 0x08, 0x14};
    for (size_t i = 0; i < sizeof(bare); i++) {
        CHECK(ns2_nfc_response_ack(bare[i], &status, &ack));
        CHECK_EQ(status, 0x00);
        CHECK_EQ(ack, 0xF8);
    }
    CHECK(!ns2_nfc_response_ack(0x01, &status, &ack));
    CHECK(!ns2_nfc_response_ack(0x99, &status, &ack));
}

/** 抽完（EOF 探测）后按推送模式生成一个完成事件：变体 1 是 MCU read3 形态
 *  （0x05 体带 31 04 完成标记），弹出一次即清；模式 0 不生成。 */
static void test_drain_event_push_variants(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);
    ns2_nfc_set_polling(true);

    /* 模式 0：抽完不生成事件。 */
    ns2_nfc_set_push_mode(0);
    uint8_t resp[NS2_FRAME_HEADER_LEN + 3 + NS2_NFC_READ_CHUNK_MAX];
    const uint8_t eof[2] = {0x58, 0x02};
    REQUIRE(run_cmd(0x06, NULL, 0, resp, sizeof(resp)) == NS2_FRAME_HEADER_LEN);
    for (int i = 0; i < 60; i++) {
        (void)ns2_nfc_report_state();
    }
    REQUIRE(run_cmd(0x15, eof, sizeof(eof), resp, sizeof(resp)) > 0);
    ns2_nfc_event_t ev;
    CHECK(!ns2_nfc_pop_event(&ev));

    /* 模式 1：0x05 体带 31 04 完成标记（MCU read3 形态），状态回 0x09。 */
    ns2_nfc_set_push_mode(1);
    const uint8_t poll_args[5] = {0x00, 0xE8, 0x03, 0x2C, 0x01};
    run_cmd(0x03, poll_args, sizeof(poll_args), resp, sizeof(resp));
    REQUIRE(run_cmd(0x06, NULL, 0, resp, sizeof(resp)) == NS2_FRAME_HEADER_LEN);
    for (int i = 0; i < 60; i++) {
        (void)ns2_nfc_report_state();
    }
    REQUIRE(run_cmd(0x15, eof, sizeof(eof), resp, sizeof(resp)) > 0);
    CHECK(ns2_nfc_pop_event(&ev));
    CHECK_EQ(ev.subcmd, 0x05);
    CHECK_EQ(ev.body_len, NS2_NFC_TAG_INFO_BODY_LEN);
    CHECK_EQ(ev.body[0], 0x09);
    CHECK_EQ(ev.body[1], 0x31);
    CHECK_EQ(ev.body[2], 0x04);
    CHECK_EQ(ev.body[6], 0x01);
    CHECK_EQ(ev.body[10], 0x07);
    CHECK_EQ(ev.body[11], 0x04); /* UID0 取自镜像。 */
    CHECK(!ns2_nfc_pop_event(&ev)); /* 事件只弹一次。 */
}

/** 开轮询 / 触发读卡 / 关轮询这类无体子命令只回帧头 ACK，关轮询回到空闲。 */
static void test_polling_commands_ack_and_idle(void)
{
    ns2_nfc_reset();
    fill_image();
    REQUIRE(ns2_nfc_stage(s_image, sizeof(s_image)) == ESP_OK);

    uint8_t resp[NS2_FRAME_HEADER_LEN + 8];
    const uint8_t poll_args[5] = {0x00, 0xE8, 0x03, 0x2C, 0x01};
    CHECK_EQ(run_cmd(0x03, poll_args, sizeof(poll_args), resp, sizeof(resp)),
             NS2_FRAME_HEADER_LEN);
    CHECK_EQ(ns2_nfc_report_state(), 0x09);
    CHECK_EQ(run_cmd(0x06, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(run_cmd(0x04, NULL, 0, resp, sizeof(resp)), NS2_FRAME_HEADER_LEN);
    CHECK_EQ(ns2_nfc_report_state(), 0x00);
    CHECK(!ns2_nfc_polling());
}

HOST_TEST_SUITE(suite_ns2_nfc, "ns2_nfc",
                {"主机开轮询且预置镜像后 NFC 状态字节才亮起，关轮询即归零",
                 test_nfc_state_follows_polling},
                {"主机取卡信息拿到与样本同构的状态体，UID 取自镜像页 0-2",
                 test_tag_info_carries_uid},
                {"没有预置镜像时取卡信息是全零体", test_tag_info_empty_without_image},
                {"镜像按 70 字节分块读完，最后一块只剩 50 字节", test_read_buffer_chunks},
                {"头区按读卡结构填充：前缀/UID/签名/尾串，标签从偏移 60 接续",
                 test_read_buffer_header_template},
                {"读取偏移越过镜像末端时不回数据", test_read_buffer_beyond_end},
                {"主机写入的存档要在写卡指令后才出现在镜像里", test_write_commits_on_write_command},
                {"带 d0 07 描述符的写卡首块剥掉描述符、数据从页 4 流式落位",
                 test_write_descriptor_stream},
                {"写卡落盘失败不拦住主机流程", test_write_sink_failure_is_not_fatal},
                {"0x0C 状态查询返回固定原值", test_nfc_status_body},
                {"应答帧头的 Status/ACK 字节按子命令取值",
                 test_response_ack_pairs_follow_captures},
                {"0x06 触发后状态走 0x14→0x15，0x05 体首字节与报告字节同源",
                 test_read_status_progression_14_to_15},
                {"0x03 重新开轮询把读卡流程复位", test_poll_start_resets_read_stage},
                {"串口可手动钉住报告状态值做对账", test_report_stage_manual_override},
                {"抽完后按推送模式生成完成事件，弹出一次即清",
                 test_drain_event_push_variants},
                {"开轮询、读卡与关轮询只回帧头 ACK，关场回到空闲",
                 test_polling_commands_ack_and_idle});
