/* memcard.cpp - the PS2 memory cards of GT3 PC (memcard.h). */
#include "platform/memcard.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/save_folders.h"             /* PC (2026-11-15): the save folders */
#include "platform/os/fs.h"
#include "platform/os/win_compat.h"
#include "sifdev.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace memcard {
namespace {

constexpr int kTotalClusters = 8000;        /* an 8 MB card as libmc reports it, 1 KB clusters */
constexpr int kMaxFds = 16;
constexpr unsigned short kAttrDir = 0x8427;  /* exists | subdir | rwx (as cards show them) */
constexpr unsigned short kAttrFile = 0x8497; /* exists | closed | file | rwx */
constexpr int64_t kJstOffset = 9 * 3600;

std::mutex g_mutex;

struct Fd {
    bool   used;
    bool   writer;                  /* opened for writing (busy) */
    fs::File file;
    int    port;
};
Fd g_fds[kMaxFds];
/* GetTickCount64 of the last change to a card (busy); 0 none yet */
ULONGLONG g_lastChange;
constexpr ULONGLONG kBusyMs = 2000;

void changed()
{
    g_lastChange = GetTickCount64();
}

struct Listing {
    std::vector<sceMcTblGetDir> entries;
    size_t next = 0;
};
Listing g_listing[2];
/* The card has been reported since start-up (libmc answers sceMcGetInfo
 * with -1, "a new formatted card", once per card it finds; then 0, "the
 * same card"). */
bool g_reported[2];

/* PC (2026-11-15): the save folder in each slot (save_folders.h; memcard.h
 * set_slot_folder) - its name ("" no card) and path with its '\' - and a
 * change waiting for the slot's open files (wanted). */
struct Slot {
    std::string name, root;
    bool wanted = false;
    std::string wantedName;
    /* changed since the game's last sceMcGetInfo of the slot: its other
     * calls answer -1 (sceMcResChangedCard), as libmc's after a swap */
    bool swapped = false;
};
Slot g_slot[2];
std::string g_savesDir;                         /* saves\<engine>\ (ANSI, with its '\'; fs.h native, kSep) */
std::vector<std::string> g_folders;             /* folders(): the last read, and when (GetTickCount64) */
ULONGLONG g_foldersAt;

const std::string &card_root(int port)
{
    return g_slot[port].root;
}

bool valid_card(int port, int slot)
{
    return (port == 0 || port == 1) && slot == 0;
}

/* PC (2026-11-15): a slot's chosen folder put in once the card has no file
 * open (the lock held). */
void apply_wanted(int port)
{
    Slot &s = g_slot[port];
    if (!s.wanted)
        return;
    for (const Fd &f : g_fds)
        if (f.used && f.port == port)
            return;
    s.wanted = false;
    s.name = s.wantedName;
    s.root = s.name.empty() ? std::string() : g_savesDir + s.name + fs::kSep;
    s.swapped = true;
    g_reported[port] = false;
    g_listing[port] = Listing();
    pclog::write("memcard: slot %d: %s", port + 1, s.name.empty() ? "no card" : s.root.c_str());
}

bool is_dir(const std::string &p);

/* PC (2026-11-15): the slot holds a card - a folder there. */
bool card_in(int port)
{
    const std::string &r = card_root(port);
    return !r.empty() && is_dir(r.substr(0, r.size() - 1));
}

/* PC (2026-11-15): the start of a card call (the lock held): 0 when the slot's
 * card can take it, else libmc's answer - no card (-10), or -1 after a swap
 * the game has not asked about yet. */
int card_check(int port, int slot)
{
    if (!valid_card(port, slot))
        return -10;
    apply_wanted(port);
    if (!card_in(port))
        return -10;
    if (g_slot[port].swapped)
        return sceMcResChangedCard;
    return 0;
}

std::string card_path(int port, const char *name)
{
    std::string p = name ? name : "";
    while (!p.empty() && (p[0] == '/' || p[0] == '\\'))
        p.erase(0, 1);
    for (char &c : p)
        if (c == '/' || c == '\\')
            c = fs::kSep;
    return card_root(port) + p;
}

/* (the native text: fs.h) */
bool is_dir(const std::string &p)
{
    return fs::is_dir(fs::native_to_utf8(p));
}

bool exists(const std::string &p)
{
    return fs::exists(fs::native_to_utf8(p));
}

std::string parent_of(const std::string &p)
{
    size_t s = p.find_last_of(fs::kSep);
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

/* Days since 1970-01-01 of a date of the proleptic Gregorian calendar, and
 * back (H. Hinnant's days_from_civil, civil_from_days). */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int64_t)yoe + era * 400 + (*m <= 2);
}

/* A file time as the card's date (Japan time, as the PS2's clock): what
 * FileTimeToSystemTime gives for it moved by 9 hours. */
void card_date(fs::Time ft, sceMcStDateTime *d)
{
    const int64_t s = fs::to_unix(ft) + kJstOffset;
    const int64_t days = (s >= 0 ? s : s - 86399) / 86400;
    const int64_t sec = s - days * 86400;
    int64_t y;
    unsigned m, day;
    civil_from_days(days, &y, &m, &day);
    d->Resv2 = 0;
    d->Sec = (unsigned char)(sec % 60);
    d->Min = (unsigned char)(sec / 60 % 60);
    d->Hour = (unsigned char)(sec / 3600);
    d->Day = (unsigned char)day;
    d->Month = (unsigned char)m;
    d->Year = (unsigned short)y;
}

/* The card's date as a file time; false when it is no date (as
 * SystemTimeToFileTime: the year 1601..30827, a month's day, 0..23:59:59). */
bool card_filetime(const sceMcStDateTime *d, fs::Time *out)
{
    static const unsigned char kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const unsigned y = d->Year, m = d->Month, day = d->Day;
    if (y < 1601 || y > 30827 || m < 1 || m > 12 || d->Hour > 23 || d->Min > 59 || d->Sec > 59)
        return false;
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (day < 1 || day > kDays[m - 1] + (m == 2 && leap ? 1u : 0u))
        return false;
    const int64_t s = days_from_civil(y, m, day) * 86400 + d->Hour * 3600 + d->Min * 60 + d->Sec;
    *out = fs::from_unix(s - kJstOffset);
    return true;
}

bool match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (;; s++) {
                if (match(pat + 1, s))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (!*s)
            return false;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s))
            return false;
    }
    return *s == 0;
}

/* dir's entries (the native text: fs.h), their names the ANSI code page's;
 * false when it cannot be listed. */
bool list_entries(const std::string &dir, std::vector<fs::Entry> *out)
{
    if (!fs::list(fs::native_to_utf8(dir), out))
        return false;
    for (fs::Entry &e : *out)
        e.name = fs::utf8_to_native(e.name);
    return true;
}

int count_entries(const std::string &dir)
{
    int n = 2;                                  /* "." and ".." */
    std::vector<fs::Entry> entries;
    if (list_entries(dir, &entries))
        n += (int)entries.size();
    return n;
}

/* Clusters used below dir (files in 1 KB clusters, directories two entries
 * per cluster). */
int used_clusters(const std::string &dir)
{
    int used = (count_entries(dir) + 1) / 2;
    std::vector<fs::Entry> entries;
    if (!list_entries(dir, &entries))
        return used;
    for (const fs::Entry &e : entries) {
        if (e.st.dir)
            used += used_clusters(dir + fs::kSep + e.name);
        else
            used += (int)((e.st.size + 1023) / 1024);
    }
    return used;
}

int free_clusters(int port)
{
    std::string root = card_root(port);
    root.pop_back();
    int f = kTotalClusters - used_clusters(root);
    return f < 0 ? 0 : f;
}

void fill_entry(sceMcTblGetDir *e, const char *name, const fs::Stat *st, int size, bool dir)
{
    memset(e, 0, sizeof(*e));
    if (st) {
        card_date(st->create, &e->_Create);
        card_date(st->modify, &e->_Modify);
    } else {
        card_date(fs::now(), &e->_Create);
        e->_Modify = e->_Create;
    }
    e->FileSizeByte = (unsigned int)size;
    e->AttrFile = dir ? kAttrDir : kAttrFile;
    strncpy((char *)e->EntryName, name, sizeof(e->EntryName) - 1);
}

void delete_tree(const std::string &dir, bool removeSelf)
{
    std::vector<fs::Entry> entries;
    if (list_entries(dir, &entries)) {
        for (const fs::Entry &e : entries) {
            std::string p = dir + fs::kSep + e.name;
            if (e.st.dir)
                delete_tree(p, true);
            else
                fs::remove_file(fs::native_to_utf8(p));
        }
    }
    if (removeSelf)
        fs::remove_dir(fs::native_to_utf8(dir));
}

} // namespace

/* PC (2026-11-15): the cards are the save folders gt3pc_engine.ini [Saves]
 * names (save_folders.h); nothing is made here (earlier: memcard1\ and
 * memcard2\ of the game folder, made when missing). */
void init()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_savesDir = paths::saves_dir();
    const std::wstring ini = save_folders::from_ansi(paths::engine_ini_path());
    pclog::write("memcard: save folders %s (%s [Saves])", g_savesDir.c_str(), paths::engine_ini_path().c_str());
    for (int port = 0; port < 2; port++) {
        std::string name = save_folders::to_ansi(save_folders::slot(ini, port));
        if (port == 1 && !name.empty() && _stricmp(name.c_str(), g_slot[0].name.c_str()) == 0) {
            pclog::write("memcard: slot 2: %s is slot 1's folder: no card", name.c_str());
            name.clear();
        }
        Slot &s = g_slot[port];
        s.name = name;
        s.root = name.empty() ? std::string() : g_savesDir + name + fs::kSep;
        if (name.empty())
            pclog::write("memcard: slot %d: no card", port + 1);
        else
            pclog::write("memcard: slot %d: %s%s", port + 1, s.root.c_str(),
                         card_in(port) ? "" : " (not there: no card)");
    }
}

int get_slot_max(int port)
{
    return (port == 0 || port == 1) ? 1 : 0;
}

int get_info(int port, int slot, int *type, int *freeClusters, int *format)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot)) {
        if (type) *type = sceMcTypeNoCard;
        return -10;
    }
    /* PC (2026-11-15): a save folder (none, or not there: no card; reported
     * as a new card when one is put in) */
    apply_wanted(port);
    g_slot[port].swapped = false;
    if (!card_in(port)) {
        if (type) *type = sceMcTypeNoCard;
        if (freeClusters) *freeClusters = 0;
        if (format) *format = 0;
        g_reported[port] = false;
        return -10;
    }
    if (type) *type = sceMcTypePS2;                         /* (a folder is a formatted card) */
    if (freeClusters) *freeClusters = free_clusters(port);
    if (format) *format = 1;
    /* The first answer is "a new card": the game's card manager (T501
     * mc_slot_poll_info) reads the free space and the format only then; a
     * plain 0 at start left its slot unformatted until it formatted the
     * card itself (which empties the folder). */
    if (!g_reported[port]) {
        g_reported[port] = true;
        return sceMcResChangedCard;
    }
    return sceMcResSucceed;
}

int open(int port, int slot, const char *name, int mode)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const int res = card_check(port, slot))             /* PC (2026-11-15) */
        return res;
    std::string p = card_path(port, name);
    if (mode & sceMcFileCreateDir) {
        if (exists(p))
            return sceMcResNoEntry;
        changed();
        return fs::make_dir(fs::native_to_utf8(p)) ? sceMcResSucceed : sceMcResNoEntry;
    }
    int fd = -1;
    for (int i = 0; i < kMaxFds; i++)
        if (!g_fds[i].used) {
            fd = i;
            break;
        }
    if (fd < 0)
        return sceMcResUpLimitHandle;
    if (is_dir(p))
        return sceMcResNoEntry;
    unsigned access = 0;
    int rw = mode & 3;
    if (rw == SCE_RDONLY || rw == SCE_RDWR) access |= fs::kRead;
    if (rw == SCE_WRONLY || rw == SCE_RDWR) access |= fs::kWrite;
    fs::Create disp = fs::Create::open_existing;
    if (mode & SCE_CREAT)
        disp = (mode & SCE_TRUNC) ? fs::Create::create_always : fs::Create::open_always;
    else if (mode & SCE_TRUNC)
        disp = fs::Create::truncate_existing;
    if ((mode & SCE_CREAT) && !is_dir(parent_of(p)))
        return sceMcResNoEntry;
    fs::File f;
    if (!f.open(fs::native_to_utf8(p), access, disp, fs::kShareRead))
        return sceMcResNoEntry;
    g_fds[fd].used = true;
    g_fds[fd].writer = (access & fs::kWrite) != 0;
    if (g_fds[fd].writer)
        changed();
    g_fds[fd].file = std::move(f);
    g_fds[fd].port = port;
    return fd;
}

int close(int fd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    g_fds[fd].file.close();
    if (g_fds[fd].writer)
        changed();
    g_fds[fd].used = false;
    return sceMcResSucceed;
}

int seek(int fd, int offset, int origin)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    const int how = origin == SCE_SEEK_END ? 2 : origin == SCE_SEEK_CUR ? 1 : 0;
    const int64_t pos = g_fds[fd].file.seek(offset, how);
    return pos < 0 || pos >= 0xFFFFFFFFLL ? sceMcResDeniedPermit : (int)(uint32_t)pos;
}

int read(int fd, void *buf, int size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    const int got = g_fds[fd].file.read(buf, (uint32_t)size);
    if (got < 0)
        return sceMcResDeniedPermit;
    return got;
}

int write(int fd, const void *buf, int size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    const uint32_t pos = (uint32_t)g_fds[fd].file.seek(0, 1);
    const uint32_t len = (uint32_t)g_fds[fd].file.size();
    const uint32_t end = pos + size;
    if (end > len) {
        int grow = (int)((end + 1023) / 1024) - (int)((len + 1023) / 1024);
        int freeNow = free_clusters(g_fds[fd].port);
        if (grow > freeNow) {
            /* nothing written: the game reports the card full (8000 clusters
             * is the PS2 card's size, the free space it shows) */
            pclog::write("memcard: card %d full: a write of %d bytes needs %d more 1 KB clusters, %d free",
                         g_fds[fd].port + 1, size, grow, freeNow);
            return sceMcResFullDevice;
        }
    }
    changed();
    const int put = g_fds[fd].file.write(buf, (uint32_t)size);
    if (put < 0) {
        pclog::write("memcard: card %d: write of %d bytes failed (error %lu)", g_fds[fd].port + 1, size,
                     fs::last_error());
        return sceMcResDeniedPermit;
    }
    if ((int)put != size)
        pclog::write("memcard: card %d: short write, %d of %d bytes", g_fds[fd].port + 1, (int)put, size);
    return (int)put;
}

int mkdir(int port, int slot, const char *name)
{
    return open(port, slot, name, sceMcFileCreateDir);
}

int remove(int port, int slot, const char *name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const int res = card_check(port, slot))             /* PC (2026-11-15) */
        return res;
    std::string p = card_path(port, name);
    if (is_dir(p)) {
        if (count_entries(p) > 2)
            return sceMcResNotEmpty;
        return fs::remove_dir(fs::native_to_utf8(p)) ? sceMcResSucceed : sceMcResDeniedPermit;
    }
    if (!exists(p))
        return sceMcResNoEntry;
    changed();
    return fs::remove_file(fs::native_to_utf8(p)) ? sceMcResSucceed : sceMcResDeniedPermit;
}

int rename(int port, int slot, const char *org, const char *newName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const int res = card_check(port, slot))             /* PC (2026-11-15) */
        return res;
    std::string from = card_path(port, org);
    if (!exists(from))
        return sceMcResNoEntry;
    std::string to = parent_of(from) + fs::kSep + newName;
    if (exists(to))
        return sceMcResNoEntry;
    changed();
    return fs::rename(fs::native_to_utf8(from), fs::native_to_utf8(to)) ? sceMcResSucceed : sceMcResDeniedPermit;
}

/* pattern: "dir/name*" lists the matching entries of dir; "dir/ *" a whole
 * directory, "." and ".." first.  mode 0 starts a listing, 1 goes on with
 * it.  Returns the entries written; sceMcResNoEntry when dir is not there. */
int get_dir(int port, int slot, const char *pattern, unsigned int mode, int maxent, sceMcTblGetDir *table)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const int res = card_check(port, slot))             /* PC (2026-11-15) */
        return res;
    Listing &l = g_listing[port];
    if (mode == 0) {
        l.entries.clear();
        l.next = 0;
        std::string pat = pattern ? pattern : "*";
        while (!pat.empty() && (pat[0] == '/' || pat[0] == '\\'))
            pat.erase(0, 1);
        size_t s = pat.find_last_of("/\\");
        std::string dirPart = s == std::string::npos ? "" : pat.substr(0, s);
        std::string namePat = s == std::string::npos ? pat : pat.substr(s + 1);
        std::string dir = card_path(port, dirPart.c_str());
        if (!dir.empty() && dir.back() == fs::kSep)
            dir.pop_back();
        /* A directory that is not there is an error, as libmc's: no entry.
         * (0 entries read as "the directory is there, the file not": the
         * game took a card without its game data for damaged data and would
         * not save a first game - MemorycardSlotMenu::makeSaveConfirm wants
         * MC_STATUS_NO_ENTRY from getMainFileKB's listing; user report.) */
        if (!is_dir(dir)) {
            std::string root = card_root(port);
            root.pop_back();
            return is_dir(root) ? sceMcResNoEntry : sceMcResNoFormat;
        }
        if (!dirPart.empty() && namePat == "*") {
            sceMcTblGetDir e;
            fill_entry(&e, ".", nullptr, 0, true);
            l.entries.push_back(e);
            fill_entry(&e, "..", nullptr, 0, true);
            l.entries.push_back(e);
        }
        std::vector<fs::Entry> entries;
        if (list_entries(dir, &entries)) {
            for (const fs::Entry &x : entries) {
                if (!match(namePat.c_str(), x.name.c_str()))
                    continue;
                bool isDir = x.st.dir;
                int size = isDir ? count_entries(dir + fs::kSep + x.name) : (int)(uint32_t)x.st.size;
                sceMcTblGetDir e;
                fill_entry(&e, x.name.c_str(), &x.st, size, isDir);
                l.entries.push_back(e);
            }
        }
    }
    int n = 0;
    while (n < maxent && l.next < l.entries.size())
        table[n++] = l.entries[l.next++];
    return n;
}

int set_file_info(int port, int slot, const char *name, const char *info, unsigned int valid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const int res = card_check(port, slot))             /* PC (2026-11-15) */
        return res;
    std::string p = card_path(port, name);
    if (!exists(p))
        return sceMcResNoEntry;
    changed();
    const sceMcTblGetDir *e = (const sceMcTblGetDir *)info;
    if (valid & (sceMcFileInfoCreate | sceMcFileInfoModify)) {
        fs::Time c, m;
        bool hc = (valid & sceMcFileInfoCreate) && card_filetime(&e->_Create, &c);
        bool hm = (valid & sceMcFileInfoModify) && card_filetime(&e->_Modify, &m);
        fs::set_times(fs::native_to_utf8(p), hc ? &c : nullptr, hm ? &m : nullptr);
    }
    if ((valid & 0x10) && e->EntryName[0]) {             /* the name */
        std::string to = parent_of(p) + fs::kSep + (const char *)e->EntryName;
        if (to != p && !fs::rename(fs::native_to_utf8(p), fs::native_to_utf8(to)))
            return sceMcResNoEntry;
    }
    return sceMcResSucceed;
}

int get_ent_space(int port, int slot, const char *path)
{
    (void)path;
    std::lock_guard<std::mutex> lock(g_mutex);              /* PC (2026-11-15): the slot's state */
    if (const int res = card_check(port, slot))
        return res;
    return 0x20;
}

int format(int port, int slot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const int res = card_check(port, slot))             /* PC (2026-11-15) */
        return res;
    std::string root = card_root(port);
    root.pop_back();
    changed();
    fs::make_dir(fs::native_to_utf8(root));
    delete_tree(root, false);
    pclog::write("memcard: card %d formatted", port + 1);
    return sceMcResSucceed;
}

int unformat(int port, int slot)
{
    return format(port, slot);
}

bool busy()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Fd &f : g_fds)
        if (f.used && f.writer)
            return true;
    return g_lastChange != 0 && GetTickCount64() - g_lastChange < kBusyMs;
}

/* ---- PC (2026-11-15): the save folders (memcard.h) ---- */

const std::string &saves_dir()
{
    return g_savesDir;
}

std::vector<std::string> folders()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const ULONGLONG now = GetTickCount64();
    if (g_foldersAt == 0 || now - g_foldersAt >= 1000) {
        g_foldersAt = now;
        g_folders.clear();
        for (const std::wstring &f : save_folders::list(save_folders::from_ansi(g_savesDir)))
            if (save_folders::name_problem(f) == nullptr)
                g_folders.push_back(save_folders::to_ansi(f));
    }
    return g_folders;
}

std::string slot_folder(int port)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (port != 0 && port != 1)
        return std::string();
    const Slot &s = g_slot[port];
    return s.wanted ? s.wantedName : s.name;
}

bool slot_folder_there(int port)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (port != 0 && port != 1)
        return false;
    const Slot &s = g_slot[port];
    const std::string &name = s.wanted ? s.wantedName : s.name;
    return !name.empty() && is_dir(g_savesDir + name);
}

bool slot_waiting(int port)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return (port == 0 || port == 1) && g_slot[port].wanted;
}

bool set_slot_folder(int port, const std::string &name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (port != 0 && port != 1)
        return false;
    const Slot &other = g_slot[1 - port];
    const std::string &otherName = other.wanted ? other.wantedName : other.name;
    if (!name.empty() && _stricmp(name.c_str(), otherName.c_str()) == 0)
        return false;
    Slot &s = g_slot[port];
    const std::string &now = s.wanted ? s.wantedName : s.name;
    if (_stricmp(name.c_str(), now.c_str()) == 0 && name == now)
        return true;
    s.wanted = true;
    s.wantedName = name;
    save_folders::set_slot(save_folders::from_ansi(paths::engine_ini_path()), port, save_folders::from_ansi(name));
    apply_wanted(port);
    if (s.wanted)
        pclog::write("memcard: slot %d: %s once the card's open file is closed", port + 1,
                     name.empty() ? "no card" : name.c_str());
    return true;
}

} // namespace memcard
