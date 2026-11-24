/* databases.cpp - the database folder converted for the PC (databases.h). */
#include "databases.h"

#include "util.h"

#include "platform/param_db.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#ifdef _WIN32
#include <windows.h>
#else
#include "win32_posix.h"
#endif

#include <cstdint>
#include <utility>

namespace databases {
namespace {

std::wstring folder_of(const std::wstring &data)
{
    return util::with_slash(data) + L"database\\";
}

bool is_file(const std::wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The regions ("_us"...) whose paramdb<r>.db has no paramdb<r>.sqlite3. */
std::vector<std::wstring> pending_regions(const std::wstring &dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"paramdb*.db").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        const std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.size() < 10 ||
            _wcsicmp(name.c_str() + name.size() - 3, L".db") != 0)
            continue;
        const std::wstring region = name.substr(7, name.size() - 10);
        if (!is_file(dir + L"paramdb" + region + L".sqlite3"))
            out.push_back(region);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

const wchar_t *const kCommonFiles[4] = {L"carcolor.db", L"carcolor.sdb", L"racemode.db", L"racedetail.db"};

bool common_pending(const std::wstring &dir)
{
    if (is_file(dir + L"common.sqlite3"))
        return false;
    for (const wchar_t *f : kCommonFiles)
        if (is_file(dir + f))
            return true;
    return false;
}

bool read_all(const std::wstring &path, std::vector<uint8_t> *out, std::wstring *why);

/* common.sqlite3 has race display rows without a name (written by the
 * launcher of 2026-10-26, before they were named). */
bool common_unnamed(const std::wstring &dir)
{
    std::vector<uint8_t> bytes;
    std::wstring ignored;
    return is_file(dir + L"common.sqlite3") && read_all(dir + L"common.sqlite3", &bytes, &ignored) &&
           paramdb::unnamed_race_rows(bytes.data(), bytes.size()) > 0;
}

/* A file whole; false with the system's reason. */
bool read_all(const std::wstring &path, std::vector<uint8_t> *out, std::wstring *why)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = util::system_error_text(GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (256ll << 20);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    if (!ok)
        *why = util::system_error_text(GetLastError());
    CloseHandle(h);
    return ok;
}

/* A file the conversion needs: false with why ("paramstr_us.db: ..."). */
bool read_needed(const std::wstring &dir, const std::wstring &name, std::vector<uint8_t> *out, std::wstring *why)
{
    if (read_all(dir + name, out, why))
        return true;
    *why = name + L": " + *why;
    return false;
}

/* The database written at `part' put in place as `done', after `check'
 * (which reads it back) passed; false with why (the .part deleted). */
template <class Check>
bool finish(const std::wstring &part, const std::wstring &done, Check check, std::wstring *why)
{
    std::vector<uint8_t> bytes;
    std::string error;
    if (!read_all(part, &bytes, why) || !check(bytes, &error)) {
        if (why->empty())
            *why = trf(L"the check of the database failed (%s); the files are kept", util::widen(error).c_str());
        DeleteFileW(part.c_str());
        return false;
    }
    if (!MoveFileExW(part.c_str(), done.c_str(), MOVEFILE_WRITE_THROUGH)) {
        *why = util::system_error_text(GetLastError());
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

/* The files deleted; the ones that would not go said in `left'. */
void delete_files(const std::wstring &dir, const std::vector<std::wstring> &names, std::wstring *left)
{
    for (const std::wstring &n : names)
        if (is_file(dir + n) && !DeleteFileW((dir + n).c_str()))
            *left += (left->empty() ? L"" : L", ") + n;
}

std::wstring notes_text(const std::vector<std::string> &notes)
{
    std::wstring t;
    for (const std::string &n : notes)
        t += (t.empty() ? L" (" : L"; ") + util::widen(n);
    return t.empty() ? t : t + L")";
}

/* database\paramdb<r>.sqlite3 from the region's files, which are then
 * deleted; false with why. */
bool convert_region(const std::wstring &dir, const std::wstring &region, std::wstring *note, std::wstring *why)
{
    paramdb::RegionFiles f;
    if (!read_needed(dir, L"paramdb" + region + L".db", &f.db, why) ||
        !read_needed(dir, L"paramstr" + region + L".db", &f.str, why) ||
        !read_needed(dir, L"paramunistr" + region + L".db", &f.ustr, why))
        return false;
    std::wstring ignored;
    read_all(dir + L".id_db_idx" + region + L".db", &f.idIdx, &ignored);   /* (the labels: else #<code>) */
    read_all(dir + L".id_db_str" + region + L".db", &f.idStr, &ignored);
    const std::wstring part = dir + L"paramdb" + region + L".sqlite3.part", done = dir + L"paramdb" + region + L".sqlite3";
    DeleteFileW(part.c_str());              /* (a stop's leftover) */
    std::string error;
    std::vector<std::string> notes;
    if (!paramdb::write_region(f, util::system_path(part), &error, &notes)) {
        *why = util::widen(error);
        return false;
    }
    auto check = [&f](const std::vector<uint8_t> &bytes, std::string *e) {
        paramdb::RegionImages img;
        std::vector<std::string> warnings;
        if (!paramdb::read_region(bytes.data(), bytes.size(), &img, &warnings, e))
            return false;
        if (!warnings.empty()) {
            *e = warnings[0];
            return false;
        }
        return paramdb::same_region(f, img, e);
    };
    if (!finish(part, done, check, why))
        return false;
    std::wstring left;
    delete_files(dir, {L"paramdb" + region + L".db", L"paramstr" + region + L".db", L"paramunistr" + region + L".db",
                       L".id_db_idx" + region + L".db", L".id_db_str" + region + L".db"}, &left);
    *note = notes_text(notes);
    if (!left.empty())
        *note += L" " + trf(L"(not deleted: %s)", left.c_str());
    return true;
}

/* database\common.sqlite3 from the colours and race modes (the race
 * display's rows without a name named), which are then deleted; false with
 * why. */
bool convert_common(const std::wstring &dir, std::vector<std::pair<uint64_t, std::string>> labels, std::wstring *note,
                    std::wstring *why)
{
    paramdb::CommonFiles f;
    if (!read_needed(dir, kCommonFiles[0], &f.carColor, why) || !read_needed(dir, kCommonFiles[1], &f.carColorStrings, why) ||
        !read_needed(dir, kCommonFiles[2], &f.raceModes, why) || !read_needed(dir, kCommonFiles[3], &f.raceDetails, why))
        return false;
    paramdb::name_race_rows(&f, &labels);     /* (the check below is against the named files) */
    const std::wstring part = dir + L"common.sqlite3.part", done = dir + L"common.sqlite3";
    DeleteFileW(part.c_str());
    std::string error;
    std::vector<std::string> notes;
    if (!paramdb::write_common(f, labels, util::system_path(part), &error, &notes)) {
        *why = util::widen(error);
        return false;
    }
    auto check = [&f](const std::vector<uint8_t> &bytes, std::string *e) {
        paramdb::CommonFiles img;
        std::vector<std::string> warnings;
        if (!paramdb::read_common(bytes.data(), bytes.size(), &img, &warnings, e))
            return false;
        if (!warnings.empty()) {
            *e = warnings[0];
            return false;
        }
        return paramdb::same_common(f, img, e);
    };
    if (!finish(part, done, check, why))
        return false;
    std::wstring left;
    delete_files(dir, {kCommonFiles[0], kCommonFiles[1], kCommonFiles[2], kCommonFiles[3]}, &left);
    *note = notes_text(notes);
    if (!left.empty())
        *note += L" " + trf(L"(not deleted: %s)", left.c_str());
    return true;
}

std::wstring text_folder(const std::wstring &data)
{
    return util::with_slash(data) + L"text\\";
}

bool texts_pending(const std::wstring &data)
{
    const std::wstring dir = text_folder(data);
    return is_file(dir + L"realtime.dat") && !is_file(dir + L"realtime.sqlite3");
}

/* text\realtime.sqlite3 from text\realtime.dat, which is then deleted;
 * false with why. */
bool convert_texts(const std::wstring &data, std::wstring *note, std::wstring *why)
{
    const std::wstring dir = text_folder(data);
    std::vector<uint8_t> file;
    if (!read_needed(dir, L"realtime.dat", &file, why))
        return false;
    const std::wstring part = dir + L"realtime.sqlite3.part", done = dir + L"realtime.sqlite3";
    DeleteFileW(part.c_str());
    std::string error;
    if (!paramdb::write_texts(file, util::system_path(part), &error)) {
        *why = util::widen(error);
        return false;
    }
    auto check = [&file](const std::vector<uint8_t> &bytes, std::string *e) {
        std::vector<uint8_t> img;
        std::vector<std::string> warnings;
        if (!paramdb::read_texts(bytes.data(), bytes.size(), &img, &warnings, e))
            return false;
        if (!warnings.empty()) {
            *e = warnings[0];
            return false;
        }
        return paramdb::same_texts(file, img, e);
    };
    if (!finish(part, done, check, why))
        return false;
    std::wstring left;
    delete_files(dir, {L"realtime.dat"}, &left);
    note->clear();
    if (!left.empty())
        *note = L" " + trf(L"(not deleted: %s)", left.c_str());
    return true;
}

/* The race display's rows of database\common.sqlite3 without a name named
 * (on a copy, put in place once checked); false with why. */
bool name_common(const std::wstring &dir, int *named, std::wstring *why)
{
    const std::wstring db = dir + L"common.sqlite3", part = dir + L"common.sqlite3.part";
    DeleteFileW(part.c_str());
    if (!CopyFileW(db.c_str(), part.c_str(), FALSE)) {
        *why = util::system_error_text(GetLastError());
        return false;
    }
    std::string error;
    if (!paramdb::name_race_database(util::system_path(part), named, &error)) {
        *why = util::widen(error);
        DeleteFileW(part.c_str());
        return false;
    }
    if (!MoveFileExW(part.c_str(), db.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *why = util::system_error_text(GetLastError());
        DeleteFileW(part.c_str());
        return false;
    }
    return true;
}

} // namespace

bool needs_conversion(const std::wstring &data)
{
    const std::wstring dir = folder_of(data);
    return !pending_regions(dir).empty() || common_pending(dir) || common_unnamed(dir) || texts_pending(data);
}

bool convert(const std::wstring &data, std::wstring *message, std::vector<std::wstring> *doneOut,
             std::vector<std::wstring> *problemsOut, std::vector<std::wstring> *updatedOut)
{
    const std::wstring dir = folder_of(data);
    std::vector<std::wstring> done, problems, updated;
    /* the labels of every region, before any region's files go (the common
     * database's cars are written by them) */
    std::vector<std::pair<uint64_t, std::string>> labels;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L".id_db_idx*.db").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring idx = fd.cFileName;
            std::vector<uint8_t> a, b;
            std::wstring ignored;
            if (read_all(dir + idx, &a, &ignored) && read_all(dir + L".id_db_str" + idx.substr(10), &b, &ignored))
                paramdb::add_labels(a, b, &labels);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const std::wstring &region : pending_regions(dir)) {
        std::wstring note, why;
        if (convert_region(dir, region, &note, &why))
            done.push_back(L"database\\paramdb" + region + note);
        else
            problems.push_back(L"database\\paramdb" + region + L": " + why);
    }
    if (common_pending(dir)) {
        std::wstring note, why;
        if (convert_common(dir, labels, &note, &why))
            done.push_back(L"database\\common" + note);
        else
            problems.push_back(L"database\\common: " + why);
    }
    if (texts_pending(data)) {
        std::wstring note, why;
        if (convert_texts(data, &note, &why))
            done.push_back(L"text\\realtime" + note);
        else
            problems.push_back(L"text\\realtime: " + why);
    }
    if (common_unnamed(dir)) {
        int named = 0;
        std::wstring why;
        if (name_common(dir, &named, &why))
            updated.push_back(trf(L"database\\common: %d race display rows named", named));
        else
            problems.push_back(trf(L"database\\common, naming its race display rows: %s", why.c_str()));
    }
    message->clear();
    if (!done.empty()) {
        std::wstring list;
        for (const std::wstring &d : done)
            list += (list.empty() ? L"" : L", ") + d;
        *message = trf(L"Databases written (<name>.sqlite3), the files they were made of deleted: %s.", list.c_str());
    }
    for (const std::wstring &u : updated)
        *message += (message->empty() ? L"" : L" ") + trf(L"Updated: %s.", u.c_str());
    for (const std::wstring &p : problems)
        *message += (message->empty() ? L"" : L" ") + trf(L"Not converted: %s.", p.c_str());
    if (doneOut != nullptr)
        *doneOut = done;
    if (problemsOut != nullptr)
        *problemsOut = problems;
    if (updatedOut != nullptr)
        *updatedOut = updated;
    return problems.empty();
}

} // namespace databases
