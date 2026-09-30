# The gbsp app's gpSP sources, compiled here with the same flags (no retro-go component).
# GBAJIT=1 in the environment at configure time: the dynarec with the Xtensa backend
# (cpu_threaded.c + xtensa/xtensa_stub.c) instead of the interpreter.
set(G ${CMAKE_CURRENT_LIST_DIR}/../../../../components/gbsp-libretro)
set(SRCS ${G}/cheats.c ${G}/cpu.cpp ${G}/gba_cc_lut.c ${G}/gba_memory.c ${G}/gbp.c ${G}/input.c
         ${G}/main.c ${G}/memmap.c ${G}/rfu.c ${G}/savestate.c ${G}/serial.c ${G}/sound.c ${G}/video.cpp)
if(DEFINED ENV{GBAJIT})
    list(APPEND SRCS ${G}/cpu_threaded.c ${G}/xtensa/xtensa_stub.c)
endif()
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${G} ${G}/libretro/libretro-common/include stub
    REQUIRES esp_timer heap freertos xjit esp_mm)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fomit-frame-pointer -ffast-math -w)
target_compile_definitions(${COMPONENT_LIB} PUBLIC OVERCLOCK_60FPS RETRO_GO=1)
if(DEFINED ENV{GBAJIT})
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HAVE_DYNAREC XTENSA_ARCH
        SMALL_TRANSLATION_CACHE ROM_BUFFER_SIZE=4)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ROM_BUFFER_SIZE=8)
endif()
if(DEFINED ENV{GBAJIT})
    # the translator expands the emitter inline thousands of times: size, and
    # literals next to the code (translate_block_arm alone is ~350 KB at -O3)
    set_source_files_properties(${G}/cpu_threaded.c PROPERTIES COMPILE_OPTIONS "-Os;-mtext-section-literals")
endif()
