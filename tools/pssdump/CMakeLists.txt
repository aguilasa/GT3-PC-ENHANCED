cmake_minimum_required(VERSION 3.25)
project(pssdump LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

# pssdump: decodes GT3's PSS movies and IPU pictures with the native decoder
# (src/engine/media/native): stream info, frames as PNG, the sound as WAV,
# decode speed, and checks over every movie (docs/movies.md).
include(${GT3PC}/src/engine/media/native/sources.cmake)
add_executable(pssdump main.cpp ${GT3_MEDIA_NATIVE_SOURCES})
target_include_directories(pssdump PRIVATE ${GT3PC}/src)
if(MSVC)
    target_compile_options(pssdump PRIVATE /utf-8 /W3 /wd4244 /wd4267 /wd4996)
    target_compile_definitions(pssdump PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# pssgame: the game's movie path without the renderer (harness/): the port's
# mpegif (T529), G002 player interface (mpeg_player_pc.cpp) and pdimpeg
# (T522), compiled with the decompiled code's options as src/CMakeLists.txt
# does, on the platform's scheduler, kernel, volume and sound output, driven
# field by field as MenuBgMovie / play_movie drive them.
if(NOT TARGET gt3_sound_native)
    add_subdirectory(${GT3PC}/src/engine/sound/native ${CMAKE_CURRENT_BINARY_DIR}/sound_native)
endif()
add_library(pssgame_decomp OBJECT
    harness/movie_game.cpp
    ${GT3PC}/src/engine/media/T529_mpegif.cpp
    ${GT3PC}/src/engine/media/mpeg_player_pc.cpp
    ${GT3PC}/src/engine/pdi/T522_pdimpeg.cpp)
target_include_directories(pssgame_decomp PRIVATE ${GT3PC}/include ${GT3PC}/src)
# The options are GT3_DECOMP_OPTIONS of src/CMakeLists.txt (set there, in
# another directory scope, so repeated here; keep the two in step).
if(MSVC)
    target_compile_options(pssgame_decomp PRIVATE /FI${GT3PC}/include/pc_prelude.h /permissive /bigobj /EHsc /W0
        /Zc:threadSafeInit-)
    target_compile_definitions(pssgame_decomp PRIVATE GT3_PC=1 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(pssgame_decomp PRIVATE
        -include ${GT3PC}/include/pc_prelude.h
        -w -fno-threadsafe-statics -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
        -msse2 -mfpmath=sse -ffp-contract=off -march=pentium4
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fpermissive>)
    target_compile_definitions(pssgame_decomp PRIVATE GT3_PC=1)
    set_target_properties(pssgame_decomp PROPERTIES CXX_EXTENSIONS ON)
endif()

add_executable(pssgame
    harness/pssgame.cpp
    $<TARGET_OBJECTS:pssgame_decomp>
    ${GT3_MEDIA_NATIVE_SOURCES}
    ${GT3_MEDIA_SERVICE_SOURCES}
    ${GT3PC}/src/platform/scheduler.cpp
    ${GT3PC}/src/platform/ee_kernel.cpp
    ${GT3PC}/src/platform/log.cpp
    ${GT3PC}/src/platform/disc.cpp
    ${GT3PC}/src/platform/host_file.cpp
    # (PC 2026-11-14: host_file and paths read a disc image's volume too)
    ${GT3PC}/src/platform/disc_image.cpp
    ${GT3PC}/src/launcher/iso9660.cpp
    ${GT3PC}/src/launcher/volume.cpp
    ${GT3PC}/src/launcher/inflate.cpp
    ${GT3PC}/src/launcher/util.cpp
    ${GT3PC}/src/platform/paths.cpp
    ${GT3PC}/src/platform/audio.cpp
    # (PC 2026-11-16: the interface's languages - util.cpp, audio.cpp translate)
    ${GT3PC}/src/platform/ui_text.cpp)
target_include_directories(pssgame PRIVATE ${GT3PC}/include ${GT3PC}/src)
target_compile_definitions(pssgame PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pssgame PRIVATE gt3_sound_native winmm ole32 avrt)
