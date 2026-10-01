//! 宿主侧界面构建脚本：经 build-support 用与固件组件同一套口径（fluent 风格、字号表、
//! 默认字体）编译 src/app.slint，额外打开元素调试信息，用例才能按 .slint 里的 id 查元素。

use std::path::PathBuf;

fn main() {
  let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
  let ui_root = manifest.parent().unwrap();
  build_support::compile(ui_root, "src/app.slint", true);
}
