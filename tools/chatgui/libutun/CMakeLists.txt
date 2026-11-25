cmake_minimum_required(VERSION 3.16)

set(LIB_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib)
set(SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
set(TRANSPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../transport)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../sodium.cmake")

# ============================================================================
# libuasync — low-level runtime library
# ============================================================================

file(GLOB UASYNC_SOURCES CONFIGURE_DEPENDS "${LIB_DIR}/*.c")
set(UASYNC_SPEEXDSP_SOURCES
    "${LIB_DIR}/speexdsp/preprocess.c"
    "${LIB_DIR}/speexdsp/filterbank.c"
    "${LIB_DIR}/speexdsp/mdf.c"
    "${LIB_DIR}/speexdsp/fftwrap.c"
    "${LIB_DIR}/speexdsp/kiss_fft.c"
    "${LIB_DIR}/speexdsp/kiss_fftr.c"
)
list(APPEND UASYNC_SOURCES ${UASYNC_SPEEXDSP_SOURCES})

add_library(uasync STATIC ${UASYNC_SOURCES})

target_include_directories(uasync PUBLIC
    ${LIB_DIR}
    ${SRC_DIR}
    ${LIB_DIR}/libopus/include
    ${LIB_DIR}/speexdsp
    ${LIB_DIR}/speexdsp/include
)

file(GLOB OPUS_SOURCES CONFIGURE_DEPENDS
    "${LIB_DIR}/libopus/celt/*.c"
    "${LIB_DIR}/libopus/silk/*.c"
    "${LIB_DIR}/libopus/silk/float/*.c"
    "${LIB_DIR}/libopus/src/*.c"
)

add_library(opus_internal STATIC ${OPUS_SOURCES})

target_include_directories(opus_internal PUBLIC
    ${LIB_DIR}/libopus
    ${LIB_DIR}/libopus/include
    ${LIB_DIR}/libopus/celt
    ${LIB_DIR}/libopus/silk
    ${LIB_DIR}/libopus/silk/float
    ${LIB_DIR}/libopus/silk/fixed
    ${LIB_DIR}/libopus/src
)

target_compile_definitions(opus_internal PRIVATE
    OPUS_BUILD
    VAR_ARRAYS
    USE_ALLOCA
)

set_target_properties(opus_internal PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC OFF
)

target_link_libraries(uasync PUBLIC opus_internal)

# uasync использует WinSock, IP Helper и BCrypt; импортные библиотеки должны идти после её архива.
if(WIN32)
    target_link_libraries(uasync PUBLIC ws2_32 iphlpapi bcrypt)
endif()

target_compile_definitions(uasync PRIVATE
    _ISOC99_SOURCE
    DEBUG_OUTPUT_STDERR
    FLOATING_POINT
    USE_KISS_FFT
    EXPORT=
)

set_target_properties(uasync PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

# ONNX Runtime (для Silero VAD) — опционально: если найден, включаем VAD.
if(CHATGUI_BUILD_USER_ARCHIVE)
    unset(ONNXRUNTIME_INCLUDE_DIR CACHE)
    unset(ONNXRUNTIME_LIB CACHE)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime/onnxruntime_c_api.h PATHS /usr/include NO_DEFAULT_PATH REQUIRED)
    find_library(ONNXRUNTIME_LIB onnxruntime PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} NO_DEFAULT_PATH REQUIRED)
else()
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime/onnxruntime_c_api.h)
    find_library(ONNXRUNTIME_LIB onnxruntime)
endif()
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
    target_include_directories(uasync PUBLIC ${ONNXRUNTIME_INCLUDE_DIR})
    target_compile_definitions(uasync PRIVATE HAVE_SILERO_VAD)
    target_link_libraries(uasync PUBLIC ${ONNXRUNTIME_LIB})
    message(STATUS "ONNX Runtime found — Silero VAD enabled")
else()
    message(STATUS "ONNX Runtime not found — Silero VAD disabled")
endif()

# ============================================================================
# libutun — VPN core library (all src/ except utun.c)
# ============================================================================

# Common sources (all platforms) — all .c from src/ except main entry point and ref/ dirs
file(GLOB_RECURSE UTUN_COMMON_SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/*.c")
list(FILTER UTUN_COMMON_SOURCES EXCLUDE REGEX "/lwip_orig/")
list(FILTER UTUN_COMMON_SOURCES EXCLUDE REGEX "/uip/")
list(FILTER UTUN_COMMON_SOURCES EXCLUDE REGEX "/transport_layer/BBR/")
# аудио-контур звонка/рации зависит от C++ (SoundTouch/voice_jitter) — собирается в libutun_voice
list(FILTER UTUN_COMMON_SOURCES EXCLUDE REGEX "/call/call_audio\\.c$")
list(FILTER UTUN_COMMON_SOURCES EXCLUDE REGEX "/call/call_tones\\.c$")
list(FILTER UTUN_COMMON_SOURCES EXCLUDE REGEX "/radio/radio_audio\\.c$")
list(REMOVE_ITEM UTUN_COMMON_SOURCES "${SRC_DIR}/utun.c")

# Platform-specific TUN implementation
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(UTUN_TUN_SOURCES ${SRC_DIR}/tun_linux.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
    set(UTUN_TUN_SOURCES ${SRC_DIR}/tun_freebsd.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(UTUN_TUN_SOURCES ${SRC_DIR}/tun_windows.c)
else()
    set(UTUN_TUN_SOURCES ${SRC_DIR}/tun_linux.c)
endif()

add_library(utun STATIC ${UTUN_COMMON_SOURCES} ${UTUN_TUN_SOURCES})

target_include_directories(utun PUBLIC
    ${SRC_DIR}
    ${SRC_DIR}/transport_layer
    ${SRC_DIR}/routing_layer
    ${SRC_DIR}/media_delivery
    ${SRC_DIR}/media_async
    ${SRC_DIR}/call
    ${LIB_DIR}
    ${SRC_DIR}/uip
    ${CMAKE_SOURCE_DIR}/db
)

target_compile_definitions(utun PRIVATE
    USE_OPENSSL
    USE_SQLITE
)

# FFmpeg (optional) — FFMPEG_* переменные вычислены в родительском CMakeLists.txt
if(FFMPEG_FOUND)
    target_include_directories(utun PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(utun PRIVATE HAVE_FFMPEG)
    target_link_libraries(utun PUBLIC ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
endif()

set_target_properties(utun PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

target_link_libraries(utun PUBLIC
    uasync
    OpenSSL::Crypto utun_sodium
    ${CMAKE_THREAD_LIBS_INIT}
)

# Platform-specific link flags
if(WIN32)
    target_link_libraries(utun PUBLIC iphlpapi advapi32 bcrypt dbghelp)
endif()
