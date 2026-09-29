# The gbsp app's gpSP sources, compiled here with the same flags (no retro-go component).
set(G ${CMAKE_CURRENT_LIST_DIR}/../../../gbsp/components/gbsp-libretro)
idf_component_register(
    SRCS ${G}/cheats.c ${G}/cpu.cpp ${G}/gba_cc_lut.c ${G}/gba_memory.c ${G}/gbp.c ${G}/input.c
         ${G}/main.c ${G}/memmap.c ${G}/rfu.c ${G}/savestate.c ${G}/serial.c ${G}/sound.c ${G}/video.cpp
    INCLUDE_DIRS ${G} ${G}/libretro/libretro-common/include stub
    REQUIRES esp_timer heap freertos)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -fomit-frame-pointer -ffast-math -w)
target_compile_definitions(${COMPONENT_LIB} PUBLIC OVERCLOCK_60FPS ROM_BUFFER_SIZE=8 RETRO_GO=1)
