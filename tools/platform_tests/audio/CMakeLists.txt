# The sound output off Windows (src/platform/sdl/audio_sdl.cpp over SDL3, the
# shared stage src/platform/audio_stage.cpp), the native SPU2 / BGM player and
# the .mp3 decoder behind engine/pdi/T521_pdibgm.cpp (dr_mp3,
# third_party_src/dr_mp3), recorded through SDL's disk driver: no sound device
# is opened.  Part of tools/platform_tests, also buildable alone:
#
#   cmake -S tools/platform_tests/audio -B build-audio-test -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE="$PWD"/cmake/toolchains/linux-i686.cmake \
#         -DGT3PC_TEST_ADS=<file.ads> -DGT3PC_TEST_MP3=<file.mp3> -DGT3PC_TEST_MP3_SECONDS=<its length>
#   cmake --build build-audio-test
#   ctest --test-dir build-audio-test --output-on-failure
#
# The music is the user's, so it is not in the repository:
#  - an .ads of the game's BGM, from its gt3.vol:
#      python3 tools/gt3vol.py --vol <GT3.VOL> extract bgm/race_a01.ads <file.ads>
#  - an .mp3 of a known length, e.g. the first 10 s of a .wav the launcher made:
#      ffmpeg -i <data_US>/bgm/race_a01.wav -t 10 -c:a libmp3lame -b:a 192k <file.mp3>
# Without them those tests are left out (the silent control still runs).
cmake_minimum_required(VERSION 3.20)
project(gt3_audio_test C CXX)

if(WIN32)
    message(FATAL_ERROR "audio_test checks the SDL output; Windows has kernel streaming and WASAPI")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
get_filename_component(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
enable_testing()

find_package(SDL3 CONFIG QUIET PATHS $ENV{HOME}/opt/i386 NO_DEFAULT_PATH)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "audio_test: no SDL3; left out")
    return()
endif()
find_package(Threads REQUIRED)

# The platform layer's options (src/CMakeLists.txt GT3_PLATFORM_FLOAT_OPTIONS).
set(GT3_PLATFORM_FLOAT_OPTIONS -msse2 -mfpmath=sse)

if(NOT TARGET gt3_sound_native)
    add_subdirectory(${GT3PC_ROOT}/src/engine/sound/native ${CMAKE_CURRENT_BINARY_DIR}/sound_native)
    # SSE2 scalar floating point, as the platform layer (the mix as Windows')
    target_compile_options(gt3_sound_native PRIVATE ${GT3_PLATFORM_FLOAT_OPTIONS})
endif()

# T521 and the test's side of it with the decomp's options (src/CMakeLists.txt
# GT3_DECOMP_OPTIONS, set in another directory scope, so repeated here; keep
# the two in step).
add_library(audio_test_decomp OBJECT
    bgm_side.cpp
    ${GT3PC_ROOT}/src/engine/pdi/T521_pdibgm.cpp)
target_include_directories(audio_test_decomp PRIVATE ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_compile_options(audio_test_decomp PRIVATE
    -include ${GT3PC_ROOT}/include/pc_prelude.h
    -w -fno-threadsafe-statics -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -msse2 -mfpmath=sse -ffp-contract=off -march=pentium4
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fpermissive>)
target_compile_definitions(audio_test_decomp PRIVATE GT3_PC=1)

# The platform files as the Linux gt3_platform builds them.
add_library(audio_test_platform STATIC
    ${GT3PC_ROOT}/src/platform/sdl/audio_sdl.cpp
    ${GT3PC_ROOT}/src/platform/audio_stage.cpp
    ${GT3PC_ROOT}/third_party_src/dr_mp3/dr_mp3.c)
target_include_directories(audio_test_platform PUBLIC ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_compile_options(audio_test_platform PRIVATE ${GT3_PLATFORM_FLOAT_OPTIONS}
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
target_link_libraries(audio_test_platform PUBLIC gt3_sound_native SDL3::SDL3 Threads::Threads)

add_executable(audio_test audio_test.cpp $<TARGET_OBJECTS:audio_test_decomp>)
target_compile_options(audio_test PRIVATE -Wall -Wextra)
target_link_libraries(audio_test PRIVATE audio_test_platform)

set(GT3PC_TEST_ADS "" CACHE FILEPATH "an .ads of the game's BGM for audio_test (empty: that test left out)")
set(GT3PC_TEST_MP3 "" CACHE FILEPATH "an .mp3 for audio_test (empty: that test left out)")
set(GT3PC_TEST_MP3_SECONDS "10" CACHE STRING "GT3PC_TEST_MP3's length in seconds")
set(GT3PC_AUDIO_TEST_ENV SDL_AUDIO_DRIVER=disk)

add_test(NAME audio_silence COMMAND audio_test silence - 2 ${CMAKE_CURRENT_BINARY_DIR}/silence.raw)
set_tests_properties(audio_silence PROPERTIES ENVIRONMENT "${GT3PC_AUDIO_TEST_ENV}")
if(EXISTS "${GT3PC_TEST_ADS}")
    add_test(NAME audio_ads COMMAND audio_test ads ${GT3PC_TEST_ADS} 5 ${CMAKE_CURRENT_BINARY_DIR}/ads.raw)
    # the same with a 20 ms period (whole milliseconds: the disk driver keeps
    # real time)
    add_test(NAME audio_ads_buffer20 COMMAND audio_test ads ${GT3PC_TEST_ADS} 5
        ${CMAKE_CURRENT_BINARY_DIR}/ads_buffer20.raw)
    set_tests_properties(audio_ads PROPERTIES ENVIRONMENT "${GT3PC_AUDIO_TEST_ENV}")
    set_tests_properties(audio_ads_buffer20 PROPERTIES ENVIRONMENT "${GT3PC_AUDIO_TEST_ENV};GT3PC_TEST_BUFFER_MS=20")
else()
    message(STATUS "audio_test: no GT3PC_TEST_ADS; the .ads tests left out")
endif()
if(EXISTS "${GT3PC_TEST_MP3}")
    add_test(NAME audio_mp3 COMMAND audio_test mp3 ${GT3PC_TEST_MP3} ${GT3PC_TEST_MP3_SECONDS}
        ${CMAKE_CURRENT_BINARY_DIR}/mp3.raw)
    # the control: the same file against a length 5 % longer must fail
    math(EXPR GT3PC_TEST_MP3_WRONG "${GT3PC_TEST_MP3_SECONDS} * 105" OUTPUT_FORMAT DECIMAL)
    add_test(NAME audio_mp3_wrong_length COMMAND audio_test mp3 ${GT3PC_TEST_MP3} ${GT3PC_TEST_MP3_WRONG}e-2
        ${CMAKE_CURRENT_BINARY_DIR}/mp3_wrong.raw)
    set_tests_properties(audio_mp3 PROPERTIES ENVIRONMENT "${GT3PC_AUDIO_TEST_ENV}")
    set_tests_properties(audio_mp3_wrong_length PROPERTIES ENVIRONMENT "${GT3PC_AUDIO_TEST_ENV}" WILL_FAIL TRUE)
else()
    message(STATUS "audio_test: no GT3PC_TEST_MP3; the .mp3 tests left out")
endif()

# The race's song panel (src/platform/song_osd.cpp) as the Linux gt3_platform
# builds it: its text folded without the Windows calls, its song_osd::
# functions defined.
add_library(audio_test_song_osd STATIC ${GT3PC_ROOT}/src/platform/song_osd.cpp)
target_include_directories(audio_test_song_osd PRIVATE ${GT3PC_ROOT}/include ${GT3PC_ROOT}/src)
target_compile_options(audio_test_song_osd PRIVATE ${GT3_PLATFORM_FLOAT_OPTIONS} -Wall -Wextra)
add_test(NAME song_osd_symbols COMMAND sh -c "nm -C '$<TARGET_FILE:audio_test_song_osd>' | grep -q ' T song_osd::set_song('")
