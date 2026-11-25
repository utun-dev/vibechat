cmake_minimum_required(VERSION 3.16)

add_executable(test_standby
    test_standby.c
)

target_link_libraries(test_standby PRIVATE utun_lite pthread)
