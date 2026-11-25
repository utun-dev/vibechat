# utun_sources.cmake — shared library description for uTun C core
#
# Include this file and call utun_setup_sources() from any CMakeLists.txt.
# The function takes LIB_DIR and SRC_DIR (paths differ per caller) and
# exports UTUN_SOURCES, UTUN_INCLUDE_DIRS, UTUN_DEFINES to parent scope.
#
# Usage:
#   set(LIB_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib)
#   set(SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
#   set(CFG_DIR  ${CMAKE_CURRENT_SOURCE_DIR})
#   set(TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
#   include(${CFG_DIR}/utun_sources.cmake)
#   utun_setup_sources("${LIB_DIR}" "${SRC_DIR}" "${CFG_DIR}" "${TOOLS_DIR}")
#   add_library(utun STATIC ${UTUN_SOURCES})
#   target_include_directories(utun PRIVATE ${UTUN_INCLUDE_DIRS})
#   target_compile_definitions(utun PRIVATE ${UTUN_DEFINES})

function(utun_setup_sources LIB_DIR SRC_DIR CONFIG_DIR TOOLS_DIR)

    # ── lib/ sources ──
    file(GLOB _lib_src CONFIGURE_DEPENDS "${LIB_DIR}/*.c")

    # ── src/ sources (all except utun.c, lwip_orig, uip, BBR) ──
    file(GLOB_RECURSE _src_src CONFIGURE_DEPENDS "${SRC_DIR}/*.c")
    list(FILTER _src_src EXCLUDE REGEX "/lwip_orig/")
    list(FILTER _src_src EXCLUDE REGEX "/uip/")
    list(FILTER _src_src EXCLUDE REGEX "/transport_layer/BBR/")
    # голосовой стек (call_audio/call_tones/voice_jitter+SoundTouch) собирается отдельной
    # C++ библиотекой utun_voice (см. src/call/voice_sources.cmake)
    list(FILTER _src_src EXCLUDE REGEX "/call/call_audio\\.c$")
    list(FILTER _src_src EXCLUDE REGEX "/call/call_tones\\.c$")
    list(FILTER _src_src EXCLUDE REGEX "/radio/radio_audio\\.c$")
    list(REMOVE_ITEM _src_src "${SRC_DIR}/utun.c")

    # ── Our added files ──
    set(_cfg_src
        "${CONFIG_DIR}/utun_config_api.c"
        "${CONFIG_DIR}/instance_lite.c"
        "${CONFIG_DIR}/voice_recorder.c"
        "${CONFIG_DIR}/attachment_sender.c"
        "${CONFIG_DIR}/video_sender.c"
        "${CONFIG_DIR}/photo_sender.c"
        "${CONFIG_DIR}/standby.c"
        "${CONFIG_DIR}/stream_bridge.c"
    )

    # ── Export sources ──
    set(UTUN_SOURCES
        ${_lib_src} ${_src_src} ${_cfg_src}
        PARENT_SCOPE
    )

    # ── Export include dirs ──
    set(UTUN_INCLUDE_DIRS
        ${LIB_DIR}
        ${LIB_DIR}/libopus/include
        ${SRC_DIR}
        ${SRC_DIR}/transport_layer
        ${SRC_DIR}/routing_layer
        ${SRC_DIR}/chat
        ${SRC_DIR}/proxy
        ${SRC_DIR}/lwip_tcp
        ${SRC_DIR}/BBR
        ${SRC_DIR}/media_delivery
        ${SRC_DIR}/media_async
        ${TOOLS_DIR}
        ${CONFIG_DIR}
        PARENT_SCOPE
    )

    # ── Export compile definitions ──
    set(UTUN_DEFINES
        USE_OPENSSL USE_SQLITE _ISOC99_SOURCE _DEFAULT_SOURCE DEBUG_OUTPUT_STDERR
        UTUN_HAVE_STANDBY
        PARENT_SCOPE
    )
endfunction()
