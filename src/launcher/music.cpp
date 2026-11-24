/* music.cpp - the streamed music converted for the PC (see music.h). */
#include "music.h"

#include "disc_files.h"                     /* PC (2026-11-15): a disc image's data folder */
#include "util.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#include "engine/media/native/mp4_movie.h"   /* Mp4AudioReader: an .mp3's samples */
#include "engine/sound/native/bgm_files.h"

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace music {

namespace {

using namespace spu2;

std::wstring bgm_folder(const std::wstring &data)
{
    return util::with_slash(data) + L"bgm\\";
}

bool is_file(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; });
    return s;
}

struct AdsFile {
    std::wstring name;
    uint64_t size;
};

/* The folder's *.ads, by name. */
std::vector<AdsFile> ads_files(const std::wstring &bgm)
{
    std::vector<AdsFile> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((bgm + L"*.ads").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back({fd.cFileName, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const AdsFile &a, const AdsFile &b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

/* A positional read of a file handle (bgm_files' AdsReadFn). */
int read_handle(void *user, uint32_t offset, void *dst, uint32_t size)
{
    OVERLAPPED o = {};
    o.Offset = offset;
    DWORD got = 0;
    if (!ReadFile((HANDLE)user, dst, size, &got, &o))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (int)got;
}

bool read_all(const std::wstring &path, std::vector<uint8_t> *out, DWORD *err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

/* Writes the pieces to path.part, then puts it in place as path. */
bool write_pieces(const std::wstring &path, const std::vector<std::pair<const void *, size_t>> &pieces, DWORD *err)
{
    const std::wstring part = path + L".part";
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    bool ok = true;
    for (const auto &p : pieces) {
        const uint8_t *s = (const uint8_t *)p.first;
        size_t left = p.second;
        while (ok && left > 0) {
            DWORD n = (DWORD)std::min<size_t>(left, 1u << 24), put = 0;
            ok = WriteFile(h, s, n, &put, nullptr) && put == n;
            s += n;
            left -= n;
        }
    }
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (ok && !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *err = GetLastError();
        ok = false;
    }
    if (!ok)
        DeleteFileW(part.c_str());
    return ok;
}

/* Windows' name of a track's file. */
std::wstring track_path(const std::wstring &bgm, const std::string &file)
{
    return bgm_system_path(file) ? shown_file(file) : bgm + from_ansi(file);
}

/* PC (2026-11-15): a file of a disc image's volume, read as bgm_files reads
 * (AdsReadFn: the bytes read, 0 past the end). */
struct DiscRead {
    const disc_files::Disc *disc;
    const disc_files::File *file;
};

int read_disc(void *user, uint32_t offset, void *dst, uint32_t size)
{
    const DiscRead &r = *(const DiscRead *)user;
    if (offset >= r.file->size)
        return 0;
    const uint32_t n = std::min<uint32_t>(size, r.file->size - offset);
    return r.disc->read(*r.file, offset, dst, n) ? (int)n : -1;
}

/* Where a track's file was found: on the disk (path), or in the data
 * folder's disc image (disc, file). */
struct Where {
    std::wstring path;
    std::shared_ptr<const disc_files::Disc> disc;
    const disc_files::File *file = nullptr;
};

/* What bgm\<file> (or its other format's file) or the file at a full path is,
 * for ads_list_text; a disc image's data folder (PC 2026-11-15): what its
 * bgm\ does not have, the image's (the folder's own first, as the game). */
bool probe_file(const std::wstring &data, const std::string &file, BgmFileInfo *info, std::string *found,
                Where *where = nullptr)
{
    const std::wstring bgm = bgm_folder(data);
    for (const std::string &name : {file, bgm_other_name(file)}) {
        if (name.empty())
            continue;
        HANDLE h = CreateFileW(track_path(bgm, name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        LARGE_INTEGER size = {};
        const bool ok = GetFileSizeEx(h, &size) &&
                        bgm_probe(read_handle, h, (uint32_t)std::min<int64_t>(size.QuadPart, 0xFFFFFFFFll), info);
        CloseHandle(h);
        if (ok) {
            *found = name;
            if (where != nullptr)
                where->path = track_path(bgm, name);
            return true;
        }
    }
    if (bgm_system_path(file))
        return false;
    const std::shared_ptr<const disc_files::Disc> disc = disc_files::of(data);
    if (!disc)
        return false;
    for (const std::string &name : {file, bgm_other_name(file)}) {
        const disc_files::File *f = name.empty() ? nullptr : disc->find("bgm\\" + name);
        if (f == nullptr)
            continue;
        DiscRead r = {disc.get(), f};
        if (bgm_probe(read_disc, &r, f->size, info)) {
            *found = name;
            if (where != nullptr) {
                where->disc = disc;
                where->file = f;
            }
            return true;
        }
    }
    return false;
}

/* The .ads head of a file (needs_conversion). */
bool has_ads_head(const std::wstring &path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    uint8_t head[ADS_HEAD_BYTES];
    AdsHeader a;
    const bool ok = read_handle(h, 0, head, sizeof(head)) == (int)sizeof(head) &&
                    ads_parse_header(head, sizeof(head), &a) && (a.type == ADS_TYPE_ADPCM || a.type == ADS_TYPE_PCM);
    CloseHandle(h);
    return ok;
}

} // namespace

bool needs_conversion(const std::wstring &data)
{
    const std::wstring bgm = bgm_folder(data);
    for (const AdsFile &f : ads_files(bgm))
        if (has_ads_head(bgm + f.name))
            return true;
    return is_file(bgm + L"ads.inf") && !is_file(bgm + L"ads.txt");
}

uint64_t growth(uint64_t ads_bytes)
{
    /* 16 bytes of ADPCM -> 28 samples of 2 bytes: x 3.5, the .ads deleted */
    return ads_bytes / 2 * 5 + (1u << 20);
}

install::Result convert(const std::wstring &data, install::Listener &listener, std::wstring *message)
{
    const std::wstring bgm = bgm_folder(data);
    std::vector<AdsFile> files = ads_files(bgm);

    /* ads.inf: the tracks' tags (an intro's: its song's), and ads.txt */
    AdsList list;
    bool haveList = false;
    std::string listError;
    std::vector<uint8_t> inf;
    DWORD err = 0;
    if (is_file(bgm + L"ads.inf")) {
        if (read_all(bgm + L"ads.inf", &inf, &err))
            haveList = ads_inf_read(inf.data(), inf.size(), &list, &listError);
        else
            listError = util::narrow(util::system_error_text(err));
    }
    std::map<std::string, std::pair<std::string, std::string>> tags;
    if (haveList)
        for (int g = 0; g < ADS_GROUPS; g++)
            for (const AdsListTrack &k : list.group[g])
                tags.emplace(lower(k.file), std::make_pair(k.title, k.artist));

    uint64_t total = 0, done = 0;
    for (const AdsFile &f : files)
        total += f.size;
    listener.progress(0, total);
    int converted = 0;
    std::vector<std::wstring> problems;
    std::vector<uint8_t> src;
    std::vector<int16_t> pcm;
    for (const AdsFile &f : files) {
        if (listener.cancel_requested()) {
            *message = trf(L"The music conversion was stopped (%d files converted); it carries on with the rest the "
                           L"next time.",
                           converted);
            return install::Result::cancelled;
        }
        const std::wstring path = bgm + f.name;
        AdsHeader h;
        if (!read_all(path, &src, &err)) {
            problems.push_back(f.name + L": " + util::system_error_text(err));
        } else if (!ads_parse_header(src.data(), (uint32_t)std::min<size_t>(src.size(), 0xFFFFFFFFu), &h) ||
                   (h.type != ADS_TYPE_ADPCM && h.type != ADS_TYPE_PCM)) {
            problems.push_back(trf(L"%s: not an .ads (left as it is)", f.name.c_str()));
        } else {
            const uint32_t body = (uint32_t)std::min<size_t>(h.bodySize, src.size() - ADS_HEAD_BYTES);
            pcm.clear();
            AdpcmHistory hist[2];
            ads_decode(h, src.data() + ADS_HEAD_BYTES, body, hist, pcm);
            const uint32_t frames = (uint32_t)(pcm.size() / h.channels);
            std::string key = lower(util::narrow(f.name));
            auto t = tags.find(key);
            if (t == tags.end() && key.compare(0, 2, "i_") == 0)
                t = tags.find(key.substr(2));           /* an intro: its song's tags */
            const std::vector<uint8_t> head =
                wav_head(h.rate, h.channels, frames, t != tags.end() ? t->second.first : std::string(),
                         t != tags.end() ? t->second.second : std::string());
            const std::wstring wav = path.substr(0, path.size() - 4) + L".wav";
            if (!write_pieces(wav, {{head.data(), head.size()}, {pcm.data(), pcm.size() * 2}}, &err)) {
                problems.push_back(trf(L"%s: writing its .wav failed: %s", f.name.c_str(),
                                       util::system_error_text(err).c_str()));
            } else {
                converted++;
                if (!DeleteFileW(path.c_str()))
                    problems.push_back(trf(L"%s: converted, but it could not be deleted: %s", f.name.c_str(),
                                           util::system_error_text(GetLastError()).c_str()));
            }
        }
        done += f.size;
        listener.progress(done, total);
    }

    bool wroteList = false;
    if (haveList && !is_file(bgm + L"ads.txt")) {
        const std::string text = ads_list_text(list, [&data](const std::string &file, BgmFileInfo *info, std::string *found) {
            return probe_file(data, file, info, found);
        });
        if (write_pieces(bgm + L"ads.txt", {{text.data(), text.size()}}, &err))
            wroteList = true;
        else
            problems.push_back(L"ads.txt: " + util::system_error_text(err));
    } else if (!haveList && !listError.empty()) {
        problems.push_back(trf(L"ads.inf: %s (no ads.txt written)", util::widen(listError).c_str()));
    }

    *message = wroteList ? trf(L"Music: %d files converted to WAV, the list written as bgm\\ads.txt.", converted)
                         : trf(L"Music: %d files converted to WAV.", converted);
    if (problems.empty())
        return install::Result::ok;
    *message += std::wstring(L" ") + tr(L"Not converted:");
    for (size_t i = 0; i < problems.size() && i < 8; i++)
        *message += L"\n  " + problems[i];
    if (problems.size() > 8)
        *message += L"\n  " + trf(L"(%d more)", (int)(problems.size() - 8));
    return install::Result::failed;
}

/* ---- the music files and the list ---- */

/* (the game opens its music files by narrow names: the ANSI code page's on
 * Windows, UTF-8 elsewhere - platform/os/fs.h's native text) */
bool ansi(const std::wstring &w, std::string *out)
{
#ifdef _WIN32
    BOOL lossy = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), nullptr, 0, "?", &lossy);
    out->assign((size_t)std::max(n, 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), (int)w.size(), &(*out)[0], n, "?", &lossy);
    return !lossy;
#else
    *out = util::narrow(w);
    return true;
#endif
}

std::wstring from_ansi(const std::string &s)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
#else
    return util::widen(s);
#endif
}

std::string list_text(const std::wstring &data)
{
    const std::wstring bgm = bgm_folder(data);
    std::vector<uint8_t> b;
    DWORD err = 0;
    if (read_all(bgm + L"ads.txt", &b, &err))
        return std::string(b.begin(), b.end());
    AdsList list;
    original_list(data, &list);
    return ads_list_text(list, prober(data));
}

bool write_list(const std::wstring &data, const std::string &text, std::wstring *message)
{
    DWORD err = 0;
    /* (PC 2026-11-15: a disc image's folder has no bgm\ until its first list) */
    util::make_folders(bgm_folder(data));
    if (write_pieces(bgm_folder(data) + L"ads.txt", {{text.data(), text.size()}}, &err))
        return true;
    *message = trf(L"bgm\\ads.txt could not be written: %s.", util::system_error_text(err).c_str());
    return false;
}

bool original_list(const std::wstring &data, AdsList *out)
{
    std::vector<uint8_t> b;
    DWORD err = 0;
    std::string e;
    if (read_all(bgm_folder(data) + L"ads.inf", &b, &err))
        return ads_inf_read(b.data(), b.size(), out, &e);
    /* PC (2026-11-15): a disc image's folder: the image's */
    const std::shared_ptr<const disc_files::Disc> disc = disc_files::of(data);
    const disc_files::File *f = disc ? disc->find("bgm\\ads.inf") : nullptr;
    return f != nullptr && disc->whole(*f, &b) && ads_inf_read(b.data(), b.size(), out, &e);
}

int repair_markers(const std::wstring &data, std::wstring *message)
{
    const std::wstring bgm = bgm_folder(data);
    std::vector<uint8_t> inf, txt;
    DWORD err = 0;
    if (!is_file(bgm + L"ads.txt") || !read_all(bgm + L"ads.inf", &inf, &err) || !read_all(bgm + L"ads.txt", &txt, &err))
        return 0;
    AdsList right, wrong;
    std::string e;
    if (!ads_inf_read(inf.data(), inf.size(), &right, &e) || !ads_inf_read(inf.data(), inf.size(), &wrong, &e, true))
        return 0;
    /* ads.inf's tracks by file name (without its extension: x.ads became x.wav) */
    std::map<std::string, std::pair<const AdsListTrack *, const AdsListTrack *>> byName;
    for (int g = 0; g < ADS_GROUPS; g++)
        for (size_t i = 0; i < right.group[g].size() && i < wrong.group[g].size(); i++)
            byName.emplace(lower(bgm_base_name(right.group[g][i].file)),
                           std::make_pair(&right.group[g][i], &wrong.group[g][i]));
    /* the same times as the text keeps them (seconds to 6 decimals) */
    auto same = [](const std::vector<double> &a, const std::vector<double> &b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (std::fabs(a[i] - b[i]) > 2e-6)
                return false;
        return true;
    };
    AdsList list;
    std::vector<std::string> warnings;
    ads_list_parse(std::string(txt.begin(), txt.end()), &list, &warnings);
    int repaired = 0;
    for (int g = 0; g < ADS_GROUPS; g++)
        for (AdsListTrack &k : list.group[g]) {
            auto it = byName.find(lower(bgm_base_name(k.file)));
            if (it == byName.end())
                continue;
            const AdsListTrack &r = *it->second.first, &w = *it->second.second;
            bool misread = true, differs = false;
            for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++) {
                misread = misread && same(k.markers[ch], w.markers[ch]);
                differs = differs || !same(r.markers[ch], w.markers[ch]);
            }
            if (!misread || !differs)
                continue;
            for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++)
                k.markers[ch] = r.markers[ch];
            repaired++;
        }
    if (repaired == 0)
        return 0;
    if (!write_list(data, ads_list_text(list, prober(data)), message))
        return -1;
    *message = repaired == 1
                   ? std::wstring(tr(L"the original songs' beat markers repaired (1 track: the conversion had misread "
                                     L"them from ads.inf)."))
                   : trf(L"the original songs' beat markers repaired (%d tracks: the conversion had misread them from "
                         L"ads.inf).",
                         repaired);
    return repaired;
}

bool song_samples(const std::wstring &data, const std::string &file, std::vector<float> *mono, int *rate,
                  std::wstring *why)
{
    BgmFileInfo info;
    std::string found;
    Where where;
    mono->clear();
    if (!probe_file(data, file, &info, &found, &where)) {
        *why = trf(L"%s: missing, or not a music file the game plays", shown_file(file).c_str());
        return false;
    }
    const std::wstring &path = where.path;
    if (where.file != nullptr) {
        /* PC (2026-11-15): a song of the disc image (its .ads; a .wav as on the disk) */
        DiscRead r = {where.disc.get(), where.file};
        std::vector<uint8_t> raw(info.kind == BgmKind::wav ? 0 : info.dataBytes);
        if (info.kind == BgmKind::mp3) {
            *why = trf(L"%s: an .mp3 in a disc image is not read", shown_file(found).c_str());
            return false;
        }
        if (info.kind == BgmKind::wav) {
            raw.resize((size_t)info.frames * info.blockAlign);
            const int got = read_disc(&r, info.dataOffset, raw.data(), (uint32_t)raw.size());
            std::vector<float> fl;
            if (got > 0)
                wav_to_float(info, raw.data(), (uint32_t)got / info.blockAlign, fl);
            if (info.channels == 1)
                mono->assign(fl.begin(), fl.end());
            else
                for (size_t i = 0; i + 1 < fl.size(); i += 2)
                    mono->push_back(0.5f * (fl[i] + fl[i + 1]));
        } else {
            const int got = read_disc(&r, info.dataOffset, raw.data(), (uint32_t)raw.size());
            std::vector<int16_t> pcm;
            AdpcmHistory hist[2];
            if (got > 0)
                ads_decode(info.ads, raw.data(), (uint32_t)got, hist, pcm);
            const size_t ch = info.channels >= 2 ? 2 : 1;
            for (size_t i = 0; i + ch <= pcm.size(); i += ch)
                mono->push_back(ch == 1 ? pcm[i] / 32768.0f : (pcm[i] + pcm[i + 1]) / 65536.0f);
        }
        *rate = (int)info.rate;
        if (mono->empty()) {
            *why = trf(L"%s: no samples could be read from the disc image", shown_file(found).c_str());
            return false;
        }
        return true;
    }
    if (info.kind == BgmKind::mp3) {
        gt3media::MfScope mf;
        gt3media::Mp4AudioReader reader;
        if (!mf.ok() || !reader.open(path.c_str())) {
#ifdef _WIN32
            *why = trf(L"%s: Media Foundation does not decode it", shown_file(found).c_str());
#else
            *why = trf(L"%s: FFmpeg does not decode it", shown_file(found).c_str());
#endif
            return false;
        }
        const int ch = reader.channels();
        std::vector<int16_t> pcm;
        while (reader.next(pcm)) {
            for (size_t i = 0; i + (size_t)ch <= pcm.size(); i += (size_t)ch)
                mono->push_back(ch == 1 ? pcm[i] / 32768.0f : (pcm[i] + pcm[i + 1]) / 65536.0f);
            pcm.clear();
        }
        *rate = reader.rate();
    } else {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            *why = shown_file(found) + L": " + util::system_error_text(GetLastError());
            return false;
        }
        std::vector<uint8_t> raw;
        if (info.kind == BgmKind::wav) {
            /* a piece at a time: the samples as float (1 or 2 channels), their mean */
            const uint32_t piece = 65536;
            std::vector<float> fl;
            mono->reserve(info.frames);
            for (uint32_t at = 0; at < info.frames; at += piece) {
                const uint32_t n = std::min(piece, info.frames - at);
                raw.resize((size_t)n * info.blockAlign);
                const int got = read_handle(h, info.dataOffset + at * info.blockAlign, raw.data(), (uint32_t)raw.size());
                if (got <= 0)
                    break;
                fl.clear();
                wav_to_float(info, raw.data(), (uint32_t)got / info.blockAlign, fl);
                if (info.channels == 1)
                    mono->insert(mono->end(), fl.begin(), fl.end());
                else
                    for (size_t i = 0; i + 1 < fl.size(); i += 2)
                        mono->push_back(0.5f * (fl[i] + fl[i + 1]));
            }
        } else {
            /* an .ads: its body decoded whole */
            raw.resize(info.dataBytes);
            const int got = read_handle(h, info.dataOffset, raw.data(), (uint32_t)raw.size());
            std::vector<int16_t> pcm;
            AdpcmHistory hist[2];
            if (got > 0)
                ads_decode(info.ads, raw.data(), (uint32_t)got, hist, pcm);
            const size_t ch = info.channels >= 2 ? 2 : 1;
            for (size_t i = 0; i + ch <= pcm.size(); i += ch)
                mono->push_back(ch == 1 ? pcm[i] / 32768.0f : (pcm[i] + pcm[i + 1]) / 65536.0f);
        }
        CloseHandle(h);
        *rate = (int)info.rate;
    }
    if (mono->empty()) {
        *why = trf(L"%s: no samples could be read", shown_file(found).c_str());
        return false;
    }
    return true;
}

AdsListProbe prober(const std::wstring &data)
{
    return [data](const std::string &file, BgmFileInfo *info, std::string *found) {
        return probe_file(data, file, info, found);
    };
}

std::vector<MusicFile> disc_music(const std::wstring &data)
{
    std::vector<MusicFile> out;
    const std::shared_ptr<const disc_files::Disc> disc = disc_files::of(data);
    if (!disc)
        return out;
    const std::wstring bgm = bgm_folder(data);
    for (const disc_files::File *f : disc->folder("bgm")) {
        const std::string name = f->rel.substr(4);              /* (after "bgm\\") */
        if (!bgm_music_name(name))
            continue;
        /* (the folder's own file, or its twin, is the one played) */
        const std::string twin = bgm_other_name(name);
        if (is_file(bgm + from_ansi(name)) || (!twin.empty() && is_file(bgm + from_ansi(twin))))
            continue;
        MusicFile m;
        m.name = from_ansi(name);
        m.bytes = name;
        m.nameOk = true;
        m.found = true;
        m.disc = true;
        DiscRead r = {disc.get(), f};
        m.playable = bgm_probe(read_disc, &r, f->size, &m.info, false, true) && m.info.frames > 0;
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<MusicFile> music_files(const std::wstring &data)
{
    const std::wstring bgm = bgm_folder(data);
    std::vector<MusicFile> out;
    for (const wchar_t *pattern : {L"*.wav", L"*.mp3"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((bgm + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            MusicFile m;
            m.name = fd.cFileName;
            m.nameOk = ansi(m.name, &m.bytes);
            m.found = true;
            HANDLE f = CreateFileW((bgm + m.name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER size = {};
                m.playable = GetFileSizeEx(f, &size) &&
                             bgm_probe(read_handle, f, (uint32_t)std::min<int64_t>(size.QuadPart, 0xFFFFFFFFll), &m.info) &&
                             m.info.kind != BgmKind::ads && m.info.frames > 0;
                CloseHandle(f);
            }
            out.push_back(std::move(m));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(),
              [](const MusicFile &a, const MusicFile &b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

/* ---- songs anywhere on the machine ---- */

namespace {

std::wstring full_path(const std::wstring &p)
{
    wchar_t b[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, nullptr);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(b) : p;
}

/* The folder's songs and its sub-folders' into out; links not followed (a
 * loop), nothing hidden. */
void scan_songs(const std::wstring &folder, int depth, std::vector<std::wstring> *out)
{
    if (depth > 64)
        return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((util::with_slash(folder) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        const std::wstring path = util::with_slash(folder) + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                scan_songs(path, depth + 1, out);
            continue;
        }
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos &&
            (_wcsicmp(name.c_str() + dot, L".wav") == 0 || _wcsicmp(name.c_str() + dot, L".mp3") == 0))
            out->push_back(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

std::string track_file(const std::wstring &data, const std::wstring &path)
{
    const std::wstring full = full_path(path);
    const size_t slash = full.find_last_of(L"\\/");
    std::string name;
    if (slash != std::wstring::npos &&
        _wcsicmp(full.substr(0, slash + 1).c_str(), full_path(bgm_folder(data)).c_str()) == 0 &&
        ansi(full.substr(slash + 1), &name))
        return name;
    return util::narrow(full);
}

std::wstring shown_file(const std::string &file)
{
    if (!bgm_system_path(file))
        return from_ansi(file);
    /* (as host_file reads it: UTF-8, else the code page's, typed by hand) */
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file.c_str(), (int)file.size(), nullptr, 0);
    if (n <= 0)
        return from_ansi(file);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file.c_str(), (int)file.size(), &w[0], n);
    return w;
}

MusicFile probe_song(const std::wstring &path)
{
    MusicFile m;
    m.name = path;
    m.nameOk = true;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return m;
    m.found = true;
    LARGE_INTEGER size = {};
    m.playable = GetFileSizeEx(f, &size) &&
                 bgm_probe(read_handle, f, (uint32_t)std::min<int64_t>(size.QuadPart, 0xFFFFFFFFll), &m.info, false, true) &&
                 m.info.kind != BgmKind::ads && m.info.frames > 0;
    CloseHandle(f);
    return m;
}

std::vector<std::wstring> folder_songs(const std::wstring &folder)
{
    std::vector<std::wstring> out;
    scan_songs(folder, 0, &out);
    std::sort(out.begin(), out.end(),
              [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

/* ---- new songs ---- */

std::vector<NewSong> find_new_songs(const std::wstring &data, std::vector<std::wstring> *unplayable)
{
    std::vector<NewSong> out;
    const std::vector<MusicFile> files = music_files(data);
    if (files.empty())
        return out;

    /* what the list names (a track's twin of the other format too) */
    std::vector<std::string> listed, keptOut;
    ads_text_files(list_text(data), &listed, &keptOut);
    std::set<std::string> known;                /* lower-case names, listed or kept out */
    for (const std::vector<std::string> *v : {&listed, &keptOut})
        for (const std::string &f : *v) {
            known.insert(lower(f));
            known.insert(lower(bgm_other_name(f)));
        }
    /* a song's name: listed, kept out, or a new one also coming in */
    auto is_song = [&](const std::string &lowerName) {
        if (known.count(lowerName))
            return true;
        for (const MusicFile &m : files)
            if (lower(m.bytes) == lowerName)
                return true;
        return false;
    };
    for (const MusicFile &m : files) {
        const std::string key = lower(m.bytes);
        if (known.count(key))
            continue;
        if (key.compare(0, 2, "i_") == 0 && is_song(key.substr(2)))
            continue;                           /* an intro: i_<a song> (kept out with it) */
        if (!m.nameOk) {
            unplayable->push_back(trf(L"%s: its name has characters the game cannot open (rename it)", m.name.c_str()));
            continue;
        }
        if (!m.playable) {
            unplayable->push_back(
                trf(L"%s: not a format the game plays (a .wav of PCM or float samples, an .mp3)", m.name.c_str()));
            continue;
        }
        NewSong s;
        s.file = m.name;
        s.fileBytes = m.bytes;
        s.title = !m.info.title.empty() ? from_ansi(m.info.title) : from_ansi(bgm_base_name(m.bytes));
        s.artist = !m.info.artist.empty() ? from_ansi(m.info.artist) : L"unknown";
        out.push_back(std::move(s));
    }
    return out;
}

bool add_songs(const std::wstring &data, const std::vector<NewSong> &songs, std::wstring *message)
{
    std::vector<AdsListTrack> tracks;
    for (const NewSong &s : songs) {
        AdsListTrack k;
        k.file = s.fileBytes;
        std::string title, artist;
        ansi(s.title, &title);
        ansi(s.artist, &artist);
        /* (one line each: no line breaks, no control characters) */
        for (std::string *t : {&title, &artist})
            for (char &c : *t)
                if ((unsigned char)c < 0x20)
                    c = ' ';
        k.title = title;
        k.artist = artist;
        k.hasTitle = k.hasArtist = true;
        tracks.push_back(std::move(k));
    }
    if (!write_list(data, ads_text_add(list_text(data), ADS_GROUP_RACE_SONGS, tracks), message))
        return false;
    *message = songs.size() == 1 ? std::wstring(tr(L"1 new song added to the race songs."))
                                 : trf(L"%d new songs added to the race songs.", (int)songs.size());
    return true;
}

} // namespace music
