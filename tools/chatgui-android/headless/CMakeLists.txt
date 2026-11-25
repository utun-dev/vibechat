cmake_minimum_required(VERSION 3.16)

add_executable(headless
    headless_main.c
    headless_control.c
)

target_include_directories(headless PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../libutun_lite
)

target_link_libraries(headless PRIVATE utun_lite pthread)
