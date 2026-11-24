/* stills.cpp - stream\course.ipic to PNG files (stills.h). */
#include "stills.h"

#include "util.h"

#include "engine/media/native/ipu_image.h"
#include "platform/png_image.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif

#include <cstdint>
#include <cstring>
#include <vector>

namespace stills {
namespace {

constexpr const wchar_t *kArchive = L"stream\\course.ipic";
constexpr const wchar_t *kFolder = L"stream\\course";
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

/* A name that is a file's name and no path. */
bool plain_name(const char *name)
{
    if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const char *p = name; *p != '\0'; p++)
        if ((unsigned char)*p < 0x20 || strchr("<>:\"/\\|?*", *p) != nullptr)
            return false;
    return true;
}

/* The picture at `p` as the PNG `path`, read back and compared.  False with
 * the reason (nothing left at path). */
bool write_still(const uint8_t *p, size_t avail, const std::wstring &path, std::wstring *why)
{
    gt3media::IpuImageInfo info;
    if (!gt3media::ipu_image_info(p, avail, &info)) {
        *why = tr(L"not a picture");
        return false;
    }
    std::vector<uint8_t> rgba((size_t)info.width * info.height * 4);
    gt3media::IpuImageStats stats;
    if (!gt3media::ipu_image_decode(p, avail, rgba.data(), 0, gt3media::IpuLayout::Linear, 0xFF, &stats, rgba.size()) ||
        stats.macroblocks != info.width / 16 * (info.height / 16)) {
        *why = tr(L"it does not decode");
        return false;
    }
    if (!png_image::encode_file(path, rgba.data(), info.width, info.height)) {
        *why = tr(L"the file could not be written");
        return false;
    }
    std::vector<uint8_t> file, back;
    int w = 0, h = 0;
    if (!read_file(path, &file) || !png_image::decode(file.data(), file.size(), &back, &w, &h) || w != info.width ||
        h != info.height || back != rgba) {
        DeleteFileW(path.c_str());
        *why = tr(L"the file does not read back the same");
        return false;
    }
    return true;
}

} // namespace

bool needs_conversion(const std::wstring &data)
{
    return is_file(util::with_slash(data) + kArchive);
}

bool convert(const std::wstring &data, std::wstring *message)
{
    message->clear();
    const std::wstring archive = util::with_slash(data) + kArchive;
    if (!is_file(archive))
        return true;
    std::vector<uint8_t> bytes;
    int count = 0;
    if (read_file(archive, &bytes))
        count = gt3media::ipic_archive_count(bytes.data(), bytes.size());
    if (count <= 0) {
        *message = trf(L"%s could not be read as an archive of stills: it is left as it is.", kArchive);
        return false;
    }
    const std::wstring folder = util::with_slash(data) + kFolder;
    if (!util::make_folders(folder)) {
        *message = trf(L"The folder %s could not be created: %s.", folder.c_str(),
                       util::system_error_text(GetLastError()).c_str());
        return false;
    }
    int written = 0, kept = 0;
    for (int i = 0; i < count; i++) {
        gt3media::IpicEntryInfo e;
        std::wstring why;
        if (!gt3media::ipic_archive_entry(bytes.data(), bytes.size(), i, &e)) {
            why = tr(L"its entry is damaged");
        } else if (!plain_name(e.name)) {
            why = tr(L"its name is no file's");
        } else {
            const std::wstring png = util::with_slash(folder) + util::widen(e.name) + L".png";
            if (is_file(png)) {
                kept++;
                continue;
            }
            if (write_still(bytes.data() + e.offset, bytes.size() - e.offset, png, &why)) {
                written++;
                continue;
            }
        }
        *message = trf(L"Still %d of %s: %s. The archive is kept.", i + 1, kArchive, why.c_str());
        return false;
    }
    if (!DeleteFileW(archive.c_str())) {
        *message = trf(L"%s could not be deleted: %s.", kArchive, util::system_error_text(GetLastError()).c_str());
        return false;
    }
    if (kept != 0)
        *message = trf(L"Stills written as PNG files (%s\\<name>.png): %d, %d already there kept; %s deleted.", kFolder,
                       written, kept, kArchive);
    else
        *message = trf(L"Stills written as PNG files (%s\\<name>.png): %d; %s deleted.", kFolder, written, kArchive);
    return true;
}

} // namespace stills
