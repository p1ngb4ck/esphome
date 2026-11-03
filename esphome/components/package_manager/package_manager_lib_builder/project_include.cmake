if(EXISTS "${CMAKE_SOURCE_DIR}/component_libs/manifest.json")
    idf_build_get_property(python PYTHON)
    add_custom_target(component_as_lib_build ALL
        COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/tools/build_libs.py
                "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}"
        DEPENDS gen_project_binary
        VERBATIM
        COMMENT "component_as_lib: packages")
endif()
