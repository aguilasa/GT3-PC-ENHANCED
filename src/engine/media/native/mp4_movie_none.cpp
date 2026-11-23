/* mp4_movie_none.cpp - mp4_movie.h without an MP4 decoder (the builds off
 * Windows, where Media Foundation is not there).
 *
 * No file is an MP4 here: mp4_duration gives -1, so the movie services play
 * the .pss beside an .mp4 (mpg_service.cpp) and the stills cache skips it
 * (still_cache.cpp); a reader never opens (a BGM .mp3 is dr_mp3's, T521). */
#include "engine/media/native/mp4_movie.h"

namespace gt3media {

MfScope::MfScope() : com_(false), mf_(true) {}
MfScope::~MfScope() {}

int64_t mp4_duration(const char *)
{
    return -1;
}

Mp4VideoReader::~Mp4VideoReader() {}
bool Mp4VideoReader::open(const char *) { return false; }
void Mp4VideoReader::close() {}
bool Mp4VideoReader::next(int64_t *, const uint8_t **) { return false; }
bool Mp4VideoReader::seek(int64_t) { return false; }
bool Mp4VideoReader::read_type() { return false; }

Mp4AudioReader::~Mp4AudioReader() {}
bool Mp4AudioReader::open(const char *) { return false; }
bool Mp4AudioReader::open(const wchar_t *) { return false; }
void Mp4AudioReader::close() {}
bool Mp4AudioReader::next(std::vector<int16_t> &) { return false; }
bool Mp4AudioReader::rewind() { return false; }
bool Mp4AudioReader::seek(int64_t) { return false; }

} // namespace gt3media
