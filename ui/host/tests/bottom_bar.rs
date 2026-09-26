//! 底栏用例：三等分状态格的居中、手柄操控提示行的对齐、OTA 进度条。
//! 断言取元素几何（.slint 里的 id）与画面墨迹；屏幕坐标是设计稿量测值，换设计稿要同步改。

use remapad_ui as ui;
use remapad_ui::{Frame, Rect};

/// 三格的像素中心：设计稿把 240 宽的整屏三等分。
const CELL_CENTERS: [f32; 3] = [40.0, 120.0, 200.0];
/// 底栏（y 208..272）里图标与标签的中线：设计稿量出图标墨迹 226..238、标签墨迹 250..259。
const ICON_CENTER_Y: f32 = 232.0;
const LABEL_CENTER_Y: f32 = 254.5;

/// 状态格里的图标与标签都居中：三格按整屏均分，格心落在 40 / 120 / 200。
#[test]
fn 底栏三格按整屏均分且图标与标签同轴居中() {
  let app = ui::new_app();
  app.set_pc_link(true);
  app.set_player_led(0b1111);
  app.set_battery_percent(90);

  let cells = [
    "BottomBar::mode-cell",
    "BottomBar::host-cell",
    "BottomBar::battery-cell",
  ];
  let icons = [
    "BottomBar::mode-icon",
    "BottomBar::host-icon",
    "BottomBar::battery-icon",
  ];
  let labels = [
    "BottomBar::mode-text",
    "BottomBar::host-leds",
    "BottomBar::battery-text",
  ];
  for (index, id) in cells.iter().enumerate() {
    let cell = ui::rect(&app, id);
    let center = CELL_CENTERS[index];
    assert!(
      (cell.x - (center - 40.0)).abs() < 0.5 && (cell.w - 80.0).abs() < 0.5,
      "{id} 不是 80 宽的等分格：{cell:?}"
    );
    assert!(
      (cell.center_x() - center).abs() < 0.5,
      "{id} 的格心 {:.1} 不在 {center}",
      cell.center_x()
    );
    for child in [icons[index], labels[index]] {
      let child_box = ui::rect(&app, child);
      assert!((child_box.center_x() - center).abs() < 0.5, "{child} 没有按格心居中");
    }
  }

  let frame = ui::frame(&app);
  for index in 0..cells.len() {
    let center = CELL_CENTERS[index];
    let icon_band = band(center, 220.0, 22.0);
    let label_band = band(center, 246.0, 14.0);
    let icon = ink_center(&frame, icon_band, icons[index]);
    let label = ink_center(&frame, label_band, labels[index]);
    assert!(
      (icon.0 - center).abs() < 1.5,
      "{} 的图标墨迹中线 {:.1} 不在格心 {center}",
      icons[index],
      icon.0
    );
    assert!(
      (icon.1 - ICON_CENTER_Y).abs() < 1.5,
      "{} 的图标没落在这条中线上",
      icons[index]
    );
    assert!(
      (label.0 - center).abs() < 1.5,
      "{} 的标签墨迹中线 {:.1} 不在格心 {center}",
      labels[index],
      label.0
    );
    assert!(
      (label.1 - LABEL_CENTER_Y).abs() < 1.5,
      "{} 的标签没落在这条中线上",
      labels[index]
    );
  }
}

/// 手柄操控提示行：图标与同一行的文字共用一条中线，整行在底栏里居中。
#[test]
fn 手柄操控提示行的图标与文字同高() {
  let app = ui::new_app();
  app.set_pad_ui_mode(true);
  let bar = ui::element(&app, "AppContent::bar");
  let bar_box = ui::rect(&app, "AppContent::bar");
  let frame = ui::frame(&app);

  let mut rows: Vec<(f32, Vec<Rect>)> = Vec::new();
  let mut parts = bar.query_descendants().match_type_name("IconBox").find_all();
  parts.extend(bar.query_descendants().match_type_name("Text").find_all());
  for part in parts {
    let position = part.absolute_position();
    let size = part.size();
    let part_box = Rect {
      x: position.x,
      y: position.y,
      w: size.width,
      h: size.height,
    };
    match rows.iter_mut().find(|(y, _)| (*y - part_box.y).abs() < 0.5) {
      Some((_, row)) => row.push(part_box),
      None => rows.push((part_box.y, vec![part_box])),
    }
  }
  assert_eq!(rows.len(), 2, "手柄提示应该有两行");

  for (_, row) in &rows {
    let raised: Vec<f32> = row
      .iter()
      .map(|p| ui::ink_center_y(&frame, *p).expect("行里的元素没画出来"))
      .collect();
    let spread = raised.iter().cloned().fold(f32::MIN, f32::max) - raised.iter().cloned().fold(f32::MAX, f32::min);
    assert!(spread <= 1.0, "同一行的图标与文字没对齐：{raised:?}");

    let left = row.iter().map(|p| p.x).fold(f32::MAX, f32::min);
    let right = row.iter().map(|p| p.right()).fold(f32::MIN, f32::max);
    let span = Rect {
      x: left,
      y: row[0].y,
      w: right - left,
      h: 17.0,
    };
    let center = ui::ink_center_x(&frame, span).expect("提示行没有墨迹");
    assert!(
      (center - bar_box.center_x()).abs() < 1.0,
      "提示行没有居中：{center:.1} ≠ {:.1}",
      bar_box.center_x()
    );
  }
}

/// OTA 进度条在底栏里水平居中，进度从条槽左端往右长（不是从中间往两头长）。
#[test]
fn ota进度条居中且从条槽左端起填充() {
  let app = ui::new_app();
  app.set_ota_phase(1);
  let bar = ui::rect(&app, "AppContent::bar");
  let track = ui::rect(&app, "BottomBar::ota-track");
  assert!((track.center_x() - bar.center_x()).abs() < 0.5, "条槽没居中：{track:?}");
  assert!((track.w - 192.0).abs() < 0.5, "条槽宽度 {:.1} 不是 192", track.w);

  for percent in [0, 25, 50, 100] {
    app.set_ota_percent(percent);
    let fill = ui::rect(&app, "BottomBar::ota-fill");
    let expected = track.w * percent as f32 / 100.0;
    assert!(
      (fill.x - track.x).abs() < 0.5,
      "{percent}% 的进度没从条槽左端起：{fill:?}"
    );
    assert!(
      (fill.w - expected).abs() < 0.5,
      "{percent}% 的进度宽 {:.1} 不是 {expected}",
      fill.w
    );
  }

  app.set_ota_phase(0);
  assert!(
    ui::ElementHandle::find_by_element_id(&app, "BottomBar::ota-track")
      .next()
      .is_none(),
    "非升级状态还画着进度条"
  );
}

/// 电量分档的字形：图标里的填充随电量一档一档变多，不是只有低电 / 满电两种形态。
#[test]
fn 电池图标随电量逐档变满() {
  let app = ui::new_app();
  let icon = ui::rect(&app, "BottomBar::battery-icon");
  let percents = [0, 20, 40, 55, 70, 85, 95, 100];
  let mut ink = Vec::new();
  for percent in percents {
    app.set_battery_percent(percent);
    ink.push(ui::frame(&app).ink(icon).len());
  }
  for (index, pair) in ink.windows(2).enumerate() {
    assert!(
      pair[1] > pair[0],
      "电量 {}% 与 {}% 画出的电量格一样满：墨迹 {ink:?}",
      percents[index],
      percents[index + 1]
    );
  }
}

/// 量墨迹用的行内区域：裁到格内，避开底栏两端弧线露出的深色底板。
fn band(center_x: f32, y: f32, height: f32) -> Rect {
  Rect {
    x: center_x - 22.0,
    y,
    w: 44.0,
    h: height,
  }
}

/// 区域内墨迹的中线（水平, 竖直）。
fn ink_center(frame: &Frame, rect: Rect, id: &str) -> (f32, f32) {
  let x = ui::ink_center_x(frame, rect).unwrap_or_else(|| panic!("{id} 没有画出来"));
  let y = ui::ink_center_y(frame, rect).unwrap_or_else(|| panic!("{id} 没有画出来"));
  (x, y)
}
