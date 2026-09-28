cmake_minimum_required(VERSION 3.22)
project(wos_android LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${SDL2_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/sdl2")
include("${CMAKE_CURRENT_LIST_DIR}/../../../../../cmake/wos_sources.cmake")
# The runtime "supply the installer" flow: the native CIC decoder + install layer +
# the vendored miniz/bzip2 codecs, linked only into the app (not the desktop game).
add_library(main SHARED ${WOS_ROOT_SOURCES} ${WOS_SOURCES} ${WOS_THIRD_PARTY}
    "${WOS_ROOT}/src/platform/sdl2/install.c"
    "${WOS_ROOT}/src/platform/sdl2/cic_unpack.c"
    ${WOS_CODEC_SOURCES})
target_include_directories(main PRIVATE
    "${WOS_ROOT}/src" "${WOS_ROOT}/src/platform/sdl2" "${WOS_ROOT}/src/third_party/codec")
foreach(source IN LISTS WOS_ROOT_SOURCES WOS_SOURCES)
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra)
endforeach()
foreach(source "${WOS_ROOT}/src/platform/sdl2/install.c" "${WOS_ROOT}/src/platform/sdl2/cic_unpack.c")
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -Wall -Wextra -Wno-unused-function)
endforeach()
foreach(source ${WOS_CODEC_SOURCES})
    set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_OPTIONS -w -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE)
endforeach()
target_link_libraries(main PRIVATE SDL2::SDL2 m)
