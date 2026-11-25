cmake_minimum_required(VERSION 3.22)
project("utun_bridge")

# Ninja запускает компиляторы отдельно от Gradle workers; ограничиваем оба уровня сборки.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS utun_compile=4 utun_link=1)
set(CMAKE_JOB_POOL_COMPILE utun_compile)
set(CMAKE_JOB_POOL_LINK utun_link)
message(STATUS "Android native build pools: compile=4, link=1 per ABI")

set(LIB_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../lib)
set(SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../src)
set(CFG_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../libutun_lite)
set(JNI_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../jni_bridge)
set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)

include(${CFG_DIR}/utun_sources.cmake)
include("${CFG_DIR}/../../sodium.cmake")
utun_setup_sources("${LIB_DIR}" "${SRC_DIR}" "${CFG_DIR}" "${TOOLS_DIR}")

# Include Opus codec wrapper (was excluded before voice support)
# list(FILTER UTUN_SOURCES EXCLUDE REGEX "opus_codec\\.c$")

# Exclude tun_windows and tun_freebsd (Android is Linux-like)
list(FILTER UTUN_SOURCES EXCLUDE REGEX "tun_windows\\.c$")
list(FILTER UTUN_SOURCES EXCLUDE REGEX "tun_freebsd\\.c$")

# Add libopus sources (for voice recording)
file(GLOB_RECURSE _OPUS_SRC "${LIB_DIR}/libopus/src/*.c"
     "${LIB_DIR}/libopus/celt/*.c" "${LIB_DIR}/libopus/silk/*.c"
     "${LIB_DIR}/libopus/silk/float/*.c")
list(FILTER _OPUS_SRC EXCLUDE REGEX "/x86/")
list(APPEND UTUN_SOURCES ${_OPUS_SRC})
list(APPEND UTUN_INCLUDE_DIRS "${LIB_DIR}/libopus" "${LIB_DIR}/libopus/include"
     "${LIB_DIR}/libopus/celt" "${LIB_DIR}/libopus/silk" "${LIB_DIR}/libopus/silk/float")
# Opus requires these to be defined (or HAVE_CONFIG_H + config.h with VAR_ARRAYS)
list(APPEND UTUN_DEFINES OPUS_BUILD VAR_ARRAYS USE_ALLOCA)

# SpeexDSP (акустическое эхоподавление AEC) — обёртка speex_aec.c уже в lib/*.c GLOB.
file(GLOB _SPEEXDSP_SRC "${LIB_DIR}/speexdsp/*.c")
list(APPEND UTUN_SOURCES ${_SPEEXDSP_SRC})
list(APPEND UTUN_INCLUDE_DIRS "${LIB_DIR}/speexdsp" "${LIB_DIR}/speexdsp/include")
list(APPEND UTUN_DEFINES FLOATING_POINT USE_KISS_FFT "EXPORT=")

# Голосовой стек звонка (SoundTouch + voice_jitter + call_audio + call_tones),
# C++ внутри, C-интерфейс наружу (call_audio.h).
set(CALL_DIR ${SRC_DIR}/call)
include(${CALL_DIR}/voice_sources.cmake)
utun_setup_voice("${LIB_DIR}" "${CALL_DIR}")
add_library(utun_voice STATIC ${UTUN_VOICE_SOURCES})
target_include_directories(utun_voice PUBLIC ${UTUN_VOICE_INCLUDE_DIRS})
set_source_files_properties(${CALL_DIR}/call_audio.c ${CALL_DIR}/call_tones.c PROPERTIES LANGUAGE C)
set_source_files_properties(${SRC_DIR}/radio/radio_audio.c PROPERTIES LANGUAGE C)
set_target_properties(utun_voice PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Prebuilt OpenSSL (cross-compiled for Android)
set(OPENSSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/openssl/${ANDROID_ABI})
add_library(openssl_crypto STATIC IMPORTED)
set_target_properties(openssl_crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_DIR}/lib/libcrypto.a"
)

# ONNX Runtime (для Silero VAD) — prebuilt .so в jniLibs/<abi>, заголовки в cpp/onnxruntime/include
set(ORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime)
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libonnxruntime.so"
)

# OBJECT library: compile .c once, link into SHARED
add_library(utun_core OBJECT ${UTUN_SOURCES})
target_link_libraries(utun_core PRIVATE utun_sodium)
# Покадровый YUV-цикл должен укладываться в 30fps и в debug APK; остальные исходники сохраняют режим сборки.
set_source_files_properties("${LIB_DIR}/video_plane.c" PROPERTIES COMPILE_OPTIONS "-O2")
target_include_directories(utun_core PRIVATE
    ${UTUN_INCLUDE_DIRS} ${JNI_DIR} ${OPENSSL_DIR}/include ${ORT_DIR}/include
)
target_compile_definitions(utun_core PRIVATE ${UTUN_DEFINES} __ANDROID__ HAVE_SILERO_VAD UTUN_WITH_VOICE)
set_target_properties(utun_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# JNI bridge + link into shared library
add_library(utun SHARED
    ${JNI_DIR}/android_jni_bridge.c
    ${JNI_DIR}/android_ble.c
    ${JNI_DIR}/../../cgathui_common/ble_etcp.c
    ${JNI_DIR}/../../cgathui_common/ble_join.c
    ${LIB_DIR}/ble/ble_transport.c
    ${LIB_DIR}/ble/ble_identity.c
    ${JNI_DIR}/android_stream_jni.c
    ${JNI_DIR}/android_call_video_jni.c
    ${JNI_DIR}/android_call_output_jni.c
    ${JNI_DIR}/android_udp_log.c
    $<TARGET_OBJECTS:utun_core>
)

target_include_directories(utun PRIVATE
    ${UTUN_INCLUDE_DIRS} ${JNI_DIR} ${OPENSSL_DIR}/include ${ORT_DIR}/include
)
target_compile_definitions(utun PRIVATE ${UTUN_DEFINES} __ANDROID__)
set_target_properties(utun PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

find_library(log-lib log)
target_link_libraries(utun ${log-lib} openssl_crypto utun_sodium utun_voice onnxruntime aaudio dl)
