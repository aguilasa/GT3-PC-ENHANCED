# FFmpeg (LGPL v2.1 or later): the MP4 movies' decoders off Windows
# (src/engine/media/native/mp4_movie_ffmpeg.cpp; Windows has Media
# Foundation).  The release tarball of a pinned version is downloaded into
# the build tree, checked against its SHA-256 (the tarball's PGP signature,
# ffmpeg-7.1.2.tar.xz.asc, is by the FFmpeg release signing key FCF9 86EA
# 15E6 E293 A564 4F10 B432 2F04 D676 58D8), and built there as static
# libraries with the project's compiler: only what an .mp4 (H.264, AAC or
# MP3 audio) and an .mp3 need, no GPL or non-free part, nothing detected
# from the system (no shared library of FFmpeg's own or its dependencies).
# FFmpeg's 32-bit x86 assembly (nasm's and inline) is not
# position-independent, so the programs (PIEs) would link with text
# relocations (DT_TEXTREL): by default FFmpeg is built without it
# (--disable-asm, plain C, about half the decoding speed).
# GT3PC_FFMPEG_ASM=ON keeps it (needs nasm) and accepts the text
# relocations.  Its licence
# (LICENSE.md, COPYING.LGPLv2.1) is copied next to the programs that link it
# (gt3pc_ffmpeg_licence).
#
#   include(${GT3PC_ROOT}/cmake/ffmpeg.cmake)
#   target_link_libraries(mytarget PRIVATE gt3_ffmpeg)
if(TARGET gt3_ffmpeg)
    return()
endif()
include(ExternalProject)

set(GT3PC_FFMPEG_VERSION 7.1.2)
set(GT3PC_FFMPEG_SHA256 089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304)
set(GT3PC_FFMPEG_DEPS ${CMAKE_BINARY_DIR}/deps)
set(GT3PC_FFMPEG_PREFIX ${GT3PC_FFMPEG_DEPS}/ffmpeg)
set(GT3PC_FFMPEG_SRC ${GT3PC_FFMPEG_DEPS}/ffmpeg-${GT3PC_FFMPEG_VERSION})

# The compiler as the project's (Clang or GCC, -m32 from the toolchain) and
# the platform layer's SSE2 floating point.
set(cflags "${CMAKE_C_FLAGS} -msse2 -mfpmath=sse")
string(STRIP "${cflags}" cflags)
set(ldflags "${CMAKE_EXE_LINKER_FLAGS}")
string(STRIP "${ldflags}" ldflags)
option(GT3PC_FFMPEG_ASM "FFmpeg with its x86 assembly (text relocations in the PIEs)" OFF)
set(asm)
if(NOT GT3PC_FFMPEG_ASM)
    set(asm --disable-asm)
endif()
include(ProcessorCount)
ProcessorCount(jobs)
if(jobs EQUAL 0)
    set(jobs 4)
endif()

set(GT3PC_FFMPEG_LIBS avformat avcodec swscale avutil)
set(byproducts)
foreach(l ${GT3PC_FFMPEG_LIBS})
    list(APPEND byproducts ${GT3PC_FFMPEG_PREFIX}/lib/lib${l}.a)
endforeach()
# The install step writes them: declared there, Ninja relinks the programs
# in the same build that rebuilds FFmpeg (a build step's byproducts are
# restated before make install replaces them).  CMake before 3.26 has no
# INSTALL_BYPRODUCTS.
if(CMAKE_VERSION VERSION_LESS 3.26)
    set(byproducts_kw BUILD_BYPRODUCTS)
else()
    set(byproducts_kw INSTALL_BYPRODUCTS)
endif()

ExternalProject_Add(gt3_ffmpeg_build
    URL https://ffmpeg.org/releases/ffmpeg-${GT3PC_FFMPEG_VERSION}.tar.xz
    URL_HASH SHA256=${GT3PC_FFMPEG_SHA256}
    TLS_VERIFY ON
    DOWNLOAD_DIR ${GT3PC_FFMPEG_DEPS}/download
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    PREFIX ${GT3PC_FFMPEG_DEPS}/ffmpeg-ep
    SOURCE_DIR ${GT3PC_FFMPEG_SRC}
    BINARY_DIR ${GT3PC_FFMPEG_DEPS}/ffmpeg-build
    INSTALL_DIR ${GT3PC_FFMPEG_PREFIX}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        --prefix=<INSTALL_DIR>
        --cc=${CMAKE_C_COMPILER}
        --arch=x86_32 --cpu=i686 --target-os=linux --enable-cross-compile
        "--extra-cflags=${cflags}" "--extra-ldflags=${ldflags}"
        --enable-static --disable-shared --enable-pic
        --disable-autodetect --disable-programs --disable-doc --disable-network
        --disable-debug --disable-everything
        --disable-avdevice --disable-avfilter --disable-swresample --disable-postproc
        --enable-avformat --enable-avcodec --enable-swscale
        --enable-protocol=file
        --enable-demuxer=mov,mp3
        --enable-decoder=h264,aac,mp3
        --enable-parser=h264,aac,mpegaudio
        ${asm}
    BUILD_COMMAND make -j${jobs}
    INSTALL_COMMAND make install
    ${byproducts_kw} ${byproducts}
    LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON
)

# The libraries in their link order (each before those it calls), the
# include folder made now (an imported target's must exist).
file(MAKE_DIRECTORY ${GT3PC_FFMPEG_PREFIX}/include)
find_package(Threads REQUIRED)
add_library(gt3_ffmpeg INTERFACE)
add_dependencies(gt3_ffmpeg gt3_ffmpeg_build)
target_include_directories(gt3_ffmpeg SYSTEM INTERFACE ${GT3PC_FFMPEG_PREFIX}/include)
target_link_libraries(gt3_ffmpeg INTERFACE ${byproducts} Threads::Threads m)

# Its licence next to a program that links it.
function(gt3pc_ffmpeg_licence target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GT3PC_FFMPEG_SRC}/LICENSE.md
                $<TARGET_FILE_DIR:${target}>/ffmpeg-LICENSE.md
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GT3PC_FFMPEG_SRC}/COPYING.LGPLv2.1
                $<TARGET_FILE_DIR:${target}>/ffmpeg-COPYING.LGPLv2.1
        VERBATIM)
endfunction()
