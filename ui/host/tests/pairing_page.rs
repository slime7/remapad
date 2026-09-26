//! 配对页用例：状态行的转圈与居中。

use remapad_ui as ui;

/// 转圈按八个盲文点位轮换，每帧只有一个点。
const SPIN_PHASES: i32 = 8;

/// 转圈按相位轮换：每帧都是字框里的一枚单点，位置绕着字框转一圈。
#[test]
fn 配对页转圈按相位轮换盲文点阵单点() {
  let app = ui::new_app();
  app.set_page(2);
  app.set_pairing(2);
  ui::settle(&app);
  let glyph = ui::rect(&app, "PairingPage::spinner-glyph");

  let mut dots = Vec::new();
  for phase in 0..SPIN_PHASES {
    app.set_spinner_phase(phase);
    let frame = ui::frame(&app);
    let bounds = frame
      .ink_bounds(glyph)
      .unwrap_or_else(|| panic!("相位 {phase} 的转圈没画出来（盲文点阵没烘成字形？）"));
    assert!(
      bounds.2 - bounds.0 <= 4 && bounds.3 - bounds.1 <= 4,
      "相位 {phase} 画的不是单点：{bounds:?}"
    );
    dots.push(bounds);
  }

  for (phase, dot) in dots.iter().enumerate() {
    let left_column = (dot.0 + dot.2) as f32 / 2.0 < glyph.center_x();
    assert_eq!(left_column, phase < 4, "相位 {phase} 落错了点列：{dot:?}");
  }
  for phase in 0..3 {
    assert!(dots[phase].1 < dots[phase + 1].1, "左列的点没往下走：{dots:?}");
  }
  for phase in 4..7 {
    assert!(dots[phase].1 > dots[phase + 1].1, "右列的点没往上走：{dots:?}");
  }
  let unique: std::collections::BTreeSet<_> = dots.iter().collect();
  assert_eq!(unique.len(), dots.len(), "两帧画在同一个位置：{dots:?}");
}

/// 状态行在广播中（带转圈）与空闲时（没有转圈）都整行居中：
/// 转圈块用 if 而不是 visible，隐藏元素占位会把状态文字挤偏。
#[test]
fn 配对页状态行在有无转圈时都居中() {
  let app = ui::new_app();
  app.set_page(2);
  ui::settle(&app);
  for pairing in [0, 2] {
    app.set_pairing(pairing);
    let frame = ui::frame(&app);
    let row = ui::rect(&app, "PairingPage::status-row");
    let text = ui::rect(&app, "PairingPage::pairing-text");
    let mut span = text;
    if let Some(spinner) = ui::element_or_none(&app, "PairingPage::spinner-glyph") {
      let position = spinner.absolute_position();
      let size = spinner.size();
      let left = span.x.min(position.x);
      let right = span.right().max(position.x + size.width);
      span.x = left;
      span.w = right - left;
    }
    let center = ui::ink_center_x(&frame, span).expect("状态行没有画出来");
    assert!(
      (center - row.center_x()).abs() < 2.0,
      "pairing={pairing} 时状态行中线 {center:.1} 偏离行心 {:.1}",
      row.center_x()
    );
  }
}
