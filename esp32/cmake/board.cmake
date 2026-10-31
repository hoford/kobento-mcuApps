# Board selection, shared by the apps under firmware/. Include before project():
#
#   idf.py -B build/<board> -DBOARD=<board> build      (default board: m5stack_tab5)
#
# Sets BOARD_DIR, adds the shared components and the board's components (its BSP and its
# `board` component, which implements board/board.h), keeps sdkconfig in the build
# directory so boards don't overwrite each other's, and loads, in order:
#   [the app's base defaults: ESP_RC_BASE_DEFAULTS]
#   boards/<board>/sdkconfig.defaults
#   <app>/sdkconfig.defaults
#   boards/<board>/sdkconfig.<app>.defaults        (if present)
#   <app>/sdkconfig.secrets                         (if present; gitignored)
# kobento-mcuApps builds on the kobento-mcu library: the engine/renderer components and the
# board definitions live there. Expected beside this repo; override with -DKOBENTO_MCU=...
set(ESP_RC_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
get_filename_component(ESP_RC_ROOT ${ESP_RC_ROOT} ABSOLUTE)
if(NOT KOBENTO_MCU)
    set(KOBENTO_MCU ${ESP_RC_ROOT}/../../kobento-mcu)
endif()
get_filename_component(KOBENTO_MCU ${KOBENTO_MCU} ABSOLUTE)
if(NOT EXISTS ${KOBENTO_MCU}/components/rcstage)
    message(FATAL_ERROR "kobento-mcu not found at ${KOBENTO_MCU} (clone it beside this repo, or pass -DKOBENTO_MCU=...)")
endif()
set(BOARD m5stack_tab5 CACHE STRING "Board: a directory under kobento-mcu/esp32/boards/")
set(BOARD_DIR ${KOBENTO_MCU}/esp32/boards/${BOARD})
if(NOT EXISTS ${BOARD_DIR}/sdkconfig.defaults)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}': no ${BOARD_DIR}/sdkconfig.defaults")
endif()
# The chip comes from the board. Set it before project(): a fresh build directory otherwise
# takes the first CONFIG_IDF_TARGET in SDKCONFIG_DEFAULTS (the Muse SDK's is esp32c5).
file(STRINGS ${BOARD_DIR}/sdkconfig.defaults _target REGEX "^CONFIG_IDF_TARGET=")
string(REGEX REPLACE "^CONFIG_IDF_TARGET=\"?([a-z0-9]+)\"?$" "\\1" _target "${_target}")
if(_target AND NOT IDF_TARGET)
    set(IDF_TARGET ${_target})
endif()
message(STATUS "kobento-mcuApps: board ${BOARD} (${_target})")

# The library components come from kobento-mcu; the app adds its own under <app>/components.
list(APPEND EXTRA_COMPONENT_DIRS ${KOBENTO_MCU}/components ${BOARD_DIR}/components)

get_filename_component(_app ${CMAKE_SOURCE_DIR} NAME)
set(SDKCONFIG_DEFAULTS ${ESP_RC_BASE_DEFAULTS} ${BOARD_DIR}/sdkconfig.defaults
    ${CMAKE_SOURCE_DIR}/sdkconfig.defaults)
if(EXISTS ${BOARD_DIR}/sdkconfig.${_app}.defaults)
    list(APPEND SDKCONFIG_DEFAULTS ${BOARD_DIR}/sdkconfig.${_app}.defaults)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/sdkconfig.secrets)
    list(APPEND SDKCONFIG_DEFAULTS ${CMAKE_SOURCE_DIR}/sdkconfig.secrets)
endif()
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig" CACHE FILEPATH "Project configuration")
endif()
