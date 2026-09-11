cmake_minimum_required(VERSION 3.22)

# PocketJS ships the ESP-IDF components in its own checkout. POCKETJS_ROOT
# selects it; without the variable the default is a sibling "pocketjs"
# directory next to this repository.
if(DEFINED ENV{POCKETJS_ROOT} AND NOT "$ENV{POCKETJS_ROOT}" STREQUAL "")
    get_filename_component(POCKETJS_ROOT "$ENV{POCKETJS_ROOT}" ABSOLUTE)
else()
    get_filename_component(POCKETJS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../pocketjs" ABSOLUTE)
endif()

set(pocketjs_components "${POCKETJS_ROOT}/hosts/esp-idf/components")
if(NOT EXISTS "${pocketjs_components}/pocketjs_ui_qjs/CMakeLists.txt")
    message(FATAL_ERROR
        "PocketJS ESP-IDF components not found under ${pocketjs_components}."
        " Set POCKETJS_ROOT to a PocketJS checkout that contains hosts/esp-idf/components.")
endif()

# ESP32-S3 links the six base components. The ESP32-P4 PPA accelerator is not
# part of this target.
list(APPEND EXTRA_COMPONENT_DIRS
    "${pocketjs_components}/pocketjs_package"
    "${pocketjs_components}/pocketjs_guest"
    "${pocketjs_components}/pocketjs_ui_core"
    "${pocketjs_components}/pocketjs_ui_qjs"
    "${pocketjs_components}/pocketjs_render_rgb565"
    "${pocketjs_components}/pocketjs_runner")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(remapad_firmware)
