#include "ns2_adv.h"

#include <string.h>

ns2_adv_mode_t ns2_adv_choose_mode(bool paired, bool pairing_requested,
                                   ns2_adv_mode_t steady)
{
    if (pairing_requested || !paired) {
        return NS2_ADV_DISCOVERY;
    }
    /* 常态形态二选一（唤醒为默认）；发现形态只由未配对/配对流程触发，
     * 传进来也按唤醒处理，避免把已配对身份静默掉。 */
    return steady == NS2_ADV_RECONNECT ? NS2_ADV_RECONNECT : NS2_ADV_WAKE;
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
