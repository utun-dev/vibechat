# Автономный runtime в /opt; системные ABI и драйверы устанавливает APT по Depends.
# build_nix.sh явно собирает vibechat; упаковка не должна запускать all с тестовыми бинарниками.
set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY TRUE)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "DEB packaging requires a native Linux build")
endif()
if(CMAKE_VERSION VERSION_LESS 3.21)
    message(FATAL_ERROR "DEB packaging requires CMake 3.21 or newer")
endif()
if(NOT QT_PACKAGE STREQUAL "Qt6" OR NOT GUI_CAPTURE_ENABLED)
    message(FATAL_ERROR "DEB packaging requires Qt 6.6+ Multimedia and FFmpeg (camera, calls and streams)")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}" MATCHES "sanitize=")
    message(FATAL_ERROR "DEB packaging requires a Release build without sanitizers")
endif()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
foreach(TOOL dpkg dpkg-deb dpkg-shlibdeps dpkg-query file objdump readelf appstreamcli)
    find_program(DEB_${TOOL} NAMES ${TOOL} REQUIRED)
endforeach()
execute_process(COMMAND "${DEB_appstreamcli}" validate --no-net "${CMAKE_CURRENT_SOURCE_DIR}/installer/org.utun.vibechat.appstream.xml"
    OUTPUT_VARIABLE DEB_APPSTREAM_OUTPUT ERROR_VARIABLE DEB_APPSTREAM_ERROR RESULT_VARIABLE DEB_APPSTREAM_RESULT)
if(NOT DEB_APPSTREAM_RESULT EQUAL 0)
    message(FATAL_ERROR "Invalid DEB AppStream catalog: ${DEB_APPSTREAM_OUTPUT}${DEB_APPSTREAM_ERROR}")
endif()
message(STATUS "DEB AppStream catalog ready: org.utun.vibechat -> package vibechat")
execute_process(COMMAND "${QT_QMAKE}" -query QT_INSTALL_PLUGINS
    OUTPUT_VARIABLE DEB_QT_PLUGINS OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE DEB_QT_RESULT)
if(NOT DEB_QT_RESULT EQUAL 0 OR NOT IS_DIRECTORY "${DEB_QT_PLUGINS}")
    message(FATAL_ERROR "Cannot locate Qt plugins for DEB packaging")
endif()

# DT_RPATH наследуется зависимостями и dlopen-плагинами: весь частный runtime ищется в одном месте.
target_link_options(vibechat PRIVATE -Wl,--disable-new-dtags)
set_target_properties(vibechat PROPERTIES INSTALL_RPATH "$ORIGIN/../lib" INSTALL_RPATH_USE_LINK_PATH FALSE)
install(TARGETS vibechat RUNTIME DESTINATION opt/vibechat/bin COMPONENT deb)
install(FILES installer/org.utun.vibechat.desktop DESTINATION usr/share/applications COMPONENT deb)
# Локальный DEB отсутствует в каталоге репозитория ОС; Discover нужна явная связь component ID с именем пакета.
install(FILES installer/org.utun.vibechat.appstream.xml DESTINATION usr/share/swcatalog/xml COMPONENT deb)
install(FILES "${CHATGUI_ICON_PNG}" DESTINATION usr/share/pixmaps
    RENAME org.utun.vibechat.png COMPONENT deb)
install(FILES ${UTUN_LICENSE_FILES} DESTINATION usr/share/doc/vibechat COMPONENT deb)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION usr/share/doc/vibechat RENAME copyright COMPONENT deb)

configure_file(installer/deploy_linux.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/deploy_linux.cmake.in" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/deploy_linux.cmake"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/deploy_linux.cmake.in")
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/deploy_linux.cmake" COMPONENT deb)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME vibechat)
set(CPACK_PACKAGE_VENDOR uTun)
set(CPACK_PACKAGE_CONTACT "uTun developers")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "VibeChat peer-to-peer messenger with voice, video and streams")
set(CPACK_PACKAGING_INSTALL_PREFIX /)
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};deb;/")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION net)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
# shlibs.local описывает включённые библиотеки как часть vibechat, а не как внешние пакеты ОС.
configure_file(installer/linux_dependencies.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/linux_dependencies.cmake" @ONLY)
set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_BINARY_DIR}/linux_dependencies.cmake")
# miniaudio загружает ALSA/PulseAudio динамически; драйверы и сертификаты остаются системными.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates, libasound2t64 | libasound2, libpulse0, libgl1, libegl1, fuse3")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "fonts-dejavu-core, xdg-desktop-portal")
set(CPACK_STRIP_FILES TRUE)
include(CPack)
message(STATUS "DEB packaging enabled: Release, private runtime in /opt/vibechat, user profile via XDG_DATA_HOME")
