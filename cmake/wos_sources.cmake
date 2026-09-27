# Both desktop and Android compile precisely the same portable game and platform sources.
get_filename_component(WOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB WOS_ROOT_SOURCES CONFIGURE_DEPENDS "${WOS_ROOT}/src/*.c")
file(GLOB_RECURSE WOS_SOURCES CONFIGURE_DEPENDS
    "${WOS_ROOT}/src/engine/*.c" "${WOS_ROOT}/src/game/*.c" "${WOS_ROOT}/src/platform/sdl2/*.c")
file(GLOB_RECURSE WOS_THIRD_PARTY CONFIGURE_DEPENDS "${WOS_ROOT}/src/third_party/*.c")
