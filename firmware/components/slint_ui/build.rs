//! 构建脚本：把 ui/ 的界面编译成 Rust 代码，并按界面用到的字号把中文字形
//! 烘成位图（运行期不再解析字体文件）。字号表与官方 ESP-IDF 组件的取值一致。

use std::path::PathBuf;

/** 界面字号表：每多一个字号就多烘一整份字形表。 */
const FONT_SIZES: &str = "12,14,16,24";

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let ui_dir = manifest.join("../../../ui/src");
    let font = std::env::var_os("REMAPAD_SLINT_FONT")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../../../ui/assets/fonts/NotoSansSC-Regular.otf"));

    println!("cargo:rerun-if-changed={}", ui_dir.display());
    println!("cargo:rerun-if-changed={}", font.display());
    println!(
        "cargo:rerun-if-changed={}",
        manifest.join("../../../ui/assets/fonts/MaterialIcons-Regular.ttf").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        manifest.join("../../../ui/assets/fonts/seguisym.ttf").display()
    );
    println!("cargo:rerun-if-env-changed=REMAPAD_SLINT_FONT");
    println!("cargo:rerun-if-env-changed=REMAPAD_FREERTOS_HZ");
    println!("cargo:rerun-if-env-changed=REMAPAD_RELEASE");

    /* 平台等待换算需要节拍率：由组件 CMake 传入 sdkconfig 的 CONFIG_FREERTOS_HZ。 */
    let freertos_hz: u32 = std::env::var("REMAPAD_FREERTOS_HZ")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(1000);
    /* 调试页开关：沿用 UI 构建原有的口径——没显式要 release 就是开发构建。 */
    let release = std::env::var("REMAPAD_RELEASE").map(|value| value == "1").unwrap_or(false);
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let header = "/// 构建脚本写入的目标配置：节拍率与开发构建开关。\n";
    let freertos_line = format!("pub const FREERTOS_HZ: u32 = {freertos_hz};\n");
    let dev_line = format!("pub const UI_DEV: bool = {};\n", !release);
    std::fs::write(
        out_dir.join("remapad_slint_ui_config.rs"),
        format!("{header}{freertos_line}{dev_line}"),
    )
    .unwrap();

    std::env::set_var("SLINT_DEFAULT_FONT", &font);
    std::env::set_var("SLINT_FONT_SIZES", FONT_SIZES);

    let config = slint_build::CompilerConfiguration::new()
        .with_style("fluent".into())
        .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer);
    slint_build::compile_with_config(ui_dir.join("app.slint"), config).unwrap();
}
