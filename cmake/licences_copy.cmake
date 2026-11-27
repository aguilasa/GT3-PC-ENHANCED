# licences_copy.cmake - writes the third-party software's licence texts into
# the folder beside the executables (PC 2026-11-23).  Run by the build
# (cmake/licences.cmake, which gives the folders and the versions):
#   cmake -DOUT=<folder> -DSDL3_DIR=.. -DSDL3_VERSION=.. -DSQLITE_DIR=..
#         -DSQLITE_VERSION=.. -DCHD_DIR=.. -DCHD_COMMIT=.. -DLZMA_DIR=..
#         -DLZMA_VERSION=.. -DMINIZ_DIR=.. -DMINIZ_VERSION=..
#         -DZSTD_LICENSE=.. -DZSTD_VERSION=.. -P licences_copy.cmake
#
# No licence text is typed here.  A library's own licence file is copied as
# it is; where the downloaded files hold none (miniz, dr_flac, SQLite) the
# notice is taken out of the library's source, between two marks of it.  A
# file or a mark that is not found stops the build: the library's version
# was changed, and its notice has to be looked at again.
cmake_minimum_required(VERSION 3.25)

foreach(v OUT SDL3_DIR SDL3_VERSION SQLITE_DIR SQLITE_VERSION CHD_DIR CHD_COMMIT LZMA_DIR LZMA_VERSION
          MINIZ_DIR MINIZ_VERSION ZSTD_LICENSE ZSTD_VERSION)
    if("${${v}}" STREQUAL "")
        message(FATAL_ERROR "licences_copy.cmake: ${v} is not given")
    endif()
endforeach()
file(MAKE_DIRECTORY ${OUT})

# A library's licence file, copied as it is (the copy dated now: the build
# compares it with the script's date).
function(licence_file name from)
    if(NOT EXISTS ${from})
        message(FATAL_ERROR "licences: ${from} is missing (${name})")
    endif()
    file(COPY_FILE ${from} ${OUT}/${name})
    file(TOUCH_NOCREATE ${OUT}/${name})
endfunction()

# A notice held in a source file: the text after the first `begin` (from it
# when `keep` is true) up to the next `end`, the comment's decoration at the
# start of the lines - the regular expression `decoration`, may be empty -
# taken off.
function(licence_notice name from begin keep end decoration)
    if(NOT EXISTS ${from})
        message(FATAL_ERROR "licences: ${from} is missing (${name})")
    endif()
    file(READ ${from} text)
    string(FIND "${text}" "${begin}" at)
    if(at LESS 0)
        message(FATAL_ERROR "licences: the notice's start is not found in ${from} (${name})")
    endif()
    if(NOT keep)
        string(LENGTH "${begin}" skip)
        math(EXPR at "${at} + ${skip}")
    endif()
    string(SUBSTRING "${text}" ${at} -1 text)
    string(FIND "${text}" "${end}" at)
    if(at LESS 1)
        message(FATAL_ERROR "licences: the notice's end is not found in ${from} (${name})")
    endif()
    string(SUBSTRING "${text}" 0 ${at} text)
    if(NOT "${decoration}" STREQUAL "")
        string(REGEX REPLACE "\n${decoration}" "\n" text "\n${text}")
    endif()
    string(STRIP "${text}" text)
    if("${text}" STREQUAL "")
        message(FATAL_ERROR "licences: the notice found in ${from} is empty (${name})")
    endif()
    file(WRITE ${OUT}/${name} "${text}\n")
endfunction()

# SDL (zlib licence), libchdr (BSD 3-Clause), the LZMA SDK (public domain),
# zstd (its BSD licence; cmake/chd.cmake downloads the file): their own files.
licence_file(SDL3.txt ${SDL3_DIR}/LICENSE.txt)
licence_file(libchdr.txt ${CHD_DIR}/LICENSE.txt)
licence_file(LZMA_SDK.txt ${LZMA_DIR}/LICENSE)
licence_file(zstd.txt ${ZSTD_LICENSE})

# miniz (MIT): the notice at the top of miniz.c, a comment between two lines
# of asterisks, each line after " * ".  It is the text of miniz's own LICENSE
# file, which libchdr's copy leaves out.
licence_notice(miniz.txt ${MINIZ_DIR}/miniz.c
    "/**************************************************************************" FALSE
    "**************************************************************************/"
    " \\* ?")

# dr_flac (public domain or MIT No Attribution): the two licences at the end
# of dr_flac.h, the file's last comment.
licence_notice(dr_flac.txt ${CHD_DIR}/include/dr_libs/dr_flac.h
    "This software is available as a choice of the following licenses." TRUE
    "*/" "")

# SQLite (public domain, nothing to reproduce): the statement its files open
# with, the head of sqlite3.h's first comment down to the line of asterisks,
# each line after "** ".
licence_notice(SQLite.txt ${SQLITE_DIR}/sqlite3.h
    "/*" FALSE
    "\n***"
    "\\*\\* ?")

# The folder's index.
string(CONCAT readme
    "Third-party software\n"
    "====================\n"
    "\n"
    "gt3pc.exe and gt3launcher.exe are built with the software below, and\n"
    "SDL3.dll is SDL itself.  Each one's licence text is a file of this folder,\n"
    "taken from that software's own files when the programs were built.\n"
    "\n"
    "\n"
    "SDL ${SDL3_VERSION} (SDL3.dll) - SDL3.txt\n"
    "    The game controllers, the wheels and their force feedback.\n"
    "    zlib licence.\n"
    "    https://www.libsdl.org\n"
    "\n"
    "SQLite ${SQLITE_VERSION} - SQLite.txt\n"
    "    The tables of the game configs, the car databases and the texts.\n"
    "    Public domain.\n"
    "    https://www.sqlite.org/copyright.html\n"
    "\n"
    "libchdr, commit ${CHD_COMMIT} - libchdr.txt\n"
    "    Disc images stored as CHD files.\n"
    "    BSD 3-Clause licence.  Those of its source files that come from MAME\n"
    "    name Aaron Giles as their copyright holder, under the same licence.\n"
    "    https://github.com/rtissera/libchdr\n"
    "\n"
    "libchdr carries the four below, built in with it:\n"
    "\n"
    "LZMA SDK ${LZMA_VERSION}, its decoder - LZMA_SDK.txt\n"
    "    Public domain.\n"
    "    https://www.7-zip.org/sdk.html\n"
    "\n"
    "miniz ${MINIZ_VERSION} - miniz.txt\n"
    "    MIT licence.\n"
    "    https://github.com/richgel999/miniz\n"
    "\n"
    "Zstandard (zstd) ${ZSTD_VERSION}, its decoder - zstd.txt\n"
    "    BSD licence: the one taken of the two zstd is offered under (this one\n"
    "    or the GPL version 2).\n"
    "    https://github.com/facebook/zstd\n"
    "\n"
    "dr_flac - dr_flac.txt\n"
    "    Public domain or MIT No Attribution, as one prefers.\n"
    "    https://github.com/mackron/dr_libs\n")
file(WRITE ${OUT}/README.txt "${readme}")
