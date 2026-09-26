//! 界面编译口径:风格、字号表与字体路径只在这一处声明,host 与 fw 的 build.rs 都经它编译,
//! PC 预览用同名环境变量对齐(见 scripts/ui-preview.py)。

use std::path::{Path, PathBuf};

/// 界面字号表:每多一档就多烘一整份字形位图,theme.slint 只能取这里的档位。
pub const FONT_SIZES: &str = "12,14,16,24";

/// 构建风格:宿主用例、固件与预览三处一致。
pub const STYLE: &str = "fluent";

/// 正文字体(相对 ui/ 根):构建期按字号烘字形位图,运行期不解析字体文件。
pub fn main_font(ui_root: &Path) -> PathBuf {
  ui_root.join("assets/fonts/NotoSansSC-Regular.otf")
}

/// 图标字体(Material Symbols)。
pub fn icon_font(ui_root: &Path) -> PathBuf {
  ui_root.join("assets/fonts/MaterialIcons-Regular.ttf")
}

/// 转圈字体(盲文点阵,素材里只有它覆盖 U+28xx)。
pub fn spinner_font(ui_root: &Path) -> PathBuf {
  ui_root.join("assets/fonts/seguisym.ttf")
}

/// 烘字形用的字体:环境变量 REMAPAD_SLINT_FONT 优先,否则取仓库默认。
pub fn effective_font(ui_root: &Path) -> PathBuf {
  std::env::var_os("REMAPAD_SLINT_FONT")
    .map(PathBuf::from)
    .unwrap_or_else(|| main_font(ui_root))
}

/// 编译一个界面入口:声明重编译依赖(.slint 源码目录、入口、三份字体),
/// 设好字体与字号环境后交给 slint-build;debug_info 打开元素 id 查询,固件构建关掉。
pub fn compile(ui_root: &Path, entry: &str, debug_info: bool) {
  let font = effective_font(ui_root);
  println!("cargo:rerun-if-changed={}", ui_root.join("src").display());
  if !entry.starts_with("src/") {
    println!("cargo:rerun-if-changed={}", ui_root.join(entry).display());
  }
  println!("cargo:rerun-if-changed={}", font.display());
  println!("cargo:rerun-if-changed={}", icon_font(ui_root).display());
  println!("cargo:rerun-if-changed={}", spinner_font(ui_root).display());
  println!("cargo:rerun-if-env-changed=REMAPAD_SLINT_FONT");

  std::env::set_var("SLINT_DEFAULT_FONT", &font);
  std::env::set_var("SLINT_FONT_SIZES", FONT_SIZES);

  let mut config = slint_build::CompilerConfiguration::new()
    .with_style(STYLE.into())
    .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer);
  if debug_info {
    config = config.with_debug_info(true);
  }
  slint_build::compile_with_config(ui_root.join(entry), config).unwrap();
}
