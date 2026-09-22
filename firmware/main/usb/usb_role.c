#include "usb_role.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"

#include "console_out.h"
#include "input_link.h"
#include "usb_transport.h"

static const char *TAG = "remapad_usb_role";

static bool s_host_active;
/** 交回串口时持有的内部 FSLS PHY 句柄：持有时 USB host 栈装不上同一块 PHY。 */
static usb_phy_handle_t s_usj_phy;

/**
 * 把内部 FSLS PHY 指回 USB-Serial/JTAG（复用开关同步翻回串口一侧）。
 * usb_del_phy 只清上下拉与焊盘、不碰复用开关，因此这一步必须显式做。
 */
static esp_err_t usj_phy_take(void)
{
    if (s_usj_phy != NULL) {
        return ESP_OK;
    }
    const usb_phy_config_t config = {
        .controller = USB_PHY_CTRL_SERIAL_JTAG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_PHY_MODE_DEFAULT,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };
    return usb_new_phy(&config, &s_usj_phy);
}

/** 放掉串口持有的 PHY 句柄，把同一块 PHY 让给 USB host 栈。 */
static void usj_phy_release(void)
{
    if (s_usj_phy != NULL) {
        usb_del_phy(s_usj_phy);
        s_usj_phy = NULL;
    }
}

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
    usj_phy_release();
    err = usb_host_start();
    if (err != ESP_OK) {
        usj_phy_take();
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
    const esp_err_t phy_err = usj_phy_take();
    console_out_use_usj();
    s_host_active = false;
    const esp_err_t err = input_link_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge link restart failed: %s", esp_err_to_name(err));
        return err;
    }
    if (phy_err != ESP_OK) {
        /* PHY 没能交回串口：COM 口要复位才会回来，界面上提示重启。 */
        ESP_LOGW(TAG, "serial phy hand-back failed: %s (reset restores the COM port)",
                 esp_err_to_name(phy_err));
    }
    ESP_LOGI(TAG, "usb role -> device (COM port is back)");
    return ESP_OK;
}

bool usb_role_host_active(void)
{
    return s_host_active;
}
