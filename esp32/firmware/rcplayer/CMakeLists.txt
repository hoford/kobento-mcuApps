# RemoteCompose player: embedded documents, serial console, Wi-Fi + rcNet receiver.
#   idf.py -B build/<board> -DBOARD=<board> build       (see cmake/board.cmake)
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rcplayer)
