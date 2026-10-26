set(srcs "shim/emu_shim.c" "shim/emu_mem.c")
set(priv_incs "")
set(mem_io "fopen=emu_mem_fopen;fclose=emu_mem_fclose;fread=emu_mem_fread;fwrite=emu_mem_fwrite;fseek=emu_mem_fseek"
    "ftell=emu_mem_ftell")

if(CONFIG_SVP_EMU_NES)
    file(GLOB nes_srcs "${CMAKE_CURRENT_LIST_DIR}/nofrendo/*.c" "${CMAKE_CURRENT_LIST_DIR}/nofrendo/nes/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/nofrendo/mappers/*.c")
    list(APPEND srcs ${nes_srcs} "ports/nes_port.c")
    list(APPEND priv_incs "nofrendo")
    set_source_files_properties(${nes_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/nofrendo/nes/state.c" PROPERTIES COMPILE_DEFINITIONS "${mem_io}")
endif()

if(CONFIG_SVP_EMU_GB)
    file(GLOB gb_srcs "${CMAKE_CURRENT_LIST_DIR}/gnuboy/*.c")
    list(APPEND srcs ${gb_srcs} "ports/gb_port.c")
    list(APPEND priv_incs "gnuboy")
    set_source_files_properties(${gb_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/gnuboy/gnuboy.c" PROPERTIES COMPILE_DEFINITIONS "${mem_io}")
endif()

if(CONFIG_SVP_EMU_SMS)
    file(GLOB sms_srcs "${CMAKE_CURRENT_LIST_DIR}/smsplus/*.c" "${CMAKE_CURRENT_LIST_DIR}/smsplus/cpu/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/smsplus/sound/*.c")
    list(APPEND srcs ${sms_srcs} "ports/sms_port.c")
    list(APPEND priv_incs "smsplus" "smsplus/cpu" "smsplus/sound")
    set_source_files_properties(${sms_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/smsplus/state.c" "ports/sms_port.c" PROPERTIES
                                COMPILE_DEFINITIONS "${mem_io}")
endif()

if(CONFIG_SVP_EMU_PCE)
    file(GLOB pce_srcs "${CMAKE_CURRENT_LIST_DIR}/pce-go/*.c")
    list(APPEND srcs ${pce_srcs} "ports/pce_port.c")
    list(APPEND priv_incs "pce-go")
    set_source_files_properties(${pce_srcs} PROPERTIES COMPILE_OPTIONS "-O3")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/pce-go/pce-go.c" PROPERTIES COMPILE_DEFINITIONS "${mem_io}")
endif()

if(CONFIG_SVP_EMU_LYNX)
    file(GLOB lynx_srcs "${CMAKE_CURRENT_LIST_DIR}/handy/*.cpp")
    list(APPEND srcs ${lynx_srcs} "ports/lynx_port.cpp")
    list(APPEND priv_incs "handy")
    set_source_files_properties(${lynx_srcs} "ports/lynx_port.cpp" PROPERTIES COMPILE_OPTIONS "-O2;-fno-rtti;-fno-exceptions"
                                COMPILE_DEFINITIONS "${mem_io}")
endif()

if(CONFIG_SVP_EMU_SNES)
    file(GLOB snes_srcs "${CMAKE_CURRENT_LIST_DIR}/snes9x/src/*.c")
    list(APPEND srcs ${snes_srcs} "ports/snes_port.c")
    list(APPEND priv_incs "snes9x" "snes9x/src")
    set_source_files_properties(${snes_srcs} "ports/snes_port.c" PROPERTIES COMPILE_OPTIONS "-Os"
                                COMPILE_DEFINITIONS "RIGHTSHIFT_IS_SAR;FAST_LSB_WORD_ACCESS;NO_ZERO_LUT")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/snes9x/src/snapshot.c" PROPERTIES
                                COMPILE_DEFINITIONS "RIGHTSHIFT_IS_SAR;FAST_LSB_WORD_ACCESS;NO_ZERO_LUT;${mem_io}")
    set(snes_hot "")
    foreach(f cpuexec cpuops getset ppu gfx tile clip dma apu spc700 soundux dsp)
        list(APPEND snes_hot "${CMAKE_CURRENT_LIST_DIR}/snes9x/src/${f}.c")
    endforeach()
    set_source_files_properties(${snes_hot} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

if(CONFIG_SVP_EMU_MD)
    file(GLOB md_srcs "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/bus/*.c" "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/cpus/M68K/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/cpus/Z80/*.c" "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/io/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/savestate/*.c" "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/sound/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/gwenesis/src/vdp/*.c")
    list(APPEND srcs ${md_srcs} "ports/md_port.c")
    list(APPEND priv_incs "gwenesis" "gwenesis/src/bus" "gwenesis/src/cpus/M68K" "gwenesis/src/cpus/Z80" "gwenesis/src/io"
         "gwenesis/src/savestate" "gwenesis/src/sound" "gwenesis/src/vdp")
    set_source_files_properties(${md_srcs} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

if(CONFIG_SVP_EMU_GW)
    file(GLOB gw_srcs "${CMAKE_CURRENT_LIST_DIR}/gw-emulator/src/*.c" "${CMAKE_CURRENT_LIST_DIR}/gw-emulator/src/cpus/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/gw-emulator/src/gw_sys/*.c")
    list(APPEND srcs ${gw_srcs} "ports/gw_port.c")
    list(APPEND priv_incs "gw-emulator/src" "gw-emulator/src/cpus" "gw-emulator/src/gw_sys")
    set_source_files_properties(${gw_srcs} PROPERTIES COMPILE_OPTIONS "-O2" COMPILE_DEFINITIONS "ROM_DATA=gw_ROM_DATA")
    set_source_files_properties("ports/gw_port.c" PROPERTIES COMPILE_DEFINITIONS "ROM_DATA=gw_ROM_DATA")
endif()

if(CONFIG_SVP_EMU_MSX)
    file(GLOB msx_srcs "${CMAKE_CURRENT_LIST_DIR}/fmsx/src/EMULib/*.c" "${CMAKE_CURRENT_LIST_DIR}/fmsx/src/fMSX/*.c"
         "${CMAKE_CURRENT_LIST_DIR}/fmsx/src/Z80/*.c")
    list(APPEND srcs ${msx_srcs} "ports/msx_port.c")
    list(APPEND priv_incs "fmsx" "fmsx/src/EMULib" "fmsx/src/fMSX" "fmsx/src/Z80")
    set(msx_names "CPU=fmsx_CPU;LoadFile=fmsx_LoadFile;LoadROM=fmsx_LoadROM;ResetZ80=fmsx_ResetZ80;ExecZ80=fmsx_ExecZ80"
        "IntZ80=fmsx_IntZ80;RunZ80=fmsx_RunZ80;RdZ80=fmsx_RdZ80;WrZ80=fmsx_WrZ80;InZ80=fmsx_InZ80;OutZ80=fmsx_OutZ80"
        "LoopZ80=fmsx_LoopZ80;PatchZ80=fmsx_PatchZ80;DebugZ80=fmsx_DebugZ80;OpZ80=fmsx_OpZ80"
        "SaveState=fmsx_SaveState;LoadState=fmsx_LoadState;VRAM=fmsx_VRAM")
    set(msx_quote "-iquote;${CMAKE_CURRENT_LIST_DIR}/fmsx/src/Z80;-iquote;${CMAKE_CURRENT_LIST_DIR}/fmsx/src/fMSX"
        "-iquote;${CMAKE_CURRENT_LIST_DIR}/fmsx/src/EMULib")
    set_source_files_properties(${msx_srcs} PROPERTIES COMPILE_OPTIONS "-O2;-include;${CMAKE_CURRENT_LIST_DIR}/fmsx/msxfix.h;${msx_quote}"
                                COMPILE_DEFINITIONS "BPS16;UNIX;LSB_FIRST;NARROW;${msx_names}")
    set_source_files_properties("ports/msx_port.c" PROPERTIES COMPILE_OPTIONS "-O2;${msx_quote}" COMPILE_DEFINITIONS "${msx_names}")
endif()

idf_component_register(SRCS ${srcs} INCLUDE_DIRS "shim" "ports" PRIV_INCLUDE_DIRS ${priv_incs} PRIV_REQUIRES esp_rom
                       LDFRAGMENTS linker.lf)

target_compile_definitions(${COMPONENT_LIB} PRIVATE RETRO_GO)
target_compile_options(${COMPONENT_LIB} PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/shim/emu_force.h" -w -Wno-error)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>)
