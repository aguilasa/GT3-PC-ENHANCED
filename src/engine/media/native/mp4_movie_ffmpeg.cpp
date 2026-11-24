/* mp4_movie_ffmpeg.cpp - H.264 / AAC movies through FFmpeg (mp4_movie.h),
 * the builds off Windows: libavformat's MPEG-4 (and MP3) demuxer,
 * libavcodec's H.264, AAC and MP3 decoders and libswscale's conversion to
 * B, G, R, x pixels, static libraries built by cmake/ffmpeg.cmake (LGPL
 * v2.1 or later).  The same readers as Media Foundation's (mp4_movie.cpp):
 * the first video or audio stream, the frames in display order with their
 * times in 100 ns, a seek landing on the key frame at or before the time. */
#include "engine/media/native/mp4_movie.h"

#include "platform/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

namespace gt3media {

/* A demuxer with one stream's decoder open. */
struct Mp4Decoder {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *cc = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    SwsContext *sws = nullptr;
    int stream = -1;
    AVRational tb{1, 1};
    bool draining = false;                  /* the file's end reached: the decoder flushed out */
    int64_t lastT = 0;                      /* the last frame's time and length (a frame without one) */
    int64_t lastLen = 0;
};

namespace {

const AVRational kHns{1, 10000000};         /* 100 ns */

std::string av_error(int err)
{
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, b, sizeof(b));
    return b;
}

/* FFmpeg's own messages: its errors only, to the standard error. */
void quiet_ffmpeg()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

void free_decoder(Mp4Decoder *&d)
{
    if (d == nullptr)
        return;
    sws_freeContext(d->sws);
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->cc);
    avformat_close_input(&d->fmt);
    delete d;
    d = nullptr;
}

/* The file's first stream of `type' with its decoder open, the others
 * discarded; nullptr (logged) when there is none that decodes. */
Mp4Decoder *open_decoder(const char *path, AVMediaType type)
{
    quiet_ffmpeg();
    const char *what = type == AVMEDIA_TYPE_VIDEO ? "video" : "audio";
    Mp4Decoder *d = new Mp4Decoder();
    int err = avformat_open_input(&d->fmt, path, nullptr, nullptr);
    if (err < 0) {
        pclog::write("movie: %s: FFmpeg cannot open it (%s)", path, av_error(err).c_str());
        free_decoder(d);
        return nullptr;
    }
    err = avformat_find_stream_info(d->fmt, nullptr);
    if (err < 0) {
        pclog::write("movie: %s: its streams cannot be read (%s)", path, av_error(err).c_str());
        free_decoder(d);
        return nullptr;
    }
    for (unsigned i = 0; i < d->fmt->nb_streams; i++) {
        AVStream *st = d->fmt->streams[i];
        if (d->stream < 0 && st->codecpar->codec_type == type)
            d->stream = (int)i;
        else
            st->discard = AVDISCARD_ALL;
    }
    if (d->stream < 0) {
        pclog::write("movie: %s: no %s stream", path, what);
        free_decoder(d);
        return nullptr;
    }
    AVStream *st = d->fmt->streams[d->stream];
    d->tb = st->time_base;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (codec == nullptr) {
        pclog::write("movie: %s: no decoder for its %s (%s; H.264, AAC and MP3 are the formats)", path, what,
                     avcodec_get_name(st->codecpar->codec_id));
        free_decoder(d);
        return nullptr;
    }
    d->cc = avcodec_alloc_context3(codec);
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    if (d->cc == nullptr || d->pkt == nullptr || d->frame == nullptr ||
        avcodec_parameters_to_context(d->cc, st->codecpar) < 0) {
        free_decoder(d);
        return nullptr;
    }
    d->cc->pkt_timebase = st->time_base;
    if (type == AVMEDIA_TYPE_VIDEO) {
        /* a few threads (a 1080p picture in a few ms; each one's frames cost
         * the 32-bit address space) */
        d->cc->thread_count = std::min(std::max(av_cpu_count(), 1), 4);
        d->cc->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    err = avcodec_open2(d->cc, codec, nullptr);
    if (err < 0) {
        pclog::write("movie: %s: its %s decoder does not open (%s)", path, what, av_error(err).c_str());
        free_decoder(d);
        return nullptr;
    }
    return d;
}

/* The next decoded frame into d->frame: 1, 0 at the end, -1 on an error. */
int receive(Mp4Decoder *d)
{
    for (;;) {
        int err = avcodec_receive_frame(d->cc, d->frame);
        if (err >= 0)
            return 1;
        if (err == AVERROR_EOF)
            return 0;
        if (err != AVERROR(EAGAIN)) {
            pclog::write("movie: a frame does not decode (%s)", av_error(err).c_str());
            return -1;
        }
        if (d->draining)
            return 0;
        err = av_read_frame(d->fmt, d->pkt);
        if (err < 0) {
            /* the end (or a read error): what the decoder holds still comes out */
            d->draining = true;
            avcodec_send_packet(d->cc, nullptr);
            continue;
        }
        if (d->pkt->stream_index == d->stream) {
            err = avcodec_send_packet(d->cc, d->pkt);
            if (err < 0 && err != AVERROR(EAGAIN) && err != AVERROR_INVALIDDATA)
                pclog::write("movie: a packet is refused (%s)", av_error(err).c_str());
        }
        av_packet_unref(d->pkt);
    }
}

/* The frame's time in 100 ns (one without its own: after the last). */
int64_t frame_time(Mp4Decoder *d, int64_t len)
{
    const int64_t pts = d->frame->best_effort_timestamp;
    int64_t t = pts != AV_NOPTS_VALUE ? av_rescale_q(pts, d->tb, kHns) : d->lastT + d->lastLen;
    d->lastT = t;
    d->lastLen = len;
    return t;
}

/* To the key frame at or before t (100 ns), the decoder emptied. */
bool seek_decoder(Mp4Decoder *d, int64_t t)
{
    const int64_t ts = av_rescale_q(t, kHns, d->tb);
    int err = av_seek_frame(d->fmt, d->stream, ts, AVSEEK_FLAG_BACKWARD);
    if (err < 0)
        err = av_seek_frame(d->fmt, d->stream, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(d->cc);
    d->draining = false;
    d->lastT = t;
    d->lastLen = 0;
    return err >= 0;
}

/* One sample of an audio frame as a float (-1 .. 1). */
float sample_at(const AVFrame *f, int ch, int i, int channels)
{
    const AVSampleFormat fmt = (AVSampleFormat)f->format;
    const bool planar = av_sample_fmt_is_planar(fmt) != 0;
    const uint8_t *base = planar ? f->extended_data[ch] : f->extended_data[0];
    const size_t k = planar ? (size_t)i : (size_t)i * (size_t)channels + (size_t)ch;
    switch (av_get_packed_sample_fmt(fmt)) {
    case AV_SAMPLE_FMT_FLT: return ((const float *)base)[k];
    case AV_SAMPLE_FMT_DBL: return (float)((const double *)base)[k];
    case AV_SAMPLE_FMT_S16: return ((const int16_t *)base)[k] * (1.0f / 32768.0f);
    case AV_SAMPLE_FMT_S32: return (float)(((const int32_t *)base)[k] * (1.0 / 2147483648.0));
    case AV_SAMPLE_FMT_U8: return (((const uint8_t *)base)[k] - 128) * (1.0f / 128.0f);
    default: return 0.0f;
    }
}

int16_t to_s16(float v)
{
    const float s = v * 32768.0f;
    return (int16_t)(s < -32768.0f ? -32768 : s > 32767.0f ? 32767 : (int)std::lrintf(s));
}

/* A wide path (UTF-32 here) as UTF-8. */
std::string utf8(const wchar_t *w)
{
    std::string s;
    for (; *w != L'\0'; w++) {
        const uint32_t c = (uint32_t)*w;
        if (c < 0x80) {
            s += (char)c;
        } else if (c < 0x800) {
            s += (char)(0xC0 | (c >> 6));
            s += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += (char)(0xE0 | (c >> 12));
            s += (char)(0x80 | ((c >> 6) & 0x3F));
            s += (char)(0x80 | (c & 0x3F));
        } else {
            s += (char)(0xF0 | (c >> 18));
            s += (char)(0x80 | ((c >> 12) & 0x3F));
            s += (char)(0x80 | ((c >> 6) & 0x3F));
            s += (char)(0x80 | (c & 0x3F));
        }
    }
    return s;
}

/* Big-endian reads of the file (mp4_duration). */
bool read_at(int fd, uint64_t off, void *dst, size_t n)
{
    return pread64(fd, dst, n, (off64_t)off) == (ssize_t)n;
}

uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* The box of type `type' among those from `off' to `end': its payload's
 * offset and end. */
bool find_box(int fd, uint64_t off, uint64_t end, const char *type, uint64_t *payload, uint64_t *boxEnd)
{
    while (off + 8 <= end) {
        uint8_t hdr[16];
        if (!read_at(fd, off, hdr, 8))
            return false;
        uint64_t size = be32(hdr);
        uint64_t head = 8;
        if (size == 1) {
            if (!read_at(fd, off + 8, hdr + 8, 8))
                return false;
            size = be64(hdr + 8);
            head = 16;
        } else if (size == 0) {
            size = end - off;
        }
        if (size < head || off + size > end)
            return false;
        if (std::memcmp(hdr + 4, type, 4) == 0) {
            *payload = off + head;
            *boxEnd = off + size;
            return true;
        }
        off += size;
    }
    return false;
}

} // namespace

/* ---- MfScope: nothing to start ---- */

MfScope::MfScope() : com_(false), mf_(true) {}
MfScope::~MfScope()
{
    (void)com_;                             /* (no COM here) */
}

/* ---- the duration ---- */

int64_t mp4_duration(const char *path)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_LARGEFILE);
    if (fd < 0)
        return -1;
    int64_t result = -1;
    struct stat64 st;
    uint64_t moov, moovEnd, mvhd, mvhdEnd;
    if (fstat64(fd, &st) == 0 && find_box(fd, 0, (uint64_t)st.st_size, "moov", &moov, &moovEnd) &&
        find_box(fd, moov, moovEnd, "mvhd", &mvhd, &mvhdEnd)) {
        uint8_t b[32];
        if (read_at(fd, mvhd, b, 1)) {
            /* version 0: 32-bit times and duration; 1: 64-bit */
            const bool v1 = b[0] == 1;
            if (read_at(fd, mvhd + 4, b, v1 ? 28 : 16)) {
                const uint32_t scale = be32(b + (v1 ? 16 : 8));
                const uint64_t dur = v1 ? be64(b + 20) : be32(b + 12);
                if (scale > 0 && dur != (v1 ? ~0ull : 0xFFFFFFFFull))
                    result = (int64_t)(dur * 10000000ull / scale);
            }
        }
    }
    ::close(fd);
    return result;
}

/* ---- video ---- */

Mp4VideoReader::~Mp4VideoReader()
{
    close();
}

void Mp4VideoReader::close()
{
    free_decoder(dec_);
}

bool Mp4VideoReader::open(const char *path)
{
    close();
    dec_ = open_decoder(path, AVMEDIA_TYPE_VIDEO);
    if (dec_ == nullptr)
        return false;
    /* the size and the pixels' shape the stream declares (its first frame's
     * from next on) */
    const AVStream *st = dec_->fmt->streams[dec_->stream];
    const AVRational sar = av_guess_sample_aspect_ratio(dec_->fmt, const_cast<AVStream *>(st), nullptr);
    par_ = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
    w_ = st->codecpar->width;
    h_ = st->codecpar->height;
    frameW_ = w_;
    frameH_ = h_;
    if (w_ <= 0 || h_ <= 0) {
        pclog::write("movie: %s: its video does not decode (no picture size; H.264 in an .mp4 is the format)", path);
        close();
        return false;
    }
    stride_ = w_ * 4;
    pixels_.assign((size_t)w_ * h_ * 4, 0);
    return true;
}

/* The conversion of the decoded frame (its display area: the decoder crops
 * it) to B, G, R, x: its size, its colours' matrix (BT.709 or BT.601; one
 * unsaid by the size, as Media Foundation guesses) and range. */
bool Mp4VideoReader::read_type()
{
    const AVFrame *f = dec_->frame;
    frameW_ = f->width;
    frameH_ = f->height;
    cropX_ = cropY_ = 0;
    if (f->width <= 0 || f->height <= 0)
        return false;
    if (f->width != w_ || f->height != h_ || pixels_.size() != (size_t)f->width * f->height * 4) {
        w_ = f->width;
        h_ = f->height;
        stride_ = w_ * 4;
        pixels_.assign((size_t)w_ * h_ * 4, 0);
    }
    if (f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0)
        par_ = av_q2d(f->sample_aspect_ratio);
    dec_->sws = sws_getCachedContext(dec_->sws, f->width, f->height, (AVPixelFormat)f->format, w_, h_,
                                     AV_PIX_FMT_BGR0, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (dec_->sws == nullptr) {
        pclog::write("movie: its pictures (%s) do not convert", av_get_pix_fmt_name((AVPixelFormat)f->format));
        return false;
    }
    int cs;
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: cs = SWS_CS_ITU709; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: cs = SWS_CS_ITU601; break;
    case AVCOL_SPC_SMPTE240M: cs = SWS_CS_SMPTE240M; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
    default: cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
    }
    const AVPixelFormat pf = (AVPixelFormat)f->format;
    const int fullRange = f->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P ||
                          pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P;
    sws_setColorspaceDetails(dec_->sws, sws_getCoefficients(cs), fullRange, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                             1 << 16, 1 << 16);
    return true;
}

bool Mp4VideoReader::next(int64_t *time, const uint8_t **pixels)
{
    if (dec_ == nullptr)
        return false;
    for (;;) {
        const int got = receive(dec_);
        if (got <= 0)
            return false;
        const AVFrame *f = dec_->frame;
        const int64_t len = f->duration > 0 ? av_rescale_q(f->duration, dec_->tb, kHns) : 0;
        const int64_t t = frame_time(dec_, len);
        if (!read_type()) {
            av_frame_unref(dec_->frame);
            continue;
        }
        uint8_t *dst[4] = {pixels_.data(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {w_ * 4, 0, 0, 0};
        sws_scale(dec_->sws, f->data, f->linesize, 0, f->height, dst, dstStride);
        av_frame_unref(dec_->frame);
        *time = t;
        *pixels = pixels_.data();
        return true;
    }
}

bool Mp4VideoReader::seek(int64_t t)
{
    if (dec_ == nullptr)
        return false;
    return seek_decoder(dec_, t);
}

/* ---- audio ---- */

Mp4AudioReader::~Mp4AudioReader()
{
    close();
}

void Mp4AudioReader::close()
{
    free_decoder(dec_);
}

bool Mp4AudioReader::open(const wchar_t *path)
{
    return open(utf8(path).c_str());
}

bool Mp4AudioReader::open(const char *path)
{
    close();
    skipTo_ = -1;
    dec_ = open_decoder(path, AVMEDIA_TYPE_AUDIO);
    if (dec_ == nullptr)
        return false;
    rate_ = dec_->cc->sample_rate;
    channels_ = dec_->cc->ch_layout.nb_channels;
    if (rate_ <= 0 || channels_ <= 0) {
        pclog::write("movie: %s: its audio does not decode (%d Hz, %d channels; AAC / MP3 are the formats)", path,
                     rate_, channels_);
        close();
        return false;
    }
    return true;
}

bool Mp4AudioReader::rewind()
{
    return seek(0);
}

bool Mp4AudioReader::seek(int64_t t)
{
    if (dec_ == nullptr)
        return false;
    const bool ok = seek_decoder(dec_, t);
    skipTo_ = ok && t > 0 ? t : -1;
    return ok;
}

bool Mp4AudioReader::next(std::vector<int16_t> &out)
{
    if (dec_ == nullptr)
        return false;
    for (;;) {
        if (receive(dec_) <= 0)
            return false;
        const AVFrame *f = dec_->frame;
        const int fch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : channels_;
        const size_t frames = f->nb_samples > 0 ? (size_t)f->nb_samples : 0;
        if (frames == 0 || fch != channels_ || f->sample_rate != rate_) {
            /* (a change of format partway: not this stream's sound) */
            av_frame_unref(dec_->frame);
            continue;
        }
        const int64_t ts = frame_time(dec_, (int64_t)frames * 10000000 / rate_);
        /* PC: after a seek, the frames before its time (the demuxer lands on
         * a packet of its own at or before it) */
        size_t first = 0;
        if (skipTo_ >= 0) {
            const int64_t ahead = skipTo_ - ts;
            first = ahead > 0 ? (size_t)std::min<int64_t>(ahead * rate_ / 10000000, (int64_t)frames) : 0;
            if (first < frames)
                skipTo_ = -1;
        }
        out.reserve(out.size() + (frames - first) * (size_t)channels());
        if (channels_ <= 2) {
            for (size_t i = first; i < frames; i++)
                for (int c = 0; c < channels_; c++)
                    out.push_back(to_s16(sample_at(f, c, (int)i, fch)));
        } else {
            /* more than two: the front pair with the centre (L R C ...) */
            for (size_t i = first; i < frames; i++) {
                const float c = 0.7071f * sample_at(f, 2, (int)i, fch);
                out.push_back(to_s16((sample_at(f, 0, (int)i, fch) + c) * 0.6f));
                out.push_back(to_s16((sample_at(f, 1, (int)i, fch) + c) * 0.6f));
            }
        }
        av_frame_unref(dec_->frame);
        return true;
    }
}

} // namespace gt3media
