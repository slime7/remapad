//! 宿主侧界面构建脚本：编译配置与 firmware/components/slint_ui/build.rs 同一套口径
//! （fluent 风格、字号表、默认字体在构建期烘字形位图），额外打开元素调试信息，
//! 用例才能按 .slint 里的 id 查元素。改编译口径时两处一起改。
//! 编两份：src/app.slint 给设备侧用例（App），preview.slint 给预览用例（PreviewApp）；
//! 生成代码按文件各进一个模块，因此两者的元素 id 都查得到。

use std::path::PathBuf;

/** 界面字号表：与固件组件一致，每多一档就多烘一份字形表。 */
const FONT_SIZES: &str = "12,14,16,24";

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let ui_dir = manifest.join("src");
    let preview = manifest.join("preview.slint");
    let font = std::env::var_os("REMAPAD_SLINT_FONT")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("assets/fonts/NotoSansSC-Regular.otf"));

    println!("cargo:rerun-if-changed={}", ui_dir.display());
    println!("cargo:rerun-if-changed={}", preview.display());
    println!("cargo:rerun-if-changed={}", font.display());
    println!("cargo:rerun-if-changed={}", manifest.join("assets/fonts/MaterialIcons-Regular.ttf").display());
    println!("cargo:rerun-if-changed={}", manifest.join("assets/fonts/seguisym.ttf").display());
    println!("cargo:rerun-if-env-changed=REMAPAD_SLINT_FONT");

    std::env::set_var("SLINT_DEFAULT_FONT", &font);
    std::env::set_var("SLINT_FONT_SIZES", FONT_SIZES);

    let make_config = || {
        slint_build::CompilerConfiguration::new()
            .with_style("fluent".into())
            .with_debug_info(true)
            .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer)
    };
    slint_build::compile_with_config(ui_dir.join("app.slint"), make_config()).unwrap();
    slint_build::compile_with_config(preview, make_config()).unwrap();
}
