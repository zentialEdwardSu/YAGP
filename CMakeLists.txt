cmake_minimum_required(VERSION 3.21)
# Ninja/Makefiles take the compiler, linker and SDK paths from the environment,
# so outside a VS developer shell CMake picks up whatever link/ld is on PATH
# (e.g. MinGW's ld.exe) and the compiler check fails cryptically. The Visual
# Studio generator finds MSVC by itself: use `cmake --preset release` instead.
if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles" AND "$ENV{OS}" STREQUAL "Windows_NT"
   AND NOT DEFINED ENV{VCINSTALLDIR})
    message(FATAL_ERROR
        "Generator '${CMAKE_GENERATOR}' needs a Visual Studio developer shell (VCINSTALLDIR is not set).\n"
        "From a normal shell use the Visual Studio generator instead:\n"
        "  cmake --preset release\n"
        "  cmake --build --preset release")
endif()
project(YAGP LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "YAGP targets Windows 10/11 with MSVC (the SDK's dcomp.h is C++ only).")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
# Static CRT: one self-contained exe, no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# giflib decoder only (MIT). The tarball has no CMakeLists.txt, so
# FetchContent_MakeAvailable just downloads/extracts it and we build the five
# decoder sources ourselves. Offline: -DFETCHCONTENT_SOURCE_DIR_GIFLIB=<dir>.
include(FetchContent)
FetchContent_Declare(giflib
    URL "https://sourceforge.net/projects/giflib/files/giflib-5.2.2.tar.gz/download"
        "https://downloads.sourceforge.net/project/giflib/giflib-5.2.2.tar.gz"
    DOWNLOAD_NAME giflib-5.2.2.tar.gz
    URL_HASH SHA256=be7ffbd057cadebe2aa144542fd90c6838c6a083b5e8a9048b8ee3b66b29d5fb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(giflib)
set(GIFLIB_DIR ${giflib_SOURCE_DIR})
add_library(giflib STATIC
    ${GIFLIB_DIR}/dgif_lib.c ${GIFLIB_DIR}/gif_err.c ${GIFLIB_DIR}/gifalloc.c
    ${GIFLIB_DIR}/gif_hash.c ${GIFLIB_DIR}/openbsd-reallocarray.c)
target_include_directories(giflib PUBLIC ${GIFLIB_DIR})
target_compile_definitions(giflib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
target_compile_options(giflib PRIVATE /W0)

# Lua 5.4 (MIT) for motion scripts. Only the core and the libraries scripts may
# use are compiled: io, os, package, debug and linit.c are left out entirely, so
# the sandbox cannot be escaped by re-opening them. Offline:
# -DFETCHCONTENT_SOURCE_DIR_LUA=<dir>.
FetchContent_Declare(lua
    URL "https://www.lua.org/ftp/lua-5.4.7.tar.gz"
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(lua)
set(LUA_DIR ${lua_SOURCE_DIR}/src)
set(LUA_CORE lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
    lparser lstate lstring ltable ltm lundump lvm lzio
    lauxlib lbaselib lcorolib lmathlib ltablib lutf8lib)
list(TRANSFORM LUA_CORE PREPEND ${LUA_DIR}/)
list(TRANSFORM LUA_CORE APPEND .c)
# lstrlib with a work limit on pattern matching, which the instruction-count
# hook cannot interrupt (see cmake/patch_lstrlib.cmake).
set(LSTRLIB_YAGP ${CMAKE_CURRENT_BINARY_DIR}/lstrlib_yagp.c)
add_custom_command(OUTPUT ${LSTRLIB_YAGP}
    COMMAND ${CMAKE_COMMAND} -DIN=${LUA_DIR}/lstrlib.c -DOUT=${LSTRLIB_YAGP}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_lstrlib.cmake
    DEPENDS ${LUA_DIR}/lstrlib.c ${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_lstrlib.cmake
    COMMENT "Patching lstrlib.c with the pattern-matching step limit")
add_library(lua STATIC ${LUA_CORE} ${LSTRLIB_YAGP})
target_include_directories(lua PUBLIC ${LUA_DIR})
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(lua PRIVATE /W0)

# rxi/log.c (MIT): small logging core used by the app and the Lua host. The
# commit is pinned and the archive hash verified. Offline:
# -DFETCHCONTENT_SOURCE_DIR_RXILOG=<dir containing src/log.c>.
FetchContent_Declare(rxilog
    URL "https://github.com/rxi/log.c/archive/f9ea34994bd58ed342d2245cd4110bb5c6790153.tar.gz"
    URL_HASH SHA256=0e1b70940986c16a8cb7ca322680b3c7614f769bc477c5b90e68a42aecc77ff6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(rxilog)
add_library(rxilog STATIC ${rxilog_SOURCE_DIR}/src/log.c)
target_include_directories(rxilog PUBLIC ${rxilog_SOURCE_DIR}/src)
target_compile_definitions(rxilog PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(rxilog PRIVATE /W0)

# stb_image v2.30 is vendored under third_party/ (MIT/public domain): still
# PNG/JPG/JPEG decoding only; giflib remains the animated-GIF decoder.

add_executable(yagp WIN32
    main.c overlay.c clip.c clip_path.c config.c cursor_hook.c dialogs.c gif_stream.c frame_cache.c
    still_image.c stb_image.c group.c
    applog.c dx11_renderer.cpp input_fsm.c monitors.c script.c contacts.c transform.c presets.rc app.manifest)
target_include_directories(yagp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
set_source_files_properties(stb_image.c PROPERTIES COMPILE_OPTIONS "/W0")
# UI language for i18n.h: zh_CN (default) or en.
set(YAGP_LANG "zh_CN" CACHE STRING "UI language (zh_CN, en)")
set_property(CACHE YAGP_LANG PROPERTY STRINGS zh_CN en)
if(YAGP_LANG STREQUAL "en")
    set(YAGP_LANG_DEFINE YAGP_LANG_EN)
elseif(YAGP_LANG STREQUAL "zh_CN")
    set(YAGP_LANG_DEFINE YAGP_LANG_ZH_CN)
else()
    message(FATAL_ERROR "Unknown YAGP_LANG '${YAGP_LANG}' (expected zh_CN or en)")
endif()

target_compile_definitions(yagp PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 ${YAGP_LANG_DEFINE})
target_link_libraries(yagp PRIVATE giflib lua rxilog
    user32 gdi32 shell32 shcore comdlg32 ole32 advapi32 d3d11 dxgi dcomp)
target_compile_options(yagp PRIVATE /W4 /utf-8 /GS /guard:cf
    $<$<COMPILE_LANGUAGE:CXX>:/GR->)
target_link_options(yagp PRIVATE /GUARD:CF /DYNAMICBASE /NXCOMPAT
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
set_property(TARGET yagp giflib lua PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Decoder regression test: python tests/test_gif_stream.py build/gif_dump.exe
add_executable(gif_dump EXCLUDE_FROM_ALL tests/gif_dump.c gif_stream.c frame_cache.c still_image.c stb_image.c)
target_include_directories(gif_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(gif_dump PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
set_source_files_properties(stb_image.c PROPERTIES COMPILE_OPTIONS "/W0")
target_link_libraries(gif_dump PRIVATE giflib user32 shell32 ole32)

# Static image regression helper: tests/test_still_image.py build/image_dump.exe
add_executable(image_dump EXCLUDE_FROM_ALL tests/image_dump.c still_image.c stb_image.c)
target_include_directories(image_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(image_dump PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

# Lua host + preset regression test: build/script_test.exe
add_executable(script_test EXCLUDE_FROM_ALL tests/script_test.c script.c monitors.c contacts.c clip_path.c
    input_fsm.c transform.c)
target_include_directories(script_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(script_test PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
    YAGP_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts" ${YAGP_LANG_DEFINE})
target_compile_options(script_test PRIVATE /W4 /utf-8)
target_link_libraries(script_test PRIVATE lua rxilog user32 shcore)

# Config v3/v4 parser and round-trip test (uses a redirected test folder).
add_executable(config_test EXCLUDE_FROM_ALL tests/config_test.c config.c)
target_include_directories(config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(config_test PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS YAGP_CONFIG_TEST)
target_compile_options(config_test PRIVATE /W4 /utf-8)
target_link_libraries(config_test PRIVATE rxilog shell32 ole32)

# Group folder scanner regression test (overlay/app stubs live in the test).
add_executable(group_test EXCLUDE_FROM_ALL tests/group_test.c group.c applog.c)
target_include_directories(group_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(group_test PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(group_test PRIVATE /W4 /utf-8)
target_link_libraries(group_test PRIVATE giflib rxilog)

# File-output regression test for applog.c: build/log_test.exe. The smaller cap
# lets the test reach the session limit without writing 16 MB.
add_executable(log_test EXCLUDE_FROM_ALL tests/log_test.c applog.c)
target_include_directories(log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(log_test PRIVATE _CRT_SECURE_NO_WARNINGS YAGP_APPLOG_TEST
    YAGP_APPLOG_LIMIT=8192)
target_compile_options(log_test PRIVATE /W4 /utf-8)
target_link_libraries(log_test PRIVATE rxilog)
