# SQLite (public domain): the product configs' tables, gameconf\<config>.sqlite3
# (src/platform/gcf_tables.*), written by the launcher and read by the game.
# The official amalgamation of a pinned release is downloaded once into
# third_party/, checked against its SHA-256; only sqlite3.c and sqlite3.h are
# extracted, built as a static library with its own (standard) options.
set(GT3PC_SQLITE_VERSION 3.53.4)
set(GT3PC_SQLITE_ID 3530400)
set(GT3PC_SQLITE_YEAR 2026)
set(GT3PC_SQLITE_SHA256 1E71DDF93849C6A6ECF58B827C0692073D2DD7EE40196158068F7B29F422E87D)
set(GT3PC_SQLITE_DIR ${GT3PC_ROOT}/third_party/sqlite-amalgamation-${GT3PC_SQLITE_ID})

if(NOT EXISTS ${GT3PC_SQLITE_DIR}/sqlite3.c)
    set(zip ${CMAKE_BINARY_DIR}/sqlite-amalgamation-${GT3PC_SQLITE_ID}.zip)
    message(STATUS "Downloading SQLite ${GT3PC_SQLITE_VERSION} for the game configs' tables")
    file(DOWNLOAD
        https://www.sqlite.org/${GT3PC_SQLITE_YEAR}/sqlite-amalgamation-${GT3PC_SQLITE_ID}.zip
        ${zip} EXPECTED_HASH SHA256=${GT3PC_SQLITE_SHA256} TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "SQLite ${GT3PC_SQLITE_VERSION} could not be downloaded: ${status}")
    endif()
    set(v sqlite-amalgamation-${GT3PC_SQLITE_ID})
    file(ARCHIVE_EXTRACT INPUT ${zip} DESTINATION ${GT3PC_ROOT}/third_party PATTERNS ${v}/sqlite3.c ${v}/sqlite3.h)
    file(REMOVE ${zip})
endif()

if(NOT TARGET gt3_sqlite)
    add_library(gt3_sqlite STATIC ${GT3PC_SQLITE_DIR}/sqlite3.c)
    target_include_directories(gt3_sqlite PUBLIC ${GT3PC_SQLITE_DIR})
    # (no extensions loaded; double-quoted strings are identifiers only; no
    # memory statistics.  SQLITE_WITHOUT_MSIZE: in gt3pc.exe malloc / free are
    # the game's global heap (src/engine/pdistd/T532_heap.cpp), not the C
    # runtime's, so the runtime's _msize must not be asked a block's size:
    # SQLite keeps the sizes itself)
    target_compile_definitions(gt3_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0 SQLITE_DEFAULT_MEMSTATUS=0
                               SQLITE_OMIT_DEPRECATED SQLITE_OMIT_SHARED_CACHE SQLITE_WITHOUT_MSIZE)
    if(MSVC)
        target_compile_options(gt3_sqlite PRIVATE /W0)
    else()
        target_compile_options(gt3_sqlite PRIVATE -w)
    endif()
endif()
