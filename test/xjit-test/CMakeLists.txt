# xjit-test: standalone test app for the shared Xtensa JIT core (components/xjit).
# Not part of rg_tool's apps: build it with idf.py, run it in QEMU or on the board.
# See website/docs/next-steps/jit-plan.md.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/xjit")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xjit-test)
