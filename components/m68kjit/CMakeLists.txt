# m68kjit: 68000 dynarec on xjit, on top of a Musashi interpreter (see include/m68kjit.h)
idf_component_register(SRCS "m68kjit.c" "m68kjit_len.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES xjit heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
