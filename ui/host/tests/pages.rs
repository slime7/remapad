//! 页表用例：调试页画在末位槽号上，切页后只画当前页与左右邻页，静止时两侧各露一条花瓣边。

use std::time::Duration;

use remapad_ui as ui;

/// 调试页按钮的底色（theme.slint 的 surface-container-highest）。
const BUTTON_BG: [u8; 3] = [0x15, 0x26, 0x3e];
/// 四叶草花瓣色（assets/carousel.svg 的填充色，565 量化后同色）。
const PETAL: [u8; 3] = [0xa6, 0xc8, 0xff];
/// 面板中心：页面内容以整屏中线对称。
const CENTER_X: f32 = 120.0;
/// 一页的步距：卡片按 200px 平铺，切页滑行也走这一页。
const SLOT_PITCH: f32 = 200.0;
/// 轮播带静止时卡面原点在屏幕 (-8, -24)。
const BAND_REST_X: f32 = -8.0;
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

/// 切页时整条带按一页步距滑行：起点在行进侧的一页之外，动画结束后回到静止位置。
#[test]
fn 切页时整条带划过一整页再回到静止位置() {
    let app = ui::new_app();
    let rest = ui::rect(&app, "AppContent::band").x;
    assert!((rest - BAND_REST_X).abs() < 0.5, "静止位置不对：{rest:.1}");

    app.set_page(1);
    ui::frame(&app);
    let sliding = ui::rect(&app, "AppContent::band").x;
    assert!(
        (sliding - (rest + SLOT_PITCH)).abs() < 1.0,
        "翻到下一页时条带没有整页滑入：{sliding:.1} 对静止 {rest:.1}"
    );

    // 按帧推进：定时器先把位移归零，动画再走完（一次大跳只推进时钟，不产生帧）。
    for _ in 0..8 {
        ui::elapse(Duration::from_millis(50));
        ui::frame(&app);
    }
    let settled = ui::rect(&app, "AppContent::band").x;
    assert!((settled - rest).abs() < 0.5, "滑行结束后条带没回到静止位置：{settled:.1}");

    // 往回翻则整页从左侧滑出。
    app.set_page(0);
    ui::frame(&app);
    let back = ui::rect(&app, "AppContent::band").x;
    assert!(
        (back - (rest - SLOT_PITCH)).abs() < 1.0,
        "翻回上一页时条带没有整页滑出：{back:.1} 对静止 {rest:.1}"
    );
}

/// 静止时左右邻页各露出一条花瓣边，与中心卡之间隔着背景色（底图按一页步距平铺）。
#[test]
fn 静止时左右邻页各露出一条花瓣边() {
    let app = ui::new_app();
    ui::settle(&app);
    let frame = ui::frame(&app);

    // 取花瓣最宽的两行：避开页面里的控件，那两行上是纯底图。
    for y in [56, 152] {
        let row = |x: f32, w: f32| ui::Rect { x, y: y as f32, w, h: 1.0 };
        assert!(
            frame.count_color(row(0.0, 16.0), PETAL, 12) >= 12,
            "y={y} 左邻页没有露出花瓣边"
        );
        assert_eq!(
            frame.count_color(row(16.0, 8.0), PETAL, 12),
            0,
            "y={y} 花瓣边与中心卡之间应隔着背景色"
        );
        assert!(
            frame.count_color(row(224.0, 16.0), PETAL, 12) >= 12,
            "y={y} 右邻页没有露出花瓣边"
        );
        assert_eq!(
            frame.count_color(row(216.0, 8.0), PETAL, 12),
            0,
            "y={y} 花瓣边与中心卡之间应隔着背景色"
        );
    }
}

/// 底图按一页步距平铺：静止画面里相隔 200px 的两列落在同一个周期上，像素必须一致。
/// 卡片底图改画法（改步距、改偏移）时这条先红，平铺接缝也就跑不掉。
#[test]
fn 静止画面以两百像素为周期() {
    let app = ui::new_app();
    ui::settle(&app);
    let frame = ui::frame(&app);

    // 只比页面内容之外的行：控件占着的地方本来就不是纯底图。
    for y in [8, 16, 24, 184, 192] {
        for x in 0..40 {
            assert_eq!(
                frame.rgb(x, y),
                frame.rgb(x + 200, y),
                "y={y} x={x} 与相隔一页的像素不一致：平铺接缝或周期画错了"
            );
        }
    }
}
