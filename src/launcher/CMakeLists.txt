# gt3launcher.exe: the launcher (configure, install the game files from the
# user's disc image, start gt3pc.exe).  Plain Win32 and the Windows SDK, with
# the game's control mappings (src/platform/input_map.cpp), its save folders
# (src/platform/save_folders.cpp: the save manager's), its music files'
# formats (src/engine/sound/native/bgm_files.cpp, spu2_adpcm.cpp: the .ads
# to .wav conversion; src/engine/media/native/mp4_movie.cpp: an .mp3 decoded
# by Media Foundation for the song manager's beat markers), its product
# configs' and database folder's tables (src/platform/gcf_tables.cpp,
# param_db.cpp, param_schema.cpp and SQLite, cmake/sqlite.cmake: the .gcf and
# .db to .sqlite3 conversions), its IPU picture decoder and PNG files
# (src/engine/media/native/ipu_image.cpp, src/platform/png_image.cpp: the
# .ipic to .png conversion), its texture library (src/engine/pgl/native/
# gs_imgs.cpp, gs_tex*.cpp: the .imgs to PNG folders conversion) and SDL3,
# delay-loaded as in the game, for the Controls tab's capture of gamepad and
# wheel inputs (cmake/sdl3.cmake) and (PC 2026-11-20) its force feedback
# testers, through the game's src/platform/wheel_ffb.cpp.
# Off Windows only its command line installer is built (below).
# Included from src/CMakeLists.txt; also buildable on its own (Windows):
#   tools\msvc.cmd cmake -S src/launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(gt3launcher LANGUAGES C CXX RC)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options(/utf-8 /W3)
    set(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
endif()

# Off Windows (included from src/CMakeLists.txt's GCC / Clang part): the
# command line installer only, gt3launcher --install / --unpack
# (main_posix.cpp; the window stays Windows'), with the install's files and
# its conversions as below (a disc image may be a CHD there too: libchdr,
# cmake/chd.cmake); the Win32 file calls they make on the platform
# layer's files (win32_posix.cpp over platform/os/fs_posix.cpp), the demo
# replays as a generated source (cmake/demos.cmake), the MP4 reader on
# FFmpeg (mp4_movie_ffmpeg.cpp, cmake/ffmpeg.cmake: the song manager's .mp3
# beat markers), SQLite and LodePNG as the game has them (gt3_sqlite,
# gt3_lodepng), the platform layer's SSE2 floating point
# (GT3_PLATFORM_FLOAT_OPTIONS) and no SDL: the interface's text reads the
# locale's variables itself (GT3PC_UI_TEXT_NO_SDL; the command line stays
# English).
if(NOT WIN32)
    set(GT3PC_DEMOS_CPP ${CMAKE_CURRENT_BINARY_DIR}/demos.cpp)
    file(GLOB GT3PC_DEMOS LIST_DIRECTORIES false CONFIGURE_DEPENDS ${GT3PC_ROOT}/demo/*)
    add_custom_command(OUTPUT ${GT3PC_DEMOS_CPP}
        COMMAND ${CMAKE_COMMAND} -DOUT=${GT3PC_DEMOS_CPP} -DDEMO_DIR=${GT3PC_ROOT}/demo
                -P ${GT3PC_ROOT}/cmake/demos.cmake
        DEPENDS ${GT3PC_DEMOS} ${GT3PC_ROOT}/cmake/demos.cmake
        COMMENT "Embedding demo/*"
        VERBATIM)
    add_executable(gt3launcher
        main_posix.cpp
        install.cpp
        disc_files.cpp
        inflate.cpp
        iso9660.cpp
        chd_image.cpp
        util.cpp
        volume.cpp
        music.cpp
        gameconf.cpp
        databases.cpp
        stills.cpp
        image_packs.cpp
        pclog_posix.cpp
        win32_posix.cpp
        ${GT3PC_DEMOS_CPP}
        ${GT3PC_ROOT}/src/platform/os/fs_posix.cpp
        ${GT3PC_ROOT}/src/platform/os/eucjp.cpp
        ${GT3PC_ROOT}/src/engine/sound/native/bgm_files.cpp
        ${GT3PC_ROOT}/src/engine/sound/native/spu2_adpcm.cpp
        ${GT3PC_ROOT}/src/engine/media/native/mp4_movie_ffmpeg.cpp
        ${GT3PC_ROOT}/src/engine/media/native/ipu_image.cpp
        ${GT3PC_ROOT}/src/engine/media/native/ipu_csc.cpp
        ${GT3PC_ROOT}/src/engine/media/native/mpeg2_tables.cpp
        ${GT3PC_ROOT}/src/engine/media/native/mpeg2_idct.cpp
        ${GT3PC_ROOT}/src/platform/png_image.cpp
        ${GT3PC_ROOT}/src/engine/pgl/native/gs_imgs.cpp
        ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex1.cpp
        ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_decode.cpp
        ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_format.cpp
        ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_layout.cpp
        ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_space.cpp
        ${GT3PC_ROOT}/src/platform/gcf_tables.cpp
        ${GT3PC_ROOT}/src/platform/param_db.cpp
        ${GT3PC_ROOT}/src/platform/param_schema.cpp
        ${GT3PC_ROOT}/src/platform/ui_text.cpp
    )
    target_include_directories(gt3launcher PRIVATE ${GT3PC_ROOT}/src)
    target_compile_definitions(gt3launcher PRIVATE GT3PC_UI_TEXT_NO_SDL)
    target_compile_options(gt3launcher PRIVATE ${GT3_PLATFORM_FLOAT_OPTIONS})
    find_package(Threads REQUIRED)
    include(${GT3PC_ROOT}/cmake/ffmpeg.cmake)
    if(NOT TARGET gt3_chd)
        include(${GT3PC_ROOT}/cmake/chd.cmake)
    endif()
    target_link_libraries(gt3launcher PRIVATE gt3_sqlite gt3_lodepng gt3_ffmpeg gt3_chd Threads::Threads)
    gt3pc_languages(gt3launcher)
    gt3pc_ffmpeg_licence(gt3launcher)
    return()
endif()

if(NOT TARGET gt3_sdl3)
    include(${GT3PC_ROOT}/cmake/sdl3.cmake)
endif()
if(NOT TARGET gt3_sqlite)
    include(${GT3PC_ROOT}/cmake/sqlite.cmake)
endif()
if(NOT TARGET gt3_chd)                      # (PC 2026-11-23: disc images stored as CHD files)
    include(${GT3PC_ROOT}/cmake/chd.cmake)
endif()

add_executable(gt3launcher WIN32
    main.cpp
    ui.cpp
    panel.cpp
    controls.cpp
    install.cpp
    disc_list.cpp
    disc_files.cpp
    engines.cpp
    display.cpp
    inflate.cpp
    iso9660.cpp
    chd_image.cpp
    util.cpp
    volume.cpp
    music.cpp
    gameconf.cpp
    databases.cpp
    stills.cpp
    image_packs.cpp
    beats.cpp
    song_manager.cpp
    mod_manager.cpp
    mod_card.cpp
    texture_manager.cpp
    save_manager.cpp
    save_card.cpp
    ps2_icon.cpp
    pclog.cpp
    ${GT3PC_ROOT}/src/platform/input_map.cpp
    ${GT3PC_ROOT}/src/platform/os/fs_win.cpp
    ${GT3PC_ROOT}/src/platform/wheel_ffb.cpp
    ${GT3PC_ROOT}/src/platform/joy_names.cpp
    ${GT3PC_ROOT}/src/platform/save_folders.cpp
    ${GT3PC_ROOT}/src/engine/sound/native/bgm_files.cpp
    ${GT3PC_ROOT}/src/engine/sound/native/spu2_adpcm.cpp
    ${GT3PC_ROOT}/src/engine/media/native/mp4_movie.cpp
    ${GT3PC_ROOT}/src/engine/media/native/ipu_image.cpp
    ${GT3PC_ROOT}/src/engine/media/native/ipu_csc.cpp
    ${GT3PC_ROOT}/src/engine/media/native/mpeg2_tables.cpp
    ${GT3PC_ROOT}/src/engine/media/native/mpeg2_idct.cpp
    ${GT3PC_ROOT}/src/platform/png_image.cpp
    ${GT3PC_ROOT}/src/engine/pgl/native/gs_imgs.cpp
    ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex1.cpp
    ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_decode.cpp
    ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_format.cpp
    ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_layout.cpp
    ${GT3PC_ROOT}/src/engine/pgl/native/gs_tex_space.cpp
    ${GT3PC_ROOT}/src/platform/gcf_tables.cpp
    ${GT3PC_ROOT}/src/platform/param_db.cpp
    ${GT3PC_ROOT}/src/platform/param_schema.cpp
    ${GT3PC_ROOT}/src/platform/ui_text.cpp
    gt3launcher.rc
    gt3launcher.manifest
)
target_compile_definitions(gt3launcher PRIVATE UNICODE _UNICODE)
target_include_directories(gt3launcher PRIVATE ${GT3PC_ROOT}/src)
target_link_libraries(gt3launcher PRIVATE comctl32 comdlg32 shell32 ole32 windowscodecs gt3_sdl3 gt3_sqlite gt3_chd)
add_custom_command(TARGET gt3launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GT3PC_SDL3_DLL} $<TARGET_FILE_DIR:gt3launcher>)
# PC (2026-11-23): the third-party software's licence texts, licences\ beside
# the executable (cmake/licences.cmake)
if(NOT COMMAND gt3pc_licences)
    include(${GT3PC_ROOT}/cmake/licences.cmake)
endif()
gt3pc_licences(gt3launcher)
# (the background art and the mods window's default picture: resources,
# decoded by WIC)
set_property(SOURCE gt3launcher.rc APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../../launcher_bg.png ${CMAKE_CURRENT_SOURCE_DIR}/../../mod.png
    ${CMAKE_CURRENT_SOURCE_DIR}/../../gt3pc.ico)
# The demo replays an install writes over the disc's and the launcher's start
# refreshes in the installed versions (install.cpp write_demos,
# refresh_demos): the licence tests' (L*) and the arcade time trials'
# (AT010001..AT100001), the files of demo\, resources of the type GT3DEMO named
# as the files (off Windows: byte arrays, cmake/demos.cmake).  A file added
# there reconfigures, a changed one rebuilds.
file(GLOB GT3PC_DEMOS LIST_DIRECTORIES false CONFIGURE_DEPENDS ${GT3PC_ROOT}/demo/*)
if(GT3PC_DEMOS)
    set(GT3PC_DEMOS_RC "")
    foreach(demo ${GT3PC_DEMOS})
        get_filename_component(name ${demo} NAME)
        string(APPEND GT3PC_DEMOS_RC "${name} GT3DEMO \"${demo}\"\n")
    endforeach()
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/demos.rc CONTENT "${GT3PC_DEMOS_RC}" @ONLY)
    target_sources(gt3launcher PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/demos.rc)
    set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/demos.rc APPEND PROPERTY OBJECT_DEPENDS ${GT3PC_DEMOS})
endif()
# PC (2026-11-16): the interface's languages, lang/*.po (src/platform/ui_text.h:
# the launcher's and the game's panels' text)
if(NOT COMMAND gt3pc_languages)
    include(${GT3PC_ROOT}/cmake/languages.cmake)
endif()
gt3pc_languages(gt3launcher)
# Small: no incremental-link padding, unreferenced code dropped (the .pdb
# keeps the debug information).
target_link_options(gt3launcher PRIVATE /INCREMENTAL:NO /OPT:REF /OPT:ICF)
