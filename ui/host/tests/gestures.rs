//! 手势用例：按住卡片时的跟手平移与抬手切页（拖动层与切页判定写在 ui/src/app.slint）。
//! 动作在预览里按固件语义结算，因此翻页能直接看页面属性。

use std::time::Duration;

use remapad_ui as ui;

/// 静止时卡片的位置：卡面原点在屏幕（-8, -24）。
const CARD_REST_X: f32 = -8.0;
/// 手势起点：卡片内容区的空白处，只有底下的拖动层接得住。
const START: (f32, f32) = (120.0, 100.0);

/// 拖动时卡片按位移的一半跟手，位移再大也封顶 24px。
#[test]
fn 拖动时卡片按位移的一半跟手且封顶二十四像素() {
    let preview = ui::new_preview();
    ui::settle(&preview);
    assert!((ui::rect(&preview, "AppContent::card").x - CARD_REST_X).abs() < 0.5, "静止位置不对");

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 20.0, START.1);
    ui::elapse(Duration::from_millis(100));
    let half = ui::rect(&preview, "AppContent::card").x;
    assert!((half - (CARD_REST_X + 10.0)).abs() < 1.0, "位移 20px 时卡片应跟到 10px：{half:.1}");

    ui::move_to(&preview, START.0 + 80.0, START.1);
    ui::elapse(Duration::from_millis(100));
    let capped = ui::rect(&preview, "AppContent::card").x;
    assert!((capped - (CARD_REST_X + 24.0)).abs() < 1.0, "跟手位移应封顶 24px：{capped:.1}");
}

/// 向右拖过 40px 再抬手：翻到上一页（第一页回到最后一页），卡片回到静止位置。
#[test]
fn 拖动越过阈值抬手翻到上一页() {
    let preview = ui::new_preview();
    assert_eq!(preview.get_page(), 0, "预览从第一页开始");

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 10.0, START.1);
    ui::move_to(&preview, START.0 + 50.0, START.1);
    ui::release(&preview, START.0 + 50.0, START.1);

    assert_eq!(preview.get_page(), preview.get_page_count() - 1, "向右拖过阈值应翻到上一页");
    ui::settle(&preview);
    let rest = ui::rect(&preview, "AppContent::card").x;
    assert!((rest - CARD_REST_X).abs() < 0.5, "抬手后卡片应回到静止位置：{rest:.1}");
}

/// 拖动没过阈值：抬手不翻页，卡片回弹到静止位置。
#[test]
fn 拖动没过阈值时抬手不翻页且回弹() {
    let preview = ui::new_preview();

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 10.0, START.1);
    ui::elapse(Duration::from_millis(100));
    assert!(ui::rect(&preview, "AppContent::card").x > CARD_REST_X + 2.0, "按住时卡片应跟手");

    ui::release(&preview, START.0 + 10.0, START.1);
    assert_eq!(preview.get_page(), 0, "位移没过阈值不该翻页");
    ui::settle(&preview);
    let rest = ui::rect(&preview, "AppContent::card").x;
    assert!((rest - CARD_REST_X).abs() < 0.5, "抬手后应回弹到静止位置：{rest:.1}");
}

/// 单步跨过 16px 的甩动不看位移阈值：向左甩一步就翻到下一页。
#[test]
fn 快速甩动一步即翻页() {
    let preview = ui::new_preview();

    ui::press(&preview, START.0 + 40.0, START.1);
    ui::move_to(&preview, START.0 + 20.0, START.1);
    ui::release(&preview, START.0 + 20.0, START.1);

    assert_eq!(preview.get_page(), 1, "向左甩动一步应翻到下一页");
}
