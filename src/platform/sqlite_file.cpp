/* sqlite_file.cpp - an SQLite database file read through SQLite itself
 * (sqlite_file.h). */
#include "platform/sqlite_file.h"

#include <sqlite3.h>

namespace sqlite_file {

Result read(const char *path, std::string *out, std::string *error)
{
    out->clear();
    error->clear();
    /* (sqlite3_open_v2 takes UTF-8, which spells any file's name) */
    const std::string p = path;
    sqlite3 *db = nullptr;
    /* Read-write, never writing: a read-only file SQLite opens read-only by
     * itself, and a hot journal (an editor that died in a commit) is rolled
     * back as any connection would; no CREATE, so a missing file is an
     * error, not a new database. */
    int rc = sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    if (rc != SQLITE_OK) {
        *error = db != nullptr ? sqlite3_errmsg(db) : "SQLite does not open it";
        sqlite3_close(db);
        return Result::error;
    }
    sqlite3_busy_timeout(db, kBusyMs);
    /* The pages copied under one read transaction (the statement sqlite3_serialize
     * keeps open while it copies): a committed snapshot, a WAL's included. */
    sqlite3_int64 size = 0;
    unsigned char *bytes = sqlite3_serialize(db, "main", &size, 0);
    if (bytes == nullptr || size <= 0) {
        const int code = sqlite3_errcode(db) & 0xFF;
        *error = bytes == nullptr && code != SQLITE_OK ? sqlite3_errmsg(db) : "it holds no pages";
        sqlite3_free(bytes);
        sqlite3_close(db);
        return code == SQLITE_BUSY || code == SQLITE_LOCKED ? Result::busy : Result::error;
    }
    out->assign(reinterpret_cast<const char *>(bytes), (size_t)size);
    sqlite3_free(bytes);
    sqlite3_close(db);
    /* A WAL-mode database says so in its header (bytes 18 and 19, the write
     * and read versions, are 2), and a copy in memory cannot be opened in
     * that mode (sqlite3_deserialize's first statement fails, "unable to open
     * database file"): the copy is a rollback-journal database, which is what
     * its pages are once read. */
    if (out->size() >= 100) {
        if ((*out)[18] == 2)
            (*out)[18] = 1;
        if ((*out)[19] == 2)
            (*out)[19] = 1;
    }
    return Result::ok;
}

} // namespace sqlite_file
