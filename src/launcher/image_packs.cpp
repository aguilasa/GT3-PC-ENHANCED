/* image_packs.cpp - the .imgs packs of a data folder as folders of PNG files
 * (image_packs.h). */
#include "image_packs.h"

#include "inflate.h"
#include "install.h"
#include "iso9660.h"
#include "util.h"
#include "volume.h"

#include "engine/pgl/native/gs_imgs.h"
#include "platform/png_image.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace image_packs {
namespace {

constexpr DWORD kMaxFile = 256u << 20;

bool is_file(const std::wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::wstring &path, std::vector<uint8_t> *out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (LONGLONG)kMaxFile;
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, out->data(), (DWORD)size.QuadPart, &got, nullptr) && got == (DWORD)size.QuadPart;
    }
    CloseHandle(h);
    return ok;
}

/* `text' as the file `path', whole or not there (written beside it, then
 * moved into place). */
bool write_text(const std::wstring &path, const std::string &text)
{
    const std::wstring part = path + L".part";
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == (DWORD)text.size();
    CloseHandle(h);
    if (ok)
        ok = MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileW(part.c_str());
    return ok;
}

/* The pack folders a converted disc's packs make (the names of the packs on
 * the discs: race\<language>\display, menu\disp ...), for needs_sizes. */
bool pack_folder_name(const std::wstring &name)
{
    static const wchar_t *const kNames[] = {L"display", L"disp", L"disp_us", L"test", L"opening", L"arcade",
                                            L"arcade_menu"};
    for (const wchar_t *n : kNames)
        if (_wcsicmp(name.c_str(), n) == 0)
            return true;
    return false;
}

/* The folders of `data' (race\, menu\ and their language folders) that look
 * like a converted pack - a pack folder's name, PNG files in it - and have no
 * sizes.txt, relative to data (`rel' the folder searched), into out. */
void find_unsized(const std::wstring &data, const std::wstring &rel, int depth, std::vector<std::wstring> *out)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((util::with_slash(data) + rel + L"*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L"..")
            continue;
        const std::wstring sub = rel + name + L"\\";
        const std::wstring full = util::with_slash(data) + sub;
        WIN32_FIND_DATAW png;
        HANDLE p = FindFirstFileExW((full + L"*.png").c_str(), FindExInfoBasic, &png, FindExSearchNameMatch, nullptr, 0);
        const bool pictures = p != INVALID_HANDLE_VALUE;
        if (pictures)
            FindClose(p);
        if (pictures && pack_folder_name(name) && !is_file(full + util::widen(gstex::kImgsSizesFile)))
            out->push_back(sub);
        else if (depth > 0)
            find_unsized(data, sub, depth - 1, out);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

/* A PNG file's size from its header (IHDR), 0 x 0 when it is no PNG. */
void png_size(const std::wstring &path, uint32_t *w, uint32_t *h)
{
    *w = *h = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    uint8_t b[24];
    DWORD got = 0;
    if (ReadFile(f, b, sizeof(b), &got, nullptr) && got == sizeof(b) && std::memcmp(b + 1, "PNG", 3) == 0 &&
        std::memcmp(b + 12, "IHDR", 4) == 0) {
        *w = ((uint32_t)b[16] << 24) | ((uint32_t)b[17] << 16) | ((uint32_t)b[18] << 8) | b[19];
        *h = ((uint32_t)b[20] << 24) | ((uint32_t)b[21] << 16) | ((uint32_t)b[22] << 8) | b[23];
    }
    CloseHandle(f);
}

/* The .imgs files under `folder' (relative to `root'), into out. */
void find_packs(const std::wstring &root, const std::wstring &folder, std::vector<std::wstring> *out)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((util::with_slash(root) + folder + L"*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            find_packs(root, folder + name + L"\\", out);
        } else if (name.size() > 5 && _wcsicmp(name.c_str() + name.size() - 5, L".imgs") == 0) {
            out->push_back(folder + name);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

/* A name that is a file's name and no path, as Windows keeps it (no ending
 * dot or space). */
bool plain_name(const std::string &name)
{
    if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ')
        return false;
    for (char c : name)
        if ((unsigned char)c < 0x20 || strchr("<>:\"/\\|?*", c) != nullptr)
            return false;
    return true;
}

bool same_ignoring_case(const std::string &a, const std::string &b)
{
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

/* The picture's RGBA bytes with a PNG's alpha. */
std::vector<uint8_t> png_pixels(const gstex::ImgsImage &img)
{
    std::vector<uint8_t> out(img.rgba.size() * 4);
    for (size_t i = 0; i < img.rgba.size(); i++) {
        const uint32_t px = img.rgba[i];
        out[i * 4] = (uint8_t)px;
        out[i * 4 + 1] = (uint8_t)(px >> 8);
        out[i * 4 + 2] = (uint8_t)(px >> 16);
        out[i * 4 + 3] = (uint8_t)gstex::imgs_alpha_to_png(px >> 24);
    }
    return out;
}

/* The PNG file at `path' as an image named `name' (the GS's alpha), as the
 * game reads it. */
bool read_png(png_image::Session &png, const std::wstring &path, const std::string &name, gstex::ImgsImage *out,
              std::vector<uint8_t> *raw)
{
    std::vector<uint8_t> file, rgba;
    int w = 0, h = 0;
    if (!read_file(path, &file) || !png.decode(file.data(), file.size(), &rgba, &w, &h))
        return false;
    out->name = name;
    out->w = (uint32_t)w;
    out->h = (uint32_t)h;
    out->rgba.resize((size_t)w * h);
    for (size_t i = 0; i < out->rgba.size(); i++)
        out->rgba[i] = (uint32_t)rgba[i * 4] | ((uint32_t)rgba[i * 4 + 1] << 8) | ((uint32_t)rgba[i * 4 + 2] << 16) |
                       (gstex::imgs_alpha_from_png(rgba[i * 4 + 3]) << 24);
    if (raw != nullptr)
        raw->swap(rgba);
    return true;
}

/* One pack (`rel' of the data folder) converted; false with the reason (the
 * pack kept).  png: the calling thread's. */
bool convert_pack(png_image::Session &png, const std::wstring &data, const std::wstring &rel, int *written, int *kept,
                  std::wstring *why)
{
    const std::wstring pack = util::with_slash(data) + rel;
    std::vector<uint8_t> bytes;
    if (!read_file(pack, &bytes)) {
        *why = tr(L"it could not be read");
        return false;
    }
    std::vector<gstex::ImgsImage> images;
    std::string err;
    if (!gstex::imgs_read(bytes.data(), bytes.size(), images, &err)) {
        *why = util::widen(err);
        return false;
    }
    for (size_t i = 0; i < images.size(); i++) {
        if (!plain_name(images[i].name)) {
            *why = trf(L"the image name \"%s\" is no file's name", util::widen(images[i].name).c_str());
            return false;
        }
        for (size_t j = 0; j < i; j++)
            if (same_ignoring_case(images[i].name, images[j].name)) {
                *why = trf(L"two images are named %s (but for their case)", util::widen(images[i].name).c_str());
                return false;
            }
    }
    const std::wstring folder = pack.substr(0, pack.size() - 5);
    if (is_file(folder)) {
        *why = tr(L"a file has the folder's name");
        return false;
    }
    if (!util::make_folders(folder)) {
        *why = trf(L"the folder could not be created: %s", util::system_error_text(GetLastError()).c_str());
        return false;
    }
    /* each picture written and read back (kept for the pack below) */
    std::vector<bool> fresh(images.size(), false);
    std::vector<gstex::ImgsImage> readBack;
    for (size_t i = 0; i < images.size(); i++) {
        const gstex::ImgsImage &img = images[i];
        const std::wstring file = util::with_slash(folder) + util::widen(img.name) + L".png";
        if (is_file(file)) {
            (*kept)++;
            continue;
        }
        const std::vector<uint8_t> pixels = png_pixels(img);
        gstex::ImgsImage back;
        std::vector<uint8_t> raw;
        if (!png.encode_file(file, pixels.data(), (int)img.w, (int)img.h, 1.0, true)) {
            *why = trf(L"%s.png could not be written", util::widen(img.name).c_str());
            return false;
        }
        if (!read_png(png, file, img.name, &back, &raw) || raw != pixels || back.w != img.w || back.h != img.h ||
            back.rgba != img.rgba) {
            DeleteFileW(file.c_str());
            *why = trf(L"%s.png does not read back the same", util::widen(img.name).c_str());
            return false;
        }
        fresh[i] = true;
        readBack.push_back(std::move(back));
        (*written)++;
    }
    /* the pack the game builds from the folder (every PNG in it: those just
     * read back, the others read now), each picture written checked against
     * the pack's own */
    std::vector<gstex::ImgsImage> folderImages;
    {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((util::with_slash(folder) + L"*.png").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, 0);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = fd.cFileName;
                /* (the pattern matches short names too: the ending checked) */
                if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
                    name.size() <= 4 || _wcsicmp(name.c_str() + name.size() - 4, L".png") != 0)
                    continue;
                name.resize(name.size() - 4);
                const std::string stem = util::narrow(name);
                bool known = false;
                for (gstex::ImgsImage &b : readBack)
                    if (!b.name.empty() && same_ignoring_case(b.name, stem)) {
                        b.name = stem;
                        folderImages.push_back(std::move(b));
                        b.name.clear();
                        known = true;
                        break;
                    }
                if (!known) {
                    gstex::ImgsImage img;
                    if (!read_png(png, util::with_slash(folder) + fd.cFileName, stem, &img, nullptr)) {
                        FindClose(f);
                        *why = trf(L"%s is not a PNG the game can read", fd.cFileName);
                        return false;
                    }
                    folderImages.push_back(std::move(img));
                }
                /* (drawn at the pack image's size, as the game takes it from
                 * sizes.txt) */
                for (const gstex::ImgsImage &orig : images)
                    if (same_ignoring_case(orig.name, stem)) {
                        folderImages.back().drawW = orig.w;
                        folderImages.back().drawH = orig.h;
                    }
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
    }
    std::vector<uint8_t> built;
    std::vector<gstex::ImgsImage> back;
    if (!gstex::imgs_build(folderImages, built, &err) || !gstex::imgs_read(built.data(), built.size(), back, &err)) {
        *why = trf(L"the game's pack from the folder: %s", util::widen(err).c_str());
        return false;
    }
    for (size_t i = 0; i < images.size(); i++) {
        const gstex::ImgsImage *b = nullptr;
        for (const gstex::ImgsImage &x : back)
            if (same_ignoring_case(x.name, images[i].name))
                b = &x;
        if (b == nullptr) {
            *why = trf(L"%s is missing from the game's pack", util::widen(images[i].name).c_str());
            return false;
        }
        if (fresh[i] && (b->w != images[i].w || b->h != images[i].h || b->rgba != images[i].rgba)) {
            *why = trf(L"%s differs in the game's pack", util::widen(images[i].name).c_str());
            return false;
        }
        if (b->drawW != images[i].w || b->drawH != images[i].h) {
            *why = trf(L"%s's draw size differs in the game's pack", util::widen(images[i].name).c_str());
            return false;
        }
    }
    /* the pictures' draw sizes, the pack's own (one there - edited - kept) */
    const std::wstring sizes = util::with_slash(folder) + util::widen(gstex::kImgsSizesFile);
    if (!is_file(sizes) && !write_text(sizes, gstex::imgs_sizes_text(images))) {
        *why = trf(L"%s could not be written", util::widen(gstex::kImgsSizesFile).c_str());
        return false;
    }
    if (!DeleteFileW(pack.c_str())) {
        *why = trf(L"it could not be deleted: %s", util::system_error_text(GetLastError()).c_str());
        return false;
    }
    return true;
}

} // namespace

bool needs_conversion(const std::wstring &data)
{
    std::vector<std::wstring> packs;
    find_packs(data, L"", &packs);
    return !packs.empty();
}

bool needs_sizes(const std::wstring &data)
{
    std::vector<std::wstring> folders;
    find_unsized(data, L"", 2, &folders);
    return !folders.empty();
}

bool write_sizes(const std::wstring &data, std::wstring *message)
{
    message->clear();
    std::vector<std::wstring> folders;
    find_unsized(data, L"", 2, &folders);
    if (folders.empty())
        return true;
    /* the disc the version was installed from (the marker's source) */
    std::wstring source;
    {
        std::vector<uint8_t> marker;
        if (read_file(util::with_slash(data) + install::kMarkerFile, &marker)) {
            const std::string text(marker.begin(), marker.end());
            size_t at = 0;
            while (at < text.size()) {
                size_t end = text.find('\n', at);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(at, end - at);
                at = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.compare(0, 7, "source ") == 0)
                    source = util::widen(line.substr(7));
            }
        }
    }
    /* its packs, read from its volume: an ISO image's GT3.VOL, or a volume
     * file itself */
    int fromDisc = 0, fromPictures = 0;
    std::wstring sourceNote;
    if (!source.empty()) {
        iso9660::Image image;
        install::DiscInfo disc;
        HANDLE vol = INVALID_HANDLE_VALUE;
        volume::Reader read;
        uint64_t volumeSize = 0;
        std::wstring err;
        bool wrong = false;
        if (image.open(source, &err) && install::check_disc(image, &disc, &err, &wrong)) {
            read = [&image, &disc](uint64_t offset, void *buf, uint32_t size) {
                std::wstring e;
                return image.read(disc.volume, offset, buf, size, &e);
            };
            volumeSize = disc.volume.size;
        } else {
            vol = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            LARGE_INTEGER size = {};
            if (vol != INVALID_HANDLE_VALUE && GetFileSizeEx(vol, &size)) {
                volumeSize = (uint64_t)size.QuadPart;
                read = [vol](uint64_t offset, void *buf, uint32_t n) {
                    OVERLAPPED o = {};
                    o.Offset = (DWORD)offset;
                    o.OffsetHigh = (DWORD)(offset >> 32);
                    DWORD got = 0;
                    return ReadFile(vol, buf, n, &got, &o) && got == n;
                };
            }
        }
        std::vector<volume::File> files;
        if (read && volume::list_files(read, volumeSize, &files, &err)) {
            for (const volume::File &f : files) {
                if (f.path.size() <= 5 || _stricmp(f.path.c_str() + f.path.size() - 5, ".imgs") != 0)
                    continue;
                std::wstring rel = util::widen(f.path.substr(0, f.path.size() - 5)) + L"\\";
                std::replace(rel.begin(), rel.end(), L'/', L'\\');
                auto it = std::find_if(folders.begin(), folders.end(),
                                       [&rel](const std::wstring &x) { return _wcsicmp(x.c_str(), rel.c_str()) == 0; });
                if (it == folders.end())
                    continue;
                std::vector<uint8_t> stored(f.stored), bytes;
                std::vector<gstex::ImgsImage> images;
                std::string why;
                std::wstring e;
                if (f.stored == 0 || !read((uint64_t)f.lsn * volume::kSectorSize, stored.data(), f.stored))
                    continue;
                if (f.packed) {
                    if (!inflate::gunzip(stored.data(), stored.size(), &bytes, &e))
                        continue;
                } else {
                    bytes.swap(stored);
                }
                if (!gstex::imgs_read(bytes.data(), bytes.size(), images, &why))
                    continue;
                if (write_text(util::with_slash(data) + *it + util::widen(gstex::kImgsSizesFile),
                               gstex::imgs_sizes_text(images))) {
                    fromDisc++;
                    folders.erase(it);
                }
            }
        } else {
            sourceNote = L" " + trf(L"(the disc image it was installed from, %s, could not be read)", source.c_str());
        }
        if (vol != INVALID_HANDLE_VALUE)
            CloseHandle(vol);
    } else {
        sourceNote = std::wstring(L" ") + tr(L"(its install does not name the disc image it came from)");
    }
    /* the rest: the pictures' sizes as they are */
    for (const std::wstring &rel : folders) {
        const std::wstring folder = util::with_slash(data) + rel;
        std::vector<gstex::ImgsImage> images;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((folder + L"*.png").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        do {
            std::wstring name = fd.cFileName;
            if (name.size() <= 4 || _wcsicmp(name.c_str() + name.size() - 4, L".png") != 0)
                continue;
            gstex::ImgsImage img;
            png_size(folder + name, &img.w, &img.h);
            if (img.w == 0)
                continue;
            name.resize(name.size() - 4);
            img.name = util::narrow(name);
            images.push_back(std::move(img));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(images.begin(), images.end(),
                  [](const gstex::ImgsImage &a, const gstex::ImgsImage &b) { return a.name < b.name; });
        if (write_text(folder + util::widen(gstex::kImgsSizesFile), gstex::imgs_sizes_text(images)))
            fromPictures++;
    }
    if (fromDisc != 0)
        *message = fromDisc == 1
                       ? trf(L"The HUD pictures' sizes (<pack>\\sizes.txt: a picture of another size is drawn at it) read "
                             L"from the disc image for %d pack folder.", fromDisc)
                       : trf(L"The HUD pictures' sizes (<pack>\\sizes.txt: a picture of another size is drawn at it) read "
                             L"from the disc image for %d pack folders.", fromDisc);
    /* (sourceNote: a note in parentheses after "now", or "") */
    if (fromPictures != 0)
        *message += (message->empty() ? L"" : L" ") +
                    (fromPictures == 1
                         ? trf(L"For %d pack folder the sizes are the pictures' as they are now%s: a picture already "
                               L"resized needs its line in sizes.txt set to the original's.",
                               fromPictures, sourceNote.c_str())
                         : trf(L"For %d pack folders the sizes are the pictures' as they are now%s: a picture already "
                               L"resized needs its line in sizes.txt set to the original's.",
                               fromPictures, sourceNote.c_str()));
    return fromPictures == 0;
}

bool convert(const std::wstring &data, std::wstring *message)
{
    message->clear();
    std::vector<std::wstring> packs;
    find_packs(data, L"", &packs);
    if (packs.empty())
        return true;
    /* the packs on worker threads (each its own folder; a PNG session a
     * thread): some 20 packs, 3,000 pictures a version */
    struct Result {
        bool ok = false;
        int written = 0, kept = 0;
        std::wstring why;
    };
    std::vector<Result> results(packs.size());
    std::atomic<size_t> next{0};
    auto work = [&] {
        png_image::Session png;
        for (size_t i; (i = next++) < packs.size();)
            results[i].ok = convert_pack(png, data, packs[i], &results[i].written, &results[i].kept, &results[i].why);
    };
    const unsigned threads = std::max(1u, std::min({std::thread::hardware_concurrency(), 8u, (unsigned)packs.size()}));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++)
        pool.emplace_back(work);
    work();
    for (std::thread &t : pool)
        t.join();
    int done = 0, written = 0, kept = 0;
    std::wstring problems;
    for (size_t i = 0; i < packs.size(); i++) {
        written += results[i].written;
        kept += results[i].kept;
        if (results[i].ok)
            done++;
        else
            problems += L"\n        " + trf(L"%s: %s; it is kept", packs[i].c_str(), results[i].why.c_str());
    }
    if (done != 0) {
        /* (the counts: "1 pack", "3 packs"; "1 picture", "120 pictures") */
        const std::wstring packsText = done == 1 ? trf(L"%d pack", done) : trf(L"%d packs", done);
        const std::wstring picturesText = written == 1 ? trf(L"%d picture", written) : trf(L"%d pictures", written);
        if (kept != 0)
            *message = trf(L"Image packs written as folders of PNG files (<pack>\\<image>.png): %s, %s, %d already there "
                           L"kept; the .imgs files deleted.",
                           packsText.c_str(), picturesText.c_str(), kept);
        else
            *message = trf(L"Image packs written as folders of PNG files (<pack>\\<image>.png): %s, %s; the .imgs files "
                           L"deleted.",
                           packsText.c_str(), picturesText.c_str());
    }
    if (!problems.empty())
        *message += (message->empty() ? std::wstring(tr(L"Image packs left as they were:"))
                                      : std::wstring(L" ") + tr(L"Left as they were:")) +
                    problems;
    return problems.empty();
}

} // namespace image_packs
