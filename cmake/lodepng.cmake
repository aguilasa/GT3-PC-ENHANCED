# LodePNG (zlib licence): the PNG files off Windows (src/platform/png_image.cpp;
# Windows has the Windows Imaging Component).  One C++ source and its header,
# its own deflate, no other library: nothing more to ship with the program.
# The files of a pinned commit are downloaded once into third_party/, each
# checked against its SHA-256, and built as a static library.
set(GT3PC_LODEPNG_COMMIT 7b2893e35891c0c85bee5595caafaf3c2044b409)
set(GT3PC_LODEPNG_DIR ${GT3PC_ROOT}/third_party/lodepng-7b2893e)
set(GT3PC_LODEPNG_FILES
    lodepng.cpp 32CDD7A395B5476A3FE4615305A99066DC4D37AAEA6988643CCA8B4ECCC352CC
    lodepng.h A49B98B45EA603A05E2A77BD6B0320E7535A9C6E18AEF8CE904692A3E02B87AB
    LICENSE BDAFA318B8E637D600CA1B60545E894881977A16A16B74809C95E805701621E3)

if(NOT EXISTS ${GT3PC_LODEPNG_DIR}/lodepng.cpp)
    message(STATUS "Downloading LodePNG ${GT3PC_LODEPNG_COMMIT} for the PNG files")
    set(part ${GT3PC_LODEPNG_DIR}.part)
    file(REMOVE_RECURSE ${part})
    set(files ${GT3PC_LODEPNG_FILES})
    while(files)
        list(POP_FRONT files name hash)
        file(DOWNLOAD
            https://raw.githubusercontent.com/lvandeve/lodepng/${GT3PC_LODEPNG_COMMIT}/${name}
            ${part}/${name} EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE_RECURSE ${part})
            message(FATAL_ERROR "LodePNG ${GT3PC_LODEPNG_COMMIT}: ${name} could not be downloaded: ${status}")
        endif()
    endwhile()
    # (the folder appears whole or not at all)
    file(RENAME ${part} ${GT3PC_LODEPNG_DIR})
endif()

if(NOT TARGET gt3_lodepng)
    add_library(gt3_lodepng STATIC ${GT3PC_LODEPNG_DIR}/lodepng.cpp)
    target_include_directories(gt3_lodepng PUBLIC ${GT3PC_LODEPNG_DIR})
    # (no file I/O of its own: the platform layer's files; the encoder's and
    # decoder's ancillary chunks - pHYs - kept)
    target_compile_definitions(gt3_lodepng PUBLIC LODEPNG_NO_COMPILE_DISK)
    if(MSVC)
        target_compile_options(gt3_lodepng PRIVATE /W0)
    else()
        target_compile_options(gt3_lodepng PRIVATE -w)
    endif()
endif()
