#include "ns2_adv.h"

#include <string.h>

ns2_adv_mode_t ns2_adv_choose_mode(bool paired, bool pairing_requested,
                                   ns2_adv_mode_t steady)
{
    if (pairing_requested || !paired) {
        return NS2_ADV_DISCOVERY;
    }
    /* 常态形态二选一（见 ns2_adv_steady_mode：默认回连，唤醒窗口内才是唤醒）；
     * 发现形态只由未配对/配对流程触发，传进来按回连处理，避免把已配对身份
     * 静默掉。 */
    return steady == NS2_ADV_WAKE ? NS2_ADV_WAKE : NS2_ADV_RECONNECT;
}

void ns2_adv_wake_window_open(ns2_adv_wake_window_t *win, int64_t now_us)
{
    win->until_us = now_us + NS2_ADV_WAKE_WINDOW_US;
}

void ns2_adv_wake_window_close(ns2_adv_wake_window_t *win)
{
    win->until_us = 0;
}

bool ns2_adv_wake_window_active(const ns2_adv_wake_window_t *win, int64_t now_us)
{
    return win->until_us != 0 && now_us < win->until_us;
}

ns2_adv_mode_t ns2_adv_steady_mode(bool wake_window)
{
    return wake_window ? NS2_ADV_WAKE : NS2_ADV_RECONNECT;
}

ns2_home_action_t ns2_adv_home_action(bool connected)
{
    return connected ? NS2_HOME_INJECT : NS2_HOME_WAKE;
}

bool ns2_adv_lr_step(ns2_adv_lr_timer_t *timer, bool paired, bool both_ready,
                     int64_t now_us)
{
    if (paired || !both_ready || now_us < timer->next_us) {
        return false;
    }
    timer->next_us = now_us + NS2_ADV_LR_RETRY_US;
    return true;
}

void ns2_adv_lr_reset(ns2_adv_lr_timer_t *timer)
{
    timer->next_us = 0;
}

bool ns2_adv_dormant_link(bool subscribed, bool features_enabled)
{
    return subscribed && !features_enabled;
}

/** 地址是否可用：全零地址写进唤醒广播等于没带地址——主机既不会回连也不会被
 *  唤醒（NVS 里存在计数虚高、尾部记录全零的历史表）。 */
static bool mac_usable(const uint8_t mac[6])
{
    if (mac == NULL) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            return true;
        }
    }
    return false;
}

const uint8_t *ns2_adv_choose_host_mac(const uint8_t *recorded,
                                       const uint8_t *const creds[], size_t cred_count)
{
    /* 记录值优先：配对交换给的是主机两条只差一位的地址，本设备实测凭证里存的
     * 那条（末字节 0x8c）发出去主机不理，而主机连接时在用的那条（末字节 0x8d）
     * 才能把它叫回来——连接对端地址是唯一有实证的判据。 */
    if (mac_usable(recorded)) {
        return recorded;
    }
    for (size_t i = 0; i < cred_count; i++) {
        if (creds != NULL && mac_usable(creds[i])) {
            return creds[i];
        }
    }
    return NULL;
}

/** 广播载荷骨架：厂商数据字段布局见 controller.md §2.1；[12]/[13] 为 PID
 *  占位，[16] 为状态位，[17..22] 为主机地址，[23] 为尾部标志 0x0F。 */
static const uint8_t s_template[NS2_ADV_PAYLOAD_LEN] = {
    0x02, 0x01, 0x06,
    0x1B, 0xFF,
    0x53, 0x05, 0x01, 0x00, 0x03,
    0x7E, 0x05,
    0x00, 0x00,
    0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void ns2_adv_payload(uint8_t out[NS2_ADV_PAYLOAD_LEN], uint16_t pid,
                     ns2_adv_mode_t mode, const uint8_t host_mac[6])
{
    memcpy(out, s_template, sizeof(s_template));
    out[12] = (uint8_t)(pid & 0xFF);
    out[13] = (uint8_t)(pid >> 8);
    if (mode == NS2_ADV_DISCOVERY || host_mac == NULL) {
        return;
    }
    memcpy(&out[5 + NS2_ADV_MFR_HOST_MAC_OFFSET], host_mac, 6);
    /* 状态位只在显式唤醒形态置 0x81：真机抓包里回连形态恒为 0x00，把 0x81
     * 写进回连广播会让休眠中的主机被每一次回连广播立刻唤醒。 */
    out[5 + NS2_ADV_MFR_STATUS_OFFSET] =
        mode == NS2_ADV_WAKE ? NS2_ADV_STATUS_WAKE : NS2_ADV_STATUS_NORMAL;
}
