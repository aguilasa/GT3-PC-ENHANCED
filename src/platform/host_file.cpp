/* host_file.cpp - "host:" files of GT3 PC (host_file.h). */
#include "platform/host_file.h"
#include "platform/disc_image.h"                /* PC (2026-11-14): a disc image's volume under the data folder */
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/settings.h"
#include "platform/os/fs.h"
#include "platform/os/win_compat.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

/* An open file: one of the disk (h), or - PC (2026-11-14), the game started
 * on a disc image (platform/disc_image.h) - one of the image's volume (disc):
 * a plain one read in place, a packed one from its unpacked bytes, inflated
 * at its first read (a probe - io_getstat opens a file for its size - costs
 * nothing). */
struct HostFile {
    fs::File file;
    const disc_image::File *disc = nullptr;
    std::shared_ptr<const std::vector<uint8_t>> unpacked;
};

const int kFiles = 64;
HostFile g_files[kFiles];
bool g_used[kFiles];
std::mutex g_mutex;
thread_local bool t_quiet;                  /* host_file_quiet */
thread_local unsigned t_held;               /* host_file_held */

/* The path in the data folder ('\' its separators) */
std::string relative_path(const char *path)
{
    std::string p = path;
    if (p.compare(0, 5, "host:") == 0)
        p = p.substr(5);
    else if (p.compare(0, 6, "host0:") == 0)
        p = p.substr(6);
    while (!p.empty() && (p[0] == '/' || p[0] == '\\'))
        p.erase(0, 1);
    for (char &c : p)
        if (c == '/')
            c = '\\';
    return p;
}

/* rel with the system's separators (fs.h kSep). */
std::string system_separators(std::string rel)
{
    if (fs::kSep != '\\')
        for (char &c : rel)
            if (c == '\\')
                c = fs::kSep;
    return rel;
}

/* "data\..." names the data folder (data_US\ ..., platform/paths.h), any
 * other host path the folder holding it (the native text: fs.h). */
std::string host_path(const char *path)
{
    const std::string rel = relative_path(path);
    if (_strnicmp(rel.c_str(), "data", 4) == 0 && (rel.size() == 4 || rel[4] == '\\'))
        return paths::data_folder() + system_separators(rel.size() > 5 ? rel.substr(5) : std::string());
    return paths::data_dir() + system_separators(rel);
}

/* The same as the file layer takes it (UTF-8). */
std::string host_path_utf8(const char *path)
{
    return fs::native_to_utf8(host_path(path));
}

bool file_log()
{
    return !t_quiet && settings::debug_file_log();
}

/* Folders deeper than this are listed empty (logged), not followed: a
 * guard against a link loop, far past any path the file API can open. */
constexpr int kTocMaxDepth = 256;

/* ---- the mod folders ------------------------------------------------------- */

/* gt3pc_engine.ini [Mods] (PC 2026-11-14: the engine's own file, was gt3pc.ini's - paths.h engine_ini_path;
 * the launcher's Mods window): Count, then ModN (a folder,
 * laid out as the data folder) and ModNOn (on / off), N = 1 the first.  An
 * enabled folder's files take the place of the data folder's files of the
 * same path (the first folder holding one wins), and its other files are
 * added: listed in the volume (host_file_toc) and opened as any.  Read once,
 * at the first file the game asks for. */
struct ModFile {
    std::string full;               /* its path (UTF-8) */
    std::string rel;                /* its path in the data folder, the folder's case ("cars\day\x") */
    uint32_t size = 0;
    int mod = 0;                    /* its folder (g_modFolders) */
};
std::once_flag g_modsOnce;
std::vector<std::string> g_modFolders;     /* (UTF-8) */
std::unordered_map<std::string, ModFile> g_modFiles;    /* by the lower-case rel */

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

/* A UTF-8 name in the ANSI code page (the game's paths are; fs.h native);
 * false when a character has none there. */
bool narrow(const std::string &utf8, std::string *out)
{
    bool exact = false;
    *out = fs::utf8_to_native(utf8, &exact);
    return exact;
}

/* A path given whole, outside the data folder (host_file.h): Windows' with
 * a drive or a server; POSIX's from the root, but for "/data/...". */
bool outside_path(const char *p)
{
#ifdef _WIN32
    return (p[0] == '\\' && p[1] == '\\') ||
           (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/'));
#else
    if (p[0] != '/')
        return false;
    const std::string rel = relative_path(p);
    return !(_strnicmp(rel.c_str(), "data", 4) == 0 && (rel.size() == 4 || rel[4] == '\\'));
#endif
}

/* Its UTF-8: as it is, else (typed by hand) the ANSI code page's bytes. */
std::string outside_utf8(const char *p)
{
    return fs::utf8_or_native(p);
}

/* The files of mod folder `mod' under `folder' (UTF-8; rel: their path so
 * far). */
void mod_scan(int mod, const std::string &folder, const std::string &rel, int depth, uint32_t *files, uint32_t *skipped)
{
    if (depth > kTocMaxDepth)
        return;
    std::vector<fs::Entry> entries;
    if (!fs::list(folder, &entries))
        return;
    for (const fs::Entry &e : entries) {
        if (e.st.hidden)
            continue;
        std::string name;
        if (!narrow(e.name, &name)) {
            (*skipped)++;                   /* (the game's paths are the ANSI code page's) */
            continue;
        }
        const std::string r = rel.empty() ? name : rel + "\\" + name;
        const std::string full = folder + fs::kSep + e.name;
        if (e.st.dir) {
            mod_scan(mod, full, r, depth + 1, files, skipped);
            continue;
        }
        /* (the folder's own card in the launcher's Mods window, no game file) */
        if (rel.empty() && (_stricmp(e.name.c_str(), "mod.txt") == 0 || _stricmp(e.name.c_str(), "mod.png") == 0))
            continue;
        const std::string key = lower(r);
        if (g_modFiles.count(key) != 0)
            continue;                       /* (a folder before this one has it) */
        ModFile m;
        m.full = full;
        m.rel = r;
        m.size = e.st.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)e.st.size;
        m.mod = mod;
        g_modFiles.emplace(key, std::move(m));
        (*files)++;
    }
}

/* gt3pc_engine.ini [Mods] key's value (UTF-8). */
std::string mods_value(const char *key, const char *def)
{
#ifdef _WIN32
    /* (the "W" calls: a folder's name in any script) */
    const std::wstring ini = fs::widen(fs::native_to_utf8(paths::engine_ini_path()));
    wchar_t v[1024] = L"";
    GetPrivateProfileStringW(L"Mods", fs::widen(key).c_str(), fs::widen(def).c_str(), v, 1024, ini.c_str());
    return fs::narrow(v);
#else
    char v[1024] = "";
    GetPrivateProfileStringA("Mods", key, def, v, sizeof(v), paths::engine_ini_path().c_str());
    return v;
#endif
}

void mods_load()
{
    /* PC (2026-11-14): the engine's own file (gt3pc_engine.ini; gt3pc.ini
     * until its launcher has moved them: paths.h) */
#ifdef _WIN32
    const int count = (int)GetPrivateProfileIntW(L"Mods", L"Count", 0,
                                                 fs::widen(fs::native_to_utf8(paths::engine_ini_path())).c_str());
#else
    const int count = (int)GetPrivateProfileIntA("Mods", "Count", 0, paths::engine_ini_path().c_str());
#endif
    for (int i = 1; i <= count && i <= 256; i++) {
        char key[32];
        snprintf(key, sizeof(key), "Mod%dOn", i);
        const std::string on = mods_value(key, "on");
        snprintf(key, sizeof(key), "Mod%d", i);
        std::string f = mods_value(key, "");
        while (!f.empty() && (f.back() == '\\' || f.back() == '/'))
            f.pop_back();
        if (f.empty() || _stricmp(on.c_str(), "on") != 0)
            continue;
        std::string shown;
        if (!narrow(f, &shown))
            shown = "a folder named outside the ANSI code page";
        if (!fs::is_dir(f)) {
            pclog::write("mods: %s is not there: left out", shown.c_str());
            continue;
        }
        uint32_t files = 0, skipped = 0;
        g_modFolders.push_back(f);
        mod_scan((int)g_modFolders.size() - 1, f, std::string(), 0, &files, &skipped);
        pclog::write("mods: %s: %u files%s", shown.c_str(), files,
                     skipped != 0 ? " (some names left out: not in the ANSI code page)" : "");
    }
    if (!g_modFolders.empty())
        pclog::write("mods: %u folders on, %u files in all", (unsigned)g_modFolders.size(),
                     (unsigned)g_modFiles.size());
}

bool ends_with_gz(const std::string &s)
{
    return s.size() > 3 && _stricmp(s.c_str() + s.size() - 3, ".gz") == 0;
}

/* A host path's mod file into *m (NULL: none, the data folder's is read).
 * False when the file is hidden: "<name>.gz", the packed copy file_resolve
 * reads in place of <name>, when a mod has <name> but not packed (the data
 * folder's packed copy would win over the mod's file). */
bool mod_lookup(const char *path, const ModFile **m)
{
    *m = nullptr;
    std::call_once(g_modsOnce, mods_load);
    if (g_modFiles.empty())
        return true;
    const std::string rel = relative_path(path);
    if (_strnicmp(rel.c_str(), "data\\", 5) != 0 || rel.size() <= 5)
        return true;
    const std::string key = lower(rel.substr(5));
    auto it = g_modFiles.find(key);
    if (it != g_modFiles.end()) {
        *m = &it->second;
        return true;
    }
    return !(ends_with_gz(key) && g_modFiles.count(key.substr(0, key.size() - 3)) != 0);
}

int add(fs::File file, const disc_image::File *disc = nullptr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < kFiles; i++)
        if (!g_used[i]) {
            g_used[i] = true;
            g_files[i] = HostFile();
            g_files[i].file = std::move(file);
            g_files[i].disc = disc;
            return i;
        }
    return -1;                              /* (file closed) */
}

/* fd's file of the disk (NULL: not open). */
const fs::File *get(int fd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return fd >= 0 && fd < kFiles && g_used[fd] && g_files[fd].file.is_open() ? &g_files[fd].file : nullptr;
}

/* fd's file of the disc image's volume (NULL: a file of the disk, or not
 * open). */
const disc_image::File *get_disc(int fd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return fd >= 0 && fd < kFiles && g_used[fd] ? g_files[fd].disc : nullptr;
}

/* A host path's file in the disc image's volume (NULL: none, or the game
 * plays an installed version): what the data folder and the mod folders do
 * not have. */
const disc_image::File *disc_lookup(const char *path)
{
    if (!disc_image::mounted())
        return nullptr;
    const std::string rel = relative_path(path);
    if (_strnicmp(rel.c_str(), "data\\", 5) != 0 || rel.size() <= 5)
        return nullptr;
    return disc_image::find(lower(rel.substr(5)));
}

/* fd's bytes of a packed file of the volume, unpacked at the first read. */
std::shared_ptr<const std::vector<uint8_t>> disc_unpacked(int fd, const disc_image::File *f)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (fd >= 0 && fd < kFiles && g_used[fd] && g_files[fd].disc == f && g_files[fd].unpacked)
            return g_files[fd].unpacked;
    }
    std::shared_ptr<const std::vector<uint8_t>> bytes = disc_image::unpacked(*f);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (bytes && fd >= 0 && fd < kFiles && g_used[fd] && g_files[fd].disc == f)
        g_files[fd].unpacked = bytes;
    return bytes;
}

/* ---- host_file_toc --------------------------------------------------------- */

/* A file or folder of the listed tree; off / nameOff: its record and name in
 * the TOC image. */
struct TocNode {
    std::string name;
    bool dir = false;
    uint32_t size = 0;
    std::vector<TocNode> children;
    uint32_t off = 0;
};

constexpr uint32_t kTocRoot = 0x14;         /* the root record (VOL_ROOT_DIR) */

/* Lists `folder' into node.  False only when the data folder itself (depth
 * 0) cannot be listed; a sub-folder that cannot be (too deep, a path past
 * what the file API takes, no access) is kept empty and logged. */
bool toc_list(const std::string &folder, TocNode *node, int depth, uint32_t *files, uint32_t *folders)
{
    if (depth > kTocMaxDepth) {
        pclog::write("data: %s is more than %d folders deep; its files are not listed", folder.c_str(),
                     kTocMaxDepth);
        return true;
    }
    std::vector<fs::Entry> entries;
    if (!fs::list(fs::native_to_utf8(folder), &entries)) {
        if (depth == 0)
            return false;
        pclog::write("data: %s could not be listed (error %lu); its files are left out", folder.c_str(),
                     fs::last_error());
        return true;
    }
    bool ok = true;
    for (const fs::Entry &e : entries) {
        /* (the name in the ANSI code page, its best fit where it has none, as
         * the "A" calls give it) */
        const std::string name = fs::utf8_to_native(e.name);
        if (e.st.hidden || ends_with_gz(name) || (depth == 0 && _stricmp(name.c_str(), paths::kInstallMarker) == 0))
            continue;
        TocNode child;
        child.name = name;
        if (e.st.dir) {
            child.dir = true;
            (*folders)++;
            if (!toc_list(folder + fs::kSep + name, &child, depth + 1, files, folders)) {
                ok = false;
                break;
            }
        } else {
            child.size = e.st.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)e.st.size;
            (*files)++;
        }
        node->children.push_back(std::move(child));
    }
    /* the volume's order: vol_dir_find's binary search compares bytes */
    std::sort(node->children.begin(), node->children.end(),
              [](const TocNode &a, const TocNode &b) { return a.name < b.name; });
    return ok;
}

/* The mod files into the listed tree (after toc_list): a file the data
 * folder has (any case; its name kept) gets the mod file's size, the others
 * are added with their folders, then each folder is sorted again.  Left out
 * as toc_list does: "*.gz" and the install marker; and a file where the
 * data folder has a folder of its name (or the other way round), logged. */
/* (PC 2026-11-14: one file's way into the tree is toc_add_file, the disc
 * image's files and the data folder's over them go the same way - what: whose
 * file it is, where: what has its name already, for the log) */
void toc_add_file(TocNode *root, const std::string &rel, uint32_t size, uint32_t *files, uint32_t *folders,
                  const char *what, const char *where)
{
    if (ends_with_gz(rel) || _stricmp(rel.c_str(), paths::kInstallMarker) == 0)
        return;
    TocNode *n = root;
    size_t at = 0;
    for (;;) {
        const size_t sep = rel.find('\\', at);
        const bool last = sep == std::string::npos;
        const std::string name = rel.substr(at, last ? std::string::npos : sep - at);
        TocNode *child = nullptr;
        for (TocNode &c : n->children)
            if (_stricmp(c.name.c_str(), name.c_str()) == 0) {
                child = &c;
                break;
            }
        if (child != nullptr && child->dir == last) {
            pclog::write("%s: %s is a %s in %s; left out", what, rel.c_str(), child->dir ? "folder" : "file", where);
            break;
        }
        if (child == nullptr) {
            TocNode c;
            c.name = name;
            c.dir = !last;
            n->children.push_back(std::move(c));
            child = &n->children.back();
            (*(last ? files : folders))++;
        }
        if (last) {
            child->size = size;
            break;
        }
        n = child;
        at = sep + 1;
    }
}

void toc_add_mods(TocNode *root, uint32_t *files, uint32_t *folders)
{
    for (const auto &e : g_modFiles)
        toc_add_file(root, e.second.rel, e.second.size, files, folders, "mods", "the data folder");
}

/* PC (2026-11-14): the files of the listed tree `from' (toc_list's: the data
 * folder's) into `root' over the disc image's (rel: their path so far). */
void toc_add_tree(TocNode *root, const TocNode &from, const std::string &rel, uint32_t *files, uint32_t *folders)
{
    for (const TocNode &c : from.children) {
        const std::string r = rel.empty() ? c.name : rel + "\\" + c.name;
        if (c.dir)
            toc_add_tree(root, c, r, files, folders);
        else
            toc_add_file(root, r, c.size, files, folders, "data", "the disc image");
    }
}

void toc_sort(TocNode *n)
{
    std::sort(n->children.begin(), n->children.end(),
              [](const TocNode &a, const TocNode &b) { return a.name < b.name; });
    for (TocNode &c : n->children)
        if (c.dir)
            toc_sort(&c);
}

/* Records in pre-order from *next: a folder's 0xC bytes + 4 per entry (at
 * least 0x10), a file's 0x10. */
void toc_place(TocNode *n, uint32_t *next)
{
    n->off = *next;
    uint32_t bytes = n->dir ? 0xC + 4 * (uint32_t)n->children.size() : 0x10;
    *next += bytes < 0x10 ? 0x10 : bytes;
    for (TocNode &c : n->children)
        toc_place(&c, next);
}

void toc_put(std::vector<uint8_t> &img, uint32_t off, uint32_t v)
{
    memcpy(&img[off], &v, 4);
}

/* Writes n's record (type 1 for a folder, else 0) and its name into `names`,
 * which follows the records at `namesAt`. */
void toc_write(const TocNode &n, uint32_t parent, std::vector<uint8_t> &img, std::string &names, uint32_t namesAt)
{
    uint32_t nameOff = namesAt + (uint32_t)names.size();
    names += n.name;
    names += '\0';
    toc_put(img, n.off, nameOff | ((n.dir ? 1u : 0u) << 24));
    if (!n.dir) {
        toc_put(img, n.off + 4, 0);         /* no sector: file_resolve asks host: */
        toc_put(img, n.off + 8, n.size);
        toc_put(img, n.off + 0xC, 0);
        return;
    }
    toc_put(img, n.off + 4, (uint32_t)n.children.size() + 1);
    toc_put(img, n.off + 8, parent);
    for (size_t i = 0; i < n.children.size(); i++)
        toc_put(img, n.off + 0xC + 4 * (uint32_t)i, n.children[i].off);
    for (const TocNode &c : n.children)
        toc_write(c, n.off, img, names, namesAt);
}

/* The path of a host path in UTF-8 (host_file_full_path_utf8; a
 * mod file: its path as listed, the real case). */
std::string full_path_utf8(const char *path)
{
    const ModFile *m;
    if (outside_path(path))
        return fs::system_path(outside_utf8(path));
    if (mod_lookup(path, &m) && m != nullptr)
        return m->full;
    return fs::system_path(host_path_utf8(path));
}

} // namespace

extern "C" {

int host_file_full_path(const char *path, char *out, uint32_t size)
{
    /* (a mod file whose path the ANSI code page cannot spell: the data
     * folder's) */
    const ModFile *m;
    std::string p;
    if (outside_path(path))
        p = path;
    else if (!(mod_lookup(path, &m) && m != nullptr && narrow(m->full, &p)))
        p = host_path(path);
#ifndef _WIN32
    p = fs::system_path(p);                 /* (the parts in their real case) */
#endif
    if (p.size() + 1 > size)
        return -1;
    memcpy(out, p.c_str(), p.size() + 1);
    return 0;
}

#ifdef _WIN32
int host_file_full_path_w(const char *path, wchar_t *out, uint32_t size)
{
    const std::wstring w = fs::widen(full_path_utf8(path));
    if (w.size() + 1 > size)
        return -1;
    memcpy(out, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    return 0;
}
#endif

int host_file_full_path_utf8(const char *path, char *out, uint32_t size)
{
    const std::string p = full_path_utf8(path);
    if (p.size() + 1 > size)
        return -1;
    memcpy(out, p.c_str(), p.size() + 1);
    return 0;
}

int host_file_present(const char *path)
{
    const ModFile *m;
    if (outside_path(path))
        return fs::is_file(outside_utf8(path)) ? 1 : 0;
    if (!mod_lookup(path, &m))
        return 0;
    if (fs::is_file(m != nullptr ? m->full : host_path_utf8(path)))
        return 1;
    return m == nullptr && disc_lookup(path) != nullptr ? 1 : 0;    /* PC (2026-11-14): the disc image's */
}

void host_file_list(const char *path, void (*fn)(const char *name, void *arg), void *arg)
{
    std::call_once(g_modsOnce, mods_load);
    const std::string rel = relative_path(path);
    std::vector<std::string> seen;          /* the mods' names, lower case */
    if (_strnicmp(rel.c_str(), "data\\", 5) == 0) {
        const std::string prefix = lower(rel.substr(5)) + "\\";
        for (const auto &file : g_modFiles) {
            const std::string &key = file.first;
            if (key.compare(0, prefix.size(), prefix) == 0 && key.find('\\', prefix.size()) == std::string::npos) {
                seen.push_back(key.substr(prefix.size()));
                fn(file.second.rel.c_str() + prefix.size(), arg);
            }
        }
    }
    std::vector<fs::Entry> entries;
    if (fs::list(host_path_utf8(path), &entries)) {
        for (const fs::Entry &e : entries) {
            if (e.st.dir || e.st.hidden)
                continue;
            const std::string name = fs::utf8_to_native(e.name);
            if (std::find(seen.begin(), seen.end(), lower(name)) == seen.end()) {
                seen.push_back(lower(name));
                fn(name.c_str(), arg);
            }
        }
    }
    /* PC (2026-11-14): then the disc image's (the folder's own files, those
     * the mods and the data folder do not have) */
    if (disc_image::mounted() && _strnicmp(rel.c_str(), "data\\", 5) == 0) {
        const std::string prefix = lower(rel.substr(5)) + "\\";
        for (const disc_image::File &d : disc_image::files()) {
            if (d.rel.size() <= prefix.size() || _strnicmp(d.rel.c_str(), prefix.c_str(), prefix.size()) != 0 ||
                d.rel.find('\\', prefix.size()) != std::string::npos)
                continue;
            const std::string name = d.rel.substr(prefix.size());
            if (std::find(seen.begin(), seen.end(), lower(name)) == seen.end())
                fn(name.c_str(), arg);
        }
    }
}

/* The failed open's error, when it means the file is there but held. */
static void note_held()
{
    const unsigned long e = fs::last_error();
    if (t_held == 0 && fs::error_held(e))
        t_held = (unsigned)e;
}

unsigned host_file_held(void)
{
    return t_held;
}

int host_file_open(const char *path)
{
    const ModFile *m;
    fs::File f;
    t_held = 0;
    if (outside_path(path)) {
        /* (any sharing: a song a player or a tag editor has open still plays) */
        f.open(outside_utf8(path), fs::kRead, fs::Create::open_existing,
               fs::kShareRead | fs::kShareWrite | fs::kShareDelete);
        if (file_log())
            pclog::write(!f.is_open() ? "file: %s missing" : "file: %s (outside the data folder)", path);
        return !f.is_open() ? -1 : add(std::move(f));
    }
    if (mod_lookup(path, &m)) {
        if (m != nullptr) {
            if (!f.open(m->full, fs::kRead, fs::Create::open_existing, fs::kShareRead))
                note_held();
        }
        if (!f.is_open()) {
            m = nullptr;                    /* (a mod file gone since: the data folder's) */
            if (!f.open(host_path_utf8(path), fs::kRead, fs::Create::open_existing, fs::kShareRead))
                note_held();
        }
        /* PC (2026-11-14): neither has it: the disc image's volume (a file
         * of the data folder that another program holds is not replaced by
         * the disc's: the caller waits for it, pc_data_file) */
        if (!f.is_open() && t_held == 0) {
            const disc_image::File *d = disc_lookup(path);
            if (d != nullptr) {
                if (file_log())
                    pclog::write("file: %s (%u bytes, the disc image's%s)", relative_path(path).c_str(), d->size,
                                 d->packed ? ", packed" : "");
                return add(fs::File(), d);
            }
        }
    }
    if (!f.is_open()) {
        /* (probes are normal: file_resolve tries host: first; quiet) */
        if (file_log())
            pclog::write("file: %s missing", relative_path(path).c_str());
        return -1;
    }
    if (file_log()) {
        const char *from = m != nullptr ? ", a mod's" : "";
        const int64_t sz = f.size();
        if (sz >= 0)
            pclog::write("file: %s (%lld bytes%s)", relative_path(path).c_str(), (long long)sz, from);
        else
            pclog::write("file: %s%s", relative_path(path).c_str(), m != nullptr ? " (a mod's)" : "");
    }
    return add(std::move(f));
}

int host_file_create(const char *path)
{
    fs::File f;
    f.open(host_path_utf8(path), fs::kWrite, fs::Create::create_always, fs::kShareNone);
    if (file_log())
        pclog::write(!f.is_open() ? "file: %s could not be written" : "file: %s written",
                     relative_path(path).c_str());
    return !f.is_open() ? -1 : add(std::move(f));
}

void host_file_quiet(int on)
{
    t_quiet = on != 0;
}

void host_file_close(int fd)
{
    std::shared_ptr<const std::vector<uint8_t>> unpacked;    /* (let go of outside the lock) */
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd >= 0 && fd < kFiles && g_used[fd]) {
        g_files[fd].file.close();
        unpacked.swap(g_files[fd].unpacked);
        g_files[fd] = HostFile();
        g_used[fd] = false;
    }
}

int64_t host_file_size(int fd)
{
    if (const disc_image::File *d = get_disc(fd))
        return d->size;                     /* (once unpacked, as an install has it) */
    const fs::File *f = get(fd);
    return f != nullptr ? f->size() : -1;
}

int host_file_read_at(int fd, uint64_t offset, void *buf, uint32_t bytes)
{
    if (const disc_image::File *d = get_disc(fd)) {
        /* PC (2026-11-14): a file of the disc image's volume */
        if (offset >= d->size)
            return 0;
        if (bytes > d->size - offset)
            bytes = (uint32_t)(d->size - offset);
        if (!d->packed)
            return disc_image::read(*d, offset, buf, bytes) ? (int)bytes : -1;
        const std::shared_ptr<const std::vector<uint8_t>> u = disc_unpacked(fd, d);
        if (!u)
            return -1;
        memcpy(buf, u->data() + (size_t)offset, bytes);
        return (int)bytes;
    }
    const fs::File *f = get(fd);
    return f != nullptr ? f->read_at(offset, buf, bytes) : -1;
}

int host_file_write_at(int fd, uint64_t offset, const void *buf, uint32_t bytes)
{
    if (get_disc(fd) != nullptr)
        return -1;                          /* (the disc image is never written) */
    const fs::File *f = get(fd);
    return f != nullptr ? f->write_at(offset, buf, bytes) : -1;
}

uint8_t *host_file_toc(const char *path, void *(*alloc)(uint32_t size), uint32_t *size)
{
    *size = 0;
    std::string folder = host_path(path);
    while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/'))
        folder.pop_back();
    TocNode root;
    root.dir = true;
    uint32_t files = 0, folders = 0;
    const bool data = _stricmp(relative_path(path).c_str(), "data") == 0;
    if (data && disc_image::mounted()) {
        /* PC (2026-11-14): the disc image's files, the data folder's (the
         * launcher's demo replays, what the user puts there) over them */
        TocNode real;
        real.dir = true;
        uint32_t realFiles = 0, realFolders = 0;
        if (!toc_list(folder, &real, 0, &realFiles, &realFolders)) {
            pclog::write("data: %s could not be listed", folder.c_str());
            return nullptr;
        }
        for (const disc_image::File &d : disc_image::files())
            toc_add_file(&root, d.rel, d.size, &files, &folders, "disc image", "the disc image");
        const uint32_t discFiles = files;
        toc_add_tree(&root, real, std::string(), &files, &folders);
        toc_sort(&root);
        pclog::write("data: %u files of the disc image, %u of the data folder (%u of them in place of the disc's)",
                     discFiles, realFiles, discFiles + realFiles - files);
    } else if (!toc_list(folder, &root, 0, &files, &folders)) {
        pclog::write("data: %s could not be listed", folder.c_str());
        return nullptr;
    }
    std::call_once(g_modsOnce, mods_load);
    if (data && !g_modFiles.empty()) {
        toc_add_mods(&root, &files, &folders);
        toc_sort(&root);
    }

    uint32_t namesAt = kTocRoot;
    toc_place(&root, &namesAt);
    std::vector<uint8_t> img(namesAt, 0);
    std::string names;
    toc_write(root, 0, img, names, namesAt);
    if (namesAt + names.size() > 0xFFFFFF) {         /* names are 24-bit offsets */
        pclog::write("data: %s holds too many files to list", folder.c_str());
        return nullptr;
    }
    img.insert(img.end(), names.begin(), names.end());
    /* The header: magic, GT3.VOL's word 1, the TOC size, the records' size
     * (the "header") and the names' (the "body"). */
    memcpy(&img[0], "RoFS", 4);
    toc_put(img, 4, 0x20002);
    toc_put(img, 8, (uint32_t)img.size());
    toc_put(img, 12, namesAt);
    toc_put(img, 16, (uint32_t)names.size());

    uint8_t *out = (uint8_t *)alloc((uint32_t)img.size());
    if (out == nullptr)
        return nullptr;
    memcpy(out, img.data(), img.size());
    *size = (uint32_t)img.size();
    pclog::write("data: %u files in %u folders listed (a %u-byte table)", files, folders, *size);
    return out;
}

} // extern "C"
