cmake_minimum_required(VERSION 3.22)

# PocketJS 官方 ESP-IDF 组件以源码形式固定在本仓库的 components/ 下，ESP-IDF 会自动
# 发现该目录。ESP32-S3 使用六个基础组件；ESP32-P4 的 PPA 加速组件不在本目标内。
# 每个组件自带 lib/esp32s3/ 下的原生 Rust 归档，构建不需要 Rust 工具链。
# 上游 PocketJS checkout 仅作为对照参考，升级与对账方式见 patches/README.md。
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(remapad_firmware)
