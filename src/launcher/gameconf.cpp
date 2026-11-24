/* gameconf.cpp - the product configs converted for the PC (gameconf.h). */
#include "gameconf.h"

#include "util.h"

#include "platform/gcf_tables.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif

#include <algorithm>
#include <vector>

namespace gameconf {
namespace {

std::wstring folder_of(const std::wstring &data)
{
    return util::with_slash(data) + L"gameconf\\";
}

bool is_file(const std::wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The folder's *.gcf, by name (without ".gcf"). */
std::vector<std::wstring> configs(const std::wstring &data)
{
    std::vector<std::wstring> out;
    const std::wstring dir = folder_of(data);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.gcf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        const size_t n = wcslen(fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n > 4 && _wcsicmp(fd.cFileName + n - 4, L".gcf") == 0)
            out.push_back(std::wstring(fd.cFileName).substr(0, n - 4));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool read_all(const std::wstring &path, std::vector<uint8_t> *out, DWORD *err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64ll << 20);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    *err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

/* A file's creation time (0 if it cannot be read). */
ULONGLONG created(const std::wstring &path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
        return 0;
    return (ULONGLONG)a.ftCreationTime.dwHighDateTime << 32 | a.ftCreationTime.dwLowDateTime;
}

/* What the game reads of a config, in the .gcf's layout with its sections
 * by type (the game finds a section by its type: their order is the file's
 * own, the database's its tables'). */
std::vector<uint8_t> image_of(gcftables::Config config)
{
    std::sort(config.sections.begin(), config.sections.end());
    return gcftables::image(config);
}

/* What the game reads of a database file (image_of); false with why. */
bool database_image(const std::wstring &path, gcftables::Config *config, std::vector<uint8_t> *img, std::wstring *why)
{
    std::vector<uint8_t> bytes;
    DWORD err = 0;
    if (!read_all(path, &bytes, &err)) {
        *why = util::system_error_text(err);
        return false;
    }
    std::string error;
    std::vector<std::string> warnings;
    if (!gcftables::read_database(bytes.data(), bytes.size(), config, &warnings, &error)) {
        *why = util::widen(error);
        return false;
    }
    if (!warnings.empty()) {
        *why = util::widen(warnings[0]);
        return false;
    }
    *img = image_of(*config);
    return true;
}

/* gameconf\<name>.gcf written out as gameconf\<name>.sqlite3, read back and
 * checked, the file then deleted; false with why. */
bool convert_one(const std::wstring &dir, const std::wstring &name, std::wstring *why)
{
    std::vector<uint8_t> gcf;
    DWORD err = 0;
    if (!read_all(dir + name + L".gcf", &gcf, &err)) {
        *why = util::system_error_text(err);
        return false;
    }
    gcftables::Config config;
    std::string error;
    std::vector<std::string> warnings;
    if (!gcftables::read_gcf(gcf.data(), gcf.size(), &config, &error, &warnings)) {
        *why = util::widen(error);
        return false;
    }
    const std::wstring part = dir + name + L".sqlite3.part", done = dir + name + L".sqlite3";
    DeleteFileW(part.c_str());              /* (a stop's leftover) */
    if (!gcftables::write_database(config, util::system_path(part), &error) ||
        (!warnings.empty() && !gcftables::keep_file(gcf, util::system_path(part), &error))) {
        *why = util::widen(error);
        DeleteFileW(part.c_str());
        return false;
    }
    gcftables::Config back;
    std::vector<uint8_t> img;
    if (!database_image(part, &back, &img, why) || img != image_of(config)) {
        if (why->empty())
            *why = tr(L"the database does not read as the file");
        *why = trf(L"the check of its database failed (%s); the .gcf is kept", why->c_str());
        DeleteFileW(part.c_str());
        return false;
    }
    if (!MoveFileExW(part.c_str(), done.c_str(), MOVEFILE_WRITE_THROUGH)) {
        *why = util::system_error_text(GetLastError());
        DeleteFileW(part.c_str());
        return false;
    }
    DeleteFileW((dir + name + L".gcf").c_str());
    return true;
}

/* gameconf\<name>.gcf beside its database (an earlier launcher kept them):
 * deleted when the database reads as it, or - an edited database - when it
 * is the file the database was made of (older than it) and the database has
 * every table it has (the game reads only the database then; sections no
 * table holds kept in it first).  Else kept: why. */
bool remove_one(const std::wstring &dir, const std::wstring &name, std::wstring *why)
{
    const std::wstring gcfPath = dir + name + L".gcf", dbPath = dir + name + L".sqlite3";
    std::vector<uint8_t> gcf;
    DWORD err = 0;
    if (!read_all(gcfPath, &gcf, &err)) {
        *why = util::system_error_text(err);
        return false;
    }
    gcftables::Config file, db;
    std::string error;
    std::vector<std::string> warnings;
    if (!gcftables::read_gcf(gcf.data(), gcf.size(), &file, &error, &warnings)) {
        *why = trf(L"%s (kept)", util::widen(error).c_str());
        return false;
    }
    std::vector<uint8_t> img;
    if (!database_image(dbPath, &db, &img, why)) {
        *why = trf(L"its database cannot be read without warnings (%s): kept", why->c_str());
        return false;
    }
    const bool same = img == image_of(file);
    if (!same) {
        bool all = true;
        for (int s : file.sections)
            all = all && db.has(s);
        if (!all) {
            *why = tr(L"its database lacks tables the game takes from it: kept");
            return false;
        }
        if (created(gcfPath) > created(dbPath)) {
            *why = tr(L"newer than its database, which the game reads: kept (delete the database to have it converted)");
            return false;
        }
    }
    if (!warnings.empty()) {
        const std::wstring part = dbPath + L".part";
        if (!CopyFileW(dbPath.c_str(), part.c_str(), FALSE) || !gcftables::keep_file(gcf, util::system_path(part), &error) ||
            !MoveFileExW(part.c_str(), dbPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            *why = trf(L"%s: kept (its sections no table holds could not be put in the database)",
                       (error.empty() ? util::system_error_text(GetLastError()) : util::widen(error)).c_str());
            DeleteFileW(part.c_str());
            return false;
        }
    }
    if (!DeleteFileW(gcfPath.c_str())) {
        *why = util::system_error_text(GetLastError());
        return false;
    }
    return true;
}

} // namespace

bool needs_conversion(const std::wstring &data)
{
    return !configs(data).empty();
}

bool convert(const std::wstring &data, std::wstring *message, std::vector<std::wstring> *doneOut,
             std::vector<std::wstring> *problemsOut, std::vector<std::wstring> *removedOut)
{
    const std::wstring dir = folder_of(data);
    std::vector<std::wstring> done, problems, removed;
    for (const std::wstring &name : configs(data)) {
        std::wstring why;
        if (is_file(dir + name + L".sqlite3")) {
            if (remove_one(dir, name, &why))
                removed.push_back(name);
            else
                problems.push_back(name + L".gcf: " + why);
        } else if (convert_one(dir, name, &why)) {
            done.push_back(name);
        } else {
            problems.push_back(name + L".gcf: " + why);
        }
    }
    message->clear();
    auto list = [](const std::vector<std::wstring> &v) {
        std::wstring s;
        for (const std::wstring &d : v)
            s += (s.empty() ? L"" : L", ") + d;
        return s;
    };
    if (!done.empty())
        *message = trf(L"Game configs written as databases (gameconf\\<config>.sqlite3), their .gcf files deleted: %s.",
                       list(done).c_str());
    if (!removed.empty())
        *message += (message->empty() ? L"" : L" ") +
                    trf(L"The .gcf files beside their databases deleted: %s.", list(removed).c_str());
    for (const std::wstring &p : problems)
        *message += (message->empty() ? L"" : L" ") + trf(L"Left as it was: %s.", p.c_str());
    if (doneOut != nullptr)
        *doneOut = done;
    if (problemsOut != nullptr)
        *problemsOut = problems;
    if (removedOut != nullptr)
        *removedOut = removed;
    return problems.empty();
}

} // namespace gameconf
