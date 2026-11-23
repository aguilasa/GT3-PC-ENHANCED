# Tests of the POSIX side of the platform layer (src/platform/os), built on
# their own, outside the game's build:
#
#   cmake -S tools/platform_tests -B build-platform-tests -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-i686.cmake
#   cmake --build build-platform-tests
#   ctest --test-dir build-platform-tests --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(gt3_platform_tests CXX)

if(WIN32)
    message(FATAL_ERROR "platform_tests check the POSIX implementations; Windows has the real calls")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
get_filename_component(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

enable_testing()

add_executable(ini_test ini_test.cpp ${GT3PC_ROOT}/src/platform/os/ini_posix.cpp)
target_include_directories(ini_test PRIVATE ${GT3PC_ROOT}/src)
target_compile_options(ini_test PRIVATE -Wall -Wextra)
add_test(NAME ini COMMAND ini_test)
