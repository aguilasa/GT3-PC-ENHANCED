/* gcf_tables.cpp - the product configs as SQLite tables (gcf_tables.h). */
#include "platform/gcf_tables.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#ifndef _WIN32
#include "platform/os/win_compat.h"         /* _stricmp */
#endif

namespace gcftables {
namespace {

/* ---- the .gcf ------------------------------------------------------------ */

/* Bounds-checked little-endian reads of the file; `ok' false once one fell
 * outside it. */
struct In {
    const uint8_t *d;
    size_t n;
    bool ok = true;
    int32_t i32(size_t o)
    {
        if (o > n || n - o < 4) {
            ok = false;
            return 0;
        }
        return (int32_t)((uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24);
    }
    int16_t i16(size_t o)
    {
        if (o > n || n - o < 2) {
            ok = false;
            return 0;
        }
        return (int16_t)(d[o] | d[o + 1] << 8);
    }
    int8_t i8(size_t o)
    {
        if (o >= n) {
            ok = false;
            return 0;
        }
        return (int8_t)d[o];
    }
    /* the record at `base' + the offset in its field at `field' */
    size_t at(size_t base, size_t field) { return base + (size_t)(int64_t)i32(field); }
    std::string str(size_t o)
    {
        if (o >= n) {
            ok = false;
            return std::string();
        }
        const void *end = std::memchr(d + o, 0, n - o);
        if (end == nullptr) {
            ok = false;
            return std::string();
        }
        return std::string((const char *)d + o, (const char *)end);
    }
    /* a list of `count' records: the list at `base' + its field's offset,
     * each entry's record at `base' + its offset */
    template <typename F> void list(size_t base, size_t countField, size_t listField, F each)
    {
        const int32_t count = i32(countField);
        if (count < 0 || count > 0x100000) {
            ok = false;
            return;
        }
        const size_t l = at(base, listField);
        for (int32_t i = 0; i < count && ok; i++)
            each(at(base, l + 4 * (size_t)i));
    }
};

/* ---- the image ----------------------------------------------------------- */

struct Out {
    std::vector<uint8_t> b;
    std::map<std::string, size_t> strings;
    size_t alloc(size_t n)
    {
        const size_t at = (b.size() + 3) & ~(size_t)3;
        b.resize(at + n, 0);
        return at;
    }
    void i32(size_t at, int32_t v)
    {
        for (int k = 0; k < 4; k++)
            b[at + (size_t)k] = (uint8_t)((uint32_t)v >> (8 * k));
    }
    void i16(size_t at, int16_t v)
    {
        b[at] = (uint8_t)v;
        b[at + 1] = (uint8_t)((uint16_t)v >> 8);
    }
    void i8(size_t at, int8_t v) { b[at] = (uint8_t)v; }
    /* the field at `field' of the record at `base' pointing at `target' */
    void rel(size_t base, size_t field, size_t target) { i32(field, (int32_t)((int64_t)target - (int64_t)base)); }
    size_t str(const std::string &s)
    {
        auto it = strings.find(s);
        if (it != strings.end())
            return it->second;
        const size_t at = b.size();
        b.insert(b.end(), s.begin(), s.end());
        b.push_back(0);
        strings.emplace(s, at);
        return at;
    }
    /* a record holding {count, list}: the list's entries written by
     * `entry(i)' (returning the entry's record) */
    template <typename F> void list(size_t base, size_t countField, size_t listField, size_t count, F entry)
    {
        i32(countField, (int32_t)count);
        const size_t l = alloc(4 * count);
        rel(base, listField, l);
        for (size_t i = 0; i < count; i++) {
            const size_t e = entry(i);
            rel(base, l + 4 * i, e);
        }
    }
};

/* The game's name order: signed bytes (gcf_find_event and the others). */
bool name_less(const std::string &a, const std::string &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return (signed char)a[i] < (signed char)b[i];
    return a.size() < b.size();
}

/* ---- the database --------------------------------------------------------- */

std::string lower_copy(const char *s)
{
    std::string t = s != nullptr ? s : "";
    for (char &ch : t)
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
    return t;
}

/* Each table's CREATE statement: the columns' meanings as comments (a
 * database browser shows them with the schema). */
struct Schema {
    int section;
    const char *name;
    const char *create;
};
const Schema kSchema[] = {
    {GENERAL, "general",
     "CREATE TABLE general (  -- the general values (the .gcf's section 0x0)\n"
     "    setting TEXT PRIMARY KEY,  -- idle_seconds: the title page's idle time before the attract demo, s;\n"
     "                               -- demo_time_limit: the attract demo race's length, s\n"
     "    value INTEGER\n"
     ")"},
    {CARS, "cars",
     "CREATE TABLE cars (  -- the arcade mode's car lists by category (section 0xE)\n"
     "    position INTEGER,   -- the order: a category's cars in it, and the categories as their first cars come\n"
     "    category TEXT,      -- CLASS_A ... (the arcade car select's classes)\n"
     "    car TEXT,           -- the car's code\n"
     "    race TEXT,          -- its arcade race's code (empty: none of its own)\n"
     "    selectable INTEGER, -- 1: can be chosen, else locked\n"
     "    level INTEGER,      -- the arcade level that opens it\n"
     "    race_step INTEGER   -- added to the last character of the arcade race code\n"
     ")"},
    {COURSES, "courses",
     "CREATE TABLE courses (  -- the course lists by game mode (section 0x8)\n"
     "    position INTEGER,  -- the order: a list's courses in it (a course's index), and the lists as their first\n"
     "                       -- courses come\n"
     "    list TEXT,         -- SINGLE_RACE, TIME_ATTACK, TWO_PLAYER, I_LINK, COMPULSORY ...\n"
     "    course TEXT,       -- the course's code (database COURSE)\n"
     "    available INTEGER, -- 1: can be chosen, else locked\n"
     "    flag INTEGER,      -- (not read by the game)\n"
     "    level INTEGER,     -- the arcade level that opens it (0 .. 5)\n"
     "    length INTEGER     -- in metres, shown in the course's information\n"
     ")"},
    {EVENTS, "events",
     "CREATE TABLE events (  -- the GT mode's events (section 0x4; the game sorts them by name)\n"
     "    event TEXT PRIMARY KEY, -- the race code (database EVENT)\n"
     "    requirement INTEGER     -- open once the licences reach it (0: at once; 1: every S licence test has a\n"
     "                            -- medal)\n"
     ")"},
    {BEST_TIMES, "best_times",
     "CREATE TABLE best_times (  -- the courses with a best time kept (section 0x6; sorted by the game)\n"
     "    course TEXT PRIMARY KEY,\n"
     "    value INTEGER  -- (not read by the game: on the discs the name's first four letters as a number)\n"
     ")"},
    {SERVICES, "services",
     "CREATE TABLE services (  -- the shops' service prices (section 0xA; sorted by the game)\n"
     "    service TEXT PRIMARY KEY, -- car_wash, oil_change, wheel; initial: a new game's money\n"
     "    price INTEGER             -- credits (x100 in Japanese: yen)\n"
     ")"},
    {DEMOS, "demos",
     "CREATE TABLE demos (  -- the attract demos (section 0xC)\n"
     "    position INTEGER,          -- the order\n"
     "    race TEXT,                 -- the demo's race code\n"
     "    course TEXT,               -- its course's code\n"
     "    shop_car TEXT,             -- a car code shown in the demo, empty for none\n"
     "    shop_car_position INTEGER, -- that car's place\n"
     "    requirement INTEGER        -- shown once the licences reach it\n"
     ")"},
    {CARD_DEMOS, "card_demos",
     "CREATE TABLE card_demos (  -- the memory card's demo replays (section 0xD)\n"
     "    position INTEGER, -- the order\n"
     "    demo TEXT,        -- its name\n"
     "    level INTEGER     -- shown once the licences reach it\n"
     ")"},
    {PRIZES, "prizes",
     "CREATE TABLE prizes (  -- the prize car sets (section 0xF; the game sorts the sets by name)\n"
     "    position INTEGER, -- the order: a set's cars in it (at most 4 a set)\n"
     "    prize_set TEXT,   -- the set's name: what it is won for\n"
     "    car TEXT,         -- the car's code\n"
     "    color INTEGER,    -- its colour (-1: any)\n"
     "    weight INTEGER,   -- its chance against the set's others\n"
     "    unknown INTEGER   -- (not read by the game)\n"
     ")"},
};

const Schema *schema_of(int section)
{
    for (const Schema &s : kSchema)
        if (s.section == section)
            return &s;
    return nullptr;
}

/* A statement's columns by name (any order, any case) and the row's values
 * as the game takes them. */
struct Rows {
    sqlite3_stmt *st = nullptr;
    std::string table;
    std::vector<std::string> *warnings;
    int row = 0;

    int column(const char *name) const
    {
        const int n = sqlite3_column_count(st);
        for (int i = 0; i < n; i++) {
            const char *c = sqlite3_column_name(st, i);
            if (c != nullptr && _stricmp(c, name) == 0)
                return i;
        }
        return -1;
    }
    void warn(const std::string &what) { warnings->push_back(table + " row " + std::to_string(row) + ": " + what); }
    std::string text(int col) const
    {
        if (col < 0 || sqlite3_column_type(st, col) == SQLITE_NULL)
            return std::string();
        const unsigned char *t = sqlite3_column_text(st, col);
        std::string s = t != nullptr ? std::string((const char *)t) : std::string();
        const size_t a = s.find_first_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t") - a + 1);
    }
    /* a whole number (empty: 0); anything else 0, warned - the row is kept
     * (the game finds a service, an event... by name without a check: a row
     * left out could crash it).  Always true. */
    bool number(int col, const char *name, int *v)
    {
        *v = 0;
        if (col < 0)
            return true;
        switch (sqlite3_column_type(st, col)) {
        case SQLITE_NULL:
            return true;
        case SQLITE_INTEGER: {
            const sqlite3_int64 x = sqlite3_column_int64(st, col);
            if (x >= -2147483648ll && x <= 2147483647ll) {
                *v = (int)x;
                return true;
            }
            break;
        }
        case SQLITE_FLOAT: {
            const double x = sqlite3_column_double(st, col);
            if (x == (double)(long long)x && x >= -2147483648.0 && x <= 2147483647.0) {
                *v = (int)x;
                return true;
            }
            break;
        }
        case SQLITE_TEXT: {
            const std::string t = text(col);
            if (t.empty())
                return true;
            char *e = nullptr;
            const long long x = std::strtoll(t.c_str(), &e, 10);
            if (*e == 0 && x >= -2147483648ll && x <= 2147483647ll) {
                *v = (int)x;
                return true;
            }
            break;
        }
        }
        warn(std::string(name) + " \"" + text(col) + "\" is not a whole number: 0 taken");
        *v = 0;
        return true;
    }
};

bool exec(sqlite3 *db, const std::string &sql, std::string *error)
{
    char *msg = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &msg) == SQLITE_OK)
        return true;
    *error = msg != nullptr ? msg : sqlite3_errmsg(db);
    sqlite3_free(msg);
    return false;
}

/* Inserts a row: the values bound in order (text or whole numbers). */
struct Value {
    bool isText;
    std::string text;
    long long number;
    Value(const std::string &s) : isText(true), text(s), number(0) {}
    Value(int n) : isText(false), number(n) {}
    Value(size_t n) : isText(false), number((long long)n) {}
};
bool insert(sqlite3_stmt *st, const std::vector<Value> &values)
{
    sqlite3_reset(st);
    for (size_t i = 0; i < values.size(); i++) {
        const Value &v = values[i];
        if (v.isText)
            sqlite3_bind_text(st, (int)i + 1, v.text.c_str(), (int)v.text.size(), SQLITE_TRANSIENT);
        else
            sqlite3_bind_int64(st, (int)i + 1, v.number);
    }
    return sqlite3_step(st) == SQLITE_DONE;
}

} // namespace

bool keep_file(const std::vector<uint8_t> &gcf, const std::string &path, std::string *error)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        *error = db != nullptr ? sqlite3_errmsg(db) : "the database cannot be opened";
        sqlite3_close(db);
        return false;
    }
    bool ok = exec(db,
                   "BEGIN; DROP TABLE IF EXISTS gcf_file; "
                   "CREATE TABLE gcf_file (  -- the .gcf this database was made of, whole: it has sections no table "
                   "holds, which nothing in the game reads\n"
                   "    file BLOB                           -- its bytes\n)",
                   error);
    sqlite3_stmt *st = nullptr;
    if (ok && sqlite3_prepare_v2(db, "INSERT INTO gcf_file VALUES (?)", -1, &st, nullptr) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        ok = false;
    }
    if (ok) {
        sqlite3_bind_blob(st, 1, gcf.data(), (int)gcf.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            *error = sqlite3_errmsg(db);
            ok = false;
        }
    }
    sqlite3_finalize(st);
    ok = ok && exec(db, "COMMIT", error);
    if (!ok)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    if (sqlite3_close(db) != SQLITE_OK && ok) {
        *error = "the database could not be closed";
        ok = false;
    }
    return ok;
}

bool Config::has(int section) const
{
    return std::find(sections.begin(), sections.end(), section) != sections.end();
}

void Config::take(const Config &from, int section)
{
    if (!from.has(section))
        return;
    switch (section) {
    case GENERAL:
        idleSeconds = from.idleSeconds;
        demoTimeLimit = from.demoTimeLimit;
        break;
    case CARS: cars = from.cars; break;
    case COURSES: courses = from.courses; break;
    case EVENTS: events = from.events; break;
    case BEST_TIMES: bestTimes = from.bestTimes; break;
    case SERVICES: services = from.services; break;
    case DEMOS: demos = from.demos; break;
    case CARD_DEMOS: cardDemos = from.cardDemos; break;
    case PRIZES: prizes = from.prizes; break;
    default: return;
    }
    if (!has(section))
        sections.push_back(section);
}

const char *table_name(int section)
{
    const Schema *s = schema_of(section);
    return s != nullptr ? s->name : "";
}

bool read_gcf(const uint8_t *data, size_t size, Config *out, std::string *error, std::vector<std::string> *warnings)
{
    In in{data, size};
    Config c;
    const int32_t count = in.i32(0);
    if (!in.ok || count <= 0 || count > 64) {
        *error = "not a product config (its header has no sections)";
        return false;
    }
    const size_t table = in.at(0, 4);
    for (int32_t s = 0; s < count && in.ok; s++) {
        const int type = in.i32(table + 8 * (size_t)s);
        const size_t d = in.at(0, table + 8 * (size_t)s + 4);
        switch (type) {
        case GENERAL:
            c.idleSeconds = in.i32(d);
            c.demoTimeLimit = in.i32(d + 4);
            break;
        case CARS:
            in.list(d, d, d + 4, [&](size_t cat) {
                const std::string name = in.str(in.at(cat, cat));
                in.list(cat, cat + 4, cat + 8, [&](size_t car) {
                    Car k;
                    k.category = name;
                    k.code = in.str(in.at(car, car));
                    k.race = in.str(in.at(car, car + 4));
                    k.selectable = in.i16(car + 8);
                    k.level = in.i16(car + 0xA);
                    k.raceStep = in.i32(car + 0xC);
                    c.cars.push_back(k);
                });
            });
            break;
        case COURSES:
            in.list(d, d, d + 4, [&](size_t list) {
                const std::string name = in.str(in.at(list, list));
                in.list(list, list + 4, list + 8, [&](size_t co) {
                    Course k;
                    k.list = name;
                    k.name = in.str(in.at(co, co));
                    k.available = in.i8(co + 4);
                    k.flag = in.i8(co + 5);
                    k.level = in.i16(co + 6);
                    k.length = in.i32(co + 8);
                    c.courses.push_back(k);
                });
            });
            break;
        case EVENTS:
        case BEST_TIMES:
        case SERVICES:
        case CARD_DEMOS: {
            std::vector<Named> &v = type == EVENTS ? c.events : type == BEST_TIMES ? c.bestTimes
                                                             : type == SERVICES   ? c.services
                                                                                  : c.cardDemos;
            in.list(d, d, d + 4, [&](size_t e) { v.push_back(Named{in.str(in.at(e, e)), in.i32(e + 4)}); });
            break;
        }
        case DEMOS:
            in.list(d, d, d + 4, [&](size_t e) {
                Demo k;
                k.race = in.str(in.at(e, e));
                k.course = in.str(in.at(e, e + 4));
                k.shopCar = in.str(in.at(e, e + 8));
                k.shopCarPos = in.i32(e + 0xC);
                k.requirement = in.i32(e + 0x10);
                c.demos.push_back(k);
            });
            break;
        case PRIZES:
            in.list(d, d, d + 4, [&](size_t set) {
                const std::string name = in.str(in.at(set, set));
                in.list(set, set + 4, set + 8, [&](size_t p) {
                    PrizeCar k;
                    k.set = name;
                    k.unknown = in.i32(p);
                    k.code = in.str(in.at(p, p + 4));
                    k.color = in.i32(p + 8);
                    k.weight = in.i32(p + 0xC);
                    c.prizes.push_back(k);
                });
            });
            break;
        default:
            warnings->push_back("section type " + std::to_string(type) + ": not one the game reads (left out)");
            continue;
        }
        c.sections.push_back(type);
    }
    if (!in.ok) {
        *error = "a part of it points outside the file";
        return false;
    }
    *out = std::move(c);
    return true;
}

bool write_database(const Config &c, const std::string &path, std::string *error)
{
    std::remove(path.c_str());
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        *error = db != nullptr ? sqlite3_errmsg(db) : "the database could not be made";
        sqlite3_close(db);
        return false;
    }
    /* (user_version 1: this layout) */
    bool ok = exec(db, "PRAGMA journal_mode = OFF; PRAGMA user_version = 1; BEGIN", error);
    for (const Schema &s : kSchema) {
        if (!ok || !c.has(s.section))
            continue;
        ok = exec(db, s.create, error);
        if (!ok)
            break;
        /* its rows */
        std::string sql;
        switch (s.section) {
        case GENERAL: sql = "INSERT INTO general VALUES (?, ?)"; break;
        case CARS: sql = "INSERT INTO cars VALUES (?, ?, ?, ?, ?, ?, ?)"; break;
        case COURSES: sql = "INSERT INTO courses VALUES (?, ?, ?, ?, ?, ?, ?)"; break;
        case DEMOS: sql = "INSERT INTO demos VALUES (?, ?, ?, ?, ?, ?)"; break;
        case CARD_DEMOS: sql = "INSERT INTO card_demos VALUES (?, ?, ?)"; break;
        case PRIZES: sql = "INSERT INTO prizes VALUES (?, ?, ?, ?, ?, ?)"; break;
        default: sql = std::string("INSERT INTO ") + s.name + " VALUES (?, ?)"; break;
        }
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            *error = sqlite3_errmsg(db);
            ok = false;
            break;
        }
        switch (s.section) {
        case GENERAL:
            ok = insert(st, {std::string("idle_seconds"), c.idleSeconds}) &&
                 insert(st, {std::string("demo_time_limit"), c.demoTimeLimit});
            break;
        case CARS:
            for (size_t i = 0; ok && i < c.cars.size(); i++) {
                const Car &k = c.cars[i];
                ok = insert(st, {i + 1, k.category, k.code, k.race, k.selectable, k.level, k.raceStep});
            }
            break;
        case COURSES:
            for (size_t i = 0; ok && i < c.courses.size(); i++) {
                const Course &k = c.courses[i];
                ok = insert(st, {i + 1, k.list, k.name, k.available, k.flag, k.level, k.length});
            }
            break;
        case EVENTS:
        case BEST_TIMES:
        case SERVICES: {
            const std::vector<Named> &v = s.section == EVENTS ? c.events : s.section == BEST_TIMES ? c.bestTimes
                                                                                                  : c.services;
            for (size_t i = 0; ok && i < v.size(); i++)
                ok = insert(st, {v[i].name, v[i].value});
            break;
        }
        case DEMOS:
            for (size_t i = 0; ok && i < c.demos.size(); i++) {
                const Demo &k = c.demos[i];
                ok = insert(st, {i + 1, k.race, k.course, k.shopCar, k.shopCarPos, k.requirement});
            }
            break;
        case CARD_DEMOS:
            for (size_t i = 0; ok && i < c.cardDemos.size(); i++)
                ok = insert(st, {i + 1, c.cardDemos[i].name, c.cardDemos[i].value});
            break;
        case PRIZES:
            for (size_t i = 0; ok && i < c.prizes.size(); i++) {
                const PrizeCar &k = c.prizes[i];
                ok = insert(st, {i + 1, k.set, k.code, k.color, k.weight, k.unknown});
            }
            break;
        }
        if (!ok)
            *error = std::string(s.name) + ": " + sqlite3_errmsg(db);
        sqlite3_finalize(st);
    }
    ok = ok && exec(db, "COMMIT", error);
    if (sqlite3_close(db) != SQLITE_OK && ok) {
        *error = "the database could not be closed";
        ok = false;
    }
    if (!ok)
        std::remove(path.c_str());
    return ok;
}

bool read_database(const void *bytes, size_t size, Config *out, std::vector<std::string> *warnings,
                   std::string *error)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        *error = "SQLite does not start";
        sqlite3_close(db);
        return false;
    }
    unsigned char *copy = (unsigned char *)sqlite3_malloc64(size);
    if (copy == nullptr) {
        sqlite3_close(db);
        *error = "no memory for it";
        return false;
    }
    std::memcpy(copy, bytes, size);
    if (sqlite3_deserialize(db, "main", copy, (sqlite3_int64)size, (sqlite3_int64)size,
                            SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY) != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return false;
    }
    /* the tables it has */
    std::vector<std::string> tables;
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table'", -1, &st, nullptr) !=
            SQLITE_OK) {
            *error = std::string("not an SQLite database (") + sqlite3_errmsg(db) + ")";
            sqlite3_close(db);
            return false;
        }
        while (sqlite3_step(st) == SQLITE_ROW)
            tables.push_back(lower_copy((const char *)sqlite3_column_text(st, 0)));
        sqlite3_finalize(st);
    }
    Config c;
    for (const Schema &s : kSchema) {
        if (std::find(tables.begin(), tables.end(), s.name) == tables.end())
            continue;
        c.sections.push_back(s.section);
        Rows r;
        r.table = s.name;
        r.warnings = warnings;
        /* the ordered tables by position (then their rows' order), if they have one */
        bool hasPosition = false;
        {
            sqlite3_stmt *probe = nullptr;
            if (sqlite3_prepare_v2(db, (std::string("SELECT * FROM \"") + s.name + "\" LIMIT 0").c_str(), -1, &probe,
                                   nullptr) == SQLITE_OK) {
                r.st = probe;
                hasPosition = r.column("position") >= 0;
                sqlite3_finalize(probe);
                r.st = nullptr;
            }
        }
        const std::string sql = std::string("SELECT * FROM \"") + s.name + "\"" +
                                (hasPosition ? " ORDER BY position, rowid" : " ORDER BY rowid");
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &r.st, nullptr) != SQLITE_OK &&
            sqlite3_prepare_v2(db, (std::string("SELECT * FROM \"") + s.name + "\"").c_str(), -1, &r.st, nullptr) !=
                SQLITE_OK) {
            warnings->push_back(std::string(s.name) + ": " + sqlite3_errmsg(db) + " (left out)");
            c.sections.pop_back();
            continue;
        }
        const bool ordered = s.section == CARS || s.section == COURSES || s.section == DEMOS ||
                             s.section == CARD_DEMOS || s.section == PRIZES;
        if (ordered && !hasPosition)
            warnings->push_back(std::string(s.name) + ": no position column (the rows in the table's order)");
        auto need = [&](const char *col) {
            if (r.column(col) < 0)
                warnings->push_back(std::string(s.name) + ": no \"" + col + "\" column (its values taken as 0 / empty)");
            return r.column(col);
        };
        switch (s.section) {
        case GENERAL: {
            const int set = need("setting"), val = need("value");
            while (sqlite3_step(r.st) == SQLITE_ROW) {
                r.row++;
                const std::string key = lower_copy(r.text(set).c_str());
                int v = 0;
                if (!r.number(val, "value", &v))
                    continue;
                if (key == "idle_seconds")
                    c.idleSeconds = v;
                else if (key == "demo_time_limit")
                    c.demoTimeLimit = v;
                else
                    r.warn("\"" + key + "\" is not a setting (idle_seconds, demo_time_limit): skipped");
            }
            break;
        }
        case CARS: {
            const int cat = need("category"), car = need("car"), race = need("race"), sel = need("selectable"),
                      lev = need("level"), step = need("race_step");
            while (sqlite3_step(r.st) == SQLITE_ROW) {
                r.row++;
                Car k;
                k.category = r.text(cat);
                k.code = r.text(car);
                k.race = r.text(race);
                if (k.category.empty() || k.code.empty()) {
                    r.warn("a car without its category or code: skipped");
                    continue;
                }
                if (r.number(sel, "selectable", &k.selectable) && r.number(lev, "level", &k.level) &&
                    r.number(step, "race_step", &k.raceStep))
                    c.cars.push_back(k);
            }
            break;
        }
        case COURSES: {
            const int list = need("list"), course = need("course"), av = need("available"), flag = need("flag"),
                      lev = need("level"), len = need("length");
            while (sqlite3_step(r.st) == SQLITE_ROW) {
                r.row++;
                Course k;
                k.list = r.text(list);
                k.name = r.text(course);
                if (k.list.empty() || k.name.empty()) {
                    r.warn("a course without its list or code: skipped");
                    continue;
                }
                if (r.number(av, "available", &k.available) && r.number(flag, "flag", &k.flag) &&
                    r.number(lev, "level", &k.level) && r.number(len, "length", &k.length))
                    c.courses.push_back(k);
            }
            break;
        }
        case EVENTS:
        case BEST_TIMES:
        case SERVICES:
        case CARD_DEMOS: {
            const char *nameCol = s.section == EVENTS ? "event" : s.section == BEST_TIMES ? "course"
                                                              : s.section == SERVICES   ? "service"
                                                                                        : "demo";
            const char *valueCol = s.section == EVENTS ? "requirement" : s.section == BEST_TIMES ? "value"
                                                                     : s.section == SERVICES   ? "price"
                                                                                               : "level";
            std::vector<Named> &v = s.section == EVENTS ? c.events : s.section == BEST_TIMES ? c.bestTimes
                                                                   : s.section == SERVICES   ? c.services
                                                                                             : c.cardDemos;
            const int nc = need(nameCol), vc = need(valueCol);
            while (sqlite3_step(r.st) == SQLITE_ROW) {
                r.row++;
                Named k;
                k.name = r.text(nc);
                if (k.name.empty()) {
                    r.warn(std::string("a row without its ") + nameCol + ": skipped");
                    continue;
                }
                if (s.section != CARD_DEMOS &&
                    std::any_of(v.begin(), v.end(), [&](const Named &o) { return o.name == k.name; })) {
                    r.warn("\"" + k.name + "\" again: skipped (the first one kept)");
                    continue;
                }
                if (r.number(vc, valueCol, &k.value))
                    v.push_back(k);
            }
            break;
        }
        case DEMOS: {
            const int race = need("race"), course = need("course"), shop = need("shop_car"),
                      pos = need("shop_car_position"), req = need("requirement");
            while (sqlite3_step(r.st) == SQLITE_ROW) {
                r.row++;
                Demo k;
                k.race = r.text(race);
                k.course = r.text(course);
                k.shopCar = r.text(shop);
                if (k.race.empty() || k.course.empty()) {
                    r.warn("a demo without its race or course: skipped");
                    continue;
                }
                if (r.number(pos, "shop_car_position", &k.shopCarPos) && r.number(req, "requirement", &k.requirement))
                    c.demos.push_back(k);
            }
            break;
        }
        case PRIZES: {
            const int set = need("prize_set"), car = need("car"), col = need("color"), w = need("weight"),
                      unk = need("unknown");
            while (sqlite3_step(r.st) == SQLITE_ROW) {
                r.row++;
                PrizeCar k;
                k.set = r.text(set);
                k.code = r.text(car);
                if (k.set.empty() || k.code.empty()) {
                    r.warn("a prize car without its set or car: skipped");
                    continue;
                }
                if (r.number(col, "color", &k.color) && r.number(w, "weight", &k.weight) &&
                    r.number(unk, "unknown", &k.unknown))
                    c.prizes.push_back(k);
            }
            break;
        }
        }
        sqlite3_finalize(r.st);
    }
    sqlite3_close(db);
    if (c.sections.empty()) {
        *error = "it has none of the config's tables";
        return false;
    }
    *out = std::move(c);
    return true;
}

std::vector<uint8_t> image(const Config &c)
{
    Out o;
    const size_t hdr = o.alloc(8);
    const size_t table = o.alloc(8 * c.sections.size());
    o.i32(hdr, (int32_t)c.sections.size());
    o.rel(hdr, hdr + 4, table);
    /* rows grouped by a key in the order the keys first come (cars by
     * category, courses by list); the sorted tables by name */
    auto groups = [](const auto &rows, auto key) {
        std::vector<std::string> names;
        for (const auto &r : rows)
            if (std::find(names.begin(), names.end(), key(r)) == names.end())
                names.push_back(key(r));
        return names;
    };
    auto sorted = [](std::vector<Named> v) {
        std::stable_sort(v.begin(), v.end(), [](const Named &a, const Named &b) { return name_less(a.name, b.name); });
        return v;
    };
    for (size_t s = 0; s < c.sections.size(); s++) {
        const int type = c.sections[s];
        o.i32(table + 8 * s, type);
        size_t d = 0;
        switch (type) {
        case GENERAL:
            d = o.alloc(8);
            o.i32(d, c.idleSeconds);
            o.i32(d + 4, c.demoTimeLimit);
            break;
        case CARS: {
            const std::vector<std::string> cats = groups(c.cars, [](const Car &k) { return k.category; });
            d = o.alloc(8);
            o.list(d, d, d + 4, cats.size(), [&](size_t i) {
                std::vector<const Car *> cars;
                for (const Car &k : c.cars)
                    if (k.category == cats[i])
                        cars.push_back(&k);
                const size_t cat = o.alloc(12);
                o.rel(cat, cat, o.str(cats[i]));
                o.list(cat, cat + 4, cat + 8, cars.size(), [&](size_t j) {
                    const size_t e = o.alloc(16);
                    o.rel(e, e, o.str(cars[j]->code));
                    o.rel(e, e + 4, o.str(cars[j]->race));
                    o.i16(e + 8, (int16_t)cars[j]->selectable);
                    o.i16(e + 0xA, (int16_t)cars[j]->level);
                    o.i32(e + 0xC, cars[j]->raceStep);
                    return e;
                });
                return cat;
            });
            break;
        }
        case COURSES: {
            const std::vector<std::string> lists = groups(c.courses, [](const Course &k) { return k.list; });
            d = o.alloc(8);
            o.list(d, d, d + 4, lists.size(), [&](size_t i) {
                std::vector<const Course *> courses;
                for (const Course &k : c.courses)
                    if (k.list == lists[i])
                        courses.push_back(&k);
                const size_t list = o.alloc(12);
                o.rel(list, list, o.str(lists[i]));
                o.list(list, list + 4, list + 8, courses.size(), [&](size_t j) {
                    const size_t e = o.alloc(12);
                    o.rel(e, e, o.str(courses[j]->name));
                    o.i8(e + 4, (int8_t)courses[j]->available);
                    o.i8(e + 5, (int8_t)courses[j]->flag);
                    o.i16(e + 6, (int16_t)courses[j]->level);
                    o.i32(e + 8, courses[j]->length);
                    return e;
                });
                return list;
            });
            break;
        }
        case EVENTS:
        case BEST_TIMES:
        case SERVICES:
        case CARD_DEMOS: {
            const std::vector<Named> v = type == EVENTS       ? sorted(c.events)
                                         : type == BEST_TIMES ? sorted(c.bestTimes)
                                         : type == SERVICES   ? sorted(c.services)
                                                              : c.cardDemos;
            d = o.alloc(8);
            o.list(d, d, d + 4, v.size(), [&](size_t i) {
                const size_t e = o.alloc(8);
                o.rel(e, e, o.str(v[i].name));
                o.i32(e + 4, v[i].value);
                return e;
            });
            break;
        }
        case DEMOS:
            d = o.alloc(8);
            o.list(d, d, d + 4, c.demos.size(), [&](size_t i) {
                const Demo &k = c.demos[i];
                const size_t e = o.alloc(20);
                o.rel(e, e, o.str(k.race));
                o.rel(e, e + 4, o.str(k.course));
                o.rel(e, e + 8, o.str(k.shopCar));
                o.i32(e + 0xC, k.shopCarPos);
                o.i32(e + 0x10, k.requirement);
                return e;
            });
            break;
        case PRIZES: {
            std::vector<std::string> sets = groups(c.prizes, [](const PrizeCar &k) { return k.set; });
            std::stable_sort(sets.begin(), sets.end(), name_less);
            d = o.alloc(8);
            o.list(d, d, d + 4, sets.size(), [&](size_t i) {
                std::vector<const PrizeCar *> cars;
                for (const PrizeCar &k : c.prizes)
                    if (k.set == sets[i])
                        cars.push_back(&k);
                const size_t set = o.alloc(12);
                o.rel(set, set, o.str(sets[i]));
                o.list(set, set + 4, set + 8, cars.size(), [&](size_t j) {
                    const size_t e = o.alloc(16);
                    o.i32(e, cars[j]->unknown);
                    o.rel(e, e + 4, o.str(cars[j]->code));
                    o.i32(e + 8, cars[j]->color);
                    o.i32(e + 0xC, cars[j]->weight);
                    return e;
                });
                return set;
            });
            break;
        }
        }
        o.rel(hdr, table + 8 * s + 4, d);
    }
    return o.b;
}

} // namespace gcftables
