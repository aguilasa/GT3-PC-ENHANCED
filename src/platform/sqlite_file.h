/* sqlite_file.h - an SQLite database file read through SQLite itself (GT3 PC;
 * game/app/pc_data_file.h, docs/database.md "While an editor has it open").
 *
 * The game builds its tables from a database's bytes in memory
 * (platform/param_db.h, gcf_tables.h: sqlite3_deserialize of a copy).  Read
 * as a plain file (host_file_open) those bytes were refused for as long as an
 * SQLite editor had the database connected: the editor's handle is open for
 * writing and the game's open shared with readers only (Windows error 32),
 * so the user had to disconnect before every start.  Here the bytes come
 * through a connection of SQLite's own, under its locks, which every SQLite
 * program honours: a connected editor holds no lock while idle; a change it
 * has not committed yet is not seen (the last committed state is); a commit
 * under way is waited for (milliseconds, up to kBusyMs); a database an editor
 * switched to WAL mode gives its committed pages from the -wal file, which a
 * plain read of the file would miss.  The connection never writes and is
 * closed before this returns.  Any thread. */
#pragma once

#include <string>

namespace sqlite_file {

enum class Result {
    ok,         /* *out holds the database's pages: a committed snapshot, as the file would read with nothing pending */
    busy,       /* another connection kept it locked beyond kBusyMs (a transaction under way): try again later */
    error,      /* not an SQLite database, missing, unreadable: *error says */
};

/* How long a locked database is waited for in one read. */
constexpr int kBusyMs = 2000;

/* The database at `path` (UTF-8, as the system names it: host_file.h
 * host_file_full_path_utf8) into *out. */
Result read(const char *path, std::string *out, std::string *error);

} // namespace sqlite_file
