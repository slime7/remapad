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

# PocketJS 官方 ESP-IDF 组件以源码形式固定在本仓库的 components/ 下，ESP-IDF 会自动
# 发现该目录。ESP32-S3 使用六个基础组件；ESP32-P4 的 PPA 加速组件不在本目标内。
# 每个组件自带 lib/esp32s3/ 下的原生 Rust 归档，构建不需要 Rust 工具链。
# 上游 PocketJS checkout 仅作为对照参考，升级与对账方式见 patches/README.md。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(remapad_firmware)
