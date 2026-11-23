cmake_minimum_required(VERSION 3.25)
project(sndtest LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

if(NOT TARGET gt3_sound_native)
    add_subdirectory(${GT3PC}/src/engine/sound/native ${CMAKE_CURRENT_BINARY_DIR}/sound_native)
endif()

add_executable(sndtest
    main.cpp
    banks.cpp
    ee_sound.cpp)
target_include_directories(sndtest PRIVATE ${GT3PC}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sndtest PRIVATE gt3_sound_native)
target_compile_definitions(sndtest PRIVATE _CRT_SECURE_NO_WARNINGS)

# sndgame: the port's integrated engine/sound and engine/pdi sound files,
# compiled with the decomp's options as in src/CMakeLists.txt (the prelude,
# /Zc:threadSafeInit-...), driven by game_side.cpp (same options) and the
# native harness sndgame.cpp.
add_library(sndgame_decomp OBJECT
    game_side.cpp
    ${GT3PC}/src/engine/sound/sound_driver.cpp
    ${GT3PC}/src/engine/sound/sound_seq.cpp
    ${GT3PC}/src/engine/pdi/T520_pdispu2.cpp
    ${GT3PC}/src/engine/pdi/T521_pdibgm.cpp)
target_include_directories(sndgame_decomp PRIVATE ${GT3PC}/include ${GT3PC}/src)
# The options are GT3_DECOMP_OPTIONS of src/CMakeLists.txt (set there, in
# another directory scope, so repeated here; keep the two in step).
if(MSVC)
    target_compile_options(sndgame_decomp PRIVATE /FI${GT3PC}/include/pc_prelude.h /permissive /bigobj /EHsc /W0
        /Zc:threadSafeInit-)
    target_compile_definitions(sndgame_decomp PRIVATE GT3_PC=1 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(sndgame_decomp PRIVATE
        -include ${GT3PC}/include/pc_prelude.h
        -w -fno-threadsafe-statics -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
        -msse2 -mfpmath=sse -ffp-contract=off -march=pentium4
        $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fpermissive>)
    target_compile_definitions(sndgame_decomp PRIVATE GT3_PC=1)
    set_target_properties(sndgame_decomp PROPERTIES CXX_EXTENSIONS ON)
endif()

add_executable(sndgame sndgame.cpp banks.cpp $<TARGET_OBJECTS:sndgame_decomp>)
target_include_directories(sndgame PRIVATE ${GT3PC}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sndgame PRIVATE gt3_sound_native)
target_compile_definitions(sndgame PRIVATE _CRT_SECURE_NO_WARNINGS)

# sndplay: the platform's sound output (src/platform/audio.cpp, audio_ks.cpp)
# with the native SPU2, playing an .ads in real time (sndplay.cpp stands in
# for the scheduler's clock and the settings).
add_executable(sndplay sndplay.cpp ${GT3PC}/src/platform/audio.cpp ${GT3PC}/src/platform/audio_ks.cpp
    ${GT3PC}/src/platform/log.cpp
    ${GT3PC}/src/platform/ui_text.cpp)  # (PC 2026-11-16: audio.cpp's notices translate)
target_include_directories(sndplay PRIVATE ${GT3PC}/src)
target_link_libraries(sndplay PRIVATE gt3_sound_native ole32 avrt setupapi ksuser)
target_compile_definitions(sndplay PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
