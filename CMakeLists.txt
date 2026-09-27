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
