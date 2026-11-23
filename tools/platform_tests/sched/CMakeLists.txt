# The game-thread scheduler and the FPU mode on POSIX (src/platform/scheduler.cpp,
# src/platform/fpu.h), built on their own, outside the game's build:
#
#   cmake -S tools/platform_tests/sched -B build-sched-test -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-i686.cmake
#   cmake --build build-sched-test
#   ctest --test-dir build-sched-test --output-on-failure
#
# The Windows build has its own scheduler test (tools/schedtest).
cmake_minimum_required(VERSION 3.20)
project(gt3_sched_test CXX)

if(WIN32)
    message(FATAL_ERROR "sched_test checks the POSIX scheduler; tools/schedtest is the Windows one")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
get_filename_component(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)

find_package(Threads REQUIRED)
# SDL3 (i686, under ~/opt/i386): the timer thread's priority (raise_timer_priority).
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    find_package(SDL3 CONFIG REQUIRED PATHS $ENV{HOME}/opt/i386 NO_DEFAULT_PATH)
endif()
enable_testing()

add_executable(sched_test
    sched_test.cpp
    ${GT3PC_ROOT}/src/platform/scheduler.cpp
    ${GT3PC_ROOT}/src/platform/ee_kernel.cpp
)
target_include_directories(sched_test PRIVATE ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_compile_options(sched_test PRIVATE -Wall -Wextra)
target_link_libraries(sched_test PRIVATE SDL3::SDL3 Threads::Threads)
add_test(NAME sched COMMAND sched_test)
