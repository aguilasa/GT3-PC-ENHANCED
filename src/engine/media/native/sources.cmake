# The native movie / IPU image decoder (plain C++17, no Windows or game
# dependency): include this file and add ${GT3_MEDIA_NATIVE_SOURCES} to a
# target whose include path has pc_enhanced/src.
#
#   include(${GT3PC}/src/engine/media/native/sources.cmake)
#   target_sources(mytarget PRIVATE ${GT3_MEDIA_NATIVE_SOURCES})
#
# GT3_MEDIA_SERVICE_SOURCES are the port's glue on top of it (the G002
# player's decoding, the IPU stills and the pdimpeg service as C functions,
# media_services.h): they need the platform layer (disc, host files, log)
# and the native sound (sound_native.h); src/CMakeLists.txt builds both
# lists into gt3_media.  Their MP4 reader is Windows Media Foundation's on
# Windows (mp4_movie.cpp) and FFmpeg's elsewhere (mp4_movie_ffmpeg.cpp: the
# target links gt3_ffmpeg, cmake/ffmpeg.cmake).
set(GT3_MEDIA_NATIVE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(GT3_MEDIA_NATIVE_SOURCES
    ${GT3_MEDIA_NATIVE_DIR}/mpeg2_tables.cpp
    ${GT3_MEDIA_NATIVE_DIR}/mpeg2_idct.cpp
    ${GT3_MEDIA_NATIVE_DIR}/mpeg2_video.cpp
    ${GT3_MEDIA_NATIVE_DIR}/ipu_csc.cpp
    ${GT3_MEDIA_NATIVE_DIR}/ipu_image.cpp
    ${GT3_MEDIA_NATIVE_DIR}/pss_demux.cpp
    ${GT3_MEDIA_NATIVE_DIR}/pss_audio.cpp
    ${GT3_MEDIA_NATIVE_DIR}/movie.cpp
)
set(GT3_MEDIA_SERVICE_SOURCES
    ${GT3_MEDIA_NATIVE_DIR}/media_services.cpp
    ${GT3_MEDIA_NATIVE_DIR}/mpg_service.cpp
    # the modded MP4 movies (docs/movies.md "MP4 movies")
    ${GT3_MEDIA_NATIVE_DIR}/mp4_frames.cpp
    ${GT3_MEDIA_NATIVE_DIR}/mp4_gpu.cpp
    ${GT3_MEDIA_NATIVE_DIR}/still_cache.cpp
)
if(WIN32)
    list(APPEND GT3_MEDIA_SERVICE_SOURCES ${GT3_MEDIA_NATIVE_DIR}/mp4_movie.cpp)
else()
    list(APPEND GT3_MEDIA_SERVICE_SOURCES ${GT3_MEDIA_NATIVE_DIR}/mp4_movie_ffmpeg.cpp)
endif()
