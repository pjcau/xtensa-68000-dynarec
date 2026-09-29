# xjit: shared Xtensa JIT core (emitter, blocks, executable memory).
# See website/docs/next-steps/jit-plan.md; tested by retro-go/xjit-test and test/.
idf_component_register(SRCS "xjit_block.c" "xjit_exec.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_mm heap)
