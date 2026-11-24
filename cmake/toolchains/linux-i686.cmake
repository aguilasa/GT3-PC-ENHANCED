# Native 32-bit x86 Linux build (i686, SysV ABI) with Clang or GCC and the
# multilib runtime (gcc-multilib, the -dev:i386 packages):
#
#   cmake -S . -B build-linux -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-i686.cmake
#
# With no -DCMAKE_BUILD_TYPE the build is RelWithDebInfo (CMakeLists.txt),
# as build.cmd builds on Windows; -DCMAKE_BUILD_TYPE=Debug for an unoptimised one.
#
# The compiler defaults to Clang; GCC (or another Clang) is chosen on the
# command line:
#
#   cmake -S . -B build-linux-gcc -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-i686.cmake \
#         -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
#
# The game stays 32-bit: its data files carry 32-bit pointers that it
# relocates in place (CMakeLists.txt checks CMAKE_SIZEOF_VOID_P).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()

set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_ASM_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")

# The 32-bit libraries and their pkg-config files (Debian/Ubuntu multiarch).
set(CMAKE_LIBRARY_ARCHITECTURE i386-linux-gnu)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig)
