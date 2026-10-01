//! 浏览器预览包的界面构建脚本：经 build-support 用与固件组件、宿主用例同一套口径
//! （fluent 风格、字号表、字体）编译 ui/preview/wasm.slint（248 × 280 玻璃窗，
//! 设备画面与动作结算在 preview-core.slint）；不需要元素调试信息。

use std::path::PathBuf;

fn main() {
  let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
  let ui_root = manifest.parent().unwrap();
  build_support::compile(ui_root, "preview/wasm.slint", false);
}
