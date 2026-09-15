#include "usb_role.h"

#include "esp_err.h"
#include "esp_log.h"

#include "console_out.h"
#include "input_link.h"
#include "usb_transport.h"

static const char *TAG = "remapad_usb_role";

static bool s_host_active;

esp_err_t usb_role_enter_host(void)
{
    if (s_host_active) {
        return ESP_OK;
    }
    /* 顺序要紧：先把日志与 CLI 出口换到 UART0（USJ 马上要让给手柄），再放掉
     * 串口链路，最后装 host 栈。任何一步失败都退回串口角色，绝不停在既没有
     * 日志也没有串口的状态。 */
    esp_err_t err = console_out_use_uart0();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart0 console unavailable: %s", esp_err_to_name(err));
        return err;
    }
    input_link_stop();
    err = usb_host_start();
    if (err != ESP_OK) {
        input_link_start();
        console_out_use_usj();
        return err;
    }
    s_host_active = true;
    ESP_LOGI(TAG, "usb role -> host (COM port disappears until reset)");
    return ESP_OK;
}

esp_err_t usb_role_leave_host(void)
{
    if (!s_host_active) {
        return ESP_OK;
    }
    usb_host_stop();
    console_out_use_usj();
    s_host_active = false;
    const esp_err_t err = input_link_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge link restart failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "usb role -> device (COM port is back)");
    return ESP_OK;
}

bool usb_role_host_active(void)
{
    return s_host_active;
}
