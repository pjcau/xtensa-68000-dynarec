# Musashi 4.5 (vendored) by default; with MUSASHI31_DIR set (docker.sh mounts it
# at /musashi31), mame-go's Musashi 3.1 and the glue mame-go uses
if(DEFINED ENV{MUSASHI31_DIR})
    set(M $ENV{MUSASHI31_DIR})
    set(S ../musashi31)
    idf_component_register(SRCS "main.c" "fuzz.c" "../../../components/m68kjit/glue/glue_musashi31.c"
                                "${S}/musashi31_all.c" "${S}/shim_dasm.c"
                                "${M}/m68kops.c" "${M}/m68kopac.c" "${M}/m68kopdm.c" "${M}/m68kopnz.c"
                           INCLUDE_DIRS "." "${S}" "${M}"
                           REQUIRES m68kjit esp_timer heap
                           LDFRAGMENTS "linker31.lf")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MAMEGO FUZZ_MUSASHI31)
    set_source_files_properties("${S}/musashi31_all.c" "${S}/shim_dasm.c" "${M}/m68kops.c" "${M}/m68kopac.c"
                                "${M}/m68kopdm.c" "${M}/m68kopnz.c" PROPERTIES COMPILE_OPTIONS "-w;-O2")
else()
    set(M ../musashi)
    idf_component_register(SRCS "main.c" "fuzz.c" "glue_musashi.c"
                                "${M}/m68kcpu.c" "${M}/m68kops.c" "${M}/m68kdasm.c" "${M}/softfloat/softfloat.c"
                           INCLUDE_DIRS "." "${M}"
                           REQUIRES m68kjit esp_timer heap
                           LDFRAGMENTS "linker.lf")
    # Musashi is third-party code: its warnings are not ours
    set_source_files_properties("${M}/m68kcpu.c" "${M}/m68kops.c" "${M}/m68kdasm.c" "${M}/softfloat/softfloat.c"
                                PROPERTIES COMPILE_OPTIONS "-w;-O2")
endif()
