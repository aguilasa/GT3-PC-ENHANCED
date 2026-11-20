# Cross-build of the Windows (x86, MSVC ABI) executables from Linux with
# LLVM's clang-cl, lld-link, llvm-rc, llvm-lib and llvm-mt over the Microsoft
# CRT and Windows SDK unpacked by xwin (https://github.com/Jake-Shadle/xwin,
# `xwin --arch x86 splat --output <dir>`).  Used only when passed explicitly:
#
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-clang-cl.cmake
#
# Cache variables (set them on the command line to override):
#   GT3PC_LLVM_BIN  the folder of the LLVM tools (default: /usr/lib/llvm-18/bin)
#   GT3PC_XWIN_DIR  xwin's splat output (default: $HOME/opt/xwin, or $XWIN_DIR)
#
# tools/lang_sync is built for Windows here and cannot run on the build
# machine: unless CMAKE_CROSSCOMPILING_EMULATOR is given (for instance a Wine
# launcher), the sync step is skipped and the catalogues in lang/ are built in
# as they are.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR X86)

set(GT3PC_LLVM_BIN /usr/lib/llvm-18/bin CACHE PATH "Folder of clang-cl, lld-link, llvm-rc, llvm-lib and llvm-mt")
if(DEFINED ENV{XWIN_DIR})
    set(xwin_default $ENV{XWIN_DIR})
else()
    set(xwin_default $ENV{HOME}/opt/xwin)
endif()
set(GT3PC_XWIN_DIR ${xwin_default} CACHE PATH "xwin splat output: crt/ and sdk/")
# try_compile projects see the same values
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES GT3PC_LLVM_BIN GT3PC_XWIN_DIR)

set(triple i686-pc-windows-msvc)
set(CMAKE_C_COMPILER ${GT3PC_LLVM_BIN}/clang-cl)
set(CMAKE_CXX_COMPILER ${GT3PC_LLVM_BIN}/clang-cl)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_LINKER ${GT3PC_LLVM_BIN}/lld-link CACHE FILEPATH "")
set(CMAKE_AR ${GT3PC_LLVM_BIN}/llvm-lib CACHE FILEPATH "")
set(CMAKE_RC_COMPILER ${GT3PC_LLVM_BIN}/llvm-rc CACHE FILEPATH "")
set(CMAKE_MT ${GT3PC_LLVM_BIN}/llvm-mt CACHE FILEPATH "")

# The CRT and SDK headers as system headers (/imsvc), the import libraries by
# /LIBPATH.  xwin's splat adds lower and mixed case links of the headers and
# libraries, so the SDK's own spellings resolve on a case-sensitive disk.
set(x ${GT3PC_XWIN_DIR})
set(includes crt/include sdk/include/ucrt sdk/include/um sdk/include/shared)
set(cflags "")
set(rcflags "")
foreach(dir ${includes})
    string(APPEND cflags " /imsvc ${x}/${dir}")
    string(APPEND rcflags " -I ${x}/${dir}")
endforeach()
# The STL of the xwin download checks for a newer clang than the host's
# (STL1000, "expected Clang 19.0.0 or newer"); clang 18 builds it.
string(APPEND cflags " /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
# __is_pointer is a clang type-trait keyword; the egcs runtime declares a
# function of that name (include/cxxrt.h, include/runtime/cxx/T549_tinfo2.h:
# "expected unqualified-id").  Nothing defines or calls it in the port, so
# the macro only renames the declarations.
string(APPEND cflags " /D__is_pointer=gt3pc_egcs_is_pointer")
# cl.exe targets SSE2 on x86 by default (/arch:SSE2); clang-cl's i686 default
# is a plain i686, without the SSE2 intrinsics the renderer uses (__m128i,
# _mm_set1_epi32).  (clang-cl 18's /arch:SSE2 does not define __SSE2__.)
string(APPEND cflags " -msse2")
set(CMAKE_C_FLAGS_INIT "${cflags}")
# include/pc_prelude.h maps the decomp's GNU spellings for MSVC, among them
# `#define __attribute__(x)`, which empties the attributes of clang's own
# intrinsic headers (mmintrin.h ... immintrin.h: "cannot initialize a
# parameter of type '__attribute__((__vector_size__...' ").  Including
# <intrin.h> (all the x86 intrinsics) before the prelude reads them intact;
# later includes find them done.
set(CMAKE_CXX_FLAGS_INIT "${cflags} /FIintrin.h")
set(CMAKE_RC_FLAGS_INIT "${rcflags}")

# Library names asked for in a spelling xwin does not provide (it adds the
# lower and upper case ones): `#pragma comment(lib, "Synchronization.lib")`
# (src/engine/pgl/native/pgl_queue.cpp) is linked through a link in the
# build folder.
set(case_dir ${CMAKE_BINARY_DIR}/xwin-case)
foreach(name Synchronization.lib)
    string(TOLOWER ${name} lower)
    if(NOT EXISTS ${case_dir}/${name} AND EXISTS ${x}/sdk/lib/um/x86/${lower})
        file(MAKE_DIRECTORY ${case_dir})
        file(CREATE_LINK ${x}/sdk/lib/um/x86/${lower} ${case_dir}/${name} SYMBOLIC)
    endif()
endforeach()

set(ldflags "/LIBPATH:${x}/crt/lib/x86 /LIBPATH:${x}/sdk/lib/um/x86 /LIBPATH:${x}/sdk/lib/ucrt/x86 /LIBPATH:${case_dir}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ldflags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${ldflags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${ldflags}")

# xwin downloads no debug CRT (msvcrtd.lib, libcmtd.lib): the compiler checks
# link in Release, and the Debug configuration does not link here.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Find libraries and headers in the target's tree, programs on the host.
set(CMAKE_FIND_ROOT_PATH ${x})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# A Windows executable the build runs (the sync of lang/): CMake prefixes
# custom commands naming an executable target with this.  The default
# skips the step (sh -c: $4 is the --build folder), leaving the stamp the
# resources depend on.  (No ';' in the script: it would split the list.)
if(NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        /bin/sh -c "echo \"lang_sync skipped (cross-build): lang/*.po built in as they are\" && ( [ -e \"$4/languages.stamp\" ] || : > \"$4/languages.stamp\" )")
endif()
