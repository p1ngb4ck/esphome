set(srcs "shim/emu_shim.c")
set(priv_incs "")

if(CONFIG_SVP_EMU_NES)
    file(GLOB nes_srcs "${CMAKE_CURRENT_LIST_DIR}/nofrendo/*.c" "${CMAKE_CURRENT_LIST_DIR}/nofrendo/nes/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/nofrendo/mappers/*.c")
    list(APPEND srcs ${nes_srcs} "ports/nes_port.c")
    list(APPEND priv_incs "nofrendo")
    set_source_files_properties(${nes_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "shim" "ports" PRIV_INCLUDE_DIRS ${priv_incs} PRIV_REQUIRES esp_rom
                       LDFRAGMENTS linker.lf)

target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO)
target_compile_options(${COMPONENT_LIB} PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/shim/emu_force.h" -w -Wno-error)
