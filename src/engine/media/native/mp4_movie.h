/* mp4_movie.h - H.264 / AAC movies (.mp4) for GT3 PC's modded movies
 * (docs/movies.md "MP4 movies").  On Windows, Media Foundation
 * decodes them (its MPEG-4 source, H.264 and AAC decoders and video
 * processor: no third-party code; mp4_movie.cpp); elsewhere FFmpeg's
 * libavformat, libavcodec and libswscale, linked in statically
 * (mp4_movie_ffmpeg.cpp, cmake/ffmpeg.cmake).  The PSS movies stay the
 * native decoder's (movie.h).
 *
 * Unlike the rest of engine/media/native, part of the port's service
 * sources.  Each reader is used by one thread at a time; a thread that uses
 * one holds an MfScope (on Windows COM and Media Foundation started; nothing
 * to start elsewhere). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#ifdef _WIN32
struct IMFSourceReader;
#endif

namespace gt3media {

#ifndef _WIN32
struct Mp4Decoder;                          /* (mp4_movie_ffmpeg.cpp) */
#endif

/* COM (multithreaded) on this thread and Media Foundation (MFStartup) for
 * the process while one lives; ok() false when either failed. */
class MfScope {
public:
    MfScope();
    ~MfScope();
    MfScope(const MfScope &) = delete;
    MfScope &operator=(const MfScope &) = delete;
    bool ok() const { return mf_; }

private:
    bool com_ = false, mf_ = false;
};

/* The movie's duration in 100 ns units from its 'moov' / 'mvhd' box (no
 * decoder: a quick read); -1 when the file has none (not an MP4). */
int64_t mp4_duration(const char *path);

/* The first video stream, decoded to 32-bit pixels (B, G, R, x in memory)
 * by the video processor, cropped to the display area. */
class Mp4VideoReader {
public:
    Mp4VideoReader() = default;
    ~Mp4VideoReader();
    Mp4VideoReader(const Mp4VideoReader &) = delete;
    Mp4VideoReader &operator=(const Mp4VideoReader &) = delete;

    /* path: a Windows path (the ANSI code page); elsewhere UTF-8.  false
     * (logged) when it cannot be decoded. */
    bool open(const char *path);
    void close();
    /* The next frame in display order: true with its time (100 ns) and its
     * pixels (top-down rows of width x 4 bytes, valid until the next call);
     * false at the end or on an error. */
    bool next(int64_t *time, const uint8_t **pixels);
    /* From time t (100 ns) on: the next frame is the one at or before it. */
    bool seek(int64_t t);

    int width() const { return w_; }
    int height() const { return h_; }
    /* The pixels' aspect (x / y; 1 square). */
    double pixel_aspect() const { return par_; }

private:
    bool read_type();

#ifdef _WIN32
    IMFSourceReader *reader_ = nullptr;
#else
    Mp4Decoder *dec_ = nullptr;
#endif
    int w_ = 0, h_ = 0;                     /* the display area */
    int frameW_ = 0, frameH_ = 0;           /* the decoded frame (aligned) */
    int cropX_ = 0, cropY_ = 0;
    int stride_ = 0;                        /* the frame's default stride (< 0: bottom-up) */
    double par_ = 1.0;
    std::vector<uint8_t> pixels_;
};

/* The first audio stream, decoded to 16-bit PCM at its own rate (1 or 2
 * channels: more are mixed down to 2).  Any file the decoder reads: an
 * .mp4's AAC (or MP3), PC: an .mp3 BGM (T521 pdibgm on Windows; the
 * launcher's beat markers). */
class Mp4AudioReader {
public:
    Mp4AudioReader() = default;
    ~Mp4AudioReader();
    Mp4AudioReader(const Mp4AudioReader &) = delete;
    Mp4AudioReader &operator=(const Mp4AudioReader &) = delete;

    /* false (logged) when the file has no audio that decodes.  wchar_t: any
     * file's name; char: the ANSI code page's (elsewhere UTF-8). */
    bool open(const char *path);
    bool open(const wchar_t *path);
    void close();
    /* Appends the next decoded frames (interleaved, channels() each) to out:
     * false at the end or on an error. */
    bool next(std::vector<int16_t> &out);
    /* From the start again (a looping BGM). */
    bool rewind();
    /* PC: from time t (100 ns) on, to the frame: what the source seeks to
     * before it is dropped (a BGM's preview from partway, T521 pdibgm). */
    bool seek(int64_t t);

    int rate() const { return rate_; }
    int channels() const { return channels_ > 2 ? 2 : channels_; }

private:
#ifdef _WIN32
    IMFSourceReader *reader_ = nullptr;
#else
    Mp4Decoder *dec_ = nullptr;
#endif
    int rate_ = 0, channels_ = 0;
    int64_t skipTo_ = -1;                   /* seek's time: the frames before it dropped (-1: none) */
};

} // namespace gt3media
