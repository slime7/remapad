//! 页表用例：调试页画在末位槽号上，切页后只画当前这一页。

use remapad_ui as ui;

/// 调试页按钮的底色（theme.slint 的 surface-container-highest）。
const BUTTON_BG: [u8; 3] = [0x15, 0x26, 0x3e];
/// 面板中心：页面内容以整屏中线对称。
const CENTER_X: f32 = 120.0;
/// 调试页槽号：dev 构建的页表在末尾追加它（固件侧的页数见 ui/slint_ui/src/host.rs 的 PAGE_COUNT）。
const DEBUG_PAGE: i32 = 7;

/// 开发构建的页面里，调试页画得出三个注入钮；切到别的页就不画了。
#[test]
fn 调试页画在末位槽号上且只画当前页() {
    let app = ui::new_app();
    app.set_page(DEBUG_PAGE);
    ui::settle(&app);
    assert_eq!(app.get_focus_count(), 3, "调试页应有 A键 / HOME / 控屏 三行可聚焦项");

    let buttons = ["DebugPage::a-btn", "DebugPage::home-btn", "DebugPage::ui-btn"];
    let boxes = buttons.map(|id| ui::rect(&app, id));
    let page7 = ui::frame(&app);
    for (id, button) in buttons.iter().zip(boxes) {
        let filled = page7.count_color(button, BUTTON_BG, 8);
        assert!(filled * 2 > (button.w * button.h) as usize, "{id} 没画出来：底色只占 {filled}");
    }
    let corner = [boxes[0], boxes[1]];
    let center = (corner[0].center_x() + corner[1].center_x()) / 2.0;
    assert!((center - CENTER_X).abs() < 0.5, "并排两钮没以整屏中线对称：{center:.1}");
    let wide = boxes[2];
    assert!((wide.center_x() - CENTER_X).abs() < 0.5, "下排那枚没居中：{wide:?}");
    assert!(corner[0].bottom() <= wide.y, "两排按钮叠在一起了");

    // 切页后这些元素不再出现在元素树里，按记住的盒子查画面。
    app.set_page(0);
    let page0 = ui::frame(&app);
    for (id, button) in buttons.iter().zip(boxes) {
        assert_eq!(page0.count_color(button, BUTTON_BG, 8), 0, "不在调试页却画了 {id}");
    }
}

/// 翻页时卡片从行进侧滑入，动画结束后回到静止位置（不是硬切）。
#[test]
fn 翻页时卡片从行进侧滑入再收回() {
    let app = ui::new_app();
    let rest = ui::rect(&app, "AppContent::card").x;

    app.set_page(1);
    ui::frame(&app);
    let sliding = ui::rect(&app, "AppContent::card").x;
    assert!(sliding > rest + 4.0, "翻到下一页时卡片没有从右侧滑入：{sliding:.1} 对静止 {rest:.1}");

    // 按帧推进：定时器先把位移归零，动画再走完（一次大跳只推进时钟，不产生帧）。
    for _ in 0..8 {
        ui::elapse(std::time::Duration::from_millis(50));
        ui::frame(&app);
    }
    let settled = ui::rect(&app, "AppContent::card").x;
    assert!((settled - rest).abs() < 0.5, "动画结束后卡片没回到静止位置：{settled:.1}");

    // 往回翻则从左侧滑入。
    app.set_page(0);
    ui::frame(&app);
    let back = ui::rect(&app, "AppContent::card").x;
    assert!(back < rest - 4.0, "翻回上一页时卡片没有从左侧滑入：{back:.1} 对静止 {rest:.1}");
}
