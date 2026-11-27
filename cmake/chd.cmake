# libchdr (BSD 3-Clause): MAME's CHD disc images read (src/launcher/
# chd_image.*: a disc image given as a .chd, PC 2026-11-23), with the three
# decoders it carries in deps/ - the LZMA SDK's LzmaDec (public domain), miniz
# (its inflate; unlicense / MIT) and zstd's decoder (BSD) - and dr_flac (public
# domain / MIT-0) in include/dr_libs.  The library has no releases: the
# archive of a pinned commit is downloaded once into third_party/, checked
# against its SHA-256; only the sources, the headers, the deps, the licence
# and the read-me are extracted.  Built as one static library from the
# library's own single-file build (unity.c), with its own (standard) options.
# Every one's licence text goes beside the executables (cmake/licences.cmake).
set(GT3PC_CHD_COMMIT 47d794151b2b64f6aba60304e6480f4f5a2bc24b)
set(GT3PC_CHD_SHA256 19B1C8D0BEB4874E88A0E674A132BF3F9C0F1A8D08C70BED624627CAD91A65CA)
set(GT3PC_CHD_DIR ${GT3PC_ROOT}/third_party/libchdr-${GT3PC_CHD_COMMIT})
# (the decoders this commit carries, each in deps/<name>-<version>)
set(GT3PC_CHD_LZMA_VERSION 26.02)
set(GT3PC_CHD_MINIZ_VERSION 3.1.2)
set(GT3PC_CHD_ZSTD_VERSION 1.5.7)
set(GT3PC_CHD_LZMA_DIR ${GT3PC_CHD_DIR}/deps/lzma-${GT3PC_CHD_LZMA_VERSION})
set(GT3PC_CHD_MINIZ_DIR ${GT3PC_CHD_DIR}/deps/miniz-${GT3PC_CHD_MINIZ_VERSION})
set(GT3PC_CHD_ZSTD_DIR ${GT3PC_CHD_DIR}/deps/zstd-${GT3PC_CHD_ZSTD_VERSION})

if(NOT EXISTS ${GT3PC_CHD_DIR}/unity.c)
    set(zip ${CMAKE_BINARY_DIR}/libchdr-${GT3PC_CHD_COMMIT}.zip)
    message(STATUS "Downloading libchdr ${GT3PC_CHD_COMMIT} for the CHD disc images")
    file(DOWNLOAD
        https://github.com/rtissera/libchdr/archive/${GT3PC_CHD_COMMIT}.zip
        ${zip} EXPECTED_HASH SHA256=${GT3PC_CHD_SHA256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "libchdr ${GT3PC_CHD_COMMIT} could not be downloaded: ${status}")
    endif()
    set(v libchdr-${GT3PC_CHD_COMMIT})
    file(ARCHIVE_EXTRACT INPUT ${zip} DESTINATION ${GT3PC_ROOT}/third_party
        PATTERNS ${v}/unity.c ${v}/LICENSE.txt ${v}/README.md ${v}/src/* ${v}/include/* ${v}/deps/*)
    file(REMOVE ${zip})
endif()

# PC (2026-11-23): zstd's licence text.  The decoder's source names "the
# LICENSE file in the root directory of this source tree", which libchdr's
# copy leaves out: the file of the same zstd release, checked against its
# SHA-256, put where the source says - for the licence texts that go beside
# the executables (cmake/licences.cmake).
set(GT3PC_CHD_ZSTD_LICENSE_SHA256 7055266497633C9025B777C78EB7235AF13922117480ED5C674677ADC381C9D8)
set(GT3PC_CHD_ZSTD_LICENSE ${GT3PC_CHD_ZSTD_DIR}/LICENSE)
if(NOT EXISTS ${GT3PC_CHD_ZSTD_LICENSE})
    set(tmp ${CMAKE_BINARY_DIR}/zstd-${GT3PC_CHD_ZSTD_VERSION}-LICENSE)
    message(STATUS "Downloading zstd ${GT3PC_CHD_ZSTD_VERSION}'s licence text")
    file(DOWNLOAD
        https://raw.githubusercontent.com/facebook/zstd/v${GT3PC_CHD_ZSTD_VERSION}/LICENSE
        ${tmp} EXPECTED_HASH SHA256=${GT3PC_CHD_ZSTD_LICENSE_SHA256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        file(REMOVE ${tmp})
        message(FATAL_ERROR "zstd ${GT3PC_CHD_ZSTD_VERSION}'s licence text could not be downloaded: ${status}")
    endif()
    file(COPY_FILE ${tmp} ${GT3PC_CHD_ZSTD_LICENSE})
    file(REMOVE ${tmp})
endif()

if(NOT TARGET gt3_chd)
    # (unity.c leaves the laserdisc codec out, which libchdr_chd.c's codec
    # table names all the same: built beside it)
    set(GT3PC_CHD_SOURCES ${GT3PC_CHD_DIR}/unity.c ${GT3PC_CHD_DIR}/src/libchdr_codec_avhuff.c)
    add_library(gt3_chd STATIC ${GT3PC_CHD_SOURCES} ${GT3PC_ROOT}/src/launcher/chd_alloc.c)
    # The library's memory from the Windows process heap, not the C library's
    # malloc family: in gt3pc.exe that is the game's own heap, whose calloc
    # does not zero (src/launcher/chd_alloc.h, put in front of the library's
    # sources).  Off Windows: the C library's own allocator under its
    # internal names (chd_alloc.c), as the game's heap replaces malloc there
    # too.
    if(MSVC)
        set(GT3PC_CHD_PRELUDE "/FI${GT3PC_ROOT}/src/launcher/chd_alloc.h")
    else()
        set(GT3PC_CHD_PRELUDE "-include;${GT3PC_ROOT}/src/launcher/chd_alloc.h")
    endif()
    set_source_files_properties(${GT3PC_CHD_SOURCES} PROPERTIES COMPILE_OPTIONS "${GT3PC_CHD_PRELUDE}")
    target_include_directories(gt3_chd PUBLIC ${GT3PC_CHD_DIR}/include)
    target_include_directories(gt3_chd PRIVATE ${GT3PC_CHD_DIR} ${GT3PC_CHD_DIR}/src
                               ${GT3PC_CHD_LZMA_DIR}/include ${GT3PC_CHD_MINIZ_DIR} ${GT3PC_CHD_ZSTD_DIR})
    # (the library's defaults, stated: whole CD sectors and their subcode as
    # chdman packed them, each decoded hunk checked against its CRC, the
    # hunk map whole in memory)
    target_compile_definitions(gt3_chd PRIVATE _CRT_SECURE_NO_WARNINGS WANT_RAW_DATA_SECTOR=1 WANT_SUBCODE=1
                               VERIFY_BLOCK_CRC=1 LOWRAM_TARGET=0)
    if(MSVC)
        target_compile_options(gt3_chd PRIVATE /W0)
    else()
        target_compile_options(gt3_chd PRIVATE -w)
    endif()
endif()
