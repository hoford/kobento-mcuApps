# Muse Gadget firmware with a RemoteCompose screen, built from the Muse Gadget SDK with the
# board and RemoteCompose pieces plugged in through the SDK's gadget_ext.h hooks.
#   idf.py -B build/<board> -DBOARD=<board> build       (see cmake/board.cmake)
#
# The SDK's cmake/project.cmake adds its main/ (the app) and components/ when included from
# another project. Ours: the shared components, the board's, and ./components (muse_rc).
cmake_minimum_required(VERSION 3.16)
set(MUSE_SDK ${CMAKE_CURRENT_LIST_DIR}/../../../../muse-gadget-sdk/esp32 CACHE PATH "muse-gadget-sdk/esp32")
get_filename_component(MUSE_SDK ${MUSE_SDK} ABSOLUTE)
set(ESP_RC_BASE_DEFAULTS ${MUSE_SDK}/sdkconfig.defaults)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake)
list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)

include(${MUSE_SDK}/cmake/project.cmake)
project(muse-gadget)
include(${MUSE_SDK}/cmake/validate_config.cmake)
