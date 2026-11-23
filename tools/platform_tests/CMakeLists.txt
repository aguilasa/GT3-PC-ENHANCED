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

add_executable(fs_test fs_test.cpp ${GT3PC_ROOT}/src/platform/os/fs_posix.cpp)
target_include_directories(fs_test PRIVATE ${GT3PC_ROOT}/src)
target_compile_options(fs_test PRIVATE -Wall -Wextra)
add_test(NAME fs COMMAND fs_test)

# EUC-JP as Windows' code page 20932 has it (src/platform/os/eucjp.cpp): the
# samples of the Windows build's databases, the code page's NLS file (Wine's:
# a Bottles prefix has one; none: that part is skipped) and glibc's iconv.
set(GT3PC_CP20932_NLS "$ENV{HOME}/.var/app/com.usebottles.bottles/data/bottles/bottles/gt3pc/drive_c/windows/syswow64/c_20932.nls"
    CACHE FILEPATH "code page 20932's NLS file (c_20932.nls) eucjp_test compares with; - for none")
if(NOT GT3PC_CP20932_NLS STREQUAL "-" AND NOT EXISTS "${GT3PC_CP20932_NLS}")
    message(STATUS "eucjp_test: no ${GT3PC_CP20932_NLS}; the NLS file part is skipped")
    set(GT3PC_CP20932_NLS "-" CACHE FILEPATH "" FORCE)
endif()
add_executable(eucjp_test eucjp_test.cpp ${GT3PC_ROOT}/src/platform/os/eucjp.cpp)
target_include_directories(eucjp_test PRIVATE ${GT3PC_ROOT}/src)
target_compile_options(eucjp_test PRIVATE -Wall -Wextra)
add_test(NAME eucjp COMMAND eucjp_test ${CMAKE_CURRENT_SOURCE_DIR}/eucjp_samples.txt ${GT3PC_CP20932_NLS})
