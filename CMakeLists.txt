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
    lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib)
list(TRANSFORM LUA_CORE PREPEND ${LUA_DIR}/)
list(TRANSFORM LUA_CORE APPEND .c)
add_library(lua STATIC ${LUA_CORE})
target_include_directories(lua PUBLIC ${LUA_DIR})
target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(lua PRIVATE /W0)

add_executable(yagp WIN32
    main.c overlay.c config.c dialogs.c gif_stream.c frame_cache.c dx11_renderer.cpp
    monitors.c script.c contacts.c presets.rc app.manifest)
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
target_link_libraries(yagp PRIVATE giflib lua
    user32 gdi32 shell32 shcore comdlg32 ole32 advapi32 d3d11 dxgi dcomp)
target_compile_options(yagp PRIVATE /W4 /utf-8 /GS /guard:cf
    $<$<COMPILE_LANGUAGE:CXX>:/GR->)
target_link_options(yagp PRIVATE /GUARD:CF /DYNAMICBASE /NXCOMPAT
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
set_property(TARGET yagp giflib lua PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Decoder regression test: python tests/test_gif_stream.py build/gif_dump.exe
add_executable(gif_dump EXCLUDE_FROM_ALL tests/gif_dump.c gif_stream.c frame_cache.c)
target_include_directories(gif_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gif_dump PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(gif_dump PRIVATE giflib user32 shell32 ole32)

# Lua host + preset regression test: build/script_test.exe
add_executable(script_test EXCLUDE_FROM_ALL tests/script_test.c script.c monitors.c contacts.c)
target_include_directories(script_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(script_test PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
    YAGP_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts" ${YAGP_LANG_DEFINE})
target_compile_options(script_test PRIVATE /W4 /utf-8)
target_link_libraries(script_test PRIVATE lua user32 shcore)
