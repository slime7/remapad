//! 系统页用例：四行信息都由固件拼好再回发，字号位图必须在构建期烘到位。

use remapad_ui as ui;

/// 固件拼的电池行（见 firmware/main/ui/ui_service.c）：数字、%、· 与 V 只出现在运行期的
/// 字符串里，靠 ui/src/app.slint 的字符集锚点才被烘成字形；锚点漏了码点，
/// 上屏就是空洞或豆腐块。
const BATTERY_TEXT: &str = "100% · 4.20V";

/// 百分号与电压之间的分隔点得画成一枚小圆点，而不是漏掉或退化成豆腐块。
#[test]
fn 系统页电池行的中点分隔符画成小圆点() {
  let app = ui::new_app();
  app.set_page(6);
  app.set_battery_text(BATTERY_TEXT.into());
  // 切页是整页滑行：量几何与取画面都要等滑行走完。
  ui::settle(&app);
  let line = ui::rect(&app, "SystemInfoPage::battery-line");
  let frame = ui::frame(&app);

  let runs = frame.ink_runs(line);
  let index = runs
    .iter()
    .position(|run| run.w <= 4.0 && run.h <= 4.0)
    .unwrap_or_else(|| panic!("电池行里没有小圆点（分隔点没烘成字形？）：{runs:?}"));
  let dot = runs[index];
  assert!(
    runs.iter().filter(|run| run.w <= 4.0 && run.h <= 4.0).count() == 1,
    "电池行里有不止一枚小圆点：{runs:?}"
  );
  assert!(index > 0 && index + 1 < runs.len(), "分隔点落在了行首或行尾：{runs:?}");
  assert!(
    runs[index - 1].w >= 5.0 && runs[index + 1].w >= 5.0,
    "分隔点紧邻的不是字：{runs:?}"
  );
  assert!(dot.x > line.x + 40.0, "分隔点没落在百分号之后：{dot:?}");
  assert!(
    dot.y > line.y + 4.0 && dot.bottom() < line.bottom() - 2.0,
    "分隔点不在文字中段：{dot:?}"
  );
}
