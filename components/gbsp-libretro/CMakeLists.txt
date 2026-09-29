set(COMPONENT_SRCDIRS ".")
set(COMPONENT_ADD_INCLUDEDIRS ". libretro/libretro-common/include")
set(COMPONENT_REQUIRES "retro-go")
# `GBAJIT=1 python rg_tool.py ... build gbsp`: gpSP's dynarec with the Xtensa
# backend (xtensa/, esp32-emu-turbo) instead of the interpreter
if(DEFINED ENV{GBAJIT} AND NOT "$ENV{GBAJIT}" STREQUAL "0")
    list(APPEND COMPONENT_SRCDIRS "xtensa")
    set(COMPONENT_REQUIRES "retro-go xjit esp_mm")
endif()
register_component()
rg_setup_compile_options(
    -O3
    -fomit-frame-pointer -ffast-math
    -DOVERCLOCK_60FPS
    -DROM_BUFFER_SIZE=8
    -Wno-unused-variable
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-error=unused-label
    -Wno-unused-label
)
if(DEFINED ENV{GBAJIT} AND NOT "$ENV{GBAJIT}" STREQUAL "0")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HAVE_DYNAREC XTENSA_ARCH SMALL_TRANSLATION_CACHE)
    # the translator expands the emitter in thousands of places: size, and
    # literals next to the code (Xtensa l32r reaches 256 KB back)
    set_source_files_properties(cpu_threaded.c PROPERTIES COMPILE_OPTIONS "-Os;-mtext-section-literals")
endif()
