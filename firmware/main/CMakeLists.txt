set(remapad_pocketjs_requires
    esp_timer
    esp_lcd
    esp_driver_ledc
    esp_lcd_touch
    esp_lcd_touch_cst816s
    bt
    nvs_flash
    pocketjs_package
    pocketjs_guest
    pocketjs_ui_core
    pocketjs_ui_qjs
    pocketjs_render_rgb565)

idf_component_register(
    SRCS
        "main.c"
        "pocketjs_host.c"
        "bridge/js_bridge.c"
        "drivers/panel.c"
        "drivers/backlight.c"
        "drivers/touch.c"
        "ns2/ns2_report.c"
        "ns2/ns2_frames.c"
        "ble/ble_controller.c"
        "ble/ble_session.c"
        "dp/dp_plane.c"
    INCLUDE_DIRS
        "."
        "bridge"
        "drivers"
        "ns2"
        "ble"
        "dp"
    REQUIRES ${remapad_pocketjs_requires}
)

set(POCKETJS_REMAPAD_PREBUILT_PACKAGE "" CACHE FILEPATH
    "Use an existing Remapad .pocket package instead of invoking the PocketJS CLI")
if(NOT POCKETJS_REMAPAD_PREBUILT_PACKAGE AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../ui/dist/remapad-ui.pocket")
    set(POCKETJS_REMAPAD_PREBUILT_PACKAGE
        "${CMAKE_CURRENT_LIST_DIR}/../../ui/dist/remapad-ui.pocket")
endif()

if(POCKETJS_REMAPAD_PREBUILT_PACKAGE)
    pocketjs_embed_package(
        TARGET ${COMPONENT_LIB}
        NAME remapad
        PACKAGE "${POCKETJS_REMAPAD_PREBUILT_PACKAGE}"
        HOST_PROFILE "../pocket.host.json")
else()
    pocketjs_compile_app(
        TARGET ${COMPONENT_LIB}
        NAME remapad
        MANIFEST "../../ui/pocket.json"
        HOST_PROFILE "../pocket.host.json"
        PROJECT_ROOT "../../ui")
endif()
