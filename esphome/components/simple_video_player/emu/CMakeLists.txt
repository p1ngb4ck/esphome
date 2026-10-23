set(srcs "shim/emu_shim.c")
set(priv_incs "")

if(CONFIG_SVP_EMU_NES)
    file(GLOB nes_srcs "${CMAKE_CURRENT_LIST_DIR}/nofrendo/*.c" "${CMAKE_CURRENT_LIST_DIR}/nofrendo/nes/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/nofrendo/mappers/*.c")
    list(APPEND srcs ${nes_srcs} "ports/nes_port.c")
    list(APPEND priv_incs "nofrendo")
    set_source_files_properties(${nes_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
endif()

if(CONFIG_SVP_EMU_GB)
    file(GLOB gb_srcs "${CMAKE_CURRENT_LIST_DIR}/gnuboy/*.c")
    list(APPEND srcs ${gb_srcs} "ports/gb_port.c")
    list(APPEND priv_incs "gnuboy")
    set_source_files_properties(${gb_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
endif()

if(CONFIG_SVP_EMU_SMS)
    file(GLOB sms_srcs "${CMAKE_CURRENT_LIST_DIR}/smsplus/*.c" "${CMAKE_CURRENT_LIST_DIR}/smsplus/cpu/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/smsplus/sound/*.c")
    list(APPEND srcs ${sms_srcs} "ports/sms_port.c")
    list(APPEND priv_incs "smsplus" "smsplus/cpu" "smsplus/sound")
    set_source_files_properties(${sms_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
endif()

if(CONFIG_SVP_EMU_PCE)
    file(GLOB pce_srcs "${CMAKE_CURRENT_LIST_DIR}/pce-go/*.c")
    list(APPEND srcs ${pce_srcs} "ports/pce_port.c")
    list(APPEND priv_incs "pce-go")
    set_source_files_properties(${pce_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
endif()

if(CONFIG_SVP_EMU_LYNX)
    file(GLOB lynx_srcs "${CMAKE_CURRENT_LIST_DIR}/handy/*.cpp")
    list(APPEND srcs ${lynx_srcs} "ports/lynx_port.cpp")
    list(APPEND priv_incs "handy")
    set_source_files_properties(${lynx_srcs} "ports/lynx_port.cpp" PROPERTIES COMPILE_OPTIONS "-O2;-fno-rtti;-fno-exceptions")
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "shim" "ports" PRIV_INCLUDE_DIRS ${priv_incs} PRIV_REQUIRES esp_rom
                       LDFRAGMENTS linker.lf)

target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO)
target_compile_options(${COMPONENT_LIB} PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/shim/emu_force.h" -w -Wno-error)
