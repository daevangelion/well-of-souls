cmake_minimum_required(VERSION 3.16)
project(wos VERSION 0.1 LANGUAGES C)

option(WOS_WERROR "Treat warnings in project code as errors" OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(SDL2 CONFIG QUIET)
if(NOT TARGET SDL2::SDL2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
endif()

include(cmake/wos_sources.cmake)
add_executable(wos ${WOS_ROOT_SOURCES} ${WOS_SOURCES} ${WOS_THIRD_PARTY})
target_include_directories(wos PRIVATE src)
foreach(source IN LISTS WOS_ROOT_SOURCES WOS_SOURCES)
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra)
    if(WOS_WERROR)
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -Werror)
    endif()
endforeach()
if(TARGET SDL2::SDL2main)
    target_link_libraries(wos PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(wos PRIVATE SDL2::SDL2 m)

add_custom_target(wos_soundfont ALL
    COMMAND ${CMAKE_COMMAND} "-DWOS_SOUNDFONT_CACHE=$ENV{HOME}/.cache/wos-soundfont"
            "-DWOS_OUTPUT_DIR=$<TARGET_FILE_DIR:wos>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_soundfont.cmake")
add_dependencies(wos_soundfont wos)

if(WIN32)
    if(WOS_SDL2_DLL)
        add_custom_command(TARGET wos POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WOS_SDL2_DLL}" "$<TARGET_FILE_DIR:wos>/SDL2.dll")
    else()
        add_custom_command(TARGET wos POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>" "$<TARGET_FILE_DIR:wos>")
    endif()
endif()

# --- Self-tests -------------------------------------------------------------
# Each self-test owns its own main() and links only the modules it exercises, so it
# excludes the SDL entry point (main_sdl2.c) and the front-end/hero modules that pull in
# game_main()'s globals. The engine, platform and third-party code is the same the game
# runs, so a self-test exercises the exact shipping engine.
option(WOS_BUILD_TESTS "Build the self-test executables" ON)
set(WOS_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extracted" CACHE PATH
    "Extracted retail data root (from tools/extract_installer.sh); self-tests read it")

if(WOS_BUILD_TESTS)
    set(WOS_SELFTEST_PLATFORM ${WOS_PLATFORM_SOURCES})
    list(FILTER WOS_SELFTEST_PLATFORM EXCLUDE REGEX "main_sdl2\\.c$")
    set(WOS_SELFTEST_COMMON ${WOS_ENGINE_SOURCES} ${WOS_SELFTEST_PLATFORM} ${WOS_THIRD_PARTY})
    add_executable(engine_selftest tests/engine_selftest.c ${WOS_SELFTEST_COMMON})
    add_executable(world_selftest tests/world_selftest.c
        ${WOS_ROOT}/src/game/world.c ${WOS_SELFTEST_COMMON})
    foreach(selftest engine_selftest world_selftest)
        target_include_directories(${selftest} PRIVATE src)
        # The self-tests are assert-driven: every check is an assert(). Release build
        # types define NDEBUG, which would compile the checks out and let a failed
        # invariant surface later as a NULL dereference instead of a clear failure.
        # -UNDEBUG keeps assert() live in the test translation unit in every config.
        set_property(SOURCE tests/${selftest}.c APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -UNDEBUG)
        if(WOS_WERROR)
            set_property(SOURCE tests/${selftest}.c APPEND PROPERTY COMPILE_OPTIONS -Werror)
        endif()
        target_link_libraries(${selftest} PRIVATE SDL2::SDL2 m)
    endforeach()
    # editors_selftest drives the authoring editors and the chat dispatcher against a scratch
    # copy of the retail data. It needs game_main.c's globals (game_data_path) and every game
    # module (editors.c calls game_go_front), so it links the whole game except the SDL entry
    # point and supplies its own main(). It must sit OUTSIDE the loop above, or the
    # target is created once per iteration and add_executable fails on the duplicate.
    set(WOS_SELFTEST_GAME ${WOS_ROOT_SOURCES} ${WOS_GAME_SOURCES})
    add_executable(editors_selftest tests/editors_selftest.c
        ${WOS_SELFTEST_GAME} ${WOS_SELFTEST_COMMON})
    target_include_directories(editors_selftest PRIVATE src)
    set_property(SOURCE tests/editors_selftest.c APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -UNDEBUG)
    if(WOS_WERROR)
        set_property(SOURCE tests/editors_selftest.c APPEND PROPERTY COMPILE_OPTIONS -Werror)
    endif()
    target_link_libraries(editors_selftest PRIVATE SDL2::SDL2 m)
    # The self-tests assert against the extracted retail data. Register them as ctest
    # cases only when that data is present: a data-less CI run still compiles and links
    # them (regression coverage) but does not report failures it cannot legitimately judge.
    enable_testing()
    if(EXISTS "${WOS_DATA_DIR}/worlds/Evergreen")
        foreach(selftest engine_selftest world_selftest)
            add_test(NAME ${selftest} COMMAND ${selftest} "${WOS_DATA_DIR}")
        endforeach()
        add_test(NAME editors_selftest COMMAND editors_selftest "${WOS_DATA_DIR}")
        message(STATUS "wos: retail data at ${WOS_DATA_DIR}; self-tests registered with ctest")
    else()
        message(STATUS "wos: no retail data at ${WOS_DATA_DIR}; self-tests built but not registered")
    endif()
endif()

# --- Runtime installer decoder ------------------------------------------------
# The native Clickteam-packer decoder (cic_unpack.c) links the vendored miniz +
# bzip2 codecs. It is NOT part of the game: the game binary stays codec-free, and
# this decoder backs the on-device "supply the installer" flow (and its host test).
if(WOS_BUILD_TESTS)
    add_executable(cic_unpack_selftest tests/cic_unpack_selftest.c
        src/platform/sdl2/cic_unpack.c src/platform/sdl2/files_sdl2.c ${WOS_CODEC_SOURCES})
    target_include_directories(cic_unpack_selftest PRIVATE
        src src/platform/sdl2 src/platform/codec)
    foreach(source src/platform/sdl2/cic_unpack.c src/platform/sdl2/files_sdl2.c tests/cic_unpack_selftest.c)
        # miniz.h exposes unused static compress helpers into the decoder TU.
        set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -Wno-unused-function)
        if(WOS_WERROR)
            set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS -Werror)
        endif()
    endforeach()
    # The vendored bzip2 sources use their own conventions and POSIX APIs (fdopen);
    # expose POSIX and silence their warnings so they build under our strict C99.
    foreach(source ${WOS_CODEC_SOURCES})
        set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS -w -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE)
    endforeach()

    # install_selftest additionally exercises the orchestration layer (file/URL/memory
    # obtain + decode + install). It links SDL2 for the URL reader.
    add_executable(install_selftest tests/install_selftest.c
        src/platform/sdl2/install.c src/platform/sdl2/cic_unpack.c
        src/platform/sdl2/files_sdl2.c ${WOS_CODEC_SOURCES})
    target_include_directories(install_selftest PRIVATE
        src src/platform/sdl2 src/platform/codec)
    foreach(source src/platform/sdl2/install.c tests/install_selftest.c)
        set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -Wno-unused-function)
        if(WOS_WERROR)
            set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS -Werror)
        endif()
    endforeach()
    target_link_libraries(install_selftest PRIVATE m)
endif()
