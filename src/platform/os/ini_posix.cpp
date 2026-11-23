/* ini_posix.cpp - the Win32 private-profile (INI) calls on POSIX (ini_posix.h). */
#ifndef _WIN32

#include "platform/os/ini_posix.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mutex>
#include <string>
#include <vector>

namespace {

/* one line of the file, as read: its text and its own line ending, so that
 * a rewrite keeps every line it does not touch byte for byte */
struct Line {
    std::string text;
    std::string eol;
};

enum class Kind { blank, comment, section, key, bare };

struct Parsed {
    Kind kind;
    std::string name;                       /* the section's or the key's, trimmed */
    std::string value;                      /* the key's, trimmed */
};

struct File {
    bool bom = false;                       /* a UTF-8 byte order mark before the first line */
    std::vector<Line> lines;
};

/* one lock for the read-modify-write of every file of this process */
std::mutex g_mutex;

bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string trim(const char *s, size_t n)
{
    size_t b = 0, e = n;
    while (b < e && is_blank(s[b]))
        b++;
    while (e > b && is_blank(s[e - 1]))
        e--;
    return std::string(s + b, e - b);
}

std::string trim(const std::string &s)
{
    return trim(s.data(), s.size());
}

bool same(const std::string &a, const char *b)
{
    return strcasecmp(a.c_str(), b) == 0;
}

Parsed parse(const std::string &text)
{
    Parsed p;
    const std::string t = trim(text);
    if (t.empty()) {
        p.kind = Kind::blank;
    } else if (t[0] == ';') {
        p.kind = Kind::comment;
    } else if (t[0] == '[') {
        p.kind = Kind::section;
        const size_t close = t.find(']');
        p.name = trim(t.data() + 1, (close == std::string::npos ? t.size() : close) - 1);
    } else {
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            p.kind = Kind::bare;
            p.name = t;
        } else {
            p.kind = Kind::key;
            p.name = trim(t.data(), eq);
            p.value = trim(t.data() + eq + 1, t.size() - eq - 1);
        }
    }
    return p;
}

bool load(const char *path, File &f)
{
    f = File();
    FILE *fp = fopen(path, "rb");
    if (fp == nullptr)
        return false;
    std::string data;
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0)
        data.append(buf, got);
    fclose(fp);
    size_t pos = 0;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        f.bom = true;
        pos = 3;
    }
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        Line l;
        if (nl == std::string::npos) {
            l.text = data.substr(pos);
            pos = data.size();
        } else {
            size_t end = nl;
            if (end > pos && data[end - 1] == '\r')
                end--;
            l.text = data.substr(pos, end - pos);
            l.eol = data.substr(end, nl + 1 - end);
            pos = nl + 1;
        }
        f.lines.push_back(l);
    }
    return true;
}

/* the line ending of the file's first ended line ("\n" for a new file) */
std::string file_eol(const File &f)
{
    for (const Line &l : f.lines)
        if (!l.eol.empty())
            return l.eol;
    return "\n";
}

/* the whole file to a temporary next to it, then renamed over it: a reader
 * in another process sees the old file or the new one, never half of it */
bool save(const char *path, const File &f)
{
    std::string data;
    if (f.bom)
        data += "\xEF\xBB\xBF";
    for (const Line &l : f.lines) {
        data += l.text;
        data += l.eol;
    }
    char suffix[32];
    snprintf(suffix, sizeof(suffix), ".tmp%ld", (long)getpid());
    const std::string tmp = std::string(path) + suffix;
    FILE *fp = fopen(tmp.c_str(), "wb");
    if (fp == nullptr)
        return false;
    const bool ok = fwrite(data.data(), 1, data.size(), fp) == data.size();
    if (fclose(fp) != 0 || !ok) {
        unlink(tmp.c_str());
        return false;
    }
    struct stat st;
    if (stat(path, &st) == 0)
        chmod(tmp.c_str(), st.st_mode & 07777);
    if (rename(tmp.c_str(), path) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

/* [begin, end) of the first section called name: begin is its header */
bool find_section(const File &f, const char *name, size_t *begin, size_t *end)
{
    for (size_t i = 0; i < f.lines.size(); i++) {
        const Parsed p = parse(f.lines[i].text);
        if (p.kind != Kind::section || !same(p.name, name))
            continue;
        size_t j = i + 1;
        while (j < f.lines.size() && parse(f.lines[j].text).kind != Kind::section)
            j++;
        *begin = i;
        *end = j;
        return true;
    }
    return false;
}

/* the first line of [begin, end) that is the key called name */
bool find_key(const File &f, size_t begin, size_t end, const char *name, size_t *at, Parsed *out)
{
    for (size_t i = begin + 1; i < end; i++) {
        Parsed p = parse(f.lines[i].text);
        if ((p.kind == Kind::key || p.kind == Kind::bare) && same(p.name, name)) {
            *at = i;
            if (out != nullptr)
                *out = p;
            return true;
        }
    }
    return false;
}

/* a value as Windows returns it: one pair of matching quotes removed */
std::string unquote(const std::string &v)
{
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0])
        return v.substr(1, v.size() - 2);
    return v;
}

/* one string, cut to the buffer: the count copied, size - 1 when cut */
DWORD copy_string(const std::string &s, LPSTR out, DWORD size)
{
    if (out == nullptr || size == 0)
        return 0;
    const size_t n = s.size() < (size_t)size - 1 ? s.size() : (size_t)size - 1;
    memcpy(out, s.data(), n);
    out[n] = '\0';
    return (DWORD)n;
}

/* NUL-separated strings and a final NUL; when cut, the last string is cut
 * and ended by two NULs and the result is size - 2 */
DWORD copy_list(const std::vector<std::string> &items, LPSTR out, DWORD size)
{
    if (out == nullptr || size == 0)
        return 0;
    std::string data;
    for (const std::string &s : items) {
        data += s;
        data += '\0';
    }
    if (data.empty())
        data += '\0';                       /* an empty list: two NULs */
    data += '\0';
    if (data.size() <= size) {
        memcpy(out, data.data(), data.size());
        return (DWORD)(items.empty() ? 0 : data.size() - 1);
    }
    if (size == 1) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, data.data(), size - 2);
    out[size - 2] = '\0';
    out[size - 1] = '\0';
    return size - 2;
}

void add_unique(std::vector<std::string> &items, const std::string &name)
{
    for (const std::string &s : items)
        if (strcasecmp(s.c_str(), name.c_str()) == 0)
            return;
    items.push_back(name);
}

/* the default as Windows uses it: NULL is "", trailing blanks dropped */
std::string default_value(LPCSTR def)
{
    if (def == nullptr)
        return std::string();
    size_t n = strlen(def);
    while (n > 0 && is_blank(def[n - 1]))
        n--;
    return std::string(def, n);
}

/* where a new key of the section [begin, end) goes: after its last line that
 * is not blank (the blank lines before the next section stay there) */
size_t insert_point(const File &f, size_t begin, size_t end)
{
    size_t at = begin + 1;
    for (size_t i = begin + 1; i < end; i++)
        if (parse(f.lines[i].text).kind != Kind::blank)
            at = i + 1;
    return at;
}

void insert_line(File &f, size_t at, const std::string &text)
{
    const std::string eol = file_eol(f);
    if (at > 0 && f.lines[at - 1].eol.empty())
        f.lines[at - 1].eol = eol;          /* the file's last line had no ending */
    Line l;
    l.text = text;
    l.eol = eol;
    f.lines.insert(f.lines.begin() + (ptrdiff_t)at, l);
}

void append_section(File &f, const char *section)
{
    if (!f.lines.empty() && parse(f.lines.back().text).kind != Kind::blank)
        insert_line(f, f.lines.size(), "");
    insert_line(f, f.lines.size(), std::string("[") + section + "]");
}

} // namespace

DWORD GetPrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR def, LPSTR out, DWORD size, LPCSTR file)
{
    if (out == nullptr || size == 0)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    File f;
    const bool loaded = file != nullptr && load(file, f);
    if (section == nullptr) {               /* all the section names */
        std::vector<std::string> names;
        for (const Line &l : f.lines) {
            const Parsed p = parse(l.text);
            if (p.kind == Kind::section)
                add_unique(names, p.name);
        }
        return copy_list(names, out, size);
    }
    size_t begin = 0, end = 0;
    const bool has_section = loaded && find_section(f, section, &begin, &end);
    if (key == nullptr) {                   /* all the key names of the section */
        std::vector<std::string> names;
        if (has_section)
            for (size_t i = begin + 1; i < end; i++) {
                const Parsed p = parse(f.lines[i].text);
                if (p.kind == Kind::key || p.kind == Kind::bare)
                    add_unique(names, p.name);
            }
        return copy_list(names, out, size);
    }
    size_t at = 0;
    Parsed p;
    if (has_section && find_key(f, begin, end, key, &at, &p))
        return copy_string(unquote(p.value), out, size);
    return copy_string(default_value(def), out, size);
}

UINT GetPrivateProfileIntA(LPCSTR section, LPCSTR key, int def, LPCSTR file)
{
    char buf[64];
    if (GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), file) == 0)
        return (UINT)def;                   /* missing or empty: the default */
    /* decimal digits after an optional sign; a value below zero gives 0, text
     * that is not a number gives 0 */
    const char *s = buf;
    bool negative = false;
    if (*s == '+' || *s == '-')
        negative = *s++ == '-';
    UINT v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10u + (UINT)(*s++ - '0');
    return negative ? 0u : v;
}

DWORD GetPrivateProfileSectionA(LPCSTR section, LPSTR out, DWORD size, LPCSTR file)
{
    if (out == nullptr || size == 0)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    File f;
    std::vector<std::string> items;
    size_t begin = 0, end = 0;
    if (section != nullptr && file != nullptr && load(file, f) && find_section(f, section, &begin, &end))
        for (size_t i = begin + 1; i < end; i++) {
            /* every line but the blank ones, comments included (the callers
             * skip them), "key=value" without the blanks around the '=' */
            const Parsed p = parse(f.lines[i].text);
            if (p.kind == Kind::key)
                items.push_back(p.name + "=" + p.value);
            else if (p.kind != Kind::blank)
                items.push_back(trim(f.lines[i].text));
        }
    return copy_list(items, out, size);
}

DWORD GetPrivateProfileSectionNamesA(LPSTR out, DWORD size, LPCSTR file)
{
    return GetPrivateProfileStringA(nullptr, nullptr, nullptr, out, size, file);
}

BOOL WritePrivateProfileStringA(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file)
{
    if (file == nullptr)
        return FALSE;
    if (section == nullptr)
        return key == nullptr && value == nullptr;  /* all NULL: "flush", nothing cached here */
    std::lock_guard<std::mutex> lock(g_mutex);
    File f;
    load(file, f);                          /* a missing file is written new */
    size_t begin = 0, end = 0;
    const bool has_section = find_section(f, section, &begin, &end);
    if (key == nullptr) {                   /* delete the section */
        if (!has_section)
            return TRUE;
        f.lines.erase(f.lines.begin() + (ptrdiff_t)begin, f.lines.begin() + (ptrdiff_t)end);
        return save(file, f);
    }
    size_t at = 0;
    Parsed p;
    const bool has_key = has_section && find_key(f, begin, end, key, &at, &p);
    if (value == nullptr) {                 /* delete the key */
        if (!has_key)
            return TRUE;
        f.lines.erase(f.lines.begin() + (ptrdiff_t)at);
        return save(file, f);
    }
    if (has_key) {
        f.lines[at].text = p.name + "=" + value;    /* the name keeps the file's spelling */
    } else if (has_section) {
        insert_line(f, insert_point(f, begin, end), std::string(key) + "=" + value);
    } else {
        append_section(f, section);
        insert_line(f, f.lines.size(), std::string(key) + "=" + value);
    }
    return save(file, f);
}

BOOL WritePrivateProfileSectionA(LPCSTR section, LPCSTR pairs, LPCSTR file)
{
    if (file == nullptr || section == nullptr)
        return FALSE;
    if (pairs == nullptr)
        return WritePrivateProfileStringA(section, nullptr, nullptr, file);
    std::lock_guard<std::mutex> lock(g_mutex);
    File f;
    load(file, f);
    size_t begin = 0, end = 0;
    if (!find_section(f, section, &begin, &end)) {
        append_section(f, section);
        begin = f.lines.size() - 1;
        end = f.lines.size();
    }
    /* the section's lines replaced by the pairs; its trailing blank lines stay */
    const size_t stop = insert_point(f, begin, end);
    f.lines.erase(f.lines.begin() + (ptrdiff_t)(begin + 1), f.lines.begin() + (ptrdiff_t)stop);
    size_t at = begin + 1;
    for (const char *s = pairs; *s != '\0'; s += strlen(s) + 1)
        insert_line(f, at++, s);
    return save(file, f);
}

#endif /* !_WIN32 */
