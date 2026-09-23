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

# 屏幕 UI 组件（slint_ui）以源码形式放在 components/ 下，ESP-IDF 自动发现；它需要
# Espressif 的 xtensa Rust 工具链，换机步骤见 ui/README.md。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(remapad_firmware)
