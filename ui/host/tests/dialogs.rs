//! 弹窗与全屏等待画面的用例：遮罩盖住当前页、页面控件不再响应点按、焦点环让位给弹窗。
//! 弹窗是 ConfirmDialog（ui/src/components.slint），整屏遮罩写在 ui/src/app.slint。

use remapad_ui as ui;

/// 角钮的常规底色（theme.slint 的 secondary-container）。
const CORNER_BG: [u8; 3] = [0x15, 0x2a, 0x1f];

/// 两个画面在给定区域里有几个像素不同。
fn diff_pixels(a: &ui::Frame, b: &ui::Frame, area: ui::Rect) -> usize {
  let (x0, y0, x1, y1) = area.pixel_bounds();
  let mut diff = 0;
  for y in y0..y1 {
    for x in x0..x1 {
      if a.rgb(x, y) != b.rgb(x, y) {
        diff += 1;
      }
    }
  }
  diff
}

/// 整屏平均亮度：判断遮罩有没有把画面压暗。
fn mean_luma(frame: &ui::Frame) -> f32 {
  let mut sum = 0u64;
  for y in 0..ui::SCREEN_HEIGHT as i32 {
    for x in 0..ui::SCREEN_WIDTH as i32 {
      let rgb = frame.rgb(x, y);
      sum += (rgb[0] as u64 + rgb[1] as u64 + rgb[2] as u64) / 3;
    }
  }
  sum as f32 / (ui::SCREEN_WIDTH * ui::SCREEN_HEIGHT) as f32
}

/// 弹窗打开时遮罩盖住整屏：页面看不出原样，弹窗盒画在正中。
#[test]
fn 弹窗打开时遮罩盖住整屏() {
  let app = ui::new_app();
  app.set_page(0);
  ui::settle(&app);
  let plus = ui::rect(&app, "BrightnessPage::plus");
  let before = ui::frame(&app);
  assert!(before.count_color(plus, CORNER_BG, 8) > 0, "没有弹窗时角钮照常画出来");

  app.set_dialog(1);
  let covered = ui::frame(&app);
  let box_rect = ui::rect(&app, "ConfirmDialog::box");
  let center = ui::SCREEN_WIDTH as f32 / 2.0;
  assert!(
    (box_rect.center_x() - center).abs() < 0.5,
    "弹窗盒没横向居中：{box_rect:?}"
  );
  assert!(
    ui::element_or_none(&app, "ConfirmDialog::cancel-btn").is_some(),
    "弹窗按钮没画出来"
  );
  assert_eq!(
    covered.count_color(plus, CORNER_BG, 8),
    0,
    "遮罩下的页面还留着原来的底色"
  );
  let (dim_before, dim_after) = (mean_luma(&before), mean_luma(&covered));
  assert!(
    dim_after * 1.2 < dim_before,
    "遮罩没把整屏压暗：{dim_before:.1} → {dim_after:.1}"
  );
}

/// 弹窗打开时页面控件收不到点按：同一处点按只落在弹窗上。
#[test]
fn 弹窗打开时页面控件不再响应点按() {
  let preview = ui::new_preview();
  preview.set_page(0);
  ui::settle(&preview);
  let plus = ui::rect(&preview, "BrightnessPage::plus");
  let start = preview.get_backlight();

  ui::tap(&preview, plus.center_x(), plus.center_y());
  assert_eq!(preview.get_backlight(), start + 20, "没有弹窗时点亮度加应生效");

  preview.invoke_action("ask-reboot".into(), 0);
  assert_eq!(preview.get_dialog(), 1, "先弹重启确认");
  ui::frame(&preview);
  ui::tap(&preview, plus.center_x(), plus.center_y());
  assert_eq!(
    preview.get_backlight(),
    start + 20,
    "弹窗盖住页面后同一处点按不该改背光"
  );
}

/// 弹窗打开时页面不再画焦点环：焦点已经交给弹窗，页面上留一个亮环会误导。
#[test]
fn 弹窗打开时页面不再画焦点环() {
  let app = ui::new_app();
  app.set_page(0);
  app.set_pad_active(true);
  app.set_focus_index(0);
  app.set_dialog(1);
  ui::frame(&app);
  let plus = ui::rect(&app, "BrightnessPage::plus");

  let first = ui::frame(&app);
  app.set_focus_index(1);
  let second = ui::frame(&app);
  assert_eq!(diff_pixels(&first, &second, plus), 0, "弹窗打开时页面上还画着焦点环");

  app.set_dialog(0);
  app.set_focus_index(0);
  ui::frame(&app);
  let third = ui::frame(&app);
  app.set_focus_index(1);
  let fourth = ui::frame(&app);
  assert!(diff_pixels(&third, &fourth, plus) > 0, "没有弹窗时焦点环应画在页面上");
}

/// 重启与关机等待画面盖住整屏：中间是提示文字，页面看不见。
#[test]
fn 重启与关机等待画面盖住整屏() {
  let app = ui::new_app();
  app.set_page(0);
  ui::settle(&app);
  let plus = ui::rect(&app, "BrightnessPage::plus");
  let full = ui::Rect {
    x: 0.0,
    y: 0.0,
    w: ui::SCREEN_WIDTH as f32,
    h: ui::SCREEN_HEIGHT as f32,
  };
  let middle = ui::Rect {
    x: 0.0,
    y: 120.0,
    w: ui::SCREEN_WIDTH as f32,
    h: 40.0,
  };
  let area = (ui::SCREEN_WIDTH * ui::SCREEN_HEIGHT / 2) as usize;

  let cases: [(&str, fn(&ui::App, bool)); 2] = [
    ("重启等待", |app, on| app.set_rebooting(on)),
    ("关机等待", |app, on| app.set_powering_off(on)),
  ];
  for (what, set) in cases {
    set(&app, true);
    let waiting = ui::frame(&app);
    assert_eq!(waiting.count_color(plus, CORNER_BG, 8), 0, "{what}画面没盖住页面");
    assert!(waiting.count_color(full, [0, 0, 0], 4) > area, "{what}画面不是整屏黑底");
    assert!(waiting.ink_bounds(middle).is_some(), "{what}画面中间没有提示文字");
    set(&app, false);
    ui::settle(&app);
    let normal = ui::frame(&app);
    assert!(
      normal.count_color(plus, CORNER_BG, 8) > 0,
      "{what}画面收掉后页面应照常画出来"
    );
  }
}
