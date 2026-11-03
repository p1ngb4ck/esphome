set(cal_libs "${CMAKE_SOURCE_DIR}/component_libs")
set(cal_symbols "${cal_libs}/component_as_lib_symbols.c")
set(cal_sources)

if(NOT CMAKE_SCRIPT_MODE_FILE AND EXISTS "${cal_libs}/manifest.json")
    idf_build_get_property(python PYTHON)
    file(GLOB_RECURSE cal_inputs
        "${cal_libs}/*.cpp" "${cal_libs}/*.c" "${cal_libs}/*.h" "${cal_libs}/*.hpp")
    list(REMOVE_ITEM cal_inputs "${cal_symbols}")
    add_custom_command(
        OUTPUT "${cal_symbols}"
        COMMAND ${python} "${CMAKE_CURRENT_LIST_DIR}/tools/build_libs.py" prelink
                "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
        DEPENDS ${cal_inputs} "${cal_libs}/manifest.json"
        VERBATIM
        COMMENT "component_as_lib: packages and the symbols they need")
    set(cal_sources "${cal_symbols}")
endif()

idf_component_register(
    SRCS ${cal_sources}
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
)
