# gbajit-test: gpSP (the gbsp sources, unchanged) in QEMU or on the board, with a ROM and a
# level state from flash partitions, printing frame/audio hashes to compare with the PC
# interpreter. JIT step 5 test bench (website/docs/next-steps/jit-plan.md).
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/xjit" "${CMAKE_CURRENT_LIST_DIR}/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gbajit-test)
