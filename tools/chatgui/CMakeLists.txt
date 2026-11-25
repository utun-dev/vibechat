cmake_minimum_required(VERSION 3.16)
project(vibechat VERSION 0.1.0 LANGUAGES C CXX)
option(CHATGUI_BUILD_DEB "Prepare the installed Linux application and its DEB package" OFF)
option(CHATGUI_BUILD_USER_ARCHIVE "Prepare the Ubuntu user archive with system libraries" OFF)
if(CHATGUI_BUILD_USER_ARCHIVE)
    if(CHATGUI_BUILD_DEB OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "User archive requires native Linux and cannot be combined with DEB")
    endif()
    # Отдельная сборка не должна подхватить SDK или библиотеки из кеша разработчика.
    set(CMAKE_IGNORE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/rlottie_build;${CMAKE_SOURCE_DIR}/ffmpeg/ffmpeg_build/install")
    set(CMAKE_SKIP_RPATH TRUE)
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chatgui_version.txt" "${PROJECT_VERSION}\n")

# Проверка Windows-упаковки с подставленными инструментами выполняется на Unix.
if(UNIX)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_windows_installer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_windows_installer.py")
        add_test(NAME test_qt_multimedia_build COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qt_multimedia_build.py")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME test_linux_installer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_linux_installer.py")
            set_tests_properties(test_linux_installer PROPERTIES TIMEOUT 180)
            add_test(NAME test_user_installer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_user_installer.py")
            set_tests_properties(test_user_installer PROPERTIES TIMEOUT 60)
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_C_STANDARD 99)

find_package(OpenSSL REQUIRED)

find_package(Qt6 COMPONENTS Widgets Network Test QUIET)
if(NOT Qt6_FOUND)
    find_package(Qt5 COMPONENTS Widgets Network Test REQUIRED)
    set(QT_LIBS Qt5::Widgets Qt5::Network)
    set(QT_CORE  Qt5::Core)
    set(QT_TEST_LIB Qt5::Test)
    set(QT_PACKAGE Qt5)
else()
    set(QT_LIBS Qt6::Widgets Qt6::Network)
    set(QT_CORE  Qt6::Core)
    set(QT_TEST_LIB Qt6::Test)
    set(QT_PACKAGE Qt6)
endif()

if(QT_PACKAGE STREQUAL "Qt6")
    find_package(KF6SyntaxHighlighting REQUIRED)
    set(CHAT_SYNTAX_LIBRARY KF6::SyntaxHighlighting)
else()
    find_package(KF5SyntaxHighlighting REQUIRED)
    set(CHAT_SYNTAX_LIBRARY KF5::SyntaxHighlighting)
endif()

add_library(chatgui_message_text STATIC src/message_text.cpp src/message_links.cpp src/code_language.cpp)
target_link_libraries(chatgui_message_text PUBLIC ${QT_LIBS} PRIVATE ${CHAT_SYNTAX_LIBRARY} utun)

# Каталоги в ресурсах: перевод работает и в переносимой сборке без файлов рядом с exe.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chatgui_qt_package.txt" "${QT_PACKAGE}\n")
get_target_property(QT_QMAKE ${QT_PACKAGE}::qmake IMPORTED_LOCATION)
execute_process(COMMAND "${QT_QMAKE}" -query QT_HOST_BINS
    OUTPUT_VARIABLE QT_BINARY_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE QT_BINARY_RESULT)
if(NOT QT_BINARY_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot query Qt host tools directory")
endif()
find_program(QT_GUI_LRELEASE NAMES lrelease HINTS "${QT_BINARY_DIR}" NO_DEFAULT_PATH)
if(NOT QT_GUI_LRELEASE)
    message(FATAL_ERROR "Qt lrelease is required to build GUI translations (install Qt Linguist tools)")
endif()
execute_process(COMMAND "${QT_QMAKE}" -query QT_INSTALL_TRANSLATIONS
    OUTPUT_VARIABLE QT_TRANSLATIONS_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE QT_TRANSLATIONS_RESULT)
if(NOT QT_TRANSLATIONS_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot query Qt translations directory")
endif()
set(QT_RUSSIAN_CATALOG "${QT_TRANSLATIONS_DIR}/qtbase_ru.qm")
if(NOT EXISTS "${QT_RUSSIAN_CATALOG}")
    message(FATAL_ERROR "Qt Russian translations missing: ${QT_RUSSIAN_CATALOG}")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/translations")
set(GUI_QM_FILES)
foreach(LANGUAGE en ru)
    set(QM_FILE "${CMAKE_CURRENT_BINARY_DIR}/translations/chatgui_${LANGUAGE}.qm")
    add_custom_command(OUTPUT "${QM_FILE}"
        COMMAND "${QT_GUI_LRELEASE}" -nounfinished "${CMAKE_CURRENT_SOURCE_DIR}/translations/chatgui_${LANGUAGE}.ts" -qm "${QM_FILE}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/translations/chatgui_${LANGUAGE}.ts"
        VERBATIM)
    list(APPEND GUI_QM_FILES "${QM_FILE}")
endforeach()
add_custom_target(chatgui_translations DEPENDS ${GUI_QM_FILES})
configure_file(translations/translations.qrc.in "${CMAKE_CURRENT_BINARY_DIR}/translations.qrc" @ONLY)

if(CHATGUI_BUILD_USER_ARCHIVE)
    find_package(rlottie REQUIRED PATHS /usr NO_DEFAULT_PATH)
else()
    find_package(rlottie REQUIRED HINTS "${CMAKE_CURRENT_SOURCE_DIR}/rlottie_build")
endif()
message(STATUS "rlottie package: ${rlottie_DIR}")
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

set(ZXING_READERS ON CACHE BOOL "")
set(ZXING_WRITERS "OLD" CACHE STRING "")
set(ZXING_ENABLE_QRCODE ON CACHE BOOL "")
set(ZXING_ENABLE_AZTEC OFF CACHE BOOL "")
set(ZXING_ENABLE_DATAMATRIX OFF CACHE BOOL "")
set(ZXING_ENABLE_MAXICODE OFF CACHE BOOL "")
set(ZXING_ENABLE_PDF417 OFF CACHE BOOL "")
set(ZXING_ENABLE_1D OFF CACHE BOOL "")
set(ZXING_C_API OFF CACHE BOOL "")
set(ZXING_EXAMPLES OFF CACHE BOOL "")
set(ZXING_EXAMPLES_QT OFF CACHE BOOL "")
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "")
set(ZXING_UNIT_TESTS OFF CACHE BOOL "")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
add_subdirectory(zxing-cpp)
set_target_properties(ZXing PROPERTIES AUTOMOC OFF)

# FFmpeg - custom built shared libraries
set(FFMPEG_INSTALL_DIR "${CMAKE_SOURCE_DIR}/ffmpeg/ffmpeg_build/install")
if(CHATGUI_BUILD_USER_ARCHIVE)
    set(ENV{PKG_CONFIG_PATH} "")
else()
    set(ENV{PKG_CONFIG_PATH} "${FFMPEG_INSTALL_DIR}/lib/pkgconfig")
endif()
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG_PC libavcodec libavformat libavutil libswscale libswresample IMPORTED_TARGET)
    if(FFMPEG_PC_FOUND)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_TARGET PkgConfig::FFMPEG_PC)
        set(FFMPEG_INCLUDE_DIRS "${FFMPEG_PC_INCLUDE_DIRS}")
    endif()
endif()
if(NOT FFMPEG_FOUND AND NOT CHATGUI_BUILD_USER_ARCHIVE)
    message(STATUS "pkg-config search failed, trying direct library search")
    find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_INSTALL_DIR}/include" NO_DEFAULT_PATH)
    find_library(AVCODEC_LIBRARY avcodec PATHS "${FFMPEG_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
    find_library(AVFORMAT_LIBRARY avformat PATHS "${FFMPEG_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
    find_library(AVUTIL_LIBRARY avutil PATHS "${FFMPEG_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
    find_library(SWSCALE_LIBRARY swscale PATHS "${FFMPEG_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIBRARY swresample PATHS "${FFMPEG_INSTALL_DIR}/lib" NO_DEFAULT_PATH)
    if(FFMPEG_INCLUDE_DIR AND AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY)
        set(FFMPEG_FOUND TRUE)
        set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
        set(FFMPEG_LIBRARIES "${AVCODEC_LIBRARY}" "${AVFORMAT_LIBRARY}" "${AVUTIL_LIBRARY}" "${SWSCALE_LIBRARY}" "${SWRESAMPLE_LIBRARY}")
        message(STATUS "FFmpeg found via direct search")
    endif()
endif()
if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found at ${FFMPEG_INSTALL_DIR}. Video transcoding disabled. Run ./ffmpeg/build_ffmpeg.sh to enable.")
    set(FFMPEG_TARGET "")
    set(FFMPEG_LIBRARIES "")
    set(FFMPEG_INCLUDE_DIRS "")
endif()
if(UNIX AND NOT APPLE AND FFMPEG_FOUND)
    set(CMAKE_BUILD_RPATH "${FFMPEG_INSTALL_DIR}/lib")
endif()

# uTun core library (C, built from ../src/ and ../lib/)
add_subdirectory(libutun)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../lib/ble" "${CMAKE_CURRENT_BINARY_DIR}/ble")
add_library(chat_ble_etcp STATIC ../cgathui_common/ble_etcp.c ../cgathui_common/ble_join.c)
target_link_libraries(chat_ble_etcp PUBLIC utun vibechat_ble_core)
set_target_properties(chat_ble_etcp PROPERTIES AUTOMOC OFF)
add_executable(test_ble_etcp ../cgathui_common/tests/test_ble_etcp.c)
target_link_libraries(test_ble_etcp PRIVATE chat_ble_etcp)
set_target_properties(test_ble_etcp PROPERTIES AUTOMOC OFF)
add_test(NAME test_ble_etcp COMMAND test_ble_etcp)
add_executable(test_ble_join ../cgathui_common/tests/test_ble_join.c)
target_link_libraries(test_ble_join PRIVATE utun)
set_target_properties(test_ble_join PROPERTIES AUTOMOC OFF)
add_test(NAME test_ble_join COMMAND test_ble_join)
add_executable(ble_etcp_probe ../cgathui_common/tests/ble_etcp_probe.c)
target_link_libraries(ble_etcp_probe PRIVATE chat_ble_etcp vibechat_ble)
set_target_properties(ble_etcp_probe PROPERTIES AUTOMOC OFF)
add_executable(ble_invite_probe ../cgathui_common/tests/ble_invite_probe.c)
target_link_libraries(ble_invite_probe PRIVATE chat_ble_etcp vibechat_ble)
set_target_properties(ble_invite_probe PROPERTIES AUTOMOC OFF)
add_library(chatgui_ble STATIC transport/desktop_ble.cpp)
target_link_libraries(chatgui_ble PUBLIC ${QT_CORE} chat_ble_etcp vibechat_ble)
add_executable(test_desktop_ble tests/test_desktop_ble.cpp transport/desktop_ble.cpp)
target_link_libraries(test_desktop_ble PRIVATE ${QT_CORE} ${QT_TEST_LIB} chat_ble_etcp)
add_test(NAME test_desktop_ble COMMAND test_desktop_ble)
add_executable(ble_desktop_probe tests/ble_desktop_probe.cpp)
target_link_libraries(ble_desktop_probe PRIVATE chatgui_ble)

# Ручная проверка codec backend перед включением в capabilities звонка; не часть ctest.
if(PkgConfig_FOUND)
    pkg_check_modules(CALL_X265 QUIET x265 IMPORTED_TARGET)
    if(CALL_X265_FOUND)
        add_executable(probe_call_x265 EXCLUDE_FROM_ALL tests/probe_call_x265.cpp)
        target_link_libraries(probe_call_x265 PRIVATE utun PkgConfig::CALL_X265)
    endif()
endif()

# Общий sender остаётся в libutun; Qt захватывает видео, FFmpeg/Opus работают в publisher workers.
add_library(chatgui_preview_cache STATIC src/media_preview_cache.cpp)
target_link_libraries(chatgui_preview_cache PUBLIC ${QT_LIBS} utun PRIVATE ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
target_include_directories(chatgui_preview_cache PRIVATE ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_FOUND)
    target_compile_definitions(chatgui_preview_cache PRIVATE HAVE_FFMPEG)
endif()
add_executable(test_media_preview_cache tests/test_media_preview_cache.cpp)
target_link_libraries(test_media_preview_cache PRIVATE chatgui_preview_cache ${QT_TEST_LIB})
target_compile_definitions(test_media_preview_cache PRIVATE TEST_PREVIEW_FFMPEG=$<BOOL:${FFMPEG_FOUND}>)
add_test(NAME test_media_preview_cache COMMAND test_media_preview_cache)
set_tests_properties(test_media_preview_cache PROPERTIES TIMEOUT 30)

add_library(chatgui_live_video STATIC src/live_video_encoder.cpp src/call_video_encoder.cpp src/call_video_decoder.cpp)
target_link_libraries(chatgui_live_video PUBLIC ${QT_LIBS} utun PRIVATE ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
target_include_directories(chatgui_live_video PRIVATE ${FFMPEG_INCLUDE_DIRS})
if(FFMPEG_FOUND)
    target_compile_definitions(chatgui_live_video PRIVATE HAVE_FFMPEG)
    add_executable(test_video_diagnostics tests/test_video_diagnostics.cpp src/gui_log.cpp)
    target_include_directories(test_video_diagnostics PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(test_video_diagnostics PRIVATE chatgui_live_video ${QT_TEST_LIB} ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
    add_test(NAME test_video_diagnostics COMMAND test_video_diagnostics)
    add_executable(test_video_frame_image tests/test_video_frame_image.cpp)
    target_compile_definitions(test_video_frame_image PRIVATE HAVE_FFMPEG)
    target_include_directories(test_video_frame_image PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(test_video_frame_image PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
    add_test(NAME test_video_frame_image COMMAND test_video_frame_image)
    set_tests_properties(test_video_frame_image PROPERTIES TIMEOUT 30)
    add_executable(test_call_video_encoder tests/test_call_video_encoder.cpp)
    target_include_directories(test_call_video_encoder PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(test_call_video_encoder PRIVATE chatgui_live_video ${QT_TEST_LIB} ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
    add_test(NAME test_call_video_encoder COMMAND test_call_video_encoder)
    set_tests_properties(test_call_video_encoder PROPERTIES TIMEOUT 120)
    if(UNIX AND NOT APPLE)
        add_executable(test_call_video_gpu tests/test_call_video_gpu.cpp)
        target_include_directories(test_call_video_gpu PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(test_call_video_gpu PRIVATE chatgui_live_video ${QT_TEST_LIB} ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
        foreach(symbol avcodec_find_encoder_by_name avcodec_open2 avcodec_send_frame avcodec_receive_packet avcodec_free_context
                       avcodec_send_packet avcodec_receive_frame)
            target_link_options(test_call_video_gpu PRIVATE "-Wl,--wrap=${symbol}")
        endforeach()
        add_test(NAME test_call_video_gpu COMMAND test_call_video_gpu)
        set_tests_properties(test_call_video_gpu PROPERTIES TIMEOUT 60)
        add_executable(test_call_video_clock tests/test_call_video_clock.cpp src/call_video_engine.cpp)
        target_link_libraries(test_call_video_clock PRIVATE chatgui_live_video)
        foreach(symbol call_audio_try_clock call_video_port_received call_video_port_open call_video_port_close call_video_port_status
                       call_video_port_pull call_video_port_request_key call_video_port_decoded avcodec_receive_frame avcodec_open2)
            target_link_options(test_call_video_clock PRIVATE "-Wl,--wrap=${symbol}")
        endforeach()
        add_test(NAME test_call_video_clock COMMAND test_call_video_clock)
        set_tests_properties(test_call_video_clock PROPERTIES TIMEOUT 30)
    endif()
endif()
add_library(chatgui_capture STATIC src/video_capture.cpp src/video_capture_worker.cpp src/video_capture_source.h)
target_link_libraries(chatgui_capture PUBLIC ${QT_LIBS} utun)
add_library(chatgui_audio_capture STATIC src/audio_capture.cpp)
target_link_libraries(chatgui_audio_capture PUBLIC ${QT_LIBS} utun)
if(WIN32)
    target_link_libraries(chatgui_audio_capture PRIVATE ole32)
endif()
add_library(chatgui_stream_publish STATIC src/video_recording_engine.cpp src/video_recording_file.cpp src/stream_publish_engine.cpp src/stream_publish_window.cpp src/call_video_engine.cpp)
target_link_libraries(chatgui_stream_publish PUBLIC chatgui_live_video chatgui_capture chatgui_audio_capture)
target_include_directories(chatgui_stream_publish PRIVATE ${CMAKE_SOURCE_DIR}/../../lib/libopus/include)
target_include_directories(chatgui_stream_publish PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(chatgui_stream_publish PRIVATE ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
if(FFMPEG_FOUND)
    target_compile_definitions(chatgui_stream_publish PRIVATE HAVE_FFMPEG)
endif()
set(GUI_CAPTURE_ENABLED 0)
if(QT_PACKAGE STREQUAL "Qt6" AND FFMPEG_FOUND)
    set(GUI_BASE_QT6_FOUND ${Qt6_FOUND})
    find_package(Qt6 6.6 COMPONENTS Multimedia MultimediaWidgets QUIET)
    set(Qt6_FOUND ${GUI_BASE_QT6_FOUND})
    if(TARGET Qt6::Multimedia AND TARGET Qt6::MultimediaWidgets)
        target_compile_definitions(chatgui_stream_publish PUBLIC HAVE_STREAM_CAPTURE)
        target_compile_definitions(chatgui_capture PUBLIC HAVE_STREAM_CAPTURE)
        target_link_libraries(chatgui_capture PUBLIC Qt6::Multimedia)
        target_link_libraries(chatgui_stream_publish PUBLIC Qt6::Multimedia Qt6::MultimediaWidgets)
        add_executable(test_video_capture tests/test_video_capture.cpp)
        add_dependencies(test_video_capture chatgui_translations)
        target_link_libraries(test_video_capture PRIVATE chatgui_capture ${QT_TEST_LIB})
        if(UNIX AND NOT APPLE)
            target_sources(test_video_capture PRIVATE src/call_video_engine.cpp)
            target_compile_definitions(test_video_capture PRIVATE TEST_CALL_VIDEO_ENGINE)
            target_link_libraries(test_video_capture PRIVATE chatgui_live_video)
            foreach(symbol call_audio_get_clock call_audio_try_clock call_video_port_open call_video_port_close call_video_port_status
                           call_video_port_pull call_video_port_request_key call_video_port_decoded)
                target_link_options(test_video_capture PRIVATE "-Wl,--wrap=${symbol}")
            endforeach()
        endif()
        add_test(NAME test_video_capture COMMAND test_video_capture)
        set_tests_properties(test_video_capture PROPERTIES TIMEOUT 40 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        set(GUI_CAPTURE_ENABLED 1)
    else()
        message(STATUS "Qt Multimedia 6.6+ missing: stream publication unavailable")
    endif()
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/chatgui_capture_enabled.txt" "${GUI_CAPTURE_ENABLED}\n")
if(GUI_CAPTURE_ENABLED AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 COMPONENTS DBus REQUIRED)
    pkg_check_modules(STREAM_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=0.3.44)
    add_library(chatgui_portal_capture STATIC src/stream_portal_capture.cpp)
    target_link_libraries(chatgui_portal_capture PUBLIC Qt6::Multimedia Qt6::DBus PRIVATE utun PkgConfig::STREAM_PIPEWIRE)
    target_compile_definitions(chatgui_capture PUBLIC UTUN_STREAM_PORTAL)
    target_link_libraries(chatgui_capture PRIVATE chatgui_portal_capture)
    add_executable(test_stream_portal tests/test_stream_portal.cpp)
    target_link_libraries(test_stream_portal PRIVATE chatgui_portal_capture Qt6::Network ${QT_TEST_LIB} utun)
    find_program(STREAM_DBUS_RUN_SESSION dbus-run-session REQUIRED)
    add_test(NAME test_stream_portal COMMAND ${STREAM_DBUS_RUN_SESSION} -- $<TARGET_FILE:test_stream_portal>)
    set_tests_properties(test_stream_portal PROPERTIES TIMEOUT 45)
endif()
if(GUI_CAPTURE_ENABLED AND UNIX AND NOT APPLE AND QT_FEATURE_xcb)
    find_package(X11 REQUIRED COMPONENTS Xfixes)
    target_sources(chatgui_capture PRIVATE src/stream_screen_cursor.cpp)
    target_compile_definitions(chatgui_capture PUBLIC UTUN_STREAM_X11_CURSOR)
    target_link_libraries(chatgui_capture PRIVATE X11::X11 X11::Xfixes)
endif()
if(GUI_CAPTURE_ENABLED AND WIN32)
    if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
        target_sources(chatgui_capture PRIVATE src/stream_screen_cursor_win.cpp)
        target_compile_definitions(chatgui_capture PUBLIC UTUN_STREAM_WIN_CURSOR)
        target_link_libraries(chatgui_capture PRIVATE user32 gdi32)
    else()
        message(STATUS "Windows cursor overlay requires Qt 6.7+ native monitor handles")
    endif()
endif()

add_library(chatgui_notifications STATIC src/message_notification.cpp src/user_activity_monitor.cpp)
target_link_libraries(chatgui_notifications PUBLIC ${QT_PACKAGE}::Widgets PRIVATE utun)
if(WIN32)
    target_link_libraries(chatgui_notifications PRIVATE user32)
elseif(APPLE)
    target_link_libraries(chatgui_notifications PRIVATE "-framework ApplicationServices")
endif()

# Голосовой стек звонка (SoundTouch + voice_jitter + call_audio + call_tones),
# C++ внутри, C-интерфейс наружу (call_audio.h).
set(LIB_DIR  "${CMAKE_SOURCE_DIR}/../../lib")
set(CALL_DIR "${CMAKE_SOURCE_DIR}/../../src/call")
include(${CALL_DIR}/voice_sources.cmake)
utun_setup_voice("${LIB_DIR}" "${CALL_DIR}")
add_library(utun_voice STATIC ${UTUN_VOICE_SOURCES})
target_include_directories(utun_voice PUBLIC ${UTUN_VOICE_INCLUDE_DIRS})
set_source_files_properties(${CALL_DIR}/call_audio.c ${CALL_DIR}/call_tones.c PROPERTIES LANGUAGE C)
set_source_files_properties(${CMAKE_SOURCE_DIR}/../../src/radio/radio_audio.c PROPERTIES LANGUAGE C)
set_target_properties(utun_voice PROPERTIES POSITION_INDEPENDENT_CODE ON)


add_library(chatgui_clipboard STATIC src/clipboard_exchange.cpp src/clipboard_files.cpp)
target_link_libraries(chatgui_clipboard PUBLIC ${QT_LIBS} PRIVATE utun)
if(WIN32)
    target_sources(chatgui_clipboard PRIVATE src/clipboard_native_windows.cpp)
    target_link_libraries(chatgui_clipboard PRIVATE ole32 shell32 uuid)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CLIPBOARD_FUSE REQUIRED IMPORTED_TARGET fuse3)
    target_sources(chatgui_clipboard PRIVATE src/clipboard_native_linux.cpp)
    target_link_libraries(chatgui_clipboard PRIVATE PkgConfig::CLIPBOARD_FUSE)
else()
    target_sources(chatgui_clipboard PRIVATE src/clipboard_native_unsupported.cpp)
endif()

add_executable(vibechat WIN32
    src/main.cpp
    src/startupfailure_dialog.cpp
    src/startupfailure_dialog.h
    src/desktop_integration.cpp
    src/gui_log.cpp
    src/single_instance.cpp
    src/mainwindow.cpp
    src/video_recording_window.cpp
    src/kanbanview.cpp
    src/kanbanview.h
    src/kanban_paint.h
    src/kanban_inline_editor.h
    src/kanban_card_list.h
    src/channellist.cpp
    src/creategroupdialog.cpp
    src/channeldelegate.cpp
    src/channelsettingsdialog.cpp
    src/grouppolicydialog.cpp
    src/chatview.cpp
    src/messagedelegate.cpp
    src/messagelist.cpp
    src/group_attachment_roles.cpp
    src/media_preview_loader.h
    src/attachment_key_ui.cpp
    src/messagefeedback.cpp
    src/accountlist.cpp
    src/devgroupdialog.cpp
    src/accountdelegate.cpp
    src/flagpainter.cpp
    src/memberpropsdialog.cpp
    src/renamedialog.cpp
    src/memberlistmodel.cpp
    src/emoji.cpp
    src/emojipanel.cpp
    src/emojitabbar.cpp
    src/formattingpanel.cpp
    src/inputbar.cpp
    src/lottieicon.cpp
    src/animtimer.cpp
    src/qrcode_utils.cpp
    src/invite_link.cpp
    src/invitedialog.cpp
    src/dminvitedialog.cpp
    src/inviteby.cpp
    src/joindialog.cpp
    src/settingsdialog.cpp
    src/networksettingspage.cpp
    src/bluetoothsettingspage.cpp
    src/databasesettingspage.cpp
    src/statuspage.cpp
    src/nodespage.cpp
    src/soundsettingspage.cpp
    src/sound_manager.cpp
    src/voice_audio_io.cpp
    src/audio_device.cpp
    src/audiorecorder.cpp
    src/voicemessageencoder.cpp
    src/voiceplayback.cpp
    src/videoplayer_engine.cpp
    src/videoplayer_window.cpp
    src/streamplayer_window.cpp
    src/streamgrid_window.cpp
    src/imageviewer_window.cpp
    src/member_files/browser.cpp
    src/member_files/batch.cpp
    src/avatars/avatarcache.cpp
    src/avatars/avatarcrop.cpp
    src/avatars/avatargallery.cpp
    src/avatars/avatarmanager.cpp
    src/avatars/groupavatarmanager.cpp
    src/audiodevicesettingspage.cpp
    src/storagesettingspage.cpp
    src/connmonitorwindow.cpp
    src/networktrafficwindow.cpp
    src/callwindow.cpp
    src/call_audio_graphs.cpp
    src/call_audio_engine.cpp
    src/radio_audio_engine.cpp
    src/radiopanel.cpp
    src/ptt_hotkey_manager.cpp
    src/radiosettingspage.cpp
    transport/utun_node.cpp
    transport/node_config.cpp
    transport/config_updater.cpp
    transport/gui_bridge_impl.cpp
    transport/stream_bridge.cpp
    transport/miniaudio_impl.c
    transport/audio_diagnostics.cpp
    db/db_manager.cpp
    ../../lib/sqlite3.c
    resources/chatgui.qrc
    "${CMAKE_CURRENT_BINARY_DIR}/translations.qrc"
)
add_dependencies(vibechat chatgui_translations)

# Linux-упаковка получает PNG из единственного WebP-исходника; готовые копии остаются в каталоге сборки.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (CHATGUI_BUILD_DEB OR CHATGUI_BUILD_USER_ARCHIVE))
    add_executable(chatgui_icon_to_png EXCLUDE_FROM_ALL installer/icon_to_png.cpp)
    set_target_properties(chatgui_icon_to_png PROPERTIES AUTOMOC OFF AUTORCC OFF)
    target_link_libraries(chatgui_icon_to_png PRIVATE ${QT_PACKAGE}::Gui)
    set(CHATGUI_ICON_PNG "${CMAKE_CURRENT_BINARY_DIR}/app_icon.png")
    add_custom_command(OUTPUT "${CHATGUI_ICON_PNG}"
        COMMAND chatgui_icon_to_png "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.webp" "${CHATGUI_ICON_PNG}"
        DEPENDS chatgui_icon_to_png "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.webp"
        COMMENT "Preparing launcher PNG from the shared WebP icon" VERBATIM)
    add_custom_target(chatgui_icon_png DEPENDS "${CHATGUI_ICON_PNG}")
    add_dependencies(vibechat chatgui_icon_png)
endif()

# Windows читает значок и UTF-8 manifest из EXE до запуска Qt, в том числе camera worker.
if(WIN32)
    enable_language(RC)
    configure_file(resources/vibechat.rc.in "${CMAKE_CURRENT_BINARY_DIR}/vibechat.rc" @ONLY)
    target_sources(vibechat PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/vibechat.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/vibechat.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.ico;${CMAKE_CURRENT_SOURCE_DIR}/resources/vibechat.manifest")
    message(STATUS "Windows executable icon: resources/icons/app_icon.ico")
    # Wintun загружается через LoadLibrary и не попадает в обход импортов EXE.
    add_custom_command(TARGET vibechat POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintun.dll"
            "$<TARGET_FILE_DIR:vibechat>/wintun.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vibechat>/licenses/wintun"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintun-LICENSE.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/wintun-api-LICENSE.txt"
            "$<TARGET_FILE_DIR:vibechat>/licenses/wintun"
        COMMENT "Deploying Wintun driver DLL and licenses" VERBATIM)
endif()

target_include_directories(vibechat PRIVATE ${CMAKE_SOURCE_DIR}/../../lib ${CMAKE_SOURCE_DIR}/../../src ${CMAKE_SOURCE_DIR}/db ${CMAKE_SOURCE_DIR}/../../lib/libopus/include ${FFMPEG_INCLUDE_DIRS})
target_compile_definitions(vibechat PRIVATE SQLITE_THREADSAFE=1 USE_SQLITE VIBECHAT_VERSION="${PROJECT_VERSION}")

# Лицензии лежат рядом с бинарником при сборке и устанавливаются с приложением.
set(UTUN_LICENSE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSING"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../THIRD_PARTY_NOTICES")
add_custom_target(vibechat_licenses ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:vibechat>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${UTUN_LICENSE_FILES} "$<TARGET_FILE_DIR:vibechat>/licenses"
    COMMENT "Copying uTun license documents")
install(FILES ${UTUN_LICENSE_FILES} DESTINATION share/licenses/utun)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(vibechat PRIVATE src/profile_paths.cpp)
    add_executable(test_profile_paths tests/test_profile_paths.cpp src/profile_paths.cpp)
    target_link_libraries(test_profile_paths PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun)
    add_test(NAME test_profile_paths COMMAND test_profile_paths)
    add_executable(test_desktop_integration tests/test_desktop_integration.cpp src/desktop_integration.cpp)
    target_link_libraries(test_desktop_integration PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun)
    add_test(NAME test_desktop_integration COMMAND test_desktop_integration)
endif()
if(FFMPEG_FOUND)
    target_compile_definitions(vibechat PRIVATE HAVE_FFMPEG)
endif()
set_source_files_properties(../../lib/sqlite3.c transport/miniaudio_impl.c PROPERTIES LANGUAGE C)

# Глобальное состояние клавиатуры для PTT: Windows user32 / Linux X11.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    if(Qt6_FOUND)
        find_package(Qt6 COMPONENTS DBus REQUIRED)
        set(QT_DBUS Qt6::DBus)
    else()
        find_package(Qt5 COMPONENTS DBus REQUIRED)
        set(QT_DBUS Qt5::DBus)
    endif()
    target_sources(vibechat PRIVATE src/ptt_portal.cpp)
    target_compile_definitions(vibechat PRIVATE UTUN_PTT_PORTAL)
    target_link_libraries(vibechat PRIVATE ${QT_DBUS})
    target_include_directories(vibechat PRIVATE ${X11_INCLUDE_DIR})
    target_compile_definitions(chatgui_notifications PRIVATE UTUN_ACTIVITY_DBUS)
    target_link_libraries(chatgui_notifications PRIVATE ${QT_DBUS})
    if(Qt6_FOUND)
        find_package(KF6IdleTime QUIET)
        set(ACTIVITY_KIDLETIME_TARGET KF6::IdleTime)
    else()
        find_package(KF5IdleTime QUIET)
        set(ACTIVITY_KIDLETIME_TARGET KF5::IdleTime)
    endif()
    if(TARGET ${ACTIVITY_KIDLETIME_TARGET})
        target_compile_definitions(chatgui_notifications PRIVATE UTUN_ACTIVITY_KIDLETIME)
        target_link_libraries(chatgui_notifications PRIVATE ${ACTIVITY_KIDLETIME_TARGET})
    else()
        message(STATUS "KIdleTime not found: Wayland activity uses GNOME IdleMonitor or ChatGUI input")
    endif()
elseif(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
endif()

if(TARGET X11::X11)
    target_link_libraries(chatgui_notifications PRIVATE X11::X11)
endif()
if(TARGET X11::Xss)
    target_compile_definitions(chatgui_notifications PRIVATE UTUN_ACTIVITY_X11)
    target_link_libraries(chatgui_notifications PRIVATE X11::Xss)
elseif(UNIX AND NOT APPLE)
    message(STATUS "X11 screen saver library not found: X11 activity uses ChatGUI input")
endif()
target_link_libraries(vibechat PRIVATE chatgui_clipboard chatgui_notifications chatgui_stream_publish chatgui_preview_cache chatgui_ble chatgui_message_text)

if(WIN32)
    target_link_libraries(vibechat PRIVATE ${QT_LIBS} rlottie::rlottie ZLIB::ZLIB OpenSSL::Crypto ZXing::ZXing utun utun_voice pthread user32 ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
else()
    target_link_libraries(vibechat PRIVATE ${QT_LIBS} rlottie::rlottie ZLIB::ZLIB OpenSSL::Crypto ZXing::ZXing utun utun_voice pthread dl ${X11_LIBRARIES} ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
endif()

# ============================================================================
# Tests
# ============================================================================
add_executable(test_member_sync
    ../../lib/sqlite3.c
    ../../tests/test_member_sync.c
)
target_include_directories(test_member_sync PRIVATE
    transport ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/../../lib
    ${CMAKE_SOURCE_DIR}/../../src
    zxing-cpp/core/src ../../tests
)
target_compile_definitions(test_member_sync PRIVATE USE_SQLITE SQLITE_THREADSAFE=1)
set_source_files_properties(../../lib/sqlite3.c ../../tests/test_member_sync.c PROPERTIES LANGUAGE C)
if(WIN32)
    target_link_libraries(test_member_sync PRIVATE OpenSSL::Crypto pthread)
else()
    target_link_libraries(test_member_sync PRIVATE OpenSSL::Crypto pthread dl)
endif()

# ============================================================================
# Tests
# ============================================================================

enable_testing()
add_executable(test_log_rotation ../../tests/test_log_rotation.c)
target_link_libraries(test_log_rotation PRIVATE uasync pthread)
add_test(NAME test_log_rotation COMMAND test_log_rotation)

add_executable(test_packet_link ../../tests/test_packet_link.c)
target_link_libraries(test_packet_link PRIVATE uasync pthread)
add_test(NAME test_packet_link COMMAND test_packet_link)

add_executable(test_etcp_packet_link ../../tests/test_etcp_packet_link.c)
target_link_libraries(test_etcp_packet_link PRIVATE utun uasync pthread)
add_test(NAME test_etcp_packet_link COMMAND test_etcp_packet_link)

add_executable(test_etcp_driver ../../tests/test_etcp_driver.c)
target_link_libraries(test_etcp_driver PRIVATE utun uasync pthread)
add_test(NAME test_etcp_driver COMMAND test_etcp_driver)

add_executable(test_channel_delegate tests/test_channel_delegate.cpp src/channeldelegate.cpp)
target_link_libraries(test_channel_delegate PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun pthread ${CMAKE_DL_LIBS})
add_test(NAME test_channel_delegate COMMAND test_channel_delegate)
set_tests_properties(test_channel_delegate PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

add_executable(test_clipboard_exchange tests/test_clipboard_exchange.cpp)
target_link_libraries(test_clipboard_exchange PRIVATE chatgui_clipboard ${QT_TEST_LIB} utun)
add_test(NAME test_clipboard_exchange COMMAND test_clipboard_exchange)
set_tests_properties(test_clipboard_exchange PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
add_executable(test_clipboard_native tests/test_clipboard_native.cpp)
target_link_libraries(test_clipboard_native PRIVATE chatgui_clipboard)
add_test(NAME test_clipboard_native COMMAND test_clipboard_native)
set_tests_properties(test_clipboard_native PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

add_executable(test_channel_list tests/test_channel_list.cpp src/channellist.cpp src/channeldelegate.cpp
    src/avatars/groupavatarmanager.cpp db/db_manager.cpp transport/gui_bridge_impl.cpp)
target_link_libraries(test_channel_list PRIVATE chatgui_clipboard ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
add_test(NAME test_channel_list COMMAND test_channel_list)
set_tests_properties(test_channel_list PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_channel_settings tests/test_channel_settings.cpp src/channelsettingsdialog.cpp
    src/grouppolicydialog.cpp
    src/avatars/groupavatarmanager.cpp db/db_manager.cpp transport/gui_bridge_impl.cpp)
target_link_libraries(test_channel_settings PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_channel_settings PRIVATE UTUN_TEST_SETTINGS_ALLOC_FAILURES)
    target_link_options(test_channel_settings PRIVATE "-Wl,--wrap=u_malloc_impl" "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_channel_settings COMMAND test_channel_settings)
set_tests_properties(test_channel_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_sound_settings tests/test_sound_settings.cpp src/soundsettingspage.cpp src/sound_manager.cpp
    src/audio_device.cpp src/voice_audio_io.cpp db/db_manager.cpp transport/gui_bridge_impl.cpp
    transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
target_link_libraries(test_sound_settings PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_sound_settings PRIVATE UTUN_TEST_SETTINGS_ALLOC_FAILURES)
    target_link_options(test_sound_settings PRIVATE "-Wl,--wrap=u_malloc_impl" "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_sound_settings COMMAND test_sound_settings)
set_tests_properties(test_sound_settings PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_single_instance tests/test_single_instance.cpp src/single_instance.cpp)
target_link_libraries(test_single_instance PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun pthread ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(test_single_instance PRIVATE user32)
endif()
add_test(NAME test_single_instance COMMAND test_single_instance)
set_tests_properties(test_single_instance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_message_notification tests/test_message_notification.cpp)
target_link_libraries(test_message_notification PRIVATE chatgui_notifications ${QT_TEST_LIB} pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(NOTIFICATION_DBUS_RUN_SESSION NAMES dbus-run-session)
    if(NOT NOTIFICATION_DBUS_RUN_SESSION)
        message(FATAL_ERROR "dbus-run-session is required for the isolated notification test")
    endif()
    target_compile_definitions(test_message_notification PRIVATE UTUN_TEST_ACTIVITY_DBUS)
    target_link_libraries(test_message_notification PRIVATE ${QT_DBUS})
    add_test(NAME test_message_notification COMMAND ${NOTIFICATION_DBUS_RUN_SESSION} -- $<TARGET_FILE:test_message_notification>)
else()
    add_test(NAME test_message_notification COMMAND test_message_notification)
endif()
set_tests_properties(test_message_notification PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(test_message_events tests/test_message_events.cpp transport/gui_bridge_impl.cpp src/connmonitorwindow.cpp)
target_link_libraries(test_message_events PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_message_events PRIVATE UTUN_TEST_BRIDGE_ALLOC_FAILURES)
    target_link_options(test_message_events PRIVATE "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_message_events COMMAND test_message_events)
set_tests_properties(test_message_events PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

add_executable(test_network_traffic tests/test_network_traffic.cpp src/networktrafficwindow.cpp src/connmonitorwindow.cpp
    transport/gui_bridge_impl.cpp)
target_link_libraries(test_network_traffic PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
add_test(NAME test_network_traffic COMMAND test_network_traffic)
set_tests_properties(test_network_traffic PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(test_call_commands tests/test_call_commands.cpp src/callwindow.cpp src/call_audio_graphs.cpp src/call_video_engine.cpp src/call_audio_engine.cpp src/sound_manager.cpp
    src/voice_audio_io.cpp src/audio_device.cpp src/connmonitorwindow.cpp transport/gui_bridge_impl.cpp
    transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
target_link_libraries(test_call_commands PRIVATE ${QT_LIBS} ${QT_TEST_LIB} chatgui_capture chatgui_live_video utun_voice utun pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_call_commands PRIVATE UTUN_TEST_CALL_HOOKS)
    foreach(symbol u_calloc_impl call_start_trampoline call_accept_trampoline call_decline_trampoline call_hangup_trampoline call_continue_trampoline)
        target_link_options(test_call_commands PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endif()
add_test(NAME test_call_commands COMMAND test_call_commands)
set_tests_properties(test_call_commands PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_member_dialogs tests/test_member_dialogs.cpp src/renamedialog.cpp src/memberpropsdialog.cpp src/devgroupdialog.cpp
    db/db_manager.cpp transport/gui_bridge_impl.cpp src/connmonitorwindow.cpp)
target_link_libraries(test_member_dialogs PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_member_dialogs PRIVATE UTUN_TEST_MEMBER_ALLOC_FAILURES)
    target_link_options(test_member_dialogs PRIVATE "-Wl,--wrap=u_malloc_impl" "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_member_dialogs COMMAND test_member_dialogs)
set_tests_properties(test_member_dialogs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(test_kanban_view tests/test_kanban_view.cpp src/kanban_card_list.h)
target_link_libraries(test_kanban_view PRIVATE ${QT_LIBS} ${QT_TEST_LIB})
add_test(NAME test_kanban_view COMMAND test_kanban_view)
set_tests_properties(test_kanban_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

# Настоящее окно и тот же набор исходников/зависимостей приложения, без второго main.
get_target_property(DM_DELETION_SOURCES vibechat SOURCES)
list(REMOVE_ITEM DM_DELETION_SOURCES src/main.cpp)
# TARGET_PROPERTY вычисляет также транзитивный QT_NEEDS_QMAIN GUI-цели; консольным тестам нужны лишь собственные defines.
get_target_property(DM_DELETION_DEFINITIONS vibechat COMPILE_DEFINITIONS)
message(STATUS "Desktop GUI tests share application definitions: ${DM_DELETION_DEFINITIONS}")
add_executable(test_dm_deletion tests/test_dm_deletion.cpp ${DM_DELETION_SOURCES})
add_dependencies(test_dm_deletion chatgui_translations)
target_include_directories(test_dm_deletion PRIVATE $<TARGET_PROPERTY:vibechat,INCLUDE_DIRECTORIES>)
target_compile_definitions(test_dm_deletion PRIVATE ${DM_DELETION_DEFINITIONS})
get_target_property(DM_DELETION_LIBRARIES vibechat LINK_LIBRARIES)
target_link_libraries(test_dm_deletion PRIVATE ${DM_DELETION_LIBRARIES} ${QT_TEST_LIB})
add_test(NAME test_dm_deletion COMMAND test_dm_deletion)
set_tests_properties(test_dm_deletion PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(test_settings_save tests/test_settings_save.cpp ${DM_DELETION_SOURCES})
add_dependencies(test_settings_save chatgui_translations)
target_include_directories(test_settings_save PRIVATE $<TARGET_PROPERTY:vibechat,INCLUDE_DIRECTORIES>)
target_compile_definitions(test_settings_save PRIVATE ${DM_DELETION_DEFINITIONS})
target_link_libraries(test_settings_save PRIVATE ${DM_DELETION_LIBRARIES} ${QT_TEST_LIB})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_settings_save PRIVATE UTUN_TEST_SETTINGS_ALLOC_FAILURES)
    target_link_options(test_settings_save PRIVATE "-Wl,--wrap=u_malloc_impl" "-Wl,--wrap=u_calloc_impl"
        "-Wl,--wrap=u_free_impl" "-Wl,--wrap=ma_context_init")
endif()
add_test(NAME test_settings_save COMMAND test_settings_save)
set_tests_properties(test_settings_save PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_SESSION_TYPE=x11" TIMEOUT 60)

add_executable(test_desktop_commands tests/test_desktop_commands.cpp ${DM_DELETION_SOURCES})
add_dependencies(test_desktop_commands chatgui_translations)
target_include_directories(test_desktop_commands PRIVATE $<TARGET_PROPERTY:vibechat,INCLUDE_DIRECTORIES>)
target_compile_definitions(test_desktop_commands PRIVATE ${DM_DELETION_DEFINITIONS})
target_link_libraries(test_desktop_commands PRIVATE ${DM_DELETION_LIBRARIES} ${QT_TEST_LIB})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_desktop_commands PRIVATE UTUN_TEST_DESKTOP_HOOKS)
    foreach(symbol u_malloc_impl u_calloc_impl u_free_impl radio_call_trampoline ma_context_init sqlite3_step sqlite3_bind_text sqlite3_finalize)
        target_link_options(test_desktop_commands PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endif()
add_test(NAME test_desktop_commands COMMAND test_desktop_commands)
if(Qt6_FOUND AND Qt6Core_VERSION VERSION_GREATER_EQUAL "6.10")
    # Имитируем два физических QScreen с разными именами/координатами, без переключения рабочих столов.
    add_test(NAME test_media_window_monitors COMMAND test_desktop_commands mediaWindowGeometry)
    set_tests_properties(test_media_window_monitors PROPERTIES TIMEOUT 60
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen:configfile=${CMAKE_CURRENT_SOURCE_DIR}/tests/media_screens.json;XDG_SESSION_TYPE=x11")
endif()
set_tests_properties(test_desktop_commands PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_SESSION_TYPE=x11" TIMEOUT 30)

add_executable(test_group_edits tests/test_group_edits.cpp ${DM_DELETION_SOURCES})
add_dependencies(test_group_edits chatgui_translations)
target_include_directories(test_group_edits PRIVATE $<TARGET_PROPERTY:vibechat,INCLUDE_DIRECTORIES>)
target_compile_definitions(test_group_edits PRIVATE ${DM_DELETION_DEFINITIONS})
target_link_libraries(test_group_edits PRIVATE ${DM_DELETION_LIBRARIES} ${QT_TEST_LIB})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(test_group_edits PRIVATE "-Wl,--wrap=u_malloc_impl" "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_group_edits COMMAND test_group_edits)
set_tests_properties(test_group_edits PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

# Ручной запуск с DISPLAY: проверяет применение desktop hints оконным менеджером, не меняя настройки рабочих столов.
if(TARGET X11::X11)
    add_executable(test_message_notification_x11 tests/test_message_notification_x11.cpp)
    target_link_libraries(test_message_notification_x11 PRIVATE chatgui_notifications ${QT_TEST_LIB} X11::X11 pthread ${CMAKE_DL_LIBS})
endif()

add_executable(test_file_drop tests/test_file_drop.cpp src/inputbar.cpp src/chatview.cpp src/audiorecorder.cpp src/audio_device.cpp src/sound_manager.cpp
    src/voicemessageencoder.cpp src/emojipanel.cpp src/emojitabbar.cpp src/formattingpanel.cpp src/emoji.cpp src/animtimer.cpp src/lottieicon.cpp
    src/voice_audio_io.cpp transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
target_include_directories(test_file_drop PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_file_drop PRIVATE ${QT_LIBS} ${QT_TEST_LIB} rlottie::rlottie ZLIB::ZLIB
    utun_voice utun pthread ${CMAKE_DL_LIBS})
add_test(NAME test_file_drop COMMAND test_file_drop)
set_tests_properties(test_file_drop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_channel_replies tests/test_channel_replies.cpp db/db_manager.cpp src/nodespage.cpp
    transport/gui_bridge_impl.cpp transport/stream_bridge.cpp)
target_include_directories(test_channel_replies PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_channel_replies PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread ${CMAKE_DL_LIBS} chatgui_ble)
add_test(NAME test_channel_replies COMMAND test_channel_replies)
set_tests_properties(test_channel_replies PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_nodes_page tests/test_nodes_page.cpp db/db_manager.cpp src/nodespage.cpp)
target_include_directories(test_nodes_page PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_nodes_page PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_nodes_page PRIVATE UTUN_TEST_NODE_FINALIZE_FAILURE)
    target_link_options(test_nodes_page PRIVATE "-Wl,--wrap=sqlite3_finalize")
endif()
add_test(NAME test_nodes_page COMMAND test_nodes_page)
set_tests_properties(test_nodes_page PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

add_executable(test_database_maintenance tests/test_database_maintenance.cpp db/db_manager.cpp src/databasesettingspage.cpp)
target_include_directories(test_database_maintenance PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_database_maintenance PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
add_test(NAME test_database_maintenance COMMAND test_database_maintenance)
set_tests_properties(test_database_maintenance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)

add_executable(test_gui_db_read tests/test_gui_db_read.cpp db/db_manager.cpp src/nodespage.cpp src/kanbanview.cpp
    src/chatview.cpp src/messagedelegate.cpp src/group_attachment_roles.cpp
    src/inputbar.cpp src/emojipanel.cpp src/emojitabbar.cpp src/formattingpanel.cpp src/audiorecorder.cpp src/voicemessageencoder.cpp
    src/animtimer.cpp src/lottieicon.cpp src/emoji.cpp src/voiceplayback.cpp src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp
    transport/miniaudio_impl.c transport/audio_diagnostics.cpp
    src/avatars/groupavatarmanager.cpp transport/gui_bridge_impl.cpp transport/stream_bridge.cpp)
target_include_directories(test_gui_db_read PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_gui_db_read PRIVATE chatgui_clipboard chatgui_preview_cache ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread chatgui_ble rlottie::rlottie ZLIB::ZLIB ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(test_gui_db_read PRIVATE "-Wl,--wrap=u_malloc_impl" "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_gui_db_read COMMAND test_gui_db_read)
set_tests_properties(test_gui_db_read PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)

add_executable(test_member_files_browser tests/test_member_files_browser.cpp src/member_files/browser.cpp src/member_files/batch.cpp db/db_manager.cpp
    src/channellist.cpp src/channeldelegate.cpp src/avatars/groupavatarmanager.cpp tests/clipboard_bridge_stub.cpp)
target_link_libraries(test_member_files_browser PRIVATE chatgui_clipboard ${QT_LIBS} ${QT_TEST_LIB} utun pthread)
add_test(NAME test_member_files_browser COMMAND test_member_files_browser)
set_tests_properties(test_member_files_browser PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_member_files_lifecycle tests/test_member_files_lifecycle.cpp src/member_files/browser.cpp src/member_files/batch.cpp
    db/db_manager.cpp transport/gui_bridge_impl.cpp)
target_link_libraries(test_member_files_lifecycle PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun utun_voice pthread ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_member_files_lifecycle PRIVATE UTUN_TEST_FILE_ALLOC_FAILURES)
    target_link_options(test_member_files_lifecycle PRIVATE "-Wl,--wrap=u_calloc_impl")
endif()
add_test(NAME test_member_files_lifecycle COMMAND test_member_files_lifecycle)
set_tests_properties(test_member_files_lifecycle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_avatar_crop tests/test_avatar_crop.cpp src/avatars/avatarcrop.cpp)
target_link_libraries(test_avatar_crop PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun pthread ${CMAKE_DL_LIBS})
add_test(NAME test_avatar_crop COMMAND test_avatar_crop)
set_tests_properties(test_avatar_crop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)

add_executable(test_avatars src/memberlistmodel.cpp src/flagpainter.cpp tests/test_avatars.cpp src/avatars/avatarcache.cpp src/avatars/avatarmanager.cpp
    src/avatars/avatarcrop.cpp src/avatars/avatargallery.cpp src/imageviewer_window.cpp db/db_manager.cpp)
target_link_libraries(test_avatars PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun pthread)
add_test(NAME test_avatars COMMAND test_avatars)
set_tests_properties(test_avatars PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

add_executable(test_group_avatars tests/test_group_avatars.cpp src/avatars/groupavatarmanager.cpp src/channelsettingsdialog.cpp
    src/grouppolicydialog.cpp
    src/channellist.cpp src/channeldelegate.cpp db/db_manager.cpp tests/clipboard_bridge_stub.cpp)
target_link_libraries(test_group_avatars PRIVATE chatgui_clipboard ${QT_LIBS} ${QT_TEST_LIB} utun pthread)
add_test(NAME test_group_avatars COMMAND test_group_avatars)
set_tests_properties(test_group_avatars PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_gui_language tests/test_gui_language.cpp src/networksettingspage.cpp
    transport/node_config.cpp transport/config_updater.cpp "${CMAKE_CURRENT_BINARY_DIR}/translations.qrc")
add_dependencies(test_gui_language chatgui_translations)
target_compile_definitions(test_gui_language PRIVATE GUI_TS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/translations")
target_link_libraries(test_gui_language PRIVATE ${QT_LIBS} ${QT_TEST_LIB} OpenSSL::Crypto utun pthread)
add_test(NAME test_gui_language COMMAND test_gui_language)
set_tests_properties(test_gui_language PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_emoji_playback tests/test_emoji_playback.cpp src/animtimer.cpp src/lottieicon.cpp
    src/emojipanel.cpp src/emojitabbar.cpp src/emoji.cpp src/chatview.cpp)
target_link_libraries(test_emoji_playback PRIVATE ${QT_LIBS} ${QT_TEST_LIB} rlottie::rlottie ZLIB::ZLIB utun)
add_test(NAME test_emoji_playback COMMAND test_emoji_playback)
set_tests_properties(test_emoji_playback PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_invite_links tests/test_invite_links.cpp src/messagedelegate.cpp src/chatview.cpp
    src/accountdelegate.cpp src/flagpainter.cpp src/animtimer.cpp src/lottieicon.cpp src/emoji.cpp
    src/voiceplayback.cpp src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp
    transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
target_include_directories(test_invite_links PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_invite_links PRIVATE ${QT_LIBS} ${QT_TEST_LIB} rlottie::rlottie ZLIB::ZLIB utun_voice utun pthread ${CMAKE_DL_LIBS})
add_test(NAME test_invite_links COMMAND test_invite_links)
set_tests_properties(test_invite_links PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

foreach(message_target test_file_drop test_emoji_playback test_invite_links test_gui_db_read)
    target_link_libraries(${message_target} PRIVATE chatgui_message_text)
endforeach()

add_executable(test_message_text tests/test_message_text.cpp)
target_link_libraries(test_message_text PRIVATE chatgui_message_text ${QT_TEST_LIB} utun)
add_test(NAME test_message_text COMMAND test_message_text)
set_tests_properties(test_message_text PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_code_language tests/test_code_language.cpp)
target_link_libraries(test_code_language PRIVATE chatgui_message_text ${QT_TEST_LIB})
add_test(NAME test_code_language COMMAND test_code_language)
set_tests_properties(test_code_language PROPERTIES TIMEOUT 30)

add_executable(test_join_dialogs tests/test_join_dialogs.cpp src/joindialog.cpp src/inviteby.cpp src/invite_link.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/translations.qrc")
add_dependencies(test_join_dialogs chatgui_translations)
target_include_directories(test_join_dialogs PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_join_dialogs PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun)
add_test(NAME test_join_dialogs COMMAND test_join_dialogs)
set_tests_properties(test_join_dialogs PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_local_invite_ui tests/test_local_invite_ui.cpp src/invitedialog.cpp src/qrcode_utils.cpp)
target_include_directories(test_local_invite_ui PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_local_invite_ui PRIVATE ${QT_LIBS} ${QT_TEST_LIB} ZXing::ZXing utun)
add_test(NAME test_local_invite_ui COMMAND test_local_invite_ui)
set_tests_properties(test_local_invite_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)

add_executable(test_config_updater
    tests/test_config_updater.cpp
    transport/config_updater.cpp
)
target_include_directories(test_config_updater PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_config_updater PRIVATE ${QT_CORE} ${QT_TEST_LIB} utun pthread)
add_test(NAME test_config_updater COMMAND test_config_updater)

add_executable(test_audio_frame_ring tests/test_audio_frame_ring.cpp)
target_link_libraries(test_audio_frame_ring PRIVATE ${QT_CORE} ${QT_TEST_LIB} utun pthread)
add_test(NAME test_audio_frame_ring COMMAND test_audio_frame_ring)
set_tests_properties(test_audio_frame_ring PROPERTIES TIMEOUT 30)

add_executable(test_node_config tests/test_node_config.cpp transport/node_config.cpp transport/config_updater.cpp)
target_link_libraries(test_node_config PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun pthread)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_node_config PRIVATE UTUN_TEST_NODE_CONFIG_FAILURES)
    target_link_options(test_node_config PRIVATE -Wl,--wrap=sc_identity_generate)
endif()
add_test(NAME test_node_config COMMAND test_node_config)
set_tests_properties(test_node_config PROPERTIES TIMEOUT 30)

# тест адаптивного джиттер-буфера голоса (кодированные кадры + SoundTouch time-stretch)
add_executable(test_radio_jitter
    tests/test_radio_jitter.cpp
)
target_include_directories(test_radio_jitter PRIVATE ${CMAKE_SOURCE_DIR}/../../src)
target_link_libraries(test_radio_jitter PRIVATE utun_voice pthread)
add_test(NAME test_radio_jitter COMMAND test_radio_jitter)

add_executable(test_call_jitter tests/test_call_jitter.cpp)
target_include_directories(test_call_jitter PRIVATE ${CMAKE_SOURCE_DIR}/../../src)
target_link_libraries(test_call_jitter PRIVATE utun_voice utun pthread)
add_test(NAME test_call_jitter COMMAND test_call_jitter)

add_executable(test_call_delay tests/test_call_delay.cpp)
target_link_libraries(test_call_delay PRIVATE utun_voice utun pthread)
add_test(NAME test_call_delay COMMAND test_call_delay)

add_executable(test_soundtouch_clock tests/test_soundtouch_clock.cpp)
target_link_libraries(test_soundtouch_clock PRIVATE utun_voice utun pthread)
add_test(NAME test_soundtouch_clock COMMAND test_soundtouch_clock)

add_executable(test_call_timeline tests/test_call_timeline.cpp)
target_link_libraries(test_call_timeline PRIVATE utun_voice utun pthread)
add_test(NAME test_call_timeline COMMAND test_call_timeline)

# тест аудио-движка рации (per-source jitter + микшер + soft-clip)
add_executable(test_radio_audio
    tests/test_radio_audio.cpp
)
target_include_directories(test_radio_audio PRIVATE ${CMAKE_SOURCE_DIR}/../../src ${CMAKE_SOURCE_DIR}/../../lib ${CMAKE_SOURCE_DIR}/../../lib/libopus/include)
target_link_libraries(test_radio_audio PRIVATE utun_voice utun pthread)
add_test(NAME test_radio_audio COMMAND test_radio_audio)

add_executable(test_radio_member_mute tests/test_radio_member_mute.cpp)
target_include_directories(test_radio_member_mute PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_radio_member_mute PRIVATE ${QT_CORE} utun_voice utun pthread)
add_test(NAME test_radio_member_mute COMMAND test_radio_member_mute)
set(RADIO_MEMBER_TEST_SOURCES src/memberlistmodel.cpp src/flagpainter.cpp src/avatars/avatarcache.cpp src/avatars/avatarmanager.cpp
    src/avatars/avatarcrop.cpp src/avatars/avatargallery.cpp src/imageviewer_window.cpp db/db_manager.cpp)
add_executable(test_member_detail tests/test_member_detail.cpp src/accountlist.cpp src/devgroupdialog.cpp src/accountdelegate.cpp
    src/memberpropsdialog.cpp src/renamedialog.cpp ${RADIO_MEMBER_TEST_SOURCES})
target_include_directories(test_member_detail PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_member_detail PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread)
add_test(NAME test_member_detail COMMAND test_member_detail)
set_tests_properties(test_member_detail PROPERTIES TIMEOUT 20 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(test_radio_mute_bridge tests/test_radio_mute_bridge.cpp transport/utun_node.cpp transport/gui_bridge_impl.cpp transport/stream_bridge.cpp
    src/accountdelegate.cpp ${RADIO_MEMBER_TEST_SOURCES})
target_include_directories(test_radio_mute_bridge PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_radio_mute_bridge PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread chatgui_ble)
add_test(NAME test_radio_mute_bridge COMMAND test_radio_mute_bridge)
set_tests_properties(test_radio_mute_bridge PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_executable(test_utun_node tests/test_utun_node.cpp src/statuspage.cpp src/startupfailure_dialog.cpp transport/utun_node.cpp transport/gui_bridge_impl.cpp
    transport/stream_bridge.cpp)
target_include_directories(test_utun_node PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_utun_node PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread chatgui_ble)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_utun_node PRIVATE UTUN_TEST_NODE_STREAM_FAILURES)
    target_link_options(test_utun_node PRIVATE "-Wl,--wrap=stream_publisher_create" "-Wl,--wrap=stream_viewers_create")
endif()
add_test(NAME test_utun_node COMMAND test_utun_node)
set_tests_properties(test_utun_node PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(test_radio_member_mute PROPERTIES TIMEOUT 20)
if(UNIX AND NOT APPLE)
    target_compile_definitions(test_radio_member_mute PRIVATE UTUN_TEST_WRAP_VAD)
    target_link_options(test_radio_member_mute PRIVATE "-Wl,--wrap=silero_vad_create_default" "-Wl,--wrap=silero_vad_destroy"
        "-Wl,--wrap=silero_vad_reset" "-Wl,--wrap=silero_vad_process")
endif()

add_executable(test_audio_event_queue tests/test_audio_event_queue.cpp)
target_link_libraries(test_audio_event_queue PRIVATE pthread)
add_test(NAME test_audio_event_queue COMMAND test_audio_event_queue)
add_executable(test_voice_audio_io tests/test_voice_audio_io.cpp src/voice_audio_io.cpp)
target_include_directories(test_voice_audio_io PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_voice_audio_io PRIVATE utun pthread)
add_test(NAME test_voice_audio_io COMMAND test_voice_audio_io)
add_executable(test_audio_compressor ../../tests/test_audio_compressor.c)
target_include_directories(test_audio_compressor PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_audio_compressor PRIVATE utun pthread)
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
    target_compile_definitions(test_audio_compressor PRIVATE TEST_SHARED_SPEECH_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/data/silero_speech.pcm")
endif()
add_test(NAME test_audio_compressor COMMAND test_audio_compressor)
set_tests_properties(test_audio_compressor PROPERTIES TIMEOUT 20)
add_executable(test_speex_ns ../../tests/test_speex_ns.c)
target_compile_options(test_speex_ns PRIVATE -UNDEBUG)
target_link_libraries(test_speex_ns PRIVATE utun)
add_test(NAME test_speex_ns COMMAND test_speex_ns)
if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIB)
    add_executable(test_silero_vad ../../tests/test_silero_vad.c)
    target_compile_definitions(test_silero_vad PRIVATE TEST_SILERO_SPEECH_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/data/silero_speech.pcm")
    target_link_libraries(test_silero_vad PRIVATE utun)
    add_test(NAME test_silero_vad COMMAND test_silero_vad "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/silero_vad.onnx")
endif()
set_tests_properties(test_audio_event_queue test_voice_audio_io PROPERTIES TIMEOUT 15)
if(UNIX AND NOT APPLE)
    add_executable(test_audio_timing tests/test_audio_timing.c transport/audio_diagnostics.cpp)
    target_link_libraries(test_audio_timing PRIVATE utun pthread dl m)
    target_compile_options(test_audio_timing PRIVATE -UNDEBUG)
    add_test(NAME test_audio_timing COMMAND test_audio_timing)
    add_executable(test_voice_tx tests/test_voice_tx.cpp)
    target_include_directories(test_voice_tx PRIVATE ${UTUN_INCLUDE_DIRS})
    target_link_libraries(test_voice_tx PRIVATE utun_voice utun pthread)
    target_link_options(test_voice_tx PRIVATE "-Wl,--wrap=radio_talk_begin" "-Wl,--wrap=radio_talk_end"
        "-Wl,--wrap=radio_talk_send" "-Wl,--wrap=call_send_media" "-Wl,--wrap=chat_setting_get_int"
        "-Wl,--wrap=silero_vad_create_default" "-Wl,--wrap=silero_vad_destroy" "-Wl,--wrap=silero_vad_reset"
        "-Wl,--wrap=silero_vad_process" "-Wl,--wrap=get_time_tb")
    add_test(NAME test_voice_tx COMMAND test_voice_tx)
    add_executable(test_call_audio_tx tests/test_call_audio_tx.cpp)
    target_include_directories(test_call_audio_tx PRIVATE ${UTUN_INCLUDE_DIRS})
    target_link_libraries(test_call_audio_tx PRIVATE utun_voice utun pthread)
    target_link_options(test_call_audio_tx PRIVATE "-Wl,--wrap=call_send_media" "-Wl,--wrap=chat_setting_get_int" "-Wl,--wrap=get_time_tb")
    foreach(scenario fresh same_owner reinit idempotent different_instance reused_address detached_drain
            discarded_loop cross_loop mixed_calls current_expiry rebind_storm detached_engine)
        add_test(NAME test_call_audio_tx_${scenario} COMMAND test_call_audio_tx ${scenario})
        set_tests_properties(test_call_audio_tx_${scenario} PROPERTIES TIMEOUT 15)
    endforeach()
    # Настоящий host bridge; неиспользуемые Android-сервисы исключаются, как в test_android_replies.
    add_executable(test_android_call_mute tests/test_android_call_mute.cpp ../chatgui-android/jni_bridge/android_jni_bridge.c)
    target_include_directories(test_android_call_mute PRIVATE ${UTUN_INCLUDE_DIRS})
    target_compile_options(test_android_call_mute PRIVATE -ffunction-sections -fdata-sections)
    target_link_libraries(test_android_call_mute PRIVATE utun_voice utun pthread)
    target_link_options(test_android_call_mute PRIVATE "-Wl,--gc-sections" "-Wl,--wrap=get_time_tb"
        "-Wl,--wrap=chat_setting_get_int" "-Wl,--wrap=call_send_media" "-Wl,--wrap=opus_codec_encode"
        "-Wl,--wrap=speex_aec_process_capture" "-Wl,--wrap=speex_ns_process_frame" "-Wl,--wrap=audio_compressor_process_frame")
    foreach(aec 0 1)
        foreach(agc 0 1)
            add_test(NAME test_android_call_mute_aec${aec}_agc${agc} COMMAND test_android_call_mute ${aec} ${agc})
            set_tests_properties(test_android_call_mute_aec${aec}_agc${agc} PROPERTIES TIMEOUT 15)
        endforeach()
    endforeach()
    add_test(NAME test_android_call_mute_legacy COMMAND test_android_call_mute legacy)
    set_tests_properties(test_android_call_mute_legacy PROPERTIES TIMEOUT 15)
    add_executable(test_android_noise_suppression tests/test_android_noise_suppression.cpp
        ../chatgui-android/jni_bridge/android_jni_bridge.c ../chatgui-android/libutun_lite/voice_recorder.c)
    target_include_directories(test_android_noise_suppression PRIVATE ${UTUN_INCLUDE_DIRS})
    target_compile_options(test_android_noise_suppression PRIVATE -ffunction-sections -fdata-sections)
    target_link_libraries(test_android_noise_suppression PRIVATE utun_voice utun pthread)
    target_link_options(test_android_noise_suppression PRIVATE "-Wl,--gc-sections" "-Wl,--wrap=clock_gettime"
        "-Wl,--wrap=speex_ns_process_frame" "-Wl,--wrap=speex_aec_process_capture" "-Wl,--wrap=speex_aec_feed_playback")
    add_test(NAME test_android_noise_suppression COMMAND test_android_noise_suppression)
    set_tests_properties(test_android_noise_suppression PROPERTIES TIMEOUT 15)
    add_executable(test_call_audio_lifecycle tests/test_call_audio_lifecycle.c)
    target_include_directories(test_call_audio_lifecycle PRIVATE ${UTUN_INCLUDE_DIRS})
    target_link_libraries(test_call_audio_lifecycle PRIVATE utun_voice utun pthread)
    foreach(scenario idempotent restart detach before_chat)
        add_test(NAME test_call_audio_lifecycle_${scenario} COMMAND test_call_audio_lifecycle ${scenario})
        set_tests_properties(test_call_audio_lifecycle_${scenario} PROPERTIES TIMEOUT 20)
    endforeach()
    add_executable(test_android_audio_lifecycle tests/test_android_audio_lifecycle.c
        ../chatgui-android/libutun_lite/instance_lite.c ../chatgui-android/libutun_lite/standby.c
        ../chatgui-android/libutun_lite/utun_config_api.c ../chatgui-android/libutun_lite/voice_recorder.c
        ../chatgui-android/libutun_lite/stream_bridge.c ../chatgui-android/jni_bridge/android_jni_bridge.c
        ../chatgui-android/jni_bridge/android_udp_log.c)
    target_include_directories(test_android_audio_lifecycle PRIVATE ${UTUN_INCLUDE_DIRS})
    target_compile_definitions(test_android_audio_lifecycle PRIVATE UTUN_WITH_VOICE)
    target_compile_options(test_android_audio_lifecycle PRIVATE -ffunction-sections -fdata-sections)
    target_link_libraries(test_android_audio_lifecycle PRIVATE utun_voice utun pthread ${CMAKE_DL_LIBS})
    target_link_options(test_android_audio_lifecycle PRIVATE "-Wl,--gc-sections" "-Wl,--wrap=android_stream_attach"
        "-Wl,--wrap=call_audio_init" "-Wl,--wrap=call_audio_destroy" "-Wl,--wrap=utun_instance_destroy"
        "-Wl,--wrap=uasync_destroy" "-Wl,--wrap=pthread_detach" "-Wl,--wrap=instance_lite_get_instance"
        "-Wl,--wrap=uasync_create" "-Wl,--wrap=utun_instance_create_from_config_async" "-Wl,--wrap=pthread_join"
        "-Wl,--wrap=uasync_poll" "-Wl,--wrap=uasync_post_reserved" "-Wl,--wrap=call_set_phone_state" "-Wl,--wrap=u_malloc_impl"
        "-Wl,--wrap=utun_bridge_get_local_sockets_json")
    foreach(scenario normal restart stream_failure voice_failure core_stop restart_core_stop
        cancel_loop_create cancel_instance_create cancel_attach cancel_restart cancel_slow_attach parse_failure schema_failure
        phone_stop phone_restart phone_publication_stop radio_config radio_config_stop radio_config_restart radio_config_publication_stop)
        add_test(NAME test_android_audio_lifecycle_${scenario} COMMAND test_android_audio_lifecycle ${scenario})
        set_tests_properties(test_android_audio_lifecycle_${scenario} PROPERTIES TIMEOUT 20)
    endforeach()
endif()

# smoke-тест видео-декодера (FFmpeg). Ручной запуск: test_video_engine <mp4>
add_executable(test_video_engine
    tests/test_video_engine.cpp
    src/videoplayer_engine.cpp
    src/sound_manager.cpp
    src/voice_audio_io.cpp
    src/audio_device.cpp
    transport/miniaudio_impl.c
    transport/audio_diagnostics.cpp
)
target_include_directories(test_video_engine PRIVATE ${CMAKE_SOURCE_DIR}/../../lib ${CMAKE_SOURCE_DIR}/../../src ${CMAKE_SOURCE_DIR}/db ${CMAKE_SOURCE_DIR}/../../lib/libopus/include ${FFMPEG_INCLUDE_DIRS})
target_compile_definitions(test_video_engine PRIVATE USE_SQLITE SQLITE_THREADSAFE=1)
if(FFMPEG_FOUND)
    target_compile_definitions(test_video_engine PRIVATE HAVE_FFMPEG)
endif()
set_source_files_properties(transport/miniaudio_impl.c PROPERTIES LANGUAGE C)
if(WIN32)
    target_link_libraries(test_video_engine PRIVATE ${QT_LIBS} ${QT_CORE} utun_voice utun pthread ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
else()
    target_link_libraries(test_video_engine PRIVATE ${QT_LIBS} ${QT_CORE} utun_voice utun pthread dl ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
endif()

if(FFMPEG_FOUND)
    add_executable(test_video_playback tests/test_video_playback.cpp src/videoplayer_engine.cpp src/videoplayer_window.cpp
        src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
    target_include_directories(test_video_playback PRIVATE ${UTUN_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
    target_compile_definitions(test_video_playback PRIVATE HAVE_FFMPEG)
    target_link_libraries(test_video_playback PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread ${CMAKE_DL_LIBS}
        ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_video_playback PRIVATE TEST_NULL_AUDIO)
        target_link_options(test_video_playback PRIVATE "-Wl,--wrap=ma_context_init" "-Wl,--wrap=ma_device_init" "-Wl,--wrap=avcodec_open2")
    endif()
    add_test(NAME test_video_playback COMMAND test_video_playback)
    set_tests_properties(test_video_playback PROPERTIES TIMEOUT 45 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(test_ptt_key_chord tests/test_ptt_key_chord.cpp)
add_executable(test_stream_view tests/test_stream_view.cpp src/streamplayer_window.cpp src/streamgrid_window.cpp transport/stream_bridge.cpp
    src/videoplayer_window.cpp src/videoplayer_engine.cpp src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp
    transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
target_include_directories(test_stream_view PRIVATE ${UTUN_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(test_stream_view PRIVATE ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread ${CMAKE_DL_LIBS} ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
if(GUI_CAPTURE_ENABLED)
    target_compile_definitions(test_stream_view PRIVATE HAVE_STREAM_CAPTURE)
    target_link_libraries(test_stream_view PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
endif()
if(FFMPEG_FOUND)
    target_compile_definitions(test_stream_view PRIVATE HAVE_FFMPEG)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_stream_view PRIVATE TEST_NULL_AUDIO)
    target_link_options(test_stream_view PRIVATE "-Wl,--wrap=ma_context_init" "-Wl,--wrap=ma_device_init"
        "-Wl,--wrap=audio_stream_frame_time" "-Wl,--wrap=stream_selection_get_info")
    if(FFMPEG_FOUND)
        target_link_options(test_stream_view PRIVATE "-Wl,--wrap=avcodec_open2" "-Wl,--wrap=avcodec_receive_frame"
            "-Wl,--wrap=avcodec_free_context")
    endif()
endif()
add_test(NAME test_stream_view COMMAND test_stream_view)
set_tests_properties(test_stream_view PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_stream_audio tests/test_stream_audio.cpp)
target_include_directories(test_stream_audio PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_stream_audio PRIVATE utun_voice utun pthread ${CMAKE_DL_LIBS})
add_test(NAME test_stream_audio COMMAND test_stream_audio)
set_tests_properties(test_stream_audio PROPERTIES TIMEOUT 30)

if(FFMPEG_FOUND)
    add_executable(test_video_recording tests/test_video_recording.cpp src/video_recording_window.cpp transport/stream_bridge.cpp
        src/videoplayer_window.cpp src/videoplayer_engine.cpp
        src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
    target_compile_definitions(test_video_recording PRIVATE HAVE_FFMPEG)
    target_include_directories(test_video_recording PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(test_video_recording PRIVATE chatgui_stream_publish ${QT_TEST_LIB} utun_voice pthread ${CMAKE_DL_LIBS}
        ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(test_video_recording PRIVATE TEST_NULL_AUDIO)
        target_link_options(test_video_recording PRIVATE "-Wl,--wrap=ma_context_init" "-Wl,--wrap=ma_device_init"
            "-Wl,--wrap=audio_stream_frame_time" "-Wl,--wrap=audio_stream_capture_missing" "-Wl,--wrap=opus_codec_encode")
    endif()
    add_test(NAME test_video_recording COMMAND test_video_recording)
    set_tests_properties(test_video_recording PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(test_stream_publish tests/test_stream_publish.cpp transport/stream_bridge.cpp src/videoplayer_window.cpp src/videoplayer_engine.cpp
    src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
add_dependencies(test_stream_publish chatgui_translations)
target_link_libraries(test_stream_publish PRIVATE chatgui_stream_publish ${QT_TEST_LIB} utun_voice pthread ${CMAKE_DL_LIBS})
if(FFMPEG_FOUND)
    target_compile_definitions(test_stream_publish PRIVATE HAVE_FFMPEG)
    target_include_directories(test_stream_publish PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(test_stream_publish PRIVATE ${FFMPEG_TARGET} ${FFMPEG_LIBRARIES})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_stream_publish PRIVATE TEST_NULL_AUDIO)
    target_link_options(test_stream_publish PRIVATE "-Wl,--wrap=ma_context_init" "-Wl,--wrap=ma_device_init"
        "-Wl,--wrap=ma_device_uninit" "-Wl,--wrap=opus_codec_encode" "-Wl,--wrap=stream_publisher_video_rate")
    if(FFMPEG_FOUND)
        target_link_options(test_stream_publish PRIVATE "-Wl,--wrap=avcodec_open2")
    endif()
endif()
add_test(NAME test_stream_publish COMMAND test_stream_publish)
set_tests_properties(test_stream_publish PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

target_link_libraries(test_ptt_key_chord PRIVATE ${QT_CORE})
add_test(NAME test_ptt_key_chord COMMAND test_ptt_key_chord)

add_executable(test_voice_file tests/test_voice_file.cpp src/voiceplayback.cpp src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp transport/miniaudio_impl.c transport/audio_diagnostics.cpp)
target_include_directories(test_voice_file PRIVATE ${UTUN_INCLUDE_DIRS})
target_link_libraries(test_voice_file PRIVATE ${QT_LIBS} utun_voice utun pthread ${CMAKE_DL_LIBS})
add_test(NAME test_voice_file COMMAND test_voice_file)

if(WIN32)
    # AudioDevice закрывает неисправный WASAPI в worker с собственным COM apartment.
    foreach(audio_target vibechat test_file_drop test_invite_links test_video_engine test_stream_view test_stream_publish test_call_commands
                         test_voice_file test_sound_settings test_settings_save test_desktop_commands)
        target_link_libraries(${audio_target} PRIVATE ole32)
    endforeach()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_audio_recovery tests/test_audio_recovery.cpp src/audio_device.cpp src/sound_manager.cpp src/voice_audio_io.cpp
        src/audiorecorder.cpp src/call_audio_engine.cpp src/radio_audio_engine.cpp
        transport/miniaudio_impl.c transport/audio_diagnostics.cpp transport/config_updater.cpp)
    target_include_directories(test_audio_recovery PRIVATE ${UTUN_INCLUDE_DIRS})
    target_link_libraries(test_audio_recovery PRIVATE chatgui_audio_capture ${QT_LIBS} utun_voice utun pthread dl)
    target_link_options(test_audio_recovery PRIVATE "-Wl,--wrap=ma_context_init" "-Wl,--wrap=ma_context_uninit"
        "-Wl,--wrap=ma_device_init" "-Wl,--wrap=ma_device_start" "-Wl,--wrap=ma_device_uninit" "-Wl,--wrap=audio_context_failed"
        "-Wl,--wrap=call_send_media")
    add_test(NAME test_audio_recovery COMMAND test_audio_recovery)
    set_tests_properties(test_audio_recovery PROPERTIES TIMEOUT 15)
    add_test(NAME test_microphone_watchdog COMMAND test_audio_recovery --microphone-watchdog)
    set_tests_properties(test_microphone_watchdog PROPERTIES TIMEOUT 25)
    add_test(NAME test_audio_buffer_tuning COMMAND test_audio_recovery --buffer-tuning)
    set_tests_properties(test_audio_buffer_tuning PROPERTIES TIMEOUT 20)
    add_executable(test_radio_panel tests/test_radio_panel.cpp src/radiopanel.cpp src/radio_audio_engine.cpp
        src/radiosettingspage.cpp src/ptt_hotkey_manager.cpp
        src/sound_manager.cpp src/voice_audio_io.cpp src/audio_device.cpp
        transport/miniaudio_impl.c transport/audio_diagnostics.cpp transport/config_updater.cpp ${RADIO_MEMBER_TEST_SOURCES})
    target_include_directories(test_radio_panel PRIVATE ${UTUN_INCLUDE_DIRS})
    target_link_libraries(test_radio_panel PRIVATE chatgui_audio_capture ${QT_LIBS} ${QT_TEST_LIB} utun_voice utun pthread dl ${X11_LIBRARIES})
    target_link_options(test_radio_panel PRIVATE "-Wl,--wrap=ma_context_init" "-Wl,--wrap=ma_device_start"
        "-Wl,--wrap=radio_talk_begin" "-Wl,--wrap=radio_talk_end" "-Wl,--wrap=radio_talk_send"
        "-Wl,--wrap=silero_vad_create_default" "-Wl,--wrap=silero_vad_destroy" "-Wl,--wrap=silero_vad_reset"
        "-Wl,--wrap=silero_vad_process")
    add_test(NAME test_radio_panel COMMAND test_radio_panel)
    set_tests_properties(test_radio_panel PROPERTIES TIMEOUT 25 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    find_program(XVFB_RUN xvfb-run)
    find_program(XDOTOOL xdotool)
    if(XVFB_RUN AND XDOTOOL)
        add_test(NAME test_ptt_hotkey COMMAND ${XVFB_RUN} -a $<TARGET_FILE:test_radio_panel> --ptt-hotkey)
        set_tests_properties(test_ptt_hotkey PROPERTIES TIMEOUT 35 ENVIRONMENT "QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11")
    endif()
    add_executable(test_ptt_portal tests/test_ptt_portal.cpp src/ptt_portal.cpp)
    target_link_libraries(test_ptt_portal PRIVATE ${QT_DBUS} ${QT_TEST_LIB} utun pthread)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(NOT DBUS_RUN_SESSION)
        message(FATAL_ERROR "dbus-run-session is required for the isolated PTT portal test")
    endif()
    add_test(NAME test_ptt_portal COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_ptt_portal>)

    add_executable(test_linux_audio tests/test_linux_audio.c transport/audio_diagnostics.cpp)
    target_link_libraries(test_linux_audio PRIVATE utun pthread dl m)
    # assert выполняет вызовы стенда, поэтому не отключаем его в Release.
    target_compile_options(test_linux_audio PRIVATE -UNDEBUG)
    add_test(NAME test_linux_audio COMMAND test_linux_audio)

    add_executable(test_audio_diagnostics tests/test_audio_diagnostics.cpp)
    target_link_libraries(test_audio_diagnostics PRIVATE utun pthread)
    target_compile_options(test_audio_diagnostics PRIVATE -UNDEBUG)
    add_test(NAME test_audio_diagnostics COMMAND test_audio_diagnostics)
endif()

if(CHATGUI_BUILD_DEB)
    include(installer/linux.cmake)
endif()
if(CHATGUI_BUILD_USER_ARCHIVE)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT GUI_CAPTURE_ENABLED)
        message(FATAL_ERROR "User archive requires Release, Qt 6.6+ Multimedia and FFmpeg")
    endif()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/user-qmake.txt" CONTENT "${QT_QMAKE}\n")
endif()
