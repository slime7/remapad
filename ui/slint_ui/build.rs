//! 构建脚本：经 build-support 把 ui/ 的界面编译成 Rust 代码，并按界面用到的字号把
//! 中文字形烘成位图（运行期不再解析字体文件），再写入目标配置（节拍率与开发构建开关）。

use std::path::PathBuf;

fn main() {
  let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
  let ui_root = manifest.parent().unwrap();
  println!("cargo:rerun-if-env-changed=REMAPAD_FREERTOS_HZ");
  println!("cargo:rerun-if-env-changed=REMAPAD_RELEASE");
  build_support::compile(ui_root, "src/app.slint", false);

  /* 平台等待换算需要节拍率：由组件 CMake 传入 sdkconfig 的 CONFIG_FREERTOS_HZ。 */
  let freertos_hz: u32 = std::env::var("REMAPAD_FREERTOS_HZ")
    .ok()
    .and_then(|value| value.parse().ok())
    .unwrap_or(1000);
  /* 调试页开关：沿用 UI 构建原有的口径——没显式要 release 就是开发构建。 */
  let release = std::env::var("REMAPAD_RELEASE")
    .map(|value| value == "1")
    .unwrap_or(false);
  let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
  let header = "/// 构建脚本写入的目标配置：节拍率与开发构建开关。\n";
  let freertos_line = format!("pub const FREERTOS_HZ: u32 = {freertos_hz};\n");
  let dev_line = format!("pub const UI_DEV: bool = {};\n", !release);
  std::fs::write(
    out_dir.join("remapad_slint_ui_config.rs"),
    format!("{header}{freertos_line}{dev_line}"),
  )
  .unwrap();
}
