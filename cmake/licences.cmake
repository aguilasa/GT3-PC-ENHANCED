# The third-party software's licence texts, beside the executables (PC
# 2026-11-23): build\bin\licences\ - the executables' folder - holds one text
# file per library built into gt3pc.exe and gt3launcher.exe or shipped with
# them, and a README.txt that names them.  SDL's, libchdr's, the LZMA SDK's
# and zstd's are copies of the libraries' own licence files; miniz, dr_flac
# and SQLite have none in what is downloaded, and theirs is the notice their
# source carries, taken out of it (cmake/licences_copy.cmake, run by the
# build in CMake's own script mode: no Python, nothing to install).
#
# The folder is written again when one of the libraries' files or the script
# changes and when one of its files is missing.  Both executables depend on
# it; `ninja -C build gt3_licences` makes it alone, linking nothing.  A
# release ships the folder with the executables.
#
# Included after cmake/sdl3.cmake, sqlite.cmake and chd.cmake (their folders
# and versions).  A library added to the build gets its text here: a line in
# GT3PC_LICENCE_FILES, its file among the inputs, its part of the script.
if(NOT TARGET gt3_licences)
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(GT3PC_LICENCES_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/licences)
    else()                                  # (the launcher built on its own)
        set(GT3PC_LICENCES_DIR ${CMAKE_CURRENT_BINARY_DIR}/licences)
    endif()
    # (the files cmake/licences_copy.cmake writes)
    set(GT3PC_LICENCE_FILES README.txt SDL3.txt SQLite.txt libchdr.txt LZMA_SDK.txt miniz.txt zstd.txt dr_flac.txt)
    list(TRANSFORM GT3PC_LICENCE_FILES PREPEND ${GT3PC_LICENCES_DIR}/ OUTPUT_VARIABLE GT3PC_LICENCE_OUTPUTS)
    add_custom_command(
        OUTPUT ${GT3PC_LICENCE_OUTPUTS}
        COMMAND ${CMAKE_COMMAND}
            -DOUT=${GT3PC_LICENCES_DIR}
            -DSDL3_DIR=${GT3PC_SDL3_DIR} -DSDL3_VERSION=${GT3PC_SDL3_VERSION}
            -DSQLITE_DIR=${GT3PC_SQLITE_DIR} -DSQLITE_VERSION=${GT3PC_SQLITE_VERSION}
            -DCHD_DIR=${GT3PC_CHD_DIR} -DCHD_COMMIT=${GT3PC_CHD_COMMIT}
            -DLZMA_DIR=${GT3PC_CHD_LZMA_DIR} -DLZMA_VERSION=${GT3PC_CHD_LZMA_VERSION}
            -DMINIZ_DIR=${GT3PC_CHD_MINIZ_DIR} -DMINIZ_VERSION=${GT3PC_CHD_MINIZ_VERSION}
            -DZSTD_LICENSE=${GT3PC_CHD_ZSTD_LICENSE} -DZSTD_VERSION=${GT3PC_CHD_ZSTD_VERSION}
            -P ${GT3PC_ROOT}/cmake/licences_copy.cmake
        DEPENDS ${GT3PC_ROOT}/cmake/licences_copy.cmake
                ${GT3PC_SDL3_DIR}/LICENSE.txt
                ${GT3PC_SQLITE_DIR}/sqlite3.h
                ${GT3PC_CHD_DIR}/LICENSE.txt
                ${GT3PC_CHD_DIR}/include/dr_libs/dr_flac.h
                ${GT3PC_CHD_LZMA_DIR}/LICENSE
                ${GT3PC_CHD_MINIZ_DIR}/miniz.c
                ${GT3PC_CHD_ZSTD_LICENSE}
        COMMENT "Third-party licence texts: ${GT3PC_LICENCES_DIR}"
        VERBATIM)
    add_custom_target(gt3_licences ALL DEPENDS ${GT3PC_LICENCE_OUTPUTS})
endif()

function(gt3pc_licences target)
    add_dependencies(${target} gt3_licences)
endfunction()
