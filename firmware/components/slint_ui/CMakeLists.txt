# Slint 屏幕 UI 组件：界面与平台层都在 Rust 侧（slint crate 的软件渲染器），
# 由 cargo 交叉编译成静态库后链进固件；.slint 在构建期编译成 Rust 代码并按字号烘字形位图。
# src/rust_heap.c 是 alloc crate 的内存出口（含放置策略），Rust 侧只剩 rustc 强制的转发项。
# 需要 Espressif 的 xtensa Rust 工具链（espup 安装，默认名字 esp）与它自带的 rust-src。

idf_component_register(
    SRCS "src/rust_heap.c"
    INCLUDE_DIRS "include"
    REQUIRES "esp_lcd" "esp_lcd_touch"
    PRIV_REQUIRES "esp_timer" "freertos" "heap" "log"
    REQUIRED_IDF_TARGETS esp32s3
)

set(REMAPAD_SLINT_RUST_TOOLCHAIN "esp" CACHE STRING
    "xtensa 目标所需的 rustup 工具链名（espup 安装）")
# 生效的工具链名：环境变量优先（临时切换，撤销后回到缓存值），否则用缓存变量；
# 两者都在配置阶段读取，改完环境变量要重跑 idf.py reconfigure。
set(slint_rust_toolchain "${REMAPAD_SLINT_RUST_TOOLCHAIN}")
if(DEFINED ENV{REMAPAD_SLINT_RUST_TOOLCHAIN})
    set(slint_rust_toolchain "$ENV{REMAPAD_SLINT_RUST_TOOLCHAIN}")
    message(STATUS "Slint UI: 工具链取环境变量 REMAPAD_SLINT_RUST_TOOLCHAIN=${slint_rust_toolchain}")
endif()
set(REMAPAD_SLINT_FONT
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/assets/fonts/NotoSansSC-Regular.otf" CACHE FILEPATH
    "界面字体：编译期按字号烘制字形位图，运行时不解析字体文件")
set(REMAPAD_SLINT_ICON_FONT
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/assets/fonts/MaterialIcons-Regular.ttf" CACHE FILEPATH
    "图标字体：Material Symbols，编译期按界面用到的字符烘字形位图")
set(REMAPAD_SLINT_SPINNER_FONT
    "${CMAKE_CURRENT_LIST_DIR}/../../../ui/assets/fonts/seguisym.ttf" CACHE FILEPATH
    "转圈用的盲文点阵取自这份字体（素材里只有它覆盖 U+28xx）")
# 调试页：沿用 UI 构建原有的口径，环境变量 REMAPAD_RELEASE=1 才当发布构建（页表不含调试页）。
if(DEFINED ENV{REMAPAD_RELEASE})
    set(slint_release "$ENV{REMAPAD_RELEASE}")
else()
    set(slint_release "0")
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

find_program(REMAPAD_SLINT_CARGO NAMES cargo)
if(NOT REMAPAD_SLINT_CARGO)
    message(FATAL_ERROR
        "PATH 里找不到 cargo：先装 rustup（https://rustup.rs）并让 ~/.cargo/bin 进 PATH，\n"
        "再按 ui/README.md 装 xtensa 工具链（scripts/setup-rust-toolchain.py 可一键装）。")
endif()
get_filename_component(slint_tool_dir "${REMAPAD_SLINT_CARGO}" DIRECTORY)

# 工具链自检：缺工具链或缺 rust-src 时在配置阶段就说清修复命令，不留给构建期的 cargo 报错。
execute_process(
    COMMAND "${REMAPAD_SLINT_CARGO}" "+${slint_rust_toolchain}" --version
    RESULT_VARIABLE slint_cargo_status
    OUTPUT_VARIABLE slint_cargo_version
    ERROR_VARIABLE slint_cargo_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT slint_cargo_status EQUAL 0)
    message(FATAL_ERROR
        "cargo +${slint_rust_toolchain} 不可用：${slint_cargo_error}\n"
        "屏幕 UI 组件需要 Espressif 的 xtensa Rust 工具链：scripts/setup-rust-toolchain.py\n"
        "换机步骤见 ui/README.md；工具链名字不同时设环境变量 REMAPAD_SLINT_RUST_TOOLCHAIN\n"
        "或加 -DREMAPAD_SLINT_RUST_TOOLCHAIN=<名字> 后重跑 idf.py build")
endif()
message(STATUS "Slint UI: ${slint_cargo_version} -> xtensa-esp32s3-none-elf")

find_program(REMAPAD_SLINT_RUSTC NAMES rustc HINTS "${slint_tool_dir}" NO_DEFAULT_PATH)
if(REMAPAD_SLINT_RUSTC)
    execute_process(
        COMMAND "${REMAPAD_SLINT_RUSTC}" "+${slint_rust_toolchain}" --print sysroot
        RESULT_VARIABLE slint_sysroot_status
        OUTPUT_VARIABLE slint_sysroot
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(slint_sysroot_status EQUAL 0
       AND NOT EXISTS "${slint_sysroot}/lib/rustlib/src/rust/library/core/Cargo.toml")
        message(FATAL_ERROR
            "工具链 ${slint_rust_toolchain} 缺 rust-src 组件（-Zbuild-std=core,alloc 要用）：\n"
            "rustup component add rust-src --toolchain ${slint_rust_toolchain}")
    endif()
endif()

set(slint_rust_dir "${CMAKE_CURRENT_LIST_DIR}")
set(slint_cargo_target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(slint_rust_lib "${slint_cargo_target_dir}/xtensa-esp32s3-none-elf/release/libslint_ui.a")

file(GLOB_RECURSE slint_rust_sources CONFIGURE_DEPENDS
    "${slint_rust_dir}/src/*.rs"
    "${slint_rust_dir}/../../../ui/src/*.slint"
    "${slint_rust_dir}/../../../ui/assets/*.svg")

add_custom_command(
    OUTPUT "${slint_rust_lib}"
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${slint_cargo_target_dir}"
        "RUSTFLAGS="
        "CARGO_ENCODED_RUSTFLAGS="
        "REMAPAD_FREERTOS_HZ=${CONFIG_FREERTOS_HZ}"
        "REMAPAD_RELEASE=${slint_release}"
        "REMAPAD_SLINT_FONT=${REMAPAD_SLINT_FONT}"
        "${REMAPAD_SLINT_CARGO}" "+${slint_rust_toolchain}" build
        "-Zbuild-std=core,alloc" --release --locked
        --target xtensa-esp32s3-none-elf
        --manifest-path "${slint_rust_dir}/Cargo.toml"
    DEPENDS ${slint_rust_sources}
        "${slint_rust_dir}/Cargo.toml"
        "${slint_rust_dir}/Cargo.lock"
        "${slint_rust_dir}/build.rs"
        "${REMAPAD_SLINT_FONT}"
        "${REMAPAD_SLINT_ICON_FONT}"
        "${REMAPAD_SLINT_SPINNER_FONT}"
    WORKING_DIRECTORY "${slint_rust_dir}"
    COMMENT "Compiling Slint UI (Rust, xtensa-esp32s3-none-elf)"
    VERBATIM)

add_custom_target(slint_ui_rust_build DEPENDS "${slint_rust_lib}")
add_library(slint_ui_rust STATIC IMPORTED GLOBAL)
set_target_properties(slint_ui_rust PROPERTIES IMPORTED_LOCATION "${slint_rust_lib}")
add_dependencies(slint_ui_rust slint_ui_rust_build)
target_link_libraries(${COMPONENT_LIB} INTERFACE slint_ui_rust)

# 平台与画面统计都在 Rust 侧，面板与触摸由固件侧经 hooks 传入。
# 归档按命令行顺序扫描：这三个入口在静态库里没有别的引用者，必须显式要求保留
# （rust_heap.c 的四个入口同在一个目标文件里，指定其中一个即可全部留下）。
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=remapad_slint_ui_start"
    "-Wl,--undefined=remapad_slint_ui_loop"
    "-Wl,--undefined=remapad_slint_heap_alloc")
