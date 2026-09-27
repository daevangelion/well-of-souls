# Check at build time too: fetching the optional bank never requires reconfiguring CMake.
if(EXISTS "${WOS_SOUNDFONT_CACHE}/TimGM6mb.sf2")
    foreach(name TimGM6mb.sf2 TimGM6mb.LICENSE.txt)
        if(NOT EXISTS "${WOS_SOUNDFONT_CACHE}/${name}")
            message(FATAL_ERROR "Incomplete soundfont cache; run tools/fetch_soundfont.sh")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${WOS_SOUNDFONT_CACHE}/${name}" "${WOS_OUTPUT_DIR}/${name}"
            RESULT_VARIABLE result)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Cannot copy ${name} into ${WOS_OUTPUT_DIR}")
        endif()
    endforeach()
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/../src/third_party/TinySoundFont.LICENSE"
    "${WOS_OUTPUT_DIR}/TinySoundFont.LICENSE"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot copy TinySoundFont license")
endif()
