set(M ../musashi)
idf_component_register(SRCS "main.c" "fuzz.c" "glue_musashi.c"
                            "${M}/m68kcpu.c" "${M}/m68kops.c" "${M}/m68kdasm.c" "${M}/softfloat/softfloat.c"
                       INCLUDE_DIRS "." "${M}"
                       REQUIRES m68kjit esp_timer heap
                       LDFRAGMENTS "linker.lf")
# Musashi is third-party code: its warnings are not ours
set_source_files_properties("${M}/m68kcpu.c" "${M}/m68kops.c" "${M}/m68kdasm.c" "${M}/softfloat/softfloat.c"
                            PROPERTIES COMPILE_OPTIONS "-w;-O2")
