cmake_minimum_required(VERSION 3.16)

set(LIB_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib)
set(SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
set(CFG_DIR  ${CMAKE_CURRENT_SOURCE_DIR})
set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include("${TOOLS_DIR}/sodium.cmake")

include(utun_sources.cmake)
utun_setup_sources("${LIB_DIR}" "${SRC_DIR}" "${CFG_DIR}" "${TOOLS_DIR}")

add_library(utun_lite STATIC ${UTUN_SOURCES})

target_include_directories(utun_lite PUBLIC ${UTUN_INCLUDE_DIRS})
target_compile_definitions(utun_lite PRIVATE ${UTUN_DEFINES})

set_target_properties(utun_lite PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

# Prebuilt opus (Linux only)
add_library(opus_internal STATIC IMPORTED)
set_target_properties(opus_internal PROPERTIES
    IMPORTED_LOCATION "${LIB_DIR}/libopus/libopus_internal.a"
)

target_link_libraries(utun_lite PUBLIC
    opus_internal
    OpenSSL::Crypto utun_sodium
    ${CMAKE_THREAD_LIBS_INIT}
)

add_subdirectory(tests)
