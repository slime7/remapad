set(remapad_pocketjs_requires
    pocketjs_package
    pocketjs_guest
    pocketjs_ui_core
    pocketjs_ui_qjs
    pocketjs_render_rgb565
    pocketjs_runner)

idf_component_register(
    SRCS
        "main.c"
        "pocketjs_host.c"
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
