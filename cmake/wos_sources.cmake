# Both desktop and Android compile precisely the same portable game and platform sources.
get_filename_component(WOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB WOS_ROOT_SOURCES CONFIGURE_DEPENDS "${WOS_ROOT}/src/*.c")
file(GLOB WOS_ENGINE_SOURCES CONFIGURE_DEPENDS "${WOS_ROOT}/src/engine/*.c")
file(GLOB WOS_GAME_SOURCES CONFIGURE_DEPENDS "${WOS_ROOT}/src/game/*.c")
file(GLOB WOS_PLATFORM_SOURCES CONFIGURE_DEPENDS "${WOS_ROOT}/src/platform/sdl2/*.c")
# The game entry point is src/game_main.c (game_main()); main_sdl2.c forwards SDL startup to it.
set(WOS_SOURCES ${WOS_ENGINE_SOURCES} ${WOS_GAME_SOURCES} ${WOS_PLATFORM_SOURCES})
file(GLOB_RECURSE WOS_THIRD_PARTY CONFIGURE_DEPENDS "${WOS_ROOT}/src/third_party/*.c")
