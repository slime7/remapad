set(remapad_pocketjs_requires
    esp_timer
    esp_adc
    esp_lcd
    esp_driver_ledc
    esp_driver_gpio
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_lcd_touch
    esp_lcd_touch_cst816s
    usb
    app_update
    esp_app_format
    bt
    nvs_flash
    mbedtls
    pocketjs_package
    pocketjs_guest
    pocketjs_ui_core
    pocketjs_ui_qjs
    pocketjs_render_rgb565)

idf_component_register(
    SRCS
        "main.c"
        "boot_splash.c"
        "pocketjs_host.c"
        "render_accel.c"
        "amiibo/amiibo_proto.c"
        "amiibo/amiibo_session.c"
        "amiibo/amiibo_store.c"
        "bridge/js_bridge.c"
        "config/app_config.c"
        "console/cli.c"
        "console/console_out.c"
        "drivers/panel.c"
        "drivers/backlight.c"
        "drivers/touch.c"
        "drivers/battery.c"
        "drivers/battery_curve.c"
        "drivers/buzzer.c"
        "drivers/pwr_key.c"
        "pad/pad_state.c"
        "pad/layout.c"
        "pad/layouts/xbox.c"
        "pad/layouts/ds3.c"
        "pad/layouts/ds4.c"
        "pad/layouts/ds5.c"
        "pad/layouts/ns.c"
        "pad/pad_device.c"
        "pad/ds_behavior.c"
        "pad/feedback.c"
        "input/input_frame.c"
        "input/input_source.c"
        "input/input_link.c"
        "ota/ota_proto.c"
        "ota/ota_session.c"
        "target/target.c"
        "target/ns2/ns2_report.c"
        "target/ns2/ns2_adv.c"
        "target/ns2/ns2_frames.c"
        "target/ns2/ns2_nfc.c"
        "target/ns2/ns2_upgrade.c"
        "target/ns2/ns2_output.c"
        "target/ns2/ns2_serial.c"
        "target/ns2/ns2_identity.c"
        "target/ns2/ns2_target.c"
        "ble/ble_controller.c"
        "ble/ble_session.c"
        "ble/ble_creds.c"
        "dp/dp_plane.c"
        "dp/dp_capture.c"
        "dp/dp_source.c"
        "dp/dp_ui.c"
        "usb/usb_input.c"
        "usb/usb_role.c"
        "usb/usb_transport.c"
        "usb/usb_audio.c"
        "usb/usb_audio_parse.c"
        "usb/haptic_synth.c"
    INCLUDE_DIRS
        "."
        "amiibo"
        "bridge"
        "config"
        "console"
        "drivers"
        "pad"
        "input"
        "ota"
        "target"
        "target/ns2"
        "ble"
        "dp"
        "usb"
    REQUIRES ${remapad_pocketjs_requires} spiffs
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
