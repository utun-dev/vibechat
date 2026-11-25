# voice_sources.cmake — голосовой стек звонка (единый для desktop/Android/headless).
#
# SoundTouch (time-stretch) + call_jitter + radio_jitter (кодированные кадры + decode-on-pull)
# + call_audio (Opus/тоны) + call_tones. C++ внутри, наружу — C-интерфейс
# (call_audio.h). Экспортирует UTUN_VOICE_SOURCES и UTUN_VOICE_INCLUDE_DIRS.
#
# Использование:
#   set(LIB_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib)
#   set(CALL_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/call)
#   include(${CALL_DIR}/voice_sources.cmake)
#   utun_setup_voice("${LIB_DIR}" "${CALL_DIR}")
#   add_library(utun_voice STATIC ${UTUN_VOICE_SOURCES})
#   target_include_directories(utun_voice PUBLIC ${UTUN_VOICE_INCLUDE_DIRS})
#   target_link_libraries(utun_voice PUBLIC opus)   # opus_codec

function(utun_setup_voice LIB_DIR CALL_DIR)
    set(_st_dir "${LIB_DIR}/soundtouch-master")
    set(_src_dir "${CALL_DIR}/..")
    set(UTUN_VOICE_SOURCES
        ${CALL_DIR}/call_audio.c
        ${CALL_DIR}/call_tones.c
        ${CALL_DIR}/call_jitter.cpp
        ${CALL_DIR}/call_delay.cpp
        ${CALL_DIR}/call_timeline.cpp
        ${_src_dir}/radio/radio_jitter.cpp
        ${_src_dir}/radio/radio_audio.c
        ${_src_dir}/stream/stream_playback_audio.cpp
        ${_st_dir}/source/SoundTouch/SoundTouch.cpp
        ${_st_dir}/source/SoundTouch/TDStretch.cpp
        ${_st_dir}/source/SoundTouch/RateTransposer.cpp
        ${_st_dir}/source/SoundTouch/FIFOSampleBuffer.cpp
        ${_st_dir}/source/SoundTouch/AAFilter.cpp
        ${_st_dir}/source/SoundTouch/FIRFilter.cpp
        ${_st_dir}/source/SoundTouch/InterpolateLinear.cpp
        ${_st_dir}/source/SoundTouch/InterpolateCubic.cpp
        ${_st_dir}/source/SoundTouch/InterpolateShannon.cpp
        ${_st_dir}/source/SoundTouch/cpu_detect_x86.cpp
        ${_st_dir}/source/SoundTouch/mmx_optimized.cpp
        ${_st_dir}/source/SoundTouch/sse_optimized.cpp
        PARENT_SCOPE
    )
    set(UTUN_VOICE_INCLUDE_DIRS
        ${CALL_DIR}
        ${_src_dir}
        ${_src_dir}/transport_layer
        ${_src_dir}/routing_layer
        ${_src_dir}/chat
        ${_src_dir}/proxy
        ${_src_dir}/media_delivery
        ${_src_dir}/media_async
        ${LIB_DIR}
        ${LIB_DIR}/libopus/include
        ${_st_dir}/include
        ${_st_dir}/include/soundtouch
        PARENT_SCOPE
    )
endfunction()
