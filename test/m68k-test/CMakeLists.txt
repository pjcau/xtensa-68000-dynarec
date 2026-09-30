# m68k-test: the 68000 frontend (components/m68kjit) against Musashi 4.5 on random
# machines, in QEMU or on the board (main/fuzz.c). Host-only checks: host/run_host_checks.sh.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/xjit" "${CMAKE_CURRENT_LIST_DIR}/../../components/m68kjit")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m68k-test)
