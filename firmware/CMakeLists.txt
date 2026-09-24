cmake_minimum_required(VERSION 3.22)

# 版本号单一来源：仓库根目录的 git describe（有 tag 时是版本号，否则是短哈希，
# 工作区有改动时带 -dirty）。它被写进镜像应用描述符的 PROJECT_VER，UI 系统页、
# 串口 CLI version 与 OTA 应答都读同一个字符串。
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE REMAPAD_PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT REMAPAD_PROJECT_VER)
    set(REMAPAD_PROJECT_VER "unknown")
endif()
message(STATUS "Remapad image version: ${REMAPAD_PROJECT_VER}")
set(PROJECT_VER "${REMAPAD_PROJECT_VER}")

# 版本号随提交刷新：CMake 只在配置期算一次 PROJECT_VER，之后重编译不会重跑这段，
# 切了提交镜像里也还挂着旧哈希。把 HEAD、分支与标签引用登记成配置依赖，这些文件
# 一变就自动重新配置。工作区改动（-dirty）不进依赖：改了文件想更新版本串要手动
# 跑一次 idf.py reconfigure。
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../.git/HEAD")
    file(GLOB REMAPAD_GIT_REFS
        "${CMAKE_CURRENT_LIST_DIR}/../.git/refs/heads/*"
        "${CMAKE_CURRENT_LIST_DIR}/../.git/refs/tags/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../.git/HEAD"
        "${CMAKE_CURRENT_LIST_DIR}/../.git/packed-refs"
        ${REMAPAD_GIT_REFS})
endif()

# 屏幕 UI 是可选装配（契约见 firmware/main/ui/ui_service.h）：
# ON（默认）时经 EXTRA_COMPONENT_DIRS 引入 ui/slint_ui 组件并链接，需要 Espressif 的
# xtensa Rust 工具链（换机步骤见 ui/README.md）；OFF 时纯 C 构建、不需要 Rust，
# 界面入口由 main 里的空实现（ui/ui_stub.c）顶上，屏幕保持熄灭、设置走 PC 串口 CLI。
set(REMAPAD_UI ON CACHE BOOL "编入屏幕 UI（需要 Espressif 的 xtensa Rust 工具链）")
if(REMAPAD_UI)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../ui/slint_ui")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 最小构建：显式点名组件集（main 及其依赖闭包）。界面组件不被 main 依赖
# （依赖方向是它依赖 main），REMAPAD_UI=ON 时要在这里点名才会进链接。
set(COMPONENTS main)
if(REMAPAD_UI)
    list(APPEND COMPONENTS slint_ui)
endif()
project(remapad_firmware)
