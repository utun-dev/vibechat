cmake_minimum_required(VERSION 3.16)
project(chatgui_android VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LIB_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)
set(SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(OpenSSL REQUIRED)

add_subdirectory(libutun_lite)
add_subdirectory(headless)
