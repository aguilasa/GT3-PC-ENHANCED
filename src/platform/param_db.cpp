/* param_db.cpp - GT3's database folder as SQLite (param_db.h). */
#include "platform/param_db.h"

/* (Windows: <windows.h>'s code pages; elsewhere the same conversions,
 * EUC-JP as code page 20932 has it: platform/os/eucjp.h) */
#include "platform/os/win_compat.h"
#ifndef _WIN32
#include "platform/os/eucjp.h"
#include "platform/os/fs.h"
#endif

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace paramdb {
namespace {

constexpr uint16_t kParamVersion = 2087;    /* paramdb's GTDT version (2086: the PAL and Japanese discs' paramdb.db) */
constexpr uint16_t kRaceVersion = 0x0804;   /* racemode.db's and racedetail.db's */
constexpr uint32_t kGtar = 0x52415447;      /* "GTAR" (the PAL disc's: "GTAr") */
constexpr UINT kEucJp = 20932;              /* Windows' EUC-JP */
constexpr UINT kLatin1 = 28591;

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
uint64_t rotl(uint64_t v, int n) { n &= 63; return n == 0 ? v : (v << n) | (v >> (64 - n)); }

std::string hex64(uint64_t v)
{
    char b[24];
    std::snprintf(b, sizeof(b), "%016llX", (unsigned long long)v);
    return b;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

using Labels = std::unordered_map<uint64_t, std::string>;

/* ---- the files ----------------------------------------------------------- */

struct Block {
    uint16_t version = 0, rowSize = 0;
    int16_t tableId = 0;
    std::vector<const uint8_t *> rows;
};

/* A GTDT block (a table: 0x10 bytes of header, its records); false when the
 * bytes are not one. */
bool parse_gtdt(const uint8_t *d, size_t size, Block *b)
{
    if (size < 16 || std::memcmp(d, "GTDT", 4) != 0)
        return false;
    b->version = rd16(d + 4);
    b->tableId = (int16_t)rd16(d + 6);
    const uint16_t n = rd16(d + 8);
    b->rowSize = rd16(d + 10);
    if ((n > 0 && b->rowSize < 8) || 16 + (size_t)n * b->rowSize > size)
        return false;
    b->rows.clear();
    for (uint16_t r = 0; r < n; r++)
        b->rows.push_back(d + 16 + (size_t)r * b->rowSize);
    return true;
}

/* paramdb (a GTAR of GTDT blocks): its tables and magic. */
bool parse_gtar(const std::vector<uint8_t> &d, std::vector<Block> *out, uint32_t *magic, std::string *error)
{
    if (d.size() < 16 || (std::memcmp(d.data(), "GTAR", 4) != 0 && std::memcmp(d.data(), "GTAr", 4) != 0)) {
        *error = "not a paramdb (no GTAR head)";
        return false;
    }
    *magic = rd32(&d[0]);
    const uint32_t n = rd32(&d[4]), indexSize = rd32(&d[8]), mask = rd32(&d[12]);
    if (n > 256 || 16 + 4ull * (n + 1) > d.size()) {
        *error = "paramdb's table index runs past the file";
        return false;
    }
    out->clear();
    for (uint32_t t = 0; t < n; t++) {
        const size_t at = (size_t)indexSize + (rd32(&d[16 + 4 * t]) & ~mask);
        Block b;
        if (at > d.size() || !parse_gtdt(&d[at], d.size() - at, &b)) {
            *error = "paramdb table " + std::to_string(t) + " is not a GTDT block";
            return false;
        }
        out->push_back(std::move(b));
    }
    return true;
}

/* A GTDT block of records (sorted by the caller). */
std::vector<uint8_t> build_gtdt(uint16_t rowSize, uint16_t version, int16_t tableId,
                                const std::vector<std::vector<uint8_t>> &recs)
{
    std::vector<uint8_t> b(16 + recs.size() * rowSize, 0);
    std::memcpy(b.data(), "GTDT", 4);
    wr16(&b[4], version);
    wr16(&b[6], (uint16_t)tableId);
    wr16(&b[8], (uint16_t)recs.size());
    wr16(&b[10], rowSize);
    wr32(&b[12], (uint32_t)b.size());
    for (size_t r = 0; r < recs.size(); r++)
        std::memcpy(&b[16 + r * rowSize], recs[r].data(), rowSize);
    return b;
}

/* An STDB string file's entries (their text bytes, without the terminator
 * and padding); false when it is not one. */
bool parse_stdb(const std::vector<uint8_t> &d, std::vector<std::string> *out)
{
    out->clear();
    if (d.size() < 16 || std::memcmp(d.data(), "STDB", 4) != 0)
        return false;
    const uint32_t n = rd32(&d[4]);
    if (16 + 4ull * n > d.size())
        return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t o = rd32(&d[16 + 4 * i]);
        if ((size_t)o + 2 > d.size())
            return false;
        size_t len = rd16(&d[o]);
        if (o + 2 + len > d.size())
            return false;
        while (len > 0 && d[o + 2 + len - 1] == 0)
            len--;
        out->push_back(std::string((const char *)&d[o + 2], len));
    }
    return true;
}

/* An STDB file of these strings, each followed by its terminator and the
 * padding to 2; the stored length bare (paramstr) or with them
 * (paramunistr, carcolor.sdb), as the discs' files. */
std::vector<uint8_t> build_stdb(const std::vector<std::string> &strings, int16_t charSize, bool lengthWithPadding)
{
    std::vector<uint8_t> d(16 + 4 * strings.size(), 0);
    std::memcpy(d.data(), "STDB", 4);
    wr32(&d[4], (uint32_t)strings.size());
    wr16(&d[8], (uint16_t)charSize);
    for (size_t i = 0; i < strings.size(); i++) {
        wr32(&d[16 + 4 * i], (uint32_t)d.size());
        const std::string &s = strings[i];
        const size_t padded = (s.size() + 2) & ~(size_t)1;
        const size_t at = d.size();
        d.resize(at + 2 + padded, 0);
        wr16(&d[at], (uint16_t)(lengthWithPadding ? padded : s.size()));
        std::memcpy(&d[at + 2], s.data(), s.size());
    }
    wr32(&d[12], (uint32_t)d.size());
    return d;
}

/* The labels of .id_db_idx / .id_db_str ({u64 code, u64 string index},
 * sorted by code). */
void parse_labels(const std::vector<uint8_t> &idx, const std::vector<uint8_t> &str, Labels *labels)
{
    std::vector<std::string> strings;
    if (idx.size() < 8 || std::memcmp(idx.data(), "IDDB", 4) != 0 || !parse_stdb(str, &strings))
        return;
    const uint32_t n = rd32(&idx[4]);
    for (uint32_t i = 0; i < n && 8 + 16ull * (i + 1) <= idx.size(); i++) {
        const uint64_t code = rd64(&idx[8 + 16 * (size_t)i]), s = rd64(&idx[16 + 16 * (size_t)i]);
        if (s < strings.size())
            labels->emplace(code, strings[(size_t)s]);
    }
}

/* ---- text ---------------------------------------------------------------- */

bool ascii(const std::string &s)
{
    for (char c : s)
        if ((unsigned char)c >= 0x80)
            return false;
    return true;
}

#ifdef _WIN32
std::wstring widen(UINT cp, const std::string &s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/* `w' in code page cp; lossy: a character it lacks ('?' then). */
std::string narrow(UINT cp, const std::wstring &w, bool *lossy)
{
    *lossy = false;
    if (w.empty())
        return std::string();
    const bool utf8 = cp == CP_UTF8;
    BOOL used = FALSE;
    const DWORD flags = utf8 ? 0 : WC_NO_BEST_FIT_CHARS;
    const int n = WideCharToMultiByte(cp, flags, w.data(), (int)w.size(), nullptr, 0, nullptr, utf8 ? nullptr : &used);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(cp, flags, w.data(), (int)w.size(), &s[0], n, nullptr, utf8 ? nullptr : &used);
    *lossy = used != FALSE || (n <= 0);
    return s;
}

/* bytes [at, at + n) of code page 20932 appended to *w; false (nothing
 * appended) when they are not valid there (MB_ERR_INVALID_CHARS). */
bool euc_append(const char *bytes, size_t n, std::wstring *w)
{
    const int k = MultiByteToWideChar(kEucJp, MB_ERR_INVALID_CHARS, bytes, (int)n, nullptr, 0);
    if (k <= 0)
        return false;
    const size_t at = w->size();
    w->resize(at + (size_t)k);
    MultiByteToWideChar(kEucJp, 0, bytes, (int)n, &(*w)[at], k);
    return true;
}
#else
/* (the code pages the database uses: UTF-8, Latin-1 and EUC-JP's 20932,
 * which converts as Windows does; wchar_t is UTF-32 here) */
std::wstring widen(UINT cp, const std::string &s)
{
    if (cp == CP_UTF8)
        return fs::widen(s);
    std::wstring w;
    if (cp == kEucJp) {
        std::u32string u;
        if (eucjp::decode(s.data(), s.size(), &u))
            w.assign(u.begin(), u.end());
        return w;
    }
    for (char c : s)
        w.push_back((wchar_t)(uint8_t)c);       /* (Latin-1) */
    return w;
}

std::string narrow(UINT cp, const std::wstring &w, bool *lossy)
{
    *lossy = false;
    if (w.empty())
        return std::string();
    if (cp == CP_UTF8)
        return fs::narrow(w);
    if (cp == kEucJp)
        return eucjp::encode(std::u32string(w.begin(), w.end()), lossy);
    std::string s;
    for (wchar_t c : w) {
        const bool has = (uint32_t)c <= 0xFF;   /* (Latin-1) */
        s += has ? (char)c : '?';
        *lossy = *lossy || !has;
    }
    return s;
}

bool euc_append(const char *bytes, size_t n, std::wstring *w)
{
    std::u32string u;
    if (n == 0 || !eucjp::decode(bytes, n, &u))
        return false;
    w->append(u.begin(), u.end());
    return true;
}
#endif

/* A high surrogate of UTF-16 (wchar_t's on Windows; none where it is UTF-32). */
bool high_surrogate(wchar_t c)
{
    return sizeof(wchar_t) == 2 && (uint32_t)c >= 0xD800 && (uint32_t)c <= 0xDBFF;
}

/* The game's EUC-JP text: code page 20932, and its escape for the Latin-1
 * letters EUC-JP lacks, 0x81 then the letter's byte ("Citro\x81\xEBn": no
 * EUC-JP byte pair starts with 0x81).  false when the bytes are neither. */
bool euc_decode(const std::string &b, std::wstring *w)
{
    w->clear();
    size_t i = 0;
    while (i < b.size()) {
        if ((uint8_t)b[i] == 0x81) {
            if (i + 1 >= b.size() || (uint8_t)b[i + 1] < 0xA0)
                return false;
            w->push_back((wchar_t)(uint8_t)b[i + 1]);
            i += 2;
            continue;
        }
        size_t j = i;
        while (j < b.size() && (uint8_t)b[j] != 0x81)
            j++;
        if (!euc_append(&b[i], j - i, w))
            return false;
        i = j;
    }
    return true;
}

/* (a Latin-1 letter as the escape, as the discs' strings have them, not as
 * code page 20932's JIS X 0212 bytes - a pair whose second byte is below
 * 0xA1 - which the game's font does not have) */
std::string euc_encode(const std::wstring &w, bool *lossy)
{
    bool latin1 = false;
    for (wchar_t c : w)
        latin1 = latin1 || (c >= 0x80 && c <= 0xFF);
    std::string s = narrow(kEucJp, w, lossy);
    if (!*lossy && !latin1)
        return s;
    /* character by character */
    s.clear();
    *lossy = false;
    for (size_t k = 0; k < w.size(); k++) {
        const size_t len = high_surrogate(w[k]) && k + 1 < w.size() ? 2 : 1;
        bool miss = false;
        const std::string c = narrow(kEucJp, w.substr(k, len), &miss);
        const bool jis0208 = c.size() == 1 ? (uint8_t)c[0] < 0x80
                                           : c.size() == 2 && (uint8_t)c[0] >= 0xA1 && (uint8_t)c[1] >= 0xA1;
        if (!miss && jis0208) {
            s += c;
        } else if (len == 1 && w[k] >= 0xA0 && w[k] <= 0xFF) {
            s += '\x81';
            s += (char)w[k];
        } else if (!miss) {
            s += c;
        } else {
            s += '?';
            *lossy = true;
        }
        k += len - 1;
    }
    return s;
}

/* A string of a game file as the database's text (UTF-8) when that gives
 * the same bytes back, else false (the raw bytes are kept: a BLOB).  euc:
 * the EUC-JP strings (paramunistr, carcolor.sdb), else Latin-1. */
bool to_text(const std::string &bytes, bool euc, std::string *text)
{
    if (ascii(bytes)) {
        *text = bytes;
        return true;
    }
    std::wstring w;
    bool lossy = false;
    if (euc) {
        if (!euc_decode(bytes, &w) || euc_encode(w, &lossy) != bytes || lossy)
            return false;
    } else {
        w = widen(kLatin1, bytes);
    }
    *text = narrow(CP_UTF8, w, &lossy);
    return true;
}

/* The database's text as the game's bytes; lossy: a character the game's
 * encoding lacks ('?' then). */
std::string from_text(const std::string &text, bool euc, bool *lossy)
{
    *lossy = false;
    if (ascii(text))
        return text;
    const std::wstring w = widen(CP_UTF8, text);
    return euc ? euc_encode(w, lossy) : narrow(kLatin1, w, lossy);
}

/* ---- labels ---------------------------------------------------------------- */

/* Labels the game's code spells (list ends, the race modes, the TCS / ASC
 * rows' car), for the codes no label file has. */
const char *const kKnownLabels[] = {
    "-", "-----", "(Null)", "00000",
    "RACE_MODE_SINGLE", "RACE_MODE_ENDULANCE", "RACE_MODE_LICENSE", "RACE_MODE_0_400", "RACE_MODE_0_1000",
    "RACE_MODE_TIME_ATTACK", "RACE_MODE_2P_BATTLE", "RACE_MODE_2P_BATTLE_WIDE",
    "RACE_MODE_MAX_SPEED_CHALLENGE", "RACE_MODE_REPLAY", "RACE_MODE_DEMO", "RACE_MODE_FREE_PRACTICE",
    "RACE_MODE_TIME_TRIAL", "RACE_MODE_LOGGER",
};

void add_known_labels(Labels *labels)
{
    for (const char *l : kKnownLabels)
        labels->emplace(name_to_code(l), l);
}

/* A label of up to 8 printable ASCII characters from its code alone (each
 * character a 7-bit group under the rotated sum: docs/database.md); ""
 * when the code is not one's. */
std::string short_label(uint64_t code)
{
    for (int len = 1; len <= 8; len++)
        for (uint64_t sum = 0x20ull * len; sum <= 0x7Eull * len; sum++) {
            const uint64_t x = code - rotl(sum, 7 * len);
            if (x >> (7 * len) != 0)
                continue;
            std::string name((size_t)len, ' ');
            uint64_t total = 0;
            bool ok = true;
            for (int i = 0; i < len && ok; i++) {
                const int c = (int)((x >> (7 * (len - 1 - i))) & 0x7F);
                ok = c >= 0x20 && c <= 0x7E;
                name[(size_t)i] = (char)c;
                total += (uint64_t)c;
            }
            if (ok && total == sum && name_to_code(name) == code)
                return name;
        }
    return std::string();
}

/* A code as text: its label (a label file's whose code it is, the game's,
 * or one of up to 8 characters that gives it), else "#<16 hex digits>"; 0:
 * empty. */
std::string code_text(uint64_t code, const Labels &labels)
{
    if (code == 0)
        return std::string();
    auto it = labels.find(code);
    std::string t;
    if (it != labels.end() && name_to_code(it->second) == code && to_text(it->second, false, &t) &&
        !t.empty() && t[0] != '#')
        return t;
    t = short_label(code);
    if (!t.empty() && t[0] != '#')
        return t;
    return "#" + hex64(code);
}

/* A label's code: "#<hex>" the code itself, else the hash of its bytes as
 * the game spells them; "": 0. */
bool text_code(const std::string &text, uint64_t *code)
{
    if (!text.empty() && text[0] == '#') {
        char *e = nullptr;
        const unsigned long long v = std::strtoull(text.c_str() + 1, &e, 16);
        if (text.size() < 2 || *e != 0)
            return false;
        *code = v;
        return true;
    }
    bool lossy = false;
    *code = text.empty() ? 0 : name_to_code(from_text(text, false, &lossy));
    return true;
}

/* ---- SQLite -------------------------------------------------------------- */

bool exec(sqlite3 *db, const std::string &sql, std::string *error)
{
    char *msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) == SQLITE_OK)
        return true;
    *error = msg != nullptr ? msg : sqlite3_errmsg(db);
    sqlite3_free(msg);
    return false;
}

/* The integer of a one-value query (PRAGMA user_version...). */
int64_t query_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = nullptr;
    int64_t v = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* A new database at `path' (replaced), in a transaction, its header's
 * application_id and user_version set. */
sqlite3 *create_db(const std::string &path, uint32_t applicationId, uint32_t userVersion, std::string *error)
{
    std::remove(path.c_str());
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        *error = db != nullptr ? sqlite3_errmsg(db) : "the database could not be made";
        sqlite3_close(db);
        return nullptr;
    }
    if (!exec(db,
              "PRAGMA journal_mode = OFF; PRAGMA application_id = " + std::to_string((int32_t)applicationId) +
                  "; PRAGMA user_version = " + std::to_string((int32_t)userVersion) + "; BEGIN",
              error)) {
        sqlite3_close(db);
        std::remove(path.c_str());
        return nullptr;
    }
    return db;
}

/* COMMIT and close (the file removed when !ok or that fails). */
bool finish_db(sqlite3 *db, const std::string &path, bool ok, std::string *error)
{
    ok = ok && exec(db, "COMMIT", error);
    if (sqlite3_close(db) != SQLITE_OK && ok) {
        *error = "the database could not be closed";
        ok = false;
    }
    if (!ok)
        std::remove(path.c_str());
    return ok;
}

/* A database's bytes opened read-only (a copy). */
sqlite3 *open_bytes(const void *bytes, size_t size, std::string *error)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        *error = "SQLite does not start";
        sqlite3_close(db);
        return nullptr;
    }
    unsigned char *copy = (unsigned char *)sqlite3_malloc64(size > 0 ? size : 1);
    if (copy == nullptr) {
        sqlite3_close(db);
        *error = "no memory for it";
        return nullptr;
    }
    std::memcpy(copy, bytes, size);
    if (sqlite3_deserialize(db, "main", copy, (sqlite3_int64)size, (sqlite3_int64)size,
                            SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return nullptr;
    }
    /* (a file that is not a database fails at its first read) */
    if (sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", nullptr, nullptr, nullptr) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

/* A table's rows in the database's order: by rowid, or as they come (a
 * table without one). */
sqlite3_stmt *select_all(sqlite3 *db, const std::string &table, const std::string &columns = "*")
{
    sqlite3_stmt *st = nullptr;
    const std::string sql = "SELECT " + columns + " FROM \"" + table + "\"";
    if (sqlite3_prepare_v2(db, (sql + " ORDER BY rowid").c_str(), -1, &st, nullptr) == SQLITE_OK)
        return st;
    sqlite3_finalize(st);
    st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK)
        return st;
    sqlite3_finalize(st);
    return nullptr;
}

/* The result column of a name (any case), -1 if none. */
int column_of(sqlite3_stmt *st, const char *name)
{
    const int n = sqlite3_column_count(st);
    for (int k = 0; k < n; k++)
        if (_stricmp(sqlite3_column_name(st, k), name) == 0)
            return k;
    return -1;
}

std::string column_text(sqlite3_stmt *st, int k)
{
    const unsigned char *t = sqlite3_column_text(st, k);
    return t != nullptr ? std::string((const char *)t, (size_t)sqlite3_column_bytes(st, k)) : std::string();
}

/* A whole number of a column (an integer, a real cut, a text of one);
 * false when it is none (0 then).  Empty: 0, true. */
bool column_whole(sqlite3_stmt *st, int k, long long *v)
{
    *v = 0;
    switch (sqlite3_column_type(st, k)) {
    case SQLITE_INTEGER: *v = sqlite3_column_int64(st, k); return true;
    case SQLITE_FLOAT: *v = (long long)sqlite3_column_double(st, k); return true;
    case SQLITE_NULL: return true;
    default: {
        const std::string s = column_text(st, k);
        char *e = nullptr;
        *v = std::strtoll(s.c_str(), &e, 0);
        if (s.empty() || *e != 0) {
            *v = 0;
            return s.empty();
        }
        return true;
    }
    }
}

const char *sql_type(Type t)
{
    switch (t) {
    case Type::Label:
    case Type::Id:
    case Type::Str:
    case Type::UStr:
        return "TEXT";
    case Type::F32:
        return "REAL";
    default:
        return "INTEGER";
    }
}

/* A CREATE TABLE statement of columns {name, type, meaning}, the meanings
 * as comments (the database's schema keeps them). */
std::string create_sql(const char *table, const char *doc,
                       const std::vector<std::pair<std::string, std::pair<const char *, const char *>>> &cols)
{
    std::string s = std::string("CREATE TABLE \"") + table + "\" (  -- " + doc + "\n";
    for (size_t i = 0; i < cols.size(); i++) {
        std::string line = "    \"" + cols[i].first + "\" " + cols[i].second.first + (i + 1 < cols.size() ? "," : "");
        if (line.size() < 40)
            line.resize(40, ' ');
        s += line + "  -- " + (cols[i].second.second != nullptr ? cols[i].second.second : "") + "\n";
    }
    return s + ")";
}

std::string create_sql(const TableDef &t)
{
    std::vector<std::pair<std::string, std::pair<const char *, const char *>>> cols;
    for (int i = 0; i < t.count; i++)
        if (t.columns[i].type != Type::Pad)
            cols.push_back({t.columns[i].name, {sql_type(t.columns[i].type), t.columns[i].doc}});
    return create_sql(t.name, t.doc, cols);
}

uint16_t type_size(Type t)
{
    switch (t) {
    case Type::Label:
    case Type::Id: return 8;
    case Type::U8:
    case Type::S8: return 1;
    case Type::U16:
    case Type::S16:
    case Type::Str:
    case Type::UStr: return 2;
    default: return 4;
    }
}

/* The schema's table covers its records exactly (every byte once). */
bool schema_ok(const TableDef &t, std::string *why)
{
    std::vector<int> used(t.rowSize, 0);
    for (int i = 0; i < t.count; i++) {
        const Column &c = t.columns[i];
        const uint16_t size = c.type == Type::Pad ? c.size : type_size(c.type);
        for (uint16_t b = 0; b < size; b++) {
            if (c.offset + b >= t.rowSize) {
                *why = std::string(t.name) + "." + (c.name != nullptr ? c.name : "pad") + " runs past the record";
                return false;
            }
            used[c.offset + b]++;
        }
    }
    for (uint16_t b = 0; b < t.rowSize; b++)
        if (used[b] != 1) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s: byte 0x%X is in %d columns", t.name, b, used[b]);
            *why = buf;
            return false;
        }
    return true;
}

/* ---- a table: records -> rows ------------------------------------------------ */

/* The strings and labels the records point at. */
struct TextFiles {
    const std::vector<std::string> *strs = nullptr, *ustrs = nullptr;
    const Labels *labels = nullptr;
};

void bind_value(sqlite3_stmt *st, int arg, const Column &c, const uint8_t *row, const TextFiles &tf)
{
    const uint8_t *p = row + c.offset;
    switch (c.type) {
    case Type::Label:
    case Type::Id: {
        const std::string t = code_text(rd64(p), *tf.labels);
        if (t.empty())
            sqlite3_bind_null(st, arg);
        else
            sqlite3_bind_text(st, arg, t.c_str(), (int)t.size(), SQLITE_TRANSIENT);
        return;
    }
    case Type::Str:
    case Type::UStr: {
        const std::vector<std::string> *v = c.type == Type::Str ? tf.strs : tf.ustrs;
        const uint16_t i = rd16(p);
        if (v == nullptr || i >= v->size()) {
            sqlite3_bind_null(st, arg);         /* (an index past the file: none) */
            return;
        }
        std::string t;
        if (to_text((*v)[i], c.type == Type::UStr, &t))
            sqlite3_bind_text(st, arg, t.c_str(), (int)t.size(), SQLITE_TRANSIENT);
        else
            sqlite3_bind_blob(st, arg, (*v)[i].data(), (int)(*v)[i].size(), SQLITE_TRANSIENT);
        return;
    }
    case Type::U8: sqlite3_bind_int64(st, arg, p[0]); return;
    case Type::S8: sqlite3_bind_int64(st, arg, (int8_t)p[0]); return;
    case Type::U16: sqlite3_bind_int64(st, arg, rd16(p)); return;
    case Type::S16: sqlite3_bind_int64(st, arg, (int16_t)rd16(p)); return;
    case Type::U32: sqlite3_bind_int64(st, arg, rd32(p)); return;
    case Type::S32: sqlite3_bind_int64(st, arg, (int32_t)rd32(p)); return;
    case Type::F32: {
        float f;
        std::memcpy(&f, p, 4);
        sqlite3_bind_double(st, arg, f);
        return;
    }
    case Type::Pad: return;
    }
}

/* Table t of these records, its rows by label (the file's are by code: no
 * order to read; rows of one label keep the file's order, which decides
 * the one the game's search finds). */
bool write_table(sqlite3 *db, const TableDef &t, const std::vector<const uint8_t *> &records, const TextFiles &tf,
                 std::string *error)
{
    if (!exec(db, create_sql(t), error))
        return false;
    std::string sql = std::string("INSERT INTO \"") + t.name + "\" VALUES (";
    int args = 0;
    for (int i = 0; i < t.count; i++)
        if (t.columns[i].type != Type::Pad)
            sql += args++ ? ", ?" : "?";
    sql += ")";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        *error = std::string(t.name) + ": " + sqlite3_errmsg(db);
        return false;
    }
    std::vector<std::pair<std::string, const uint8_t *>> rows;
    for (const uint8_t *r : records)
        rows.push_back({code_text(rd64(r), *tf.labels), r});
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    bool ok = true;
    for (size_t r = 0; ok && r < rows.size(); r++) {
        sqlite3_reset(st);
        int arg = 1;
        for (int i = 0; i < t.count; i++)
            if (t.columns[i].type != Type::Pad)
                bind_value(st, arg++, t.columns[i], rows[r].second, tf);
        if (sqlite3_step(st) != SQLITE_DONE) {
            *error = std::string(t.name) + " row " + rows[r].first + ": " + sqlite3_errmsg(db);
            ok = false;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

/* Padding must be padding (0): false with where it is not. */
bool padding_ok(const TableDef &t, const std::vector<const uint8_t *> &records, std::string *error)
{
    for (const uint8_t *row : records)
        for (int i = 0; i < t.count; i++) {
            const Column &c = t.columns[i];
            if (c.type != Type::Pad)
                continue;
            for (uint16_t b = 0; b < c.size; b++)
                if (row[c.offset + b] != 0) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "%s: padding byte 0x%X is not 0 (row %s): the schema does not fit",
                                  t.name, c.offset + b, hex64(rd64(row)).c_str());
                    *error = buf;
                    return false;
                }
        }
    return true;
}

/* ---- a table: rows -> records ------------------------------------------------ */

/* The strings of a rebuilt string file, each once. */
struct Interner {
    std::vector<std::string> v;
    std::unordered_map<std::string, uint16_t> index;

    Interner() { add(std::string()); }  /* (string 0: the empty one) */
    uint16_t add(const std::string &s)
    {
        auto it = index.find(s);
        if (it != index.end())
            return it->second;
        const uint16_t i = (uint16_t)v.size();
        v.push_back(s);
        index.emplace(s, i);
        return i;
    }
};

/* Table t's records from the database, sorted by code (rows of one code in
 * the database's order); found: the database has the table. */
std::vector<std::vector<uint8_t>> read_table(sqlite3 *db, const TableDef &t, Interner *strs, Interner *ustrs,
                                            std::vector<std::string> *warnings, bool *found)
{
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> rows;
    sqlite3_stmt *st = select_all(db, t.name);
    *found = st != nullptr;
    if (st == nullptr) {
        warnings->push_back(std::string(t.name) + ": no such table (the game gets it empty)");
        return {};
    }
    /* the schema's columns in the result (any order, any case) */
    std::vector<int> at((size_t)t.count, -1);
    for (int i = 0; i < t.count; i++) {
        if (t.columns[i].type == Type::Pad)
            continue;
        at[(size_t)i] = column_of(st, t.columns[i].name);
        if (at[(size_t)i] < 0)
            warnings->push_back(std::string(t.name) + ": no column " + t.columns[i].name + " (0 / empty taken)");
    }
    int rowNo = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        rowNo++;
        std::vector<uint8_t> rec(t.rowSize, 0);
        uint64_t code = 0;
        bool skip = false;
        auto warn = [&](const std::string &what) {
            warnings->push_back(std::string(t.name) + " row " + std::to_string(rowNo) + ": " + what);
        };
        for (int i = 0; i < t.count && !skip; i++) {
            const Column &c = t.columns[i];
            const int k = at[(size_t)i];
            if (c.type == Type::Pad || k < 0)
                continue;
            uint8_t *p = rec.data() + c.offset;
            const int vt = sqlite3_column_type(st, k);
            switch (c.type) {
            case Type::Label:
            case Type::Id: {
                uint64_t v = 0;
                if (vt == SQLITE_INTEGER) {
                    v = (uint64_t)sqlite3_column_int64(st, k);
                } else if (vt != SQLITE_NULL) {
                    const std::string s = column_text(st, k);
                    if (!text_code(s, &v)) {
                        warn(std::string(c.name) + " \"" + s + "\" is not a label or #<hex code>: 0 taken");
                        v = 0;
                    }
                }
                if (c.type == Type::Label) {
                    if (v == 0) {
                        warn("no label: skipped");
                        skip = true;
                        break;
                    }
                    code = v;
                }
                wr64(p, v);
                break;
            }
            case Type::Str:
            case Type::UStr: {
                std::string s;
                if (vt == SQLITE_BLOB) {
                    s.assign((const char *)sqlite3_column_blob(st, k), (size_t)sqlite3_column_bytes(st, k));
                } else if (vt != SQLITE_NULL) {
                    bool lossy = false;
                    s = from_text(column_text(st, k), c.type == Type::UStr, &lossy);
                    if (lossy)
                        warn(std::string(c.name) + ": a character the game's text cannot show ('?' taken)");
                }
                Interner *in = c.type == Type::Str ? strs : ustrs;
                wr16(p, in != nullptr ? in->add(s) : 0);
                break;
            }
            case Type::F32: {
                const float v = (float)sqlite3_column_double(st, k);
                std::memcpy(p, &v, 4);
                break;
            }
            default: {
                long long v = 0;
                if (!column_whole(st, k, &v))
                    warn(std::string(c.name) + " \"" + column_text(st, k) + "\" is not a whole number: 0 taken");
                const bool sgn = c.type == Type::S8 || c.type == Type::S16 || c.type == Type::S32;
                const int bits = 8 * type_size(c.type);
                const long long lo = sgn ? -(1ll << (bits - 1)) : 0;
                const long long hi = sgn ? (1ll << (bits - 1)) - 1 : (1ll << bits) - 1;
                if (v < lo || v > hi) {
                    warn(std::string(c.name) + " " + std::to_string(v) + " is outside " + std::to_string(lo) + " .. " +
                         std::to_string(hi) + ": cut to fit");
                    v = (std::min)((std::max)(v, lo), hi);
                }
                if (bits == 8)
                    p[0] = (uint8_t)v;
                else if (bits == 16)
                    wr16(p, (uint16_t)v);
                else
                    wr32(p, (uint32_t)v);
                break;
            }
            }
        }
        if (!skip)
            rows.push_back({code, std::move(rec)});
    }
    sqlite3_finalize(st);
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    if (rows.size() > 0xFFFF) {
        warnings->push_back(std::string(t.name) + ": more than 65535 rows: the rest left out");
        rows.resize(0xFFFF);
    }
    std::vector<std::vector<uint8_t>> out;
    out.reserve(rows.size());
    for (auto &r : rows)
        out.push_back(std::move(r.second));
    return out;
}

/* The columns of the database's tables that `known' (table -> its columns,
 * lower case) does not have, by the rows' codes (Extras). */
void read_extras(sqlite3 *db, const std::map<std::string, std::set<std::string>> &known, Extras *out)
{
    out->clear();
    std::vector<std::string> tables;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'",
                           -1, &st, nullptr) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            tables.push_back(column_text(st, 0));
    sqlite3_finalize(st);
    for (const std::string &name : tables) {
        auto kt = known.find(lower(name));
        sqlite3_stmt *q = select_all(db, name);
        if (q == nullptr)
            continue;
        /* the key: Label (a code's text), else Code (a colour's) */
        int key = column_of(q, "Label");
        const bool byLabel = key >= 0;
        if (!byLabel)
            key = column_of(q, "Code");
        ExtraTable t;
        t.name = name;
        std::vector<int> cols;
        for (int k = 0; k < sqlite3_column_count(q); k++) {
            const std::string c = sqlite3_column_name(q, k);
            if (k != key && (kt == known.end() || kt->second.count(lower(c)) == 0)) {
                cols.push_back(k);
                t.columns.push_back(c);
            }
        }
        if (key < 0 || cols.empty()) {
            sqlite3_finalize(q);
            continue;
        }
        while (sqlite3_step(q) == SQLITE_ROW) {
            uint64_t code = 0;
            if (byLabel) {
                if (!text_code(column_text(q, key), &code))
                    continue;
            } else {
                code = (uint64_t)sqlite3_column_int64(q, key);
            }
            if (t.rows.count(code))
                continue;
            std::vector<Value> vals(cols.size());
            for (size_t i = 0; i < cols.size(); i++) {
                Value &v = vals[i];
                switch (sqlite3_column_type(q, cols[i])) {
                case SQLITE_INTEGER: v.kind = Value::Integer; v.i = sqlite3_column_int64(q, cols[i]); v.r = (double)v.i; break;
                case SQLITE_FLOAT: v.kind = Value::Real; v.r = sqlite3_column_double(q, cols[i]); v.i = (int64_t)v.r; break;
                case SQLITE_TEXT: v.kind = Value::Text; v.s = column_text(q, cols[i]); break;
                case SQLITE_BLOB:
                    v.kind = Value::Blob;
                    v.s.assign((const char *)sqlite3_column_blob(q, cols[i]), (size_t)sqlite3_column_bytes(q, cols[i]));
                    break;
                default: break;
                }
            }
            t.rows.emplace(code, std::move(vals));
        }
        sqlite3_finalize(q);
        out->push_back(std::move(t));
    }
}

void add_known(const TableDef &t, std::map<std::string, std::set<std::string>> *known)
{
    std::set<std::string> &cols = (*known)[lower(t.name)];
    for (int i = 0; i < t.count; i++)
        if (t.columns[i].type != Type::Pad)
            cols.insert(lower(t.columns[i].name));
}

/* ---- carcolor.db ----------------------------------------------------------- */

/* carcolor.db ("GT2K": header, the cars {u64 code, u32 count, u32 offset}
 * sorted by code, the pool of colour lists, the colour table) as read. */
struct CarColors {
    struct Car {
        uint64_t code;
        uint32_t offset;
        std::vector<uint32_t> colors;
    };
    struct Color {
        uint32_t code, nameIntl, nameJp, rgb;
    };
    std::vector<Car> cars;          /* by code, as the file */
    std::vector<Color> colors;      /* by code */
};

bool parse_carcolor(const std::vector<uint8_t> &d, CarColors *out, std::string *error)
{
    out->cars.clear();
    out->colors.clear();
    if (d.size() < 0x18 || std::memcmp(d.data(), "GT2K", 4) != 0) {
        *error = "carcolor.db is not one (no GT2K head)";
        return false;
    }
    if (rd32(&d[4]) != 0) {
        *error = "carcolor.db: its header's word 4 is not 0";
        return false;
    }
    const uint32_t numCars = rd32(&d[8]), lists = rd32(&d[0xC]), table = rd32(&d[0x10]);
    if (0x18 + 0x10ull * numCars > d.size() || (uint64_t)lists + 4 > d.size() || (uint64_t)table + 4 > d.size()) {
        *error = "carcolor.db: its parts run past the file";
        return false;
    }
    const uint32_t numColors = rd32(&d[table]);
    if ((uint64_t)table + 4 + 0x10ull * numColors > d.size()) {
        *error = "carcolor.db: its colour table runs past the file";
        return false;
    }
    for (uint32_t i = 0; i < numCars; i++) {
        const uint8_t *e = &d[0x18 + 0x10 * (size_t)i];
        CarColors::Car car;
        car.code = rd64(e);
        const uint32_t n = rd32(e + 8);
        car.offset = rd32(e + 12);
        const uint64_t at = (uint64_t)lists + (car.offset >> 2) * 4ull;
        if (at + 4ull * n > d.size()) {
            *error = "carcolor.db: car " + hex64(car.code) + "'s colours run past the file";
            return false;
        }
        for (uint32_t k = 0; k < n; k++)
            car.colors.push_back(rd32(&d[(size_t)at + 4 * k]));
        out->cars.push_back(std::move(car));
    }
    for (uint32_t i = 0; i < numColors; i++) {
        const uint8_t *c = &d[table + 4 + 0x10 * (size_t)i];
        out->colors.push_back({rd32(c), rd32(c + 4), rd32(c + 8), rd32(c + 12)});
    }
    return true;
}

const char kColorDoc[] = "the paint colours (carcolor.db's colour table, the names carcolor.sdb's)";
const char kCarColorDoc[] = "the colours a car is offered in, in their order (carcolor.db's lists)";

const std::vector<std::pair<std::string, std::pair<const char *, const char *>>> &color_columns()
{
    static const std::vector<std::pair<std::string, std::pair<const char *, const char *>>> c = {
        {"Code", {"INTEGER", "the colour's code (CAR_COLOR.Color, a save's colour): unique, never 4294967295"}},
        {"NameIntl", {"TEXT", "its name for the countries US, UK, FR, DE, IT, ES"}},
        {"NameJp", {"TEXT", "its name for Japan"}},
        {"Red", {"INTEGER", "its swatch (the colour menus): red 0..255"}},
        {"Green", {"INTEGER", "green 0..255"}},
        {"Blue", {"INTEGER", "blue 0..255"}},
    };
    return c;
}

const std::vector<std::pair<std::string, std::pair<const char *, const char *>>> &car_color_columns()
{
    static const std::vector<std::pair<std::string, std::pair<const char *, const char *>>> c = {
        {"Car", {"TEXT", "the car's label (CAR.Label)"}},
        {"Position", {"INTEGER", "the colour's place in the car's list, 0 the first (the order the game offers them)"}},
        {"Color", {"INTEGER", "the colour (COLOR.Code)"}},
    };
    return c;
}

} // namespace

uint64_t name_to_code(const std::string &label)
{
    uint64_t sum = 0;
    for (unsigned char c : label)
        sum += c;
    uint64_t code = sum & 0xFFFF;
    for (unsigned char c : label)
        code = rotl(code, 7) + c;
    return code;
}

void add_labels(const std::vector<uint8_t> &idIdx, const std::vector<uint8_t> &idStr,
                std::vector<std::pair<uint64_t, std::string>> *labels)
{
    Labels m;
    parse_labels(idIdx, idStr, &m);
    for (auto &l : m)
        labels->push_back(l);
}

/* ---- the region: files -> database --------------------------------------- */

bool write_region(const RegionFiles &f, const std::string &path, std::string *error, std::vector<std::string> *notes)
{
    std::vector<Block> blocks;
    uint32_t magic = kGtar;
    if (!parse_gtar(f.db, &blocks, &magic, error))
        return false;
    const std::vector<TableDef> &defs = param_tables();
    if (blocks.size() != defs.size()) {
        *error = "paramdb has " + std::to_string(blocks.size()) + " tables, GT3's " + std::to_string(defs.size()) +
                 " (another game's database?)";
        return false;
    }
    const uint16_t version = blocks[0].version;
    for (const TableDef &t : defs) {
        std::string why;
        if (!schema_ok(t, &why)) {
            *error = "the schema: " + why;
            return false;
        }
        const Block &b = blocks[(size_t)t.index];
        if (b.rowSize != t.rowSize) {
            *error = std::string(t.name) + ": its records are not GT3's size (another game's database?)";
            return false;
        }
        if (b.version != version || b.tableId != t.index) {
            *error = std::string(t.name) + ": its version or id is not the others'";
            return false;
        }
        if (!padding_ok(t, b.rows, error))
            return false;
    }
    std::vector<std::string> strs, ustrs;
    if (!parse_stdb(f.str, &strs) || !parse_stdb(f.ustr, &ustrs)) {
        *error = "the string files (paramstr, paramunistr) cannot be read";
        return false;
    }
    Labels labels;
    parse_labels(f.idIdx, f.idStr, &labels);
    add_known_labels(&labels);

    /* what the notes say: codes without a label, strings no record points at,
     * strings kept as their bytes */
    std::set<uint16_t> usedStr, usedUstr;
    int unlabelled = 0, raw = 0;
    for (const TableDef &t : defs)
        for (const uint8_t *row : blocks[(size_t)t.index].rows)
            for (int i = 0; i < t.count; i++) {
                const Column &c = t.columns[i];
                const uint8_t *p = row + c.offset;
                std::string text;
                if (c.type == Type::Str || c.type == Type::UStr) {
                    (c.type == Type::Str ? usedStr : usedUstr).insert(rd16(p));
                    const std::vector<std::string> &v = c.type == Type::Str ? strs : ustrs;
                    if (rd16(p) < v.size() && !to_text(v[rd16(p)], c.type == Type::UStr, &text))
                        raw++;
                } else if ((c.type == Type::Label || c.type == Type::Id) && rd64(p) != 0 &&
                           code_text(rd64(p), labels)[0] == '#') {
                    unlabelled++;
                }
            }
    if (unlabelled > 0)
        notes->push_back(std::to_string(unlabelled) + " codes without a label, written #<code>");
    if (raw > 0)
        notes->push_back(std::to_string(raw) + " strings that are no text, kept as their bytes");
    const size_t unusedStr = strs.size() - (std::min)(strs.size(), usedStr.size());
    const size_t unusedUstr = ustrs.size() - (std::min)(ustrs.size(), usedUstr.size());
    if (unusedStr + unusedUstr > 0)
        notes->push_back(std::to_string(unusedStr + unusedUstr) + " strings no record uses, left out");

    sqlite3 *db = create_db(path, magic, version, error);
    if (db == nullptr)
        return false;
    TextFiles tf;
    tf.strs = &strs;
    tf.ustrs = &ustrs;
    tf.labels = &labels;
    bool ok = true;
    for (const TableDef &t : defs)
        if (ok)
            ok = write_table(db, t, blocks[(size_t)t.index].rows, tf, error);
    return finish_db(db, path, ok, error);
}

/* ---- the region: database -> the game's files ----------------------------- */

bool read_region(const void *bytes, size_t size, RegionImages *out, std::vector<std::string> *warnings,
                 std::string *error)
{
    sqlite3 *db = open_bytes(bytes, size, error);
    if (db == nullptr)
        return false;
    const uint32_t magic = (uint32_t)query_int(db, "PRAGMA application_id");
    const uint16_t version = (uint16_t)query_int(db, "PRAGMA user_version");
    Interner strs, ustrs;
    const std::vector<TableDef> &defs = param_tables();
    std::vector<std::vector<uint8_t>> blocks(defs.size());
    int found = 0;
    for (size_t ti = 0; ti < defs.size(); ti++) {
        const TableDef &t = defs[ti];
        bool has = false;
        const std::vector<std::vector<uint8_t>> recs = read_table(db, t, &strs, &ustrs, warnings, &has);
        found += has;
        blocks[ti] = build_gtdt(t.rowSize, version != 0 ? version : kParamVersion, (int16_t)t.index, recs);
    }
    std::map<std::string, std::set<std::string>> known;
    for (const TableDef &t : defs)
        add_known(t, &known);
    read_extras(db, known, &out->extras);
    sqlite3_close(db);
    if (found == 0) {
        *error = "it has none of paramdb's tables";
        return false;
    }
    if (strs.v.size() > 0xFFFF || ustrs.v.size() > 0xFFFF) {
        *error = "more than 65535 different strings";
        return false;
    }
    /* paramdb: the magic, the index (n + 1 offsets from its end, 8-aligned),
     * the blocks */
    const size_t n = blocks.size();
    const uint32_t indexSize = (uint32_t)((16 + 4 * (n + 1) + 7) & ~(size_t)7);
    std::vector<uint8_t> &d = out->db;
    d.assign(indexSize, 0);
    wr32(&d[0], magic == kGtar || magic == 0x72415447 ? magic : kGtar);
    wr32(&d[4], (uint32_t)n);
    wr32(&d[8], indexSize);
    wr32(&d[12], 7);
    for (size_t i = 0; i < n; i++) {
        d.resize((d.size() + 7) & ~(size_t)7, 0);
        wr32(&d[16 + 4 * i], (uint32_t)(d.size() - indexSize));
        d.insert(d.end(), blocks[i].begin(), blocks[i].end());
    }
    wr32(&d[16 + 4 * n], (uint32_t)(d.size() - indexSize));
    out->str = build_stdb(strs.v, 1, false);
    out->ustr = build_stdb(ustrs.v, -1, true);
    return true;
}

/* ---- verification ---------------------------------------------------------- */

namespace {

/* Two tables' records the same, column by column (strings by their bytes). */
bool same_rows(const TableDef &t, const Block &x, const Block &y, const std::vector<std::string> *sa,
               const std::vector<std::string> *ua, const std::vector<std::string> *sb,
               const std::vector<std::string> *ub, std::string *why)
{
    if (x.rowSize != y.rowSize || x.rows.size() != y.rows.size() || x.version != y.version || x.tableId != y.tableId) {
        *why = std::string(t.name) + ": its size, rows, version or id";
        return false;
    }
    for (size_t r = 0; r < x.rows.size(); r++)
        for (int i = 0; i < t.count; i++) {
            const Column &c = t.columns[i];
            const uint8_t *p = x.rows[r] + c.offset, *q = y.rows[r] + c.offset;
            bool same;
            if (c.type == Type::Str || c.type == Type::UStr) {
                const std::vector<std::string> *va = c.type == Type::Str ? sa : ua;
                const std::vector<std::string> *vb = c.type == Type::Str ? sb : ub;
                const uint16_t ia = rd16(p), ib = rd16(q);
                /* (an index past the file reads as none: the empty string) */
                const std::string ta = va != nullptr && ia < va->size() ? (*va)[ia] : std::string();
                const std::string tb = vb != nullptr && ib < vb->size() ? (*vb)[ib] : std::string();
                same = ta == tb;
            } else {
                const uint16_t size = c.type == Type::Pad ? c.size : type_size(c.type);
                same = std::memcmp(p, q, size) == 0;
            }
            if (!same) {
                *why = std::string(t.name) + " row " + hex64(rd64(x.rows[r])) + " " +
                       (c.type == Type::Pad ? "padding" : c.name);
                return false;
            }
        }
    return true;
}

} // namespace

bool same_region(const RegionFiles &f, const RegionImages &img, std::string *why)
{
    std::vector<Block> a, b;
    uint32_t ma = 0, mb = 0;
    std::string e;
    if (!parse_gtar(f.db, &a, &ma, &e) || !parse_gtar(img.db, &b, &mb, &e)) {
        *why = e;
        return false;
    }
    std::vector<std::string> sa, ua, sb, ub;
    if (!parse_stdb(f.str, &sa) || !parse_stdb(f.ustr, &ua) || !parse_stdb(img.str, &sb) || !parse_stdb(img.ustr, &ub)) {
        *why = "a string file";
        return false;
    }
    if (a.size() != b.size() || ma != mb) {
        *why = "the number of tables or the magic";
        return false;
    }
    for (const TableDef &t : param_tables())
        if (!same_rows(t, a[(size_t)t.index], b[(size_t)t.index], &sa, &ua, &sb, &ub, why))
            return false;
    return true;
}

/* ---- the shared database ------------------------------------------------------ */

bool write_common(const CommonFiles &f, const std::vector<std::pair<uint64_t, std::string>> &labelList,
                  const std::string &path, std::string *error, std::vector<std::string> *notes)
{
    Labels labels;
    for (const auto &l : labelList)
        labels.emplace(l.first, l.second);
    add_known_labels(&labels);
    /* the race modes */
    const std::vector<TableDef> &race = race_tables();
    Block blocks[2];
    const std::vector<uint8_t> *files[2] = {&f.raceModes, &f.raceDetails};
    const char *names[2] = {"racemode.db", "racedetail.db"};
    for (int i = 0; i < 2; i++) {
        std::string why;
        if (!schema_ok(race[(size_t)i], &why)) {
            *error = "the schema: " + why;
            return false;
        }
        if (!parse_gtdt(files[i]->data(), files[i]->size(), &blocks[i]) || blocks[i].rowSize != race[(size_t)i].rowSize) {
            *error = std::string(names[i]) + " is not GT3's (no GTDT block of its records)";
            return false;
        }
        if (blocks[i].version != blocks[0].version || blocks[i].tableId != -1) {
            *error = std::string(names[i]) + ": its version or id is not racemode.db's";
            return false;
        }
        if (!padding_ok(race[(size_t)i], blocks[i].rows, error))
            return false;
    }
    /* the colours */
    CarColors cc;
    if (!parse_carcolor(f.carColor, &cc, error))
        return false;
    std::vector<std::string> names2;
    if (!parse_stdb(f.carColorStrings, &names2)) {
        *error = "carcolor.sdb cannot be read";
        return false;
    }
    for (const CarColors::Color &c : cc.colors) {
        if (c.nameIntl >= names2.size() || c.nameJp >= names2.size()) {
            *error = "carcolor.db: colour " + std::to_string(c.code) + "'s name is not in carcolor.sdb";
            return false;
        }
        if (c.rgb >> 24 != 0) {
            *error = "carcolor.db: colour " + std::to_string(c.code) + "'s swatch has a top byte";
            return false;
        }
    }
    /* (the race details' names are known to none: their codes are #<code>
     * without a note) */
    int unlabelled = 0;
    for (const CarColors::Car &c : cc.cars)
        unlabelled += code_text(c.code, labels)[0] == '#';
    if (unlabelled > 0)
        notes->push_back(std::to_string(unlabelled) + " cars without a label, written #<code>");

    sqlite3 *db = create_db(path, 0x4B325447 /* "GT2K" */, blocks[0].version, error);
    if (db == nullptr)
        return false;
    TextFiles tf;
    tf.labels = &labels;
    bool ok = write_table(db, race[0], blocks[0].rows, tf, error) && write_table(db, race[1], blocks[1].rows, tf, error);
    /* COLOR, by code */
    ok = ok && exec(db, create_sql("COLOR", kColorDoc, color_columns()), error);
    sqlite3_stmt *st = nullptr;
    if (ok && sqlite3_prepare_v2(db, "INSERT INTO COLOR VALUES (?, ?, ?, ?, ?, ?)", -1, &st, nullptr) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        ok = false;
    }
    for (size_t i = 0; ok && i < cc.colors.size(); i++) {
        const CarColors::Color &c = cc.colors[i];
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, c.code);
        for (int k = 0; k < 2; k++) {
            const std::string &b = names2[k == 0 ? c.nameIntl : c.nameJp];
            std::string t;
            if (to_text(b, true, &t))
                sqlite3_bind_text(st, 2 + k, t.c_str(), (int)t.size(), SQLITE_TRANSIENT);
            else
                sqlite3_bind_blob(st, 2 + k, b.data(), (int)b.size(), SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(st, 4, (int)(c.rgb & 0xFF));
        sqlite3_bind_int(st, 5, (int)(c.rgb >> 8 & 0xFF));
        sqlite3_bind_int(st, 6, (int)(c.rgb >> 16 & 0xFF));
        if (sqlite3_step(st) != SQLITE_DONE) {
            *error = std::string("COLOR: ") + sqlite3_errmsg(db);
            ok = false;
        }
    }
    sqlite3_finalize(st);
    /* CAR_COLOR, the cars in their lists' order (the file's: by label) */
    ok = ok && exec(db, create_sql("CAR_COLOR", kCarColorDoc, car_color_columns()), error);
    st = nullptr;
    if (ok && sqlite3_prepare_v2(db, "INSERT INTO CAR_COLOR VALUES (?, ?, ?)", -1, &st, nullptr) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        ok = false;
    }
    std::vector<const CarColors::Car *> byList;
    for (const CarColors::Car &c : cc.cars)
        byList.push_back(&c);
    std::stable_sort(byList.begin(), byList.end(), [](const auto *a, const auto *b) { return a->offset < b->offset; });
    for (size_t i = 0; ok && i < byList.size(); i++) {
        const std::string car = code_text(byList[i]->code, labels);
        for (size_t k = 0; ok && k < byList[i]->colors.size(); k++) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, car.c_str(), (int)car.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (int64_t)k);
            sqlite3_bind_int64(st, 3, byList[i]->colors[k]);
            if (sqlite3_step(st) != SQLITE_DONE) {
                *error = std::string("CAR_COLOR: ") + sqlite3_errmsg(db);
                ok = false;
            }
        }
    }
    sqlite3_finalize(st);
    return finish_db(db, path, ok, error);
}

bool read_common(const void *bytes, size_t size, CommonFiles *out, std::vector<std::string> *warnings,
                 std::string *error)
{
    sqlite3 *db = open_bytes(bytes, size, error);
    if (db == nullptr)
        return false;
    const uint16_t version = (uint16_t)query_int(db, "PRAGMA user_version");
    /* the race modes */
    const std::vector<TableDef> &race = race_tables();
    std::vector<uint8_t> *files[2] = {&out->raceModes, &out->raceDetails};
    int found = 0;
    for (int i = 0; i < 2; i++) {
        bool has = false;
        const std::vector<std::vector<uint8_t>> recs = read_table(db, race[(size_t)i], nullptr, nullptr, warnings, &has);
        found += has;
        *files[i] = build_gtdt(race[(size_t)i].rowSize, version != 0 ? version : kRaceVersion, -1, recs);
    }
    /* the colours, by code */
    struct Color {
        uint32_t code, rgb;
        std::string names[2];
    };
    std::vector<Color> colors;
    std::set<uint32_t> codes;
    sqlite3_stmt *st = select_all(db, "COLOR");
    if (st == nullptr) {
        warnings->push_back("COLOR: no such table (no car has a colour)");
    } else {
        found++;
        const int kc[6] = {column_of(st, "Code"), column_of(st, "NameIntl"), column_of(st, "NameJp"),
                           column_of(st, "Red"), column_of(st, "Green"), column_of(st, "Blue")};
        int rowNo = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            rowNo++;
            auto warn = [&](const std::string &w) { warnings->push_back("COLOR row " + std::to_string(rowNo) + ": " + w); };
            long long v = 0;
            if (kc[0] < 0 || !column_whole(st, kc[0], &v) || v < 0 || v >= 0xFFFFFFFFll) {
                warn("no colour code (0 .. 4294967294): skipped");
                continue;
            }
            Color c;
            c.code = (uint32_t)v;
            if (!codes.insert(c.code).second) {
                warn("its code again: skipped");
                continue;
            }
            for (int k = 0; k < 2; k++) {
                const int col = kc[1 + k];
                if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL)
                    continue;
                if (sqlite3_column_type(st, col) == SQLITE_BLOB) {
                    c.names[k].assign((const char *)sqlite3_column_blob(st, col), (size_t)sqlite3_column_bytes(st, col));
                } else {
                    bool lossy = false;
                    c.names[k] = from_text(column_text(st, col), true, &lossy);
                    if (lossy)
                        warn("a character the game's text cannot show ('?' taken)");
                }
            }
            c.rgb = 0;
            for (int k = 0; k < 3; k++) {
                long long x = 0;
                if (kc[3 + k] >= 0 && (!column_whole(st, kc[3 + k], &x) || x < 0 || x > 255)) {
                    warn("a swatch value that is not 0 .. 255: cut to fit");
                    x = (std::min)((std::max)(x, 0ll), 255ll);
                }
                c.rgb |= (uint32_t)x << (8 * k);
            }
            colors.push_back(std::move(c));
        }
        sqlite3_finalize(st);
    }
    std::sort(colors.begin(), colors.end(), [](const Color &a, const Color &b) { return a.code < b.code; });
    /* the cars' lists: by car, in Position order (rows of one place in the
     * database's order) */
    struct Car {
        uint64_t code = 0;
        std::string label;
        std::vector<std::pair<long long, uint32_t>> colors;     /* position, colour */
    };
    std::vector<Car> cars;
    std::unordered_map<uint64_t, size_t> carAt;
    st = select_all(db, "CAR_COLOR");
    if (st == nullptr) {
        warnings->push_back("CAR_COLOR: no such table (no car has a colour)");
    } else {
        found++;
        const int kc[3] = {column_of(st, "Car"), column_of(st, "Position"), column_of(st, "Color")};
        int rowNo = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            rowNo++;
            auto warn = [&](const std::string &w) {
                warnings->push_back("CAR_COLOR row " + std::to_string(rowNo) + ": " + w);
            };
            uint64_t code = 0;
            const std::string label = kc[0] >= 0 ? column_text(st, kc[0]) : std::string();
            if (!text_code(label, &code) || code == 0) {
                warn("no car (a label or #<hex code>): skipped");
                continue;
            }
            long long pos = 0, color = 0;
            if (kc[1] >= 0 && !column_whole(st, kc[1], &pos))
                warn("Position is not a whole number: 0 taken");
            if (kc[2] < 0 || !column_whole(st, kc[2], &color) || color < 0 || color > 0xFFFFFFFFll) {
                warn("no colour code: skipped");
                continue;
            }
            if (!codes.count((uint32_t)color))
                warn("colour " + std::to_string(color) + " is not in COLOR (the game would stop at its name)");
            auto it = carAt.find(code);
            if (it == carAt.end()) {
                it = carAt.emplace(code, cars.size()).first;
                cars.push_back(Car());
                cars.back().code = code;
                cars.back().label = label;
            }
            cars[it->second].colors.push_back({pos, (uint32_t)color});
        }
        sqlite3_finalize(st);
    }
    std::map<std::string, std::set<std::string>> known;
    for (const TableDef &t : race)
        add_known(t, &known);
    for (const auto &c : color_columns())
        known["color"].insert(lower(c.first));
    for (const auto &c : car_color_columns())
        known["car_color"].insert(lower(c.first));
    read_extras(db, known, &out->extras);
    sqlite3_close(db);
    if (found == 0) {
        *error = "it has none of the shared tables (RACE_MODE, RACE_DETAIL, COLOR, CAR_COLOR)";
        return false;
    }
    for (Car &c : cars)
        std::stable_sort(c.colors.begin(), c.colors.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    /* carcolor.db: the lists in the pool by label, the cars by code */
    std::vector<size_t> byLabel(cars.size()), byCode(cars.size());
    for (size_t i = 0; i < cars.size(); i++)
        byLabel[i] = byCode[i] = i;
    std::stable_sort(byLabel.begin(), byLabel.end(), [&](size_t a, size_t b) { return cars[a].label < cars[b].label; });
    std::stable_sort(byCode.begin(), byCode.end(), [&](size_t a, size_t b) { return cars[a].code < cars[b].code; });
    std::vector<uint32_t> offset(cars.size()), pool;
    for (size_t i : byLabel) {
        offset[i] = 4 + 4 * (uint32_t)pool.size();
        for (const auto &c : cars[i].colors)
            pool.push_back(c.second);
    }
    /* carcolor.sdb: the names in their first use's order (the pool's, then
     * the colours no car has, by code), each once */
    std::vector<std::string> strings;
    std::unordered_map<std::string, uint32_t> stringAt;
    auto intern = [&](const std::string &s) {
        auto it = stringAt.find(s);
        if (it != stringAt.end())
            return it->second;
        stringAt.emplace(s, (uint32_t)strings.size());
        strings.push_back(s);
        return (uint32_t)strings.size() - 1;
    };
    std::unordered_map<uint32_t, size_t> colorAt;
    for (size_t i = 0; i < colors.size(); i++)
        colorAt.emplace(colors[i].code, i);
    std::vector<uint32_t> nameIdx(colors.size() * 2, 0);
    std::vector<bool> named(colors.size(), false);
    auto name = [&](size_t i) {
        if (named[i])
            return;
        named[i] = true;
        nameIdx[2 * i] = intern(colors[i].names[0]);
        nameIdx[2 * i + 1] = intern(colors[i].names[1]);
    };
    for (uint32_t code : pool) {
        auto it = colorAt.find(code);
        if (it != colorAt.end())
            name(it->second);
    }
    for (size_t i = 0; i < colors.size(); i++)
        name(i);
    const uint32_t lists = 0x18 + 0x10 * (uint32_t)cars.size();
    const uint32_t table = lists + 4 + 4 * (uint32_t)pool.size();
    std::vector<uint8_t> &d = out->carColor;
    d.assign(table + 4 + 0x10 * colors.size(), 0);
    std::memcpy(d.data(), "GT2K", 4);
    wr32(&d[8], (uint32_t)cars.size());
    wr32(&d[0xC], lists);
    wr32(&d[0x10], table);
    wr32(&d[0x14], (uint32_t)d.size());
    for (size_t k = 0; k < byCode.size(); k++) {
        const Car &c = cars[byCode[k]];
        uint8_t *e = &d[0x18 + 0x10 * k];
        wr64(e, c.code);
        wr32(e + 8, (uint32_t)c.colors.size());
        wr32(e + 12, offset[byCode[k]]);
    }
    wr32(&d[lists], (uint32_t)pool.size());
    for (size_t k = 0; k < pool.size(); k++)
        wr32(&d[lists + 4 + 4 * k], pool[k]);
    wr32(&d[table], (uint32_t)colors.size());
    for (size_t k = 0; k < colors.size(); k++) {
        uint8_t *c = &d[table + 4 + 0x10 * k];
        wr32(c, colors[k].code);
        wr32(c + 4, nameIdx[2 * k]);
        wr32(c + 8, nameIdx[2 * k + 1]);
        wr32(c + 12, colors[k].rgb);
    }
    out->carColorStrings = build_stdb(strings, -1, true);
    return true;
}

bool same_common(const CommonFiles &f, const CommonFiles &img, std::string *why)
{
    const std::vector<TableDef> &race = race_tables();
    const std::vector<uint8_t> *a[2] = {&f.raceModes, &f.raceDetails}, *b[2] = {&img.raceModes, &img.raceDetails};
    for (int i = 0; i < 2; i++) {
        Block x, y;
        if (!parse_gtdt(a[i]->data(), a[i]->size(), &x) || !parse_gtdt(b[i]->data(), b[i]->size(), &y)) {
            *why = std::string(race[(size_t)i].name) + ": not a GTDT block";
            return false;
        }
        if (!same_rows(race[(size_t)i], x, y, nullptr, nullptr, nullptr, nullptr, why))
            return false;
    }
    CarColors x, y;
    std::vector<std::string> sx, sy;
    std::string e;
    if (!parse_carcolor(f.carColor, &x, &e) || !parse_carcolor(img.carColor, &y, &e) ||
        !parse_stdb(f.carColorStrings, &sx) || !parse_stdb(img.carColorStrings, &sy)) {
        *why = e.empty() ? "carcolor.sdb" : e;
        return false;
    }
    if (x.cars.size() != y.cars.size() || x.colors.size() != y.colors.size()) {
        *why = "carcolor.db: the number of cars or colours";
        return false;
    }
    for (size_t i = 0; i < x.cars.size(); i++)
        if (x.cars[i].code != y.cars[i].code || x.cars[i].colors != y.cars[i].colors) {
            *why = "carcolor.db: car " + hex64(x.cars[i].code) + "'s colours";
            return false;
        }
    for (size_t i = 0; i < x.colors.size(); i++) {
        const CarColors::Color &p = x.colors[i], &q = y.colors[i];
        auto text = [](const std::vector<std::string> &s, uint32_t k) { return k < s.size() ? s[k] : std::string("?"); };
        if (p.code != q.code || p.rgb != q.rgb || text(sx, p.nameIntl) != text(sy, q.nameIntl) ||
            text(sx, p.nameJp) != text(sy, q.nameJp)) {
            *why = "carcolor.db: colour " + std::to_string(p.code);
            return false;
        }
    }
    return true;
}

/* ---- the race display's names ------------------------------------------------ */

namespace {

using Names = std::vector<std::pair<uint64_t, std::string>>;    /* old code, new name */

/* What a RACE_DETAIL row shows (RaceDisplay::setLayout's object of its Type,
 * by its SubType where that picks one), as its name's middle. */
std::string detail_what(uint16_t type, uint16_t sub)
{
    static const char *const kItems[27] = {
        "EMPTY", "POSITION", "LAP", "METERS", "BOOST", "TIRE_WEAR", "WARNING", "EVENTS", "RECORD", "MAP",
        "MIRROR", "TIME", "STEERING", "AB_MONITOR", "ONBOARD_TACHO", "REMOTE_TACHO", "BATTLE_TACHO",
        "SPEEDOMETER", "DIGITAL_SPEED", "GEAR", "ODOMETER", "TRIP_METER", "INDICATORS", "CAR_NAME",
        "REPLAY_MODE", "EVENTS2", "TITLE",
    };
    static const char *const kTimes[15] = {
        nullptr, "LAP_TIMES", "TOTAL_TIME", "BEST_LAP", "LAST_LAP", "LAP_TIME", "BEST_LAP_B", "RECORD_TIME",
        "RECORD_LAP", "RECORD_HOLDER", "TIME", "RECORD_SPEED", "TOP_SPEED", "FASTEST_TIME", "TRIAL_TIME",
    };
    static const char *const kPlaces[3] = {"RECORD_1ST", "RECORD_2ND", "RECORD_3RD"};
    switch (type) {
    case 3:
        if (sub == 0)
            return "TACHO_SET_ONBOARD";
        if (sub == 1)
            return "TACHO_SET_REMOTE";
        if (sub == 0x40)
            return "PANEL_FRAME";
        if (sub == 0x41)
            return "PANEL";
        return "METERS" + std::to_string(sub);         /* (shows nothing) */
    case 8:
        return sub < 3 ? std::string(kPlaces[sub]) : "RECORD" + std::to_string(sub);
    case 10:
        return sub == 0 ? "MIRROR" : "MIRROR" + std::to_string(sub);
    case 11:
        return sub < 15 && kTimes[sub] != nullptr ? std::string(kTimes[sub]) : "TIME" + std::to_string(sub);
    case 13:
        return sub == 1 ? "BATTERY" : "AB_MONITOR";
    case 19:
        return sub & 1 ? "GEAR_LAMP_ABOVE" : "GEAR";
    case 26:
        return sub == 0 ? "TITLE" : "MODE_MESSAGE";
    }
    /* (the other objects do not read their SubType) */
    return type < 27 ? kItems[type] : "TYPE" + std::to_string(type);
}

/* Names for the modes' and details' rows `labels' knows no label of. */
void race_names(const Block &modes, const Block &details, const Labels &labels, Names *modeNames, Names *detailNames)
{
    auto unnamed = [&labels](uint64_t c) { return code_text(c, labels)[0] == '#'; };
    std::set<std::string> taken;
    std::set<uint64_t> modeCodes, detailCodes;
    for (const uint8_t *r : modes.rows) {
        modeCodes.insert(rd64(r));
        if (!unnamed(rd64(r)))
            taken.insert(code_text(rd64(r), labels));
    }
    for (const uint8_t *r : details.rows) {
        detailCodes.insert(rd64(r));
        if (!unnamed(rd64(r)))
            taken.insert(code_text(rd64(r), labels));
    }
    /* (a name twice, or one whose code a row has: _2, _3...) */
    auto unique = [&taken](const std::string &name, std::set<uint64_t> *codes) {
        std::string n = name;
        for (int k = 2; taken.count(n) || codes->count(name_to_code(n)); k++)
            n = name + "_" + std::to_string(k);
        taken.insert(n);
        codes->insert(name_to_code(n));
        return n;
    };
    /* the modes, by id */
    std::vector<const uint8_t *> byId = modes.rows;
    std::stable_sort(byId.begin(), byId.end(), [](const uint8_t *a, const uint8_t *b) { return a[8] < b[8]; });
    std::unordered_map<uint64_t, std::string> modeName;
    for (const uint8_t *r : byId) {
        const uint64_t c = rd64(r);
        if (modeName.count(c))
            continue;
        std::string n = code_text(c, labels);
        if (unnamed(c)) {
            n = unique("RACE_MODE_ID" + std::to_string(r[8]), &modeCodes);
            modeNames->push_back({c, n});
        }
        modeName.emplace(c, n);
    }
    /* the details: the modes' (by id, in their lists' order) by their mode,
     * then the others (by code) as UNUSED */
    const uint64_t null = name_to_code("(Null)");
    std::unordered_map<uint64_t, std::string> owner;
    std::vector<uint64_t> order;
    for (const uint8_t *r : byId) {
        std::string m = modeName[rd64(r)];
        if (m.compare(0, 10, "RACE_MODE_") == 0 && m.size() > 10)
            m = m.substr(10);
        for (int k = 0; k < 32; k++) {
            const uint64_t d = rd64(r + 0x10 + 8 * k);
            if (d == null)
                break;
            if (owner.emplace(d, m).second)
                order.push_back(d);
        }
    }
    std::vector<uint64_t> rest;
    std::unordered_map<uint64_t, const uint8_t *> rowOf;
    for (const uint8_t *r : details.rows)
        if (rowOf.emplace(rd64(r), r).second && !owner.count(rd64(r)))
            rest.push_back(rd64(r));
    std::sort(rest.begin(), rest.end());
    order.insert(order.end(), rest.begin(), rest.end());
    std::set<uint64_t> done;
    for (uint64_t c : order) {
        auto it = rowOf.find(c);
        if (it == rowOf.end() || !unnamed(c) || !done.insert(c).second)
            continue;
        const uint8_t *r = it->second;
        auto ow = owner.find(c);
        std::string n = (ow != owner.end() ? ow->second : std::string("UNUSED")) + "_" + detail_what(rd16(r + 8), rd16(r + 0xA));
        const int16_t view = (int16_t)rd16(r + 0x18);
        if (rd16(r + 8) != 0) {                     /* (an empty one: no view) */
            if (view == 0)
                n += "_BUMPER";
            else if (view == 1)
                n += "_CHASE";
            else if (view != 0x7F)
                n += "_VIEW" + std::to_string(view);
        }
        detailNames->push_back({c, unique(n, &detailCodes)});
    }
}

/* A GTDT block with its codes renamed - its Label column by `rows', its Id
 * columns by `ids' (old -> new) - and its records re-sorted by code (rows
 * of one code in their order). */
std::vector<uint8_t> renamed_block(const Block &b, const TableDef &t, const std::unordered_map<uint64_t, uint64_t> &rows,
                                   const std::unordered_map<uint64_t, uint64_t> &ids)
{
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> recs;
    for (const uint8_t *r : b.rows) {
        std::vector<uint8_t> rec(r, r + b.rowSize);
        for (int i = 0; i < t.count; i++) {
            const Column &c = t.columns[i];
            if (c.type != Type::Label && c.type != Type::Id)
                continue;
            const std::unordered_map<uint64_t, uint64_t> &m = c.type == Type::Label ? rows : ids;
            auto it = m.find(rd64(&rec[c.offset]));
            if (it != m.end())
                wr64(&rec[c.offset], it->second);
        }
        recs.push_back({rd64(rec.data()), std::move(rec)});
    }
    std::stable_sort(recs.begin(), recs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::vector<uint8_t>> out;
    for (auto &r : recs)
        out.push_back(std::move(r.second));
    return build_gtdt(b.rowSize, b.version, b.tableId, out);
}

/* The names put in the race files (name_race_rows). */
void apply_race_names(CommonFiles *f, const Names &modeNames, const Names &detailNames)
{
    Block modes, details;
    if (!parse_gtdt(f->raceModes.data(), f->raceModes.size(), &modes) ||
        !parse_gtdt(f->raceDetails.data(), f->raceDetails.size(), &details))
        return;
    std::unordered_map<uint64_t, uint64_t> mm, dm;
    for (const auto &p : modeNames)
        mm[p.first] = name_to_code(p.second);
    for (const auto &p : detailNames)
        dm[p.first] = name_to_code(p.second);
    const std::vector<TableDef> &race = race_tables();
    std::vector<uint8_t> m2 = renamed_block(modes, race[0], mm, dm);
    std::vector<uint8_t> d2 = renamed_block(details, race[1], dm, {});
    f->raceModes = std::move(m2);
    f->raceDetails = std::move(d2);
}

/* A table's CREATE statement made the schema's (its comments) when it
 * differs: the table rebuilt, its rows by label as a new database's (rows
 * of one label in their order), the columns the schema does not know kept
 * after the schema's.  Left alone when it lacks one of the schema's columns
 * or has no rowid. */
bool refresh_table(sqlite3 *db, const TableDef &t, std::string *error)
{
    std::string name, stored;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name = ? COLLATE NOCASE",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t.name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            name = column_text(st, 0);
            stored = column_text(st, 1);
        }
    }
    sqlite3_finalize(st);
    if (name.empty() || lower(stored).find("without rowid") != std::string::npos)
        return true;
    std::vector<std::pair<std::string, std::string>> have;     /* its columns: name, declared type */
    if (sqlite3_prepare_v2(db, ("PRAGMA table_info(\"" + name + "\")").c_str(), -1, &st, nullptr) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            have.push_back({column_text(st, 1), column_text(st, 2)});
    sqlite3_finalize(st);
    std::set<std::string> known;
    std::vector<std::pair<std::string, std::pair<const char *, const char *>>> cols;
    for (int i = 0; i < t.count; i++)
        if (t.columns[i].type != Type::Pad) {
            known.insert(lower(t.columns[i].name));
            cols.push_back({t.columns[i].name, {sql_type(t.columns[i].type), t.columns[i].doc}});
        }
    size_t found = 0;
    for (const auto &h : have)
        found += known.count(lower(h.first));
    if (found != known.size())
        return true;
    for (const auto &h : have)
        if (!known.count(lower(h.first)))
            cols.push_back({h.first, {h.second.c_str(), ""}});
    const std::string sql = create_sql(t.name, t.doc, cols);
    if (sql == stored)
        return true;
    std::string list;
    for (const auto &c : cols)
        list += (list.empty() ? "\"" : ", \"") + c.first + "\"";
    const std::string old = std::string(t.name) + "__old";
    return exec(db, "ALTER TABLE \"" + name + "\" RENAME TO \"" + old + "\"; " + sql + "; INSERT INTO \"" + t.name +
                        "\" (" + list + ") SELECT " + list + " FROM \"" + old + "\" ORDER BY \"Label\", rowid; DROP TABLE \"" +
                        old + "\"",
                error);
}

/* What the game reads of an open shared database. */
bool read_open_common(sqlite3 *db, CommonFiles *out, std::string *error)
{
    sqlite3_int64 size = 0;
    unsigned char *bytes = sqlite3_serialize(db, "main", &size, 0);
    if (bytes == nullptr) {
        *error = "the database cannot be read";
        return false;
    }
    std::vector<std::string> warnings;
    const bool ok = read_common(bytes, (size_t)size, out, &warnings, error);
    sqlite3_free(bytes);
    return ok;
}

} // namespace

int name_race_rows(CommonFiles *files, std::vector<std::pair<uint64_t, std::string>> *labelList)
{
    Labels labels;
    for (const auto &l : *labelList)
        labels.emplace(l.first, l.second);
    add_known_labels(&labels);
    Block modes, details;
    if (!parse_gtdt(files->raceModes.data(), files->raceModes.size(), &modes) ||
        !parse_gtdt(files->raceDetails.data(), files->raceDetails.size(), &details))
        return 0;
    Names mn, dn;
    race_names(modes, details, labels, &mn, &dn);
    for (const auto &p : mn)
        labelList->push_back({name_to_code(p.second), p.second});
    for (const auto &p : dn)
        labelList->push_back({name_to_code(p.second), p.second});
    if (!mn.empty() || !dn.empty())
        apply_race_names(files, mn, dn);
    return (int)(mn.size() + dn.size());
}

int unnamed_race_rows(const void *bytes, size_t size)
{
    std::string error;
    sqlite3 *db = open_bytes(bytes, size, &error);
    if (db == nullptr)
        return 0;
    const int64_t n = query_int(db, "SELECT count(*) FROM RACE_MODE WHERE Label LIKE '#%'") +
                      query_int(db, "SELECT count(*) FROM RACE_DETAIL WHERE Label LIKE '#%'");
    sqlite3_close(db);
    return (int)n;
}

bool name_race_database(const std::string &path, int *named, std::string *error)
{
    *named = 0;
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        *error = db != nullptr ? sqlite3_errmsg(db) : "the database cannot be opened";
        sqlite3_close(db);
        return false;
    }
    CommonFiles before;
    if (!read_open_common(db, &before, error)) {
        sqlite3_close(db);
        return false;
    }
    /* the labels: its names (the #<code> ones are none) */
    const std::vector<TableDef> &race = race_tables();
    Labels labels;
    add_known_labels(&labels);
    for (const TableDef &t : race)
        for (int i = 0; i < t.count; i++) {
            const Column &c = t.columns[i];
            if (c.type != Type::Label && c.type != Type::Id)
                continue;
            sqlite3_stmt *st = select_all(db, t.name, std::string("\"") + c.name + "\"");
            while (st != nullptr && sqlite3_step(st) == SQLITE_ROW) {
                const std::string text = column_text(st, 0);
                if (text.empty() || text[0] == '#')
                    continue;
                bool lossy = false;
                const std::string b = from_text(text, false, &lossy);
                labels.emplace(name_to_code(b), b);
            }
            sqlite3_finalize(st);
        }
    Block modes, details;
    Names mn, dn;
    if (parse_gtdt(before.raceModes.data(), before.raceModes.size(), &modes) &&
        parse_gtdt(before.raceDetails.data(), before.raceDetails.size(), &details))
        race_names(modes, details, labels, &mn, &dn);
    /* the names by SQL: the rows' labels and the modes' lists */
    bool ok = exec(db, "BEGIN", error);
    auto rename = [&](const char *table, const char *column, const std::string &from, const std::string &to) {
        sqlite3_stmt *st = nullptr;
        const std::string sql = std::string("UPDATE \"") + table + "\" SET \"" + column + "\" = ?1 WHERE upper(\"" +
                                column + "\") = ?2";
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            *error = std::string(table) + ": " + sqlite3_errmsg(db);
            return false;
        }
        sqlite3_bind_text(st, 1, to.c_str(), (int)to.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, from.c_str(), (int)from.size(), SQLITE_TRANSIENT);
        const bool done = sqlite3_step(st) == SQLITE_DONE;
        if (!done)
            *error = std::string(table) + ": " + sqlite3_errmsg(db);
        sqlite3_finalize(st);
        return done;
    };
    for (size_t i = 0; ok && i < mn.size(); i++)
        ok = rename(race[0].name, "Label", "#" + hex64(mn[i].first), mn[i].second);
    for (size_t i = 0; ok && i < dn.size(); i++) {
        const std::string from = "#" + hex64(dn[i].first);
        ok = rename(race[1].name, "Label", from, dn[i].second);
        for (int k = 0; ok && k < race[0].count; k++)
            if (race[0].columns[k].type == Type::Id)
                ok = rename(race[0].name, race[0].columns[k].name, from, dn[i].second);
    }
    for (const TableDef &t : race)
        ok = ok && refresh_table(db, t, error);
    ok = ok && exec(db, "COMMIT", error);
    /* the check: what the game reads now is what it read, the names' codes
     * in place of the old */
    CommonFiles after;
    if (ok && !read_open_common(db, &after, error))
        ok = false;
    if (ok) {
        apply_race_names(&before, mn, dn);
        std::string why;
        if (!same_common(before, after, &why)) {
            *error = "the check after the naming failed (" + why + ")";
            ok = false;
        }
    }
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    if (sqlite3_close(db) != SQLITE_OK && ok) {
        *error = "the database could not be closed";
        ok = false;
    }
    *named = ok ? (int)(mn.size() + dn.size()) : 0;
    return ok;
}

/* ---- the real-time texts ------------------------------------------------------ */

namespace {

/* The game's countries 0..6 (T516), the language blocks the game reads. */
const char *const kTextLanguages[7] = {"Japanese", "EnglishUS", "EnglishUK", "French", "German", "Italian", "Spanish"};
const char *const kTextLanguageDocs[7] = {
    "its text in Japanese (country 0); empty: the game keeps the text it has (its built-in one)",
    "in English (US), country 1",
    "in English (UK), country 2",
    "in French, country 3",
    "in German, country 4",
    "in Italian, country 5",
    "in Spanish, country 6",
};
constexpr uint32_t kMltx = 0x58544C4D;          /* "MLTX" */
constexpr int kTextBlocks = 7;

/* text/realtime.dat as read: its version, its names (sorted), each block's
 * text of each name. */
struct Texts {
    uint32_t version = 2;
    std::vector<std::string> names;
    std::vector<std::vector<std::string>> blocks;
};

/* The string at `at' up to its terminator or `end'. */
std::string string_at(const std::vector<uint8_t> &d, size_t at, size_t end)
{
    size_t e = at;
    while (e < end && d[e] != 0)
        e++;
    return std::string((const char *)&d[at], e - at);
}

/* text/realtime.dat ("MLTX", u32 version, u32 the blocks' offset, u32 the
 * names, u32 a block's size, {u32 name offset, u32 text offset in a
 * block}[names], the names, the blocks). */
bool parse_mltx(const std::vector<uint8_t> &d, Texts *out, std::string *error)
{
    if (d.size() < 0x14 || std::memcmp(d.data(), "MLTX", 4) != 0) {
        *error = "not a realtime.dat (no MLTX head)";
        return false;
    }
    out->version = rd32(&d[4]);
    const uint32_t texts = rd32(&d[8]), count = rd32(&d[12]), size = rd32(&d[16]);
    if (0x14 + 8ull * count > d.size() || texts > d.size() || (size == 0 && count > 0)) {
        *error = "realtime.dat: its index runs past the file";
        return false;
    }
    const size_t blocks = size != 0 ? (d.size() - texts) / size : 0;
    out->names.clear();
    out->blocks.assign(blocks, std::vector<std::string>());
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t n = rd32(&d[0x14 + 8 * i]), t = rd32(&d[0x18 + 8 * i]);
        if (n >= d.size() || t >= size) {
            *error = "realtime.dat: entry " + std::to_string(i) + " points past the file";
            return false;
        }
        out->names.push_back(string_at(d, n, d.size()));
        for (size_t b = 0; b < blocks; b++)
            out->blocks[b].push_back(string_at(d, texts + b * size + t, texts + (b + 1) * size));
    }
    return true;
}

/* The file of these texts as the discs' are: the names sorted (the game's
 * binary search), after the index; each text a slot of its longest
 * language's size + 1 in every block. */
std::vector<uint8_t> build_mltx(const Texts &t)
{
    const size_t count = t.names.size();
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; i++)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&t](size_t a, size_t b) { return t.names[a] < t.names[b]; });
    size_t names = 0;
    for (const std::string &n : t.names)
        names += n.size() + 1;
    std::vector<uint32_t> slot(count);
    uint32_t size = 0;
    for (size_t i : order) {
        size_t longest = 0;
        for (const auto &b : t.blocks)
            longest = (std::max)(longest, b[i].size());
        slot[i] = size;
        size += (uint32_t)longest + 1;
    }
    const size_t texts = 0x14 + 8 * count + names;
    std::vector<uint8_t> d(texts + (size_t)size * t.blocks.size(), 0);
    std::memcpy(d.data(), "MLTX", 4);
    wr32(&d[4], t.version);
    wr32(&d[8], (uint32_t)texts);
    wr32(&d[12], (uint32_t)count);
    wr32(&d[16], size);
    size_t at = 0x14 + 8 * count;
    for (size_t k = 0; k < count; k++) {
        const size_t i = order[k];
        wr32(&d[0x14 + 8 * k], (uint32_t)at);
        wr32(&d[0x18 + 8 * k], slot[i]);
        std::memcpy(&d[at], t.names[i].data(), t.names[i].size());
        at += t.names[i].size() + 1;
        for (size_t b = 0; b < t.blocks.size(); b++)
            std::memcpy(&d[texts + b * size + slot[i]], t.blocks[b][i].data(), t.blocks[b][i].size());
    }
    return d;
}

} // namespace

bool write_texts(const std::vector<uint8_t> &file, const std::string &path, std::string *error)
{
    Texts t;
    if (!parse_mltx(file, &t, error))
        return false;
    std::vector<std::pair<std::string, std::pair<const char *, const char *>>> cols = {
        {"Name", {"TEXT", "the text's name, <table>_<key>: a table of the game's (RACEDISPLAY the race display's words, "
                          "DATABASEPARTS the part names, DATABASERACECATEGORY the race categories, MEMORYCARDTEXT, "
                          "SETTING, SHORTMENU, RACEMENUTEXT, RACEMSG, GTMODE, PRIZE_RT_MSG) and the key it asks for; "
                          "a name no table asks for is not used"}},
    };
    std::vector<std::string> blockNames;
    for (size_t b = 0; b < t.blocks.size(); b++)
        blockNames.push_back(b < kTextBlocks ? kTextLanguages[b] : "Block" + std::to_string(b));
    for (size_t b = 0; b < t.blocks.size(); b++)
        cols.push_back({blockNames[b],
                        {b < kTextBlocks ? "TEXT" : "BLOB",
                         b < kTextBlocks ? kTextLanguageDocs[b]
                                         : "a further block of the file (a GT Concept disc's languages): the game does "
                                           "not read it; kept as it is (its bytes when not ASCII)"}});
    sqlite3 *db = create_db(path, kMltx, t.version, error);
    if (db == nullptr)
        return false;
    bool ok = exec(db,
                   create_sql("TEXTS",
                              "the texts the game prints in each language (T517 GT2KRealTimeText: the race display's "
                              "words, the part names...), a row a text",
                              cols),
                   error);
    std::string sql = "INSERT INTO TEXTS VALUES (?";
    for (size_t b = 0; b < t.blocks.size(); b++)
        sql += ", ?";
    sql += ")";
    sqlite3_stmt *st = nullptr;
    if (ok && sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        ok = false;
    }
    for (size_t i = 0; ok && i < t.names.size(); i++) {
        sqlite3_reset(st);
        std::string name;
        if (!to_text(t.names[i], false, &name))
            name = t.names[i];
        sqlite3_bind_text(st, 1, name.c_str(), (int)name.size(), SQLITE_TRANSIENT);
        for (size_t b = 0; b < t.blocks.size(); b++) {
            const std::string &s = t.blocks[b][i];
            std::string text;
            if (s.empty())
                sqlite3_bind_null(st, 2 + (int)b);
            else if (b < kTextBlocks ? to_text(s, true, &text) : ascii(s))
                sqlite3_bind_text(st, 2 + (int)b, b < kTextBlocks ? text.c_str() : s.c_str(),
                                  (int)(b < kTextBlocks ? text.size() : s.size()), SQLITE_TRANSIENT);
            else
                sqlite3_bind_blob(st, 2 + (int)b, s.data(), (int)s.size(), SQLITE_TRANSIENT);
        }
        if (sqlite3_step(st) != SQLITE_DONE) {
            *error = std::string("TEXTS: ") + sqlite3_errmsg(db);
            ok = false;
        }
    }
    sqlite3_finalize(st);
    return finish_db(db, path, ok, error);
}

bool read_texts(const void *bytes, size_t size, std::vector<uint8_t> *out, std::vector<std::string> *warnings,
                std::string *error)
{
    sqlite3 *db = open_bytes(bytes, size, error);
    if (db == nullptr)
        return false;
    Texts t;
    const int64_t version = query_int(db, "PRAGMA user_version");
    t.version = version != 0 ? (uint32_t)version : 2;
    sqlite3_stmt *st = select_all(db, "TEXTS");
    if (st == nullptr) {
        sqlite3_close(db);
        *error = "it has no table TEXTS";
        return false;
    }
    /* the columns: Name, the seven languages, Block7, Block8... while there */
    const int nameAt = column_of(st, "Name");
    std::vector<int> at;
    for (int b = 0; b < kTextBlocks; b++) {
        at.push_back(column_of(st, kTextLanguages[b]));
        if (at.back() < 0)
            warnings->push_back(std::string("TEXTS: no column ") + kTextLanguages[b] + " (its texts empty)");
    }
    for (int b = kTextBlocks;; b++) {
        const int k = column_of(st, ("Block" + std::to_string(b)).c_str());
        if (k < 0)
            break;
        at.push_back(k);
    }
    t.blocks.assign(at.size(), std::vector<std::string>());
    std::set<std::string> seen;
    int rowNo = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        rowNo++;
        auto warn = [&](const std::string &w) { warnings->push_back("TEXTS row " + std::to_string(rowNo) + ": " + w); };
        bool lossy = false;
        const std::string name = nameAt >= 0 ? from_text(column_text(st, nameAt), false, &lossy) : std::string();
        if (name.empty()) {
            warn("no name: skipped");
            continue;
        }
        if (!seen.insert(name).second) {
            warn("the name " + name + " again: skipped");
            continue;
        }
        t.names.push_back(name);
        for (size_t b = 0; b < at.size(); b++) {
            std::string s;
            const int k = at[b];
            if (k >= 0 && sqlite3_column_type(st, k) == SQLITE_BLOB) {
                s.assign((const char *)sqlite3_column_blob(st, k), (size_t)sqlite3_column_bytes(st, k));
            } else if (k >= 0 && sqlite3_column_type(st, k) != SQLITE_NULL) {
                s = from_text(column_text(st, k), true, &lossy);
                if (lossy)
                    warn(name + ": a character the game's text cannot show ('?' taken)");
            }
            const size_t nul = s.find('\0');
            if (nul != std::string::npos)
                s.resize(nul);
            t.blocks[b].push_back(s);
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    *out = build_mltx(t);
    return true;
}

bool same_texts(const std::vector<uint8_t> &file, const std::vector<uint8_t> &image, std::string *why)
{
    Texts a, b;
    std::string e;
    if (!parse_mltx(file, &a, &e) || !parse_mltx(image, &b, &e)) {
        *why = e;
        return false;
    }
    if (a.version != b.version || a.names != b.names || a.blocks.size() != b.blocks.size()) {
        *why = "the version, the names or the number of languages";
        return false;
    }
    for (size_t k = 0; k < a.blocks.size(); k++)
        for (size_t i = 0; i < a.names.size(); i++)
            if (a.blocks[k][i] != b.blocks[k][i]) {
                *why = a.names[i] + " in block " + std::to_string(k);
                return false;
            }
    return true;
}

} // namespace paramdb
