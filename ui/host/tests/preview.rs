//! 预览用例：PC 预览窗里的控制条与设备画面写的是同一套动作名，
//! 点按等价于固件侧的分发（动作清单见 firmware/main/ui/ui_service.c 的 ui_service_handle_action）。

use remapad_ui as ui;

/// 设备视图高度：控制条画在它下面，不属于设备画面。
const DEVICE_HEIGHT: i32 = 280;

/// 控制条翻页后，设备画面切到下一张卡片。
#[test]
fn 预览控制条翻页后设备画面切到下一张卡片() {
    let preview = ui::new_preview();
    assert_eq!(preview.get_page(), 0, "预览从第一张卡片开始");
    let track = ui::rect(&preview, "BrightnessPage::track");
    assert!(track.bottom() <= DEVICE_HEIGHT as f32, "亮度页没画在设备视图里：{track:?}");

    let before = ui::frame(&preview);
    preview.invoke_action("next-page".into(), 0);
    assert_eq!(preview.get_page(), 1, "点下一页没有切页");

    let after = ui::frame(&preview);
    let mut changed = 0;
    for y in 0..DEVICE_HEIGHT {
        for x in 0..ui::SCREEN_WIDTH as i32 {
            if before.rgb(x, y) != after.rgb(x, y) {
                changed += 1;
            }
        }
    }
    assert!(changed > 1000, "切页后设备画面几乎没变：{changed} 个像素");
}

/// 控制条的确认键走设备上的焦点分发：电源页第二项确认后弹出关机确认。
#[test]
fn 预览里按确认键走设备上的焦点分发() {
    let preview = ui::new_preview();
    preview.set_page(3);
    preview.set_focus_index(1);
    preview.set_pad_active(true);
    assert_eq!(preview.get_dialog(), 0, "预览起步时没有弹窗");

    preview.invoke_activate_focused();
    assert_eq!(preview.get_dialog(), 2, "电源页第二项确认后应弹出关机确认");
    assert!(ui::element_or_none(&preview, "ConfirmDialog::confirm-btn").is_some(), "确认弹窗没画出来");

    preview.invoke_action("dialog-cancel".into(), 0);
    assert_eq!(preview.get_dialog(), 0, "取消后弹窗应关掉");
    assert!(ui::element_or_none(&preview, "ConfirmDialog::confirm-btn").is_none(), "弹窗没有关");
}

/// 手柄的上下键到底后再按回到另一端：焦点在可聚焦项之间循环，不卡在首尾。
#[test]
fn 焦点到底后再按继续循环到另一端() {
    let preview = ui::new_preview();
    preview.set_page(0);
    preview.set_focus_index(0);

    preview.invoke_focus_step(-1);
    assert_eq!(preview.get_focus_index(), 1, "第一项再往上应回到最后一项");
    preview.invoke_focus_step(1);
    assert_eq!(preview.get_focus_index(), 0, "最后一项再往下应回到第一项");
}
