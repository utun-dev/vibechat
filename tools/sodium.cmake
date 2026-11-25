# Импорт статической libsodium для компилятора текущего проекта, включая NDK.
if(TARGET utun_sodium)
    return()
endif()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
get_filename_component(_sodium_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_sodium_cc "${CMAKE_C_COMPILER}")
set(_sodium_options)
if(CMAKE_C_COMPILER_TARGET)
    list(APPEND _sodium_options "--target=${CMAKE_C_COMPILER_TARGET}")
    string(APPEND _sodium_cc " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
execute_process(COMMAND "${CMAKE_C_COMPILER}" ${_sodium_options} -dumpmachine
    OUTPUT_VARIABLE _sodium_target OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _sodium_rc)
if(NOT _sodium_rc EQUAL 0)
    message(FATAL_ERROR "Cannot identify the libsodium compiler target")
endif()
set(_sodium_prepare "${_sodium_root}/tools/prepare_dependencies.py")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "CC=${_sodium_cc}" "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}"
    "${Python3_EXECUTABLE}" "${_sodium_prepare}" --only sodium --sodium-target "${_sodium_target}" --if-needed
    --output "${CMAKE_BINARY_DIR}" --cache "${_sodium_root}/.cache/dependencies/downloads"
    ENCODING UTF-8 RESULT_VARIABLE _sodium_rc)
if(NOT _sodium_rc EQUAL 0)
    message(FATAL_ERROR "Cannot prepare libsodium for ${_sodium_target}")
endif()
set(_sodium_prefix "${CMAKE_BINARY_DIR}/.cache/dependencies/sodium/${_sodium_target}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sodium_prepare}" "${_sodium_prefix}/lib/libsodium.a"
    "${_sodium_prefix}/include/sodium.h")
add_library(utun_sodium STATIC IMPORTED GLOBAL)
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android|FreeBSD)$")
    set_property(TARGET utun_sodium PROPERTY INTERFACE_LINK_OPTIONS "LINKER:--exclude-libs,libsodium.a")
endif()
set_target_properties(utun_sodium PROPERTIES IMPORTED_LOCATION "${_sodium_prefix}/lib/libsodium.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_sodium_prefix}/include" INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC)
