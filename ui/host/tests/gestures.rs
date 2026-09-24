//! 手势用例：按住时轮播带的跟手、阈值定住与往回滑取消换页（拖动层与切页判定写在 ui/src/app.slint）。
//! 动作在预览里按固件语义结算，因此翻页能直接看页面属性。

use std::time::Duration;

use remapad_ui as ui;

/// 静止时轮播带的位置：卡面原点在屏幕（-8, -24）。
const BAND_REST_X: f32 = -8.0;
/// 切页阈值（Drag.switch-travel）：拖动到此定住，松手即换页。
const SWITCH_TRAVEL: f32 = 40.0;
/// 一页的步距：切页滑行按它算。
const SLOT_PITCH: f32 = 200.0;
/// 手势起点：卡片内容区的空白处，只有底下的拖动层接得住。
const START: (f32, f32) = (120.0, 100.0);

/// 拖动时条带跟手平移，越过切页阈值就定在阈值上（按住时不会把整页拖出去）。
#[test]
fn 拖动时条带跟手到切页阈值就定住() {
    let preview = ui::new_preview();
    ui::settle(&preview);
    assert!(
        (ui::rect(&preview, "AppContent::band").x - BAND_REST_X).abs() < 0.5,
        "静止位置不对"
    );

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 20.0, START.1);
    ui::elapse(Duration::from_millis(100));
    let follow = ui::rect(&preview, "AppContent::band").x;
    assert!((follow - (BAND_REST_X + 20.0)).abs() < 1.0, "位移 20px 时条带应跟到 20px：{follow:.1}");

    ui::move_to(&preview, START.0 + 80.0, START.1);
    ui::elapse(Duration::from_millis(100));
    let held = ui::rect(&preview, "AppContent::band").x;
    assert!(
        (held - (BAND_REST_X + SWITCH_TRAVEL)).abs() < 1.0,
        "越过切页阈值后条带应定在阈值上：{held:.1}"
    );
    ui::release(&preview, START.0 + 80.0, START.1);
}

/// 向右拖过阈值再抬手：翻到上一页（第一页回到最后一页），条带接着整页滑行到静止位置。
#[test]
fn 拖动越过阈值抬手翻到上一页() {
    let preview = ui::new_preview();
    assert_eq!(preview.get_page(), 0, "预览从第一页开始");

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 10.0, START.1);
    ui::elapse(Duration::from_millis(100));
    ui::move_to(&preview, START.0 + 50.0, START.1);
    ui::elapse(Duration::from_millis(100));
    ui::release(&preview, START.0 + 50.0, START.1);

    assert_eq!(preview.get_page(), preview.get_page_count() - 1, "向右拖过阈值应翻到上一页");
    // 页码一换，条带按一页步距反向瞬移补上这次换位：定住的 40px 再减去回位的那一页。
    let crossed = ui::rect(&preview, "AppContent::band").x;
    assert!(
        (crossed - (BAND_REST_X + SWITCH_TRAVEL - SLOT_PITCH)).abs() < 2.0,
        "抬手后条带应接着整页滑行：{crossed:.1}"
    );
    ui::settle(&preview);
    let rest = ui::rect(&preview, "AppContent::band").x;
    assert!((rest - BAND_REST_X).abs() < 0.5, "滑行结束后条带应回到静止位置：{rest:.1}");
}

/// 拖动没过阈值：抬手不翻页，条带回弹到静止位置。
#[test]
fn 拖动没过阈值时抬手不翻页且回弹() {
    let preview = ui::new_preview();

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 10.0, START.1);
    ui::elapse(Duration::from_millis(100));
    let follow = ui::rect(&preview, "AppContent::band").x;
    assert!(follow > BAND_REST_X + 8.0, "按住时条带应跟手：{follow:.1}");

    ui::release(&preview, START.0 + 10.0, START.1);
    assert_eq!(preview.get_page(), 0, "位移没过阈值不该翻页");
    ui::settle(&preview);
    let rest = ui::rect(&preview, "AppContent::band").x;
    assert!((rest - BAND_REST_X).abs() < 0.5, "抬手后应回弹到静止位置：{rest:.1}");
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

/// 往回滑取消换页：甩出去以后再滑回来（还没越过起点）就不换页，条带回弹。
#[test]
fn 往回滑动取消换页() {
    let preview = ui::new_preview();

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 - 60.0, START.1);
    ui::elapse(Duration::from_millis(100));
    ui::move_to(&preview, START.0 - 20.0, START.1);
    ui::elapse(Duration::from_millis(100));
    ui::release(&preview, START.0 - 20.0, START.1);

    assert_eq!(preview.get_page(), 0, "往回滑过抖动下限应取消这次换页");
    ui::settle(&preview);
    let rest = ui::rect(&preview, "AppContent::band").x;
    assert!((rest - BAND_REST_X).abs() < 0.5, "取消换页后条带应回到静止位置：{rest:.1}");
}

/// 滑行期间新的拖动让位：按住并移动不会再把条带带走，等静止了才恢复跟手。
#[test]
fn 滑行期间按住拖动不再跟手() {
    let preview = ui::new_preview();

    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 - 60.0, START.1);
    ui::release(&preview, START.0 - 60.0, START.1);
    assert_eq!(preview.get_page(), 1, "向左拖过阈值应翻到下一页");

    let sliding = ui::rect(&preview, "AppContent::band").x;
    ui::press(&preview, START.0, START.1);
    ui::move_to(&preview, START.0 + 30.0, START.1);
    let held = ui::rect(&preview, "AppContent::band").x;
    assert!((held - sliding).abs() < 0.5, "滑行期间条带不该再跟手：{held:.1} 对 {sliding:.1}");
}
