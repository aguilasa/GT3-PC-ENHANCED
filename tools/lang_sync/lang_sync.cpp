/* lang_sync - the interface's languages' catalogues (src/platform/ui_text.h,
 * docs/languages.md "The interface's languages").  Built and run by every
 * build (cmake/languages.cmake): no Python, nothing to install.
 *
 *   lang_sync --root <repo> --build <build dir>   what the build runs
 *   lang_sync                  extract, bring every lang/*.po up to date, check
 *   lang_sync --check          only check (nothing written); exit 1 on an error
 *   lang_sync --todo CODE      CODE's strings without a (current) translation
 *   lang_sync --todo CODE --json   the same as JSON (a translator's tool's input)
 *   lang_sync --fill CODE FILE.json   {"English": "translation", ...} into
 *                              lang/CODE.po (fuzzy cleared), then its check
 *   lang_sync --new CODE       start lang/CODE.po (a language ui_text.cpp knows)
 *   lang_sync --ask [--out DIR]   ask, in the console, for each untranslated
 *                              string's translation (build.cmd, between the
 *                              catalogues' sync and the programs' build:
 *                              blank skips one, skipall the rest; or e: all
 *                              of them into DIR\translate.txt - default
 *                              <root>\build - edited, then read back at Enter;
 *                              nothing is asked when no console is the input)
 *   lang_sync --status         what is still open and the errors (build.cmd's end)
 * (--root defaults to the folder above the one holding src/platform/ui_text.cpp,
 * searched up from the current folder.)
 *
 * English is the only language written in the code: tr("...") / tr(L"...")
 * translate where they are called, trf("... %s", ...) a sentence's format,
 * N_("...") only marks a table's string.  The tool:
 *   - extracts every tr / trf / N_ string of src/ (comments skipped) into
 *     lang/template.pot, with the files that use it;
 *   - brings each lang/<code>.po up to date as gettext's msgmerge does: a new
 *     string gets an empty translation; a string whose English changed keeps
 *     its old translation marked fuzzy with the old English (#| msgid) - the
 *     programs take no fuzzy one, they show the English until it is looked
 *     at; a string no longer used is kept at the end as obsolete (#~).  A
 *     file is written only when its text changes;
 *   - checks the translations: errors - a translation's printf conversions
 *     differ from the English's (ui_text_check.h: the programs show the
 *     English instead), a panel string (used outside src/launcher/, no lower
 *     case word: drawn by the overlay) with a character the overlay's font
 *     lacks (overlay_font.h; Japanese from MS Gothic); warnings - a panel
 *     line wider than the widest English one (the settings menu's panel
 *     would grow), a final newline unlike the English's.
 * --build writes <build dir>/languages.txt (what is left to do, how) and
 * languages_todo.txt (the strings per language), touches languages.stamp
 * when a catalogue changed (the programs' resources depend on it), prints a
 * short summary, and never fails the build (an untranslated string shows in
 * English; a broken translation is not taken). */
#include "platform/overlay_font.h"
#include "platform/ui_text.h"
#include "platform/ui_text_check.h"
#include "platform/log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* ui_text.cpp's log (the language list is ui_text.cpp's own) */
namespace pclog {
void vwrite(const char *, va_list) {}
void write(const char *, ...) {}
} // namespace pclog

namespace {

/* ---- UTF-8 ------------------------------------------------------------- */

void put_utf8(std::string &out, uint32_t c)
{
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

uint32_t next_utf8(const std::string &s, size_t &i)
{
    const unsigned char c = (unsigned char)s[i];
    const int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (n > 0 && i + n < s.size() + 0) {
        uint32_t v = c & (0x3F >> n);
        size_t k = 1;
        for (; k <= (size_t)n && i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80; k++)
            v = (v << 6) | ((unsigned char)s[i + k] & 0x3F);
        if (k > (size_t)n) {
            i += n + 1;
            return v;
        }
    }
    i += 1;
    return c;
}

/* The overlay's width in cells (a Japanese character two). */
int cells(const std::string &line)
{
    int n = 0;
    for (size_t i = 0; i < line.size();) {
        const uint32_t c = next_utf8(line, i);
        n += overlay_font::wide_range(c) && !(c >= 0xFF61 && c <= 0xFF9F) ? 2 : 1;
    }
    return n;
}

/* A character the overlay draws: its own font's, or MS Gothic's ranges. */
bool drawable(uint32_t c)
{
    if (c == ' ' || c == '\n')
        return true;
    overlay_font::Glyph g;
    if (overlay_font::narrow_glyph(overlay_font::upper(c), &g))
        return true;
    return c >= 0x80 && overlay_font::wide_range(c);
}

std::vector<std::string> split_lines(const std::string &s)
{
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find('\n', a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos)
            return out;
        a = b + 1;
    }
}

/* ---- the sources ------------------------------------------------------- */

std::string unescape(const std::string &body)
{
    std::string out;
    for (size_t i = 0; i < body.size();) {
        const char c = body[i];
        if (c != '\\' || i + 1 >= body.size()) {
            out += c;
            i++;
            continue;
        }
        const char e = body[i + 1];
        if (e == 'x') {
            size_t j = i + 2;
            uint32_t v = 0;
            while (j < body.size() && isxdigit((unsigned char)body[j]))
                v = v * 16 + (uint32_t)(isdigit((unsigned char)body[j]) ? body[j] - '0' : (tolower(body[j]) - 'a' + 10)), j++;
            put_utf8(out, v);
            i = j;
        } else if (e == 'u' || e == 'U') {
            const size_t n = e == 'u' ? 4 : 8;
            const uint32_t v = (uint32_t)strtoul(body.substr(i + 2, n).c_str(), nullptr, 16);
            put_utf8(out, v);
            i += 2 + n;
        } else if (e >= '0' && e <= '7' && !(e == '0' && !(i + 2 < body.size() && isdigit((unsigned char)body[i + 2])))) {
            size_t j = i + 1;
            uint32_t v = 0;
            for (int k = 0; k < 3 && j < body.size() && body[j] >= '0' && body[j] <= '7'; k++, j++)
                v = v * 8 + (uint32_t)(body[j] - '0');
            put_utf8(out, v);
            i = j;
        } else {
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '0': out += '\0'; break;
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            default: out += e; break;       /* \\ \" \' \? */
            }
            i += 2;
        }
    }
    return out;
}

struct Tok {
    int kind;                                   /* 0 a name, 1 a string literal, 2 another character */
    std::string text;
    int line;
};

bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Names, string literals (unescaped) and other characters; comments and
 * character literals skipped. */
std::vector<Tok> tokens(const std::string &t)
{
    std::vector<Tok> out;
    size_t i = 0;
    const size_t n = t.size();
    int line = 1;
    while (i < n) {
        const char c = t[i];
        if (c == '\n') {
            line++;
            i++;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            i++;
        } else if (t.compare(i, 2, "//") == 0) {
            const size_t j = t.find('\n', i);
            i = j == std::string::npos ? n : j;
        } else if (t.compare(i, 2, "/*") == 0) {
            size_t j = t.find("*/", i + 2);
            j = j == std::string::npos ? n : j + 2;
            line += (int)std::count(t.begin() + i, t.begin() + j, '\n');
            i = j;
        } else if (c == '\'') {
            size_t j = i + 1;
            while (j < n && t[j] != '\'' && t[j] != '\n')
                j += t[j] == '\\' ? 2 : 1;
            i = j + 1;
        } else {
            /* a string literal: (L|u8|u|U)? R? " */
            size_t p = i;
            if (t.compare(p, 2, "u8") == 0)
                p += 2;
            else if (t[p] == 'L' || t[p] == 'u' || t[p] == 'U')
                p++;
            bool raw = false;
            if (p < n && t[p] == 'R') {
                raw = true;
                p++;
            }
            if (p < n && t[p] == '"' && (i == 0 || !ident_char(t[i - 1]))) {
                const size_t start = i;
                std::string value;
                if (raw) {
                    const size_t d = t.find('(', p + 1);
                    const std::string delim = ")" + t.substr(p + 1, d - (p + 1)) + "\"";
                    const size_t j = t.find(delim, d);
                    value = t.substr(d + 1, j - (d + 1));
                    i = j + delim.size();
                } else {
                    size_t j = p + 1;
                    while (j < n && t[j] != '"')
                        j += t[j] == '\\' ? 2 : 1;
                    value = unescape(t.substr(p + 1, j - (p + 1)));
                    i = j + 1;
                }
                out.push_back({1, value, line});
                line += (int)std::count(t.begin() + start, t.begin() + std::min(i, n), '\n');
                continue;
            }
            if (isalpha((unsigned char)c) || c == '_') {
                size_t j = i;
                while (j < n && ident_char(t[j]))
                    j++;
                out.push_back({0, t.substr(i, j - i), line});
                i = j;
            } else {
                out.push_back({2, std::string(1, c), line});
                i++;
            }
        }
    }
    return out;
}

struct Ref {
    std::string file;
    int line;
};

/* The English strings in the sources' order, their places. */
struct Template {
    std::vector<std::string> order;
    std::map<std::string, std::vector<Ref>> refs;
};

/* src's files, a folder's own before its subfolders', each sorted */
void list_files(const fs::path &dir, const std::string &rel, std::vector<std::string> &out)
{
    std::vector<std::string> files, dirs;
    for (const auto &e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().u8string();
        if (e.is_directory())
            dirs.push_back(name);
        else
            files.push_back(name);
    }
    std::sort(files.begin(), files.end());
    std::sort(dirs.begin(), dirs.end());
    for (const std::string &f : files) {
        const std::string ext = fs::path(f).extension().u8string();
        if (ext == ".cpp" || ext == ".h" || ext == ".c" || ext == ".inc")
            out.push_back(rel + "/" + f);
    }
    for (const std::string &d : dirs)
        list_files(dir / fs::u8path(d), rel + "/" + d, out);
}

bool has_two_letters(const std::string &s)
{
    for (size_t i = 0; i + 1 < s.size(); i++)
        if (isalpha((unsigned char)s[i]) && (unsigned char)s[i] < 0x80 && isalpha((unsigned char)s[i + 1]) &&
            (unsigned char)s[i + 1] < 0x80)
            return true;
    return false;
}

std::string read_file(const fs::path &p, bool *ok = nullptr)
{
    std::ifstream f(p, std::ios::binary);
    if (ok != nullptr)
        *ok = (bool)f;
    std::ostringstream s;
    s << f.rdbuf();
    std::string t = s.str();
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF)
        t.erase(0, 3);
    return t;
}

Template extract(const fs::path &root)
{
    Template tpl;
    std::vector<std::string> files;
    list_files(root / "src", "src", files);
    for (const std::string &rel : files) {
        const std::string text = read_file(root / fs::u8path(rel));
        if (text.find("tr(") == std::string::npos && text.find("N_(") == std::string::npos)
            continue;
        const std::vector<Tok> toks = tokens(text);
        for (size_t k = 0; k + 2 < toks.size(); k++) {
            const Tok &t = toks[k];
            if (t.kind != 0 || (t.text != "tr" && t.text != "N_" && t.text != "trf"))
                continue;
            if (toks[k + 1].kind != 2 || toks[k + 1].text != "(")
                continue;
            if (k > 0 && toks[k - 1].kind == 2 && (toks[k - 1].text == "." || toks[k - 1].text == ">"))
                continue;                   /* (obj.tr(, ->tr(: not ours; ui_text::tr( is) */
            size_t j = k + 2;
            std::string s;
            bool any = false;
            while (j < toks.size() && toks[j].kind == 1) {
                s += toks[j].text;
                any = true;
                j++;
            }
            if (!any || j >= toks.size() || toks[j].kind != 2)
                continue;
            if (!(toks[j].text == ")" || (t.text == "trf" && toks[j].text == ",")))
                continue;
            if (!has_two_letters(s))
                continue;                   /* nothing to translate */
            if (tpl.refs.find(s) == tpl.refs.end())
                tpl.order.push_back(s);
            tpl.refs[s].push_back({rel, t.line});
        }
    }
    return tpl;
}

/* A string the overlay draws: used outside the launcher's own sources, and
 * upper case (a printf conversion's or a single letter's lower case is no
 * word: "%d CARS", "X100"); a mixed case one used outside is a shared table
 * the launcher shows (bgm_files.cpp's events, input_map.cpp's names). */
bool is_panel(const std::vector<Ref> &refs, const std::string &english)
{
    std::string words;
    for (size_t i = 0; i < english.size(); i++) {
        if (english[i] != '%') {
            words += english[i];
            continue;
        }
        size_t j = i + 1;
        while (j < english.size() && strchr("-+ #0", english[j]) != nullptr && english[j] != '\0')
            j++;
        while (j < english.size() && (isdigit((unsigned char)english[j]) || english[j] == '*'))
            j++;
        if (j < english.size() && english[j] == '.')
            for (j++; j < english.size() && (isdigit((unsigned char)english[j]) || english[j] == '*'); j++) {
            }
        for (const char *len : {"hh", "ll", "I64", "I32", "h", "l", "L", "z", "j", "t", "I", "w"})
            if (english.compare(j, strlen(len), len) == 0) {
                j += strlen(len);
                break;
            }
        if (j < english.size() && isalpha((unsigned char)english[j]) && (unsigned char)english[j] < 0x80)
            i = j;                          /* (the conversion left out) */
        else
            words += english[i];
    }
    for (size_t i = 0; i + 1 < words.size(); i++)
        if (words[i] >= 'a' && words[i] <= 'z' && words[i + 1] >= 'a' && words[i + 1] <= 'z')
            return false;
    for (const Ref &r : refs)
        if (r.file.compare(0, 13, "src/launcher/") != 0)
            return true;
    return false;
}

/* ---- .po files --------------------------------------------------------- */

struct Entry {
    bool has_id = false;
    std::string msgid, msgstr;
    std::vector<std::string> refs;              /* files */
    std::set<std::string> flags;
    bool has_previous = false;
    std::string previous;
    std::vector<std::string> comments;          /* "# ..." */
    bool obsolete = false;
};

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        b--;
    return s.substr(a, b - a);
}

/* a quoted .po string's text */
std::string po_unquote(const std::string &s)
{
    const std::string t = trim(s);
    if (t.size() < 2 || t.front() != '"' || t.back() != '"')
        return std::string();
    return unescape(t.substr(1, t.size() - 2));
}

std::vector<Entry> parse_po(const std::string &text)
{
    std::vector<Entry> entries;
    Entry cur;
    int field = 0;                              /* 1 msgid, 2 msgstr */
    auto flush = [&]() {
        if (cur.has_id)
            entries.push_back(cur);
        cur = Entry();
        field = 0;
    };
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const bool obsolete = line.compare(0, 2, "#~") == 0;
        if (obsolete) {
            line = trim(line.substr(2));
            if (!line.empty() && line[0] == '|')
                continue;
        }
        if (trim(line).empty()) {
            flush();
            continue;
        }
        if (!obsolete && line[0] == '#') {
            if (field == 2)
                flush();
            if (line.compare(0, 2, "#:") == 0) {
                std::istringstream refs(line.substr(2));
                std::string ref;
                while (refs >> ref) {
                    /* (a "file:line" of an earlier tool: the file) */
                    const size_t colon = ref.rfind(':');
                    if (colon != std::string::npos && colon + 1 < ref.size() &&
                        ref.find_first_not_of("0123456789", colon + 1) == std::string::npos)
                        ref.erase(colon);
                    if (std::find(cur.refs.begin(), cur.refs.end(), ref) == cur.refs.end())
                        cur.refs.push_back(ref);
                }
            } else if (line.compare(0, 2, "#,") == 0) {
                std::istringstream flags(line.substr(2));
                std::string f;
                while (std::getline(flags, f, ','))
                    if (!trim(f).empty())
                        cur.flags.insert(trim(f));
            } else if (line.compare(0, 2, "#|") == 0) {
                const std::string rest = trim(line.substr(2));
                if (rest.compare(0, 6, "msgid ") == 0) {
                    cur.has_previous = true;
                    cur.previous = po_unquote(rest.substr(6));
                } else if (!rest.empty() && rest[0] == '"' && cur.has_previous) {
                    cur.previous += po_unquote(rest);
                }
            } else if (line.compare(0, 2, "# ") == 0 || line == "#") {
                cur.comments.push_back(line.size() > 2 ? line.substr(2) : std::string());
            }
            continue;
        }
        if (obsolete)
            cur.obsolete = true;
        if (line.compare(0, 6, "msgid ") == 0) {
            if (field == 2) {
                flush();
                cur.obsolete = obsolete;
            }
            cur.has_id = true;
            cur.msgid = po_unquote(line.substr(6));
            field = 1;
        } else if (line.compare(0, 7, "msgstr ") == 0) {
            cur.msgstr = po_unquote(line.substr(7));
            field = 2;
        } else if (line[0] == '"') {
            if (field == 1)
                cur.msgid += po_unquote(line);
            else if (field == 2)
                cur.msgstr += po_unquote(line);
        }
    }
    flush();
    return entries;
}

std::string po_quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default: out += c; break;
        }
    }
    return out + "\"";
}

void po_field(std::vector<std::string> &out, const char *name, const std::string &s, const std::string &prefix)
{
    std::string body = s;
    while (!body.empty() && body.back() == '\n')
        body.pop_back();
    if (s.empty() || body.find('\n') == std::string::npos) {
        out.push_back(prefix + name + " " + po_quote(s));
        return;
    }
    out.push_back(prefix + name + " \"\"");
    const std::vector<std::string> parts = split_lines(s);
    for (size_t k = 0; k < parts.size(); k++) {
        const std::string piece = parts[k] + (k + 1 < parts.size() ? "\n" : "");
        if (!piece.empty())
            out.push_back(prefix + po_quote(piece));
    }
}

void po_refs(std::vector<std::string> &out, const std::vector<std::string> &refs)
{
    std::string line = "#:";
    for (const std::string &r : refs) {
        if (line.size() + 1 + r.size() > 100 && line != "#:") {
            out.push_back(line);
            line = "#:";
        }
        line += " " + r;
    }
    if (line != "#:")
        out.push_back(line);
}

std::string po_text(const std::string &code, const std::vector<Entry> &entries)
{
    std::vector<std::string> out = {
        "# GT3 PC's interface text (src/platform/ui_text.h): " + code + ".",
        "# Brought up to date by every build (tools/lang_sync); the translations are edited here (msgstr).",
        "msgid \"\"",
        "msgstr \"\"",
        "\"Content-Type: text/plain; charset=UTF-8\\n\"",
        "\"Language: " + code + "\\n\"",
        ""};
    for (const Entry &e : entries) {
        for (const std::string &c : e.comments)
            out.push_back(c.empty() ? "#" : "# " + c);
        if (!e.obsolete)
            po_refs(out, e.refs);
        if (!e.flags.empty()) {
            std::string f = "#,";
            bool first = true;
            for (const std::string &x : e.flags) {
                f += (first ? " " : ", ") + x;
                first = false;
            }
            out.push_back(f);
        }
        if (e.has_previous && !e.obsolete)
            po_field(out, "msgid", e.previous, "#| ");
        const std::string prefix = e.obsolete ? "#~ " : "";
        po_field(out, "msgid", e.msgid, prefix);
        po_field(out, "msgstr", e.msgstr, prefix);
        out.push_back("");
    }
    std::string text;
    for (size_t i = 0; i < out.size(); i++)
        text += out[i] + (i + 1 < out.size() ? "\n" : "");
    return text;
}

std::string template_text(const Template &tpl)
{
    std::vector<std::string> out = {"# GT3 PC's interface text: the English strings (tools/lang_sync; not built in).",
                                    "msgid \"\"", "msgstr \"\"", "\"Content-Type: text/plain; charset=UTF-8\\n\"", ""};
    for (const std::string &s : tpl.order) {
        std::vector<std::string> files;
        for (const Ref &r : tpl.refs.at(s))
            if (std::find(files.begin(), files.end(), r.file) == files.end())
                files.push_back(r.file);
        po_refs(out, files);
        po_field(out, "msgid", s, "");
        out.push_back("msgstr \"\"");
        out.push_back("");
    }
    std::string text;
    for (size_t i = 0; i < out.size(); i++)
        text += out[i] + (i + 1 < out.size() ? "\n" : "");
    return text;
}

/* written only when its text changed (the resources depend on the files);
 * false: it could not be */
bool write_if_changed(const fs::path &p, const std::string &text, bool *changed)
{
    bool ok = false;
    const std::string old = read_file(p, &ok);
    if (ok && old == text) {
        *changed = false;
        return true;
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f << text;
    *changed = true;
    return (bool)f;
}

/* ---- the update -------------------------------------------------------- */

/* how alike two strings are, 0 .. 1 (2 x their longest common subsequence
 * over their lengths: difflib's ratio, nearly) */
double similarity(const std::string &a, const std::string &b)
{
    if (a.empty() && b.empty())
        return 1.0;
    std::vector<int> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    for (size_t i = 1; i <= a.size(); i++) {
        for (size_t j = 1; j <= b.size(); j++)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : std::max(prev[j], cur[j - 1]);
        std::swap(prev, cur);
    }
    return 2.0 * prev[b.size()] / (double)(a.size() + b.size());
}

std::vector<std::string> files_of(const std::vector<Ref> &refs)
{
    std::vector<std::string> files;
    for (const Ref &r : refs)
        if (std::find(files.begin(), files.end(), r.file) == files.end())
            files.push_back(r.file);
    return files;
}

std::vector<Entry> update(const std::vector<Entry> &old, const Template &tpl)
{
    std::map<std::string, const Entry *> byId;
    for (const Entry &e : old)
        if (!e.msgid.empty() && byId.find(e.msgid) == byId.end())
            byId[e.msgid] = &e;
    std::vector<const Entry *> gone;
    for (const Entry &e : old)
        if (!e.msgid.empty() && tpl.refs.find(e.msgid) == tpl.refs.end() && !e.msgstr.empty())
            gone.push_back(&e);
    std::vector<Entry> out;
    std::set<std::string> used;
    for (const std::string &s : tpl.order) {
        Entry n;
        n.has_id = true;
        n.msgid = s;
        n.refs = files_of(tpl.refs.at(s));
        const auto it = byId.find(s);
        if (it != byId.end()) {
            const Entry &e = *it->second;
            n.msgstr = e.msgstr;
            n.flags = e.flags;
            n.comments = e.comments;
            n.has_previous = e.has_previous && e.flags.count("fuzzy") != 0;
            n.previous = n.has_previous ? e.previous : std::string();
            used.insert(s);
        } else {
            /* an English text changed: the old one's translation, to be looked at */
            const Entry *best = nullptr;
            double score = 0.0;
            for (const Entry *g : gone) {
                if (used.count(g->msgid) != 0)
                    continue;
                double r = similarity(s, g->msgid);
                for (const std::string &f : g->refs)
                    if (std::find(n.refs.begin(), n.refs.end(), f) != n.refs.end()) {
                        r += 0.1;
                        break;
                    }
                if (r > score) {
                    best = g;
                    score = r;
                }
            }
            if (best != nullptr && score >= 0.75) {
                used.insert(best->msgid);
                n.msgstr = best->msgstr;
                n.has_previous = true;
                n.previous = best->msgid;
                n.flags = {"fuzzy"};
            }
        }
        out.push_back(n);
    }
    for (const Entry &e : old) {
        if (e.msgid.empty() || tpl.refs.find(e.msgid) != tpl.refs.end() || used.count(e.msgid) != 0 || e.msgstr.empty())
            continue;
        Entry o = e;
        o.obsolete = true;
        o.refs.clear();
        o.has_previous = false;
        o.previous.clear();
        out.push_back(o);
    }
    return out;
}

/* ---- checks ------------------------------------------------------------ */

struct Issue {
    bool error;
    std::string text;
};

std::string where(const std::vector<Ref> &refs)
{
    return refs.empty() ? std::string("?") : refs[0].file + ":" + std::to_string(refs[0].line);
}

std::string shorten(const std::string &s, size_t n = 60)
{
    std::string t;
    for (char c : s)
        t += c == '\n' ? std::string("\\n") : std::string(1, c);
    if (t.size() > n) {
        size_t cut = n;
        while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80)
            cut--;
        t = t.substr(0, cut) + "...";
    }
    return t;
}

int widest_panel_line(const Template &tpl)
{
    int w = 0;
    for (const std::string &s : tpl.order)
        if (is_panel(tpl.refs.at(s), s))
            for (const std::string &l : split_lines(s))
                w = std::max(w, cells(l));
    return w;
}

/* One translation's problems (at: "fr: file:line" before each); fuzzy: its
 * English changed (only the conversions matter then: it is not used). */
void check_one(const std::string &at, const std::string &msgid, const std::string &msgstr, bool fuzzy,
               const std::vector<Ref> &refs, int widest, std::vector<Issue> &out)
{
    if (!ui_text_check::conversions_match(msgid, msgstr))
        out.push_back({true, at + ": its printf conversions (" + ui_text_check::conversions(msgstr) +
                                 ") are not the English's (" + ui_text_check::conversions(msgid) +
                                 "): the English shows instead - \"" + shorten(msgid) + "\""});
    if (fuzzy)
        return;
    if (is_panel(refs, msgid)) {
        std::string bad;
        for (size_t i = 0; i < msgstr.size();) {
            const size_t a = i;
            const uint32_t c = next_utf8(msgstr, i);
            if (!drawable(c) && bad.find(msgstr.substr(a, i - a)) == std::string::npos) {
                char u[16];
                snprintf(u, sizeof(u), " (U+%04X)", (unsigned)c);
                bad += (bad.empty() ? "" : ", ") + msgstr.substr(a, i - a) + u;
            }
        }
        if (!bad.empty())
            out.push_back({true, at + ": the panels' font has no " + bad + " - \"" + shorten(msgid) + "\""});
        int limit = widest;
        for (const std::string &l : split_lines(msgid))
            limit = std::max(limit, cells(l));
        for (const std::string &l : split_lines(msgstr))
            if (cells(l) > limit)
                out.push_back({false, at + ": a line of " + std::to_string(cells(l)) + " cells, wider than the panel's " +
                                          std::to_string(limit) + " - \"" + shorten(l) + "\""});
    }
    const bool en = !msgid.empty() && msgid.back() == '\n', tr = !msgstr.empty() && msgstr.back() == '\n';
    if (en != tr)
        out.push_back({false, at + ": the English " + (en ? std::string("ends") : std::string("does not end")) +
                                  " with a new line, the translation not - \"" + shorten(msgid) + "\""});
}

void check(const std::string &code, const std::vector<Entry> &entries, const Template &tpl, std::vector<Issue> &out)
{
    const int widest = widest_panel_line(tpl);
    for (const Entry &e : entries) {
        if (e.obsolete || e.msgstr.empty())
            continue;
        const auto it = tpl.refs.find(e.msgid);
        if (it == tpl.refs.end())
            continue;
        check_one(code + ": " + where(it->second), e.msgid, e.msgstr, e.flags.count("fuzzy") != 0, it->second, widest,
                  out);
    }
}

/* ---- JSON (--todo --json, --fill) --------------------------------------- */

std::string json_quote(const std::string &s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", c);
                out += u;
            } else {
                out += (char)c;
            }
        }
    }
    return out + "\"";
}

/* {"English": "translation", ...}; false and a reason when it is not one */
bool parse_json_object(const std::string &t, std::vector<std::pair<std::string, std::string>> &out, std::string &why)
{
    size_t i = 0;
    auto ws = [&]() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n'))
            i++;
    };
    auto str = [&](std::string &s) -> bool {
        ws();
        if (i >= t.size() || t[i] != '"')
            return false;
        for (i++; i < t.size() && t[i] != '"'; i++) {
            if (t[i] != '\\') {
                s += t[i];
                continue;
            }
            if (++i >= t.size())
                return false;
            switch (t[i]) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u': {
                uint32_t v = (uint32_t)strtoul(t.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (v >= 0xD800 && v <= 0xDBFF && t.compare(i + 1, 2, "\\u") == 0) {
                    const uint32_t lo = (uint32_t)strtoul(t.substr(i + 3, 4).c_str(), nullptr, 16);
                    v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                put_utf8(s, v);
                break;
            }
            default: s += t[i]; break;      /* \" \\ \/ */
            }
        }
        if (i >= t.size())
            return false;
        i++;
        return true;
    };
    ws();
    if (i < t.size() && (unsigned char)t[i] == 0xEF)
        i += 3;                             /* (a BOM) */
    ws();
    if (i >= t.size() || t[i] != '{') {
        why = "not a JSON object";
        return false;
    }
    i++;
    ws();
    if (i < t.size() && t[i] == '}')
        return true;
    for (;;) {
        std::string k, v;
        if (!str(k)) {
            why = "a key is not a string";
            return false;
        }
        ws();
        if (i >= t.size() || t[i] != ':') {
            why = "no ':' after \"" + shorten(k) + "\"";
            return false;
        }
        i++;
        if (!str(v)) {
            why = "the value of \"" + shorten(k) + "\" is not a string";
            return false;
        }
        out.push_back({k, v});
        ws();
        if (i < t.size() && t[i] == ',') {
            i++;
            continue;
        }
        if (i < t.size() && t[i] == '}')
            return true;
        why = "no ',' or '}' after \"" + shorten(k) + "\"";
        return false;
    }
}

/* ---- the program ------------------------------------------------------- */

struct Lang {
    std::string code;
    fs::path path;
};

std::vector<Lang> languages(const fs::path &root)
{
    std::vector<Lang> out;
    for (int i = ui_text::kEnglish + 1; i < ui_text::kCount; i++)
        out.push_back({ui_text::code(i), root / "lang" / (std::string(ui_text::code(i)) + ".po")});
    return out;
}

std::vector<Entry> load(const fs::path &p)
{
    bool ok = false;
    const std::string text = read_file(p, &ok);
    return ok ? parse_po(text) : std::vector<Entry>();
}

struct Counts {
    int done = 0, fuzzy = 0, missing = 0, obsolete = 0;
};

Counts count(const std::vector<Entry> &entries)
{
    Counts c;
    for (const Entry &e : entries) {
        if (e.obsolete)
            c.obsolete++;
        else if (e.msgstr.empty())
            c.missing++;
        else if (e.flags.count("fuzzy") != 0)
            c.fuzzy++;
        else
            c.done++;
    }
    return c;
}

/* ---- --ask: the translations typed in the console (build.cmd) ----------- */

/* The language's name in English (the prompts are). */
const char *english_name(const std::string &code)
{
    static const std::pair<const char *, const char *> kNames[] = {
        {"fr", "French"},  {"de", "German"},  {"it", "Italian"},
        {"es", "Spanish"}, {"ja", "Japanese"}, {"ru", "Russian"},
        {"pt_BR", "Brazilian Portuguese"},    {"pl", "Polish"}};
    for (const auto &n : kNames)
        if (code == n.first)
            return n.second;
    return code.c_str();
}

/* The console's input: a person types the answers (else nothing is asked:
 * a tool or a script runs the build).  GT3PC_LANG_ASK_INPUT=1: the answers
 * from the standard input whatever it is (a test's file of answers, UTF-8). */
bool is_console()
{
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

bool console_input()
{
#ifdef _WIN32
    char v[8];
    return is_console() || (GetEnvironmentVariableA("GT3PC_LANG_ASK_INPUT", v, sizeof(v)) > 0 && v[0] == '1');
#else
    const char *v = getenv("GT3PC_LANG_ASK_INPUT");
    return is_console() || (v != nullptr && v[0] == '1');
#endif
}

/* A line typed (UTF-8, without its end); false: the input ended. */
bool read_line(std::string &out)
{
    out.clear();
#ifdef _WIN32
    if (!is_console()) {
        char b[8192];
        if (fgets(b, sizeof(b), stdin) == nullptr)
            return false;
        out = b;
        if (out.compare(0, 3, "\xEF\xBB\xBF") == 0)
            out.erase(0, 3);                /* (a pipe's byte order mark) */
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        printf("%s\n", out.c_str());        /* (the answer as a person would see it typed) */
        return true;
    }
#else
    /* (a terminal's input is UTF-8 here: a line read as it is, echoed by
     * the terminal itself) */
    const bool console = is_console();
    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n')
        out += (char)ch;
    if (ch == EOF && out.empty())
        return false;
    if (out.compare(0, 3, "\xEF\xBB\xBF") == 0)
        out.erase(0, 3);                    /* (a pipe's byte order mark) */
    while (!out.empty() && out.back() == '\r')
        out.pop_back();
    if (!console)
        printf("%s\n", out.c_str());       /* (the answer as a person would see it typed) */
    return true;
#endif
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring w;
    wchar_t buf[512];
    for (;;) {
        DWORD n = 0;
        if (!ReadConsoleW(in, buf, 512, &n, nullptr) || n == 0)
            return false;
        w.append(buf, n);
        if (w.back() == L'\n')
            break;
    }
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r'))
        w.pop_back();
    if (!w.empty()) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        out.resize((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], len, nullptr, nullptr);
    }
    return true;
#endif
}

/* The upper case of a UTF-8 string (the panels' strings: CharUpper's
 * Unicode cases - accents, Cyrillic). */
#ifndef _WIN32
/* CharUpper's simple (one character) upper case of the scripts the
 * catalogues use: Latin-1, Latin Extended-A, Greek, Cyrillic; ß and the
 * rest as they are. */
unsigned upper_cp(unsigned c)
{
    if (c >= 'a' && c <= 'z')
        return c - 0x20;
    if (c < 0x80)
        return c;
    if (c == 0xB5)
        return 0x39C;                           /* micro sign: Greek MU */
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 0x20;
    if (c == 0xFF)
        return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x131)
            return 'I';                         /* dotless i */
        if (c == 0x17F)
            return 'S';                         /* long s */
        if (c == 0x138 || c == 0x149)
            return c;                           /* kra, 'n: no upper case */
        const bool oddLower = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (oddLower)
            return (c & 1) == 0 ? c - 1 : c;
        return (c & 1) != 0 ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2)
        return c - 0x20;                        /* Greek */
    if (c == 0x3C2)
        return 0x3A3;                           /* final sigma */
    if (c == 0x3AC)
        return 0x386;
    if (c >= 0x3AD && c <= 0x3AF)
        return c - 0x25;
    if (c == 0x3CC)
        return 0x38C;
    if (c == 0x3CD || c == 0x3CE)
        return c - 0x3F;
    if (c >= 0x430 && c <= 0x44F)
        return c - 0x20;                        /* Cyrillic */
    if (c >= 0x450 && c <= 0x45F)
        return c - 0x50;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return (c & 1) != 0 ? c - 1 : c;
    if (c >= 0x4C1 && c <= 0x4CE)
        return (c & 1) == 0 ? c - 1 : c;
    if (c == 0x4CF)
        return 0x4C0;
    return c;
}
#endif

std::string upper_utf8(const std::string &s)
{
    if (s.empty())
        return s;
#ifndef _WIN32
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = (unsigned char)s[i];
        const size_t n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n != 0 && i + n <= s.size();
        unsigned c = n == 1 ? b : n == 2 ? b & 0x1Fu : n == 3 ? b & 0x0Fu : b & 0x07u;
        for (size_t k = 1; ok && k < n; k++) {
            ok = ((unsigned char)s[i + k] & 0xC0) == 0x80;
            c = (c << 6) | ((unsigned char)s[i + k] & 0x3Fu);
        }
        if (!ok) {
            out += s[i++];                      /* (not UTF-8: the byte as it is) */
            continue;
        }
        i += n;
        c = upper_cp(c);
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
#else
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    CharUpperBuffW(&w[0], (DWORD)w.size());
    n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
#endif
}

std::string trim_ascii(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

/* The English's printf conversions as written ("%s, %.1f"), for the prompt. */
std::string conversion_list(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%')
            continue;
        if (i + 1 < s.size() && s[i + 1] == '%') {
            i++;
            continue;
        }
        size_t j = i + 1;
        while (j < s.size() && !isalpha((unsigned char)s[j]))
            j++;
        while (j < s.size() && strchr("hlLzjtwI", s[j]) != nullptr && j + 1 < s.size() && isalpha((unsigned char)s[j + 1]))
            j++;
        out += (out.empty() ? "" : ", ") + s.substr(i, j + 1 - i);
        i = j;
    }
    return out;
}

/* The English as the prompts show it: one line, or each of its lines. */
void print_english(const std::string &s, const char *indent)
{
    const std::vector<std::string> lines = split_lines(s);
    if (lines.size() == 1) {
        printf("\"%s\"", s.c_str());
        return;
    }
    for (size_t i = 0; i < lines.size(); i++)
        printf("\n%s\"%s%s\"", indent, lines[i].c_str(), i + 1 < lines.size() ? "\\n" : "");
}

/* An answer as written (typed, or in the export file): \n a new line, \\ a
 * backslash; the English's own spaces at its ends and its last new line
 * kept; a panel's (an in-game menu's text) in upper case. */
std::string prepare(const std::string &english, const std::string &written, bool panel)
{
    std::string s;
    for (size_t i = 0; i < written.size(); i++) {
        if (written[i] == '\\' && i + 1 < written.size() && (written[i + 1] == 'n' || written[i + 1] == '\\')) {
            s += written[i + 1] == 'n' ? '\n' : '\\';
            i++;
        } else if (written[i] != '\r') {
            s += written[i];
        }
    }
    auto blank = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && (blank(s.back()) || s.back() == '\n'))
        s.pop_back();
    size_t a = 0;
    while (a < s.size() && blank(s[a]))
        a++;
    s.erase(0, a);
    size_t lead = 0;
    while (lead < english.size() && english[lead] == ' ')
        lead++;
    std::string body = english;
    const bool newline = !body.empty() && body.back() == '\n';
    if (newline)
        body.pop_back();
    size_t trail = 0;
    while (trail < body.size() && body[body.size() - 1 - trail] == ' ')
        trail++;
    s = std::string(lead, ' ') + s + std::string(trail, ' ') + (newline ? "\n" : "");
    return panel ? upper_utf8(s) : s;
}

/* Why a translation is not taken (the programs' rules), and notes. */
struct Verdict {
    std::vector<std::string> errors, notes;
};

Verdict judge(const std::string &english, const std::string &s, const std::vector<Ref> &refs, int widest)
{
    Verdict v;
    if (!ui_text_check::conversions_match(english, s)) {
        v.errors.push_back(ui_text_check::conversions(english).empty()
                               ? "the English has no %s / %d values - write none (a percent sign: %%)"
                               : "it needs the English's " + conversion_list(english) + ", in that order");
        return v;
    }
    std::vector<Issue> issues;
    check_one("@", english, s, false, refs, widest, issues);
    for (const Issue &i : issues) {
        std::string t = i.text.compare(0, 3, "@: ") == 0 ? i.text.substr(3) : i.text;
        const size_t quoted = t.rfind(" - \"");             /* (the English again: the prompt shows it) */
        if (quoted != std::string::npos)
            t.erase(quoted);
        (i.error ? v.errors : v.notes).push_back(t);
    }
    return v;
}

/* The translation into its language's catalogue, the file written at once
 * (an interrupted session keeps what was given). */
bool apply(std::vector<Entry> &book, const Lang &lang, const std::string &english, const std::string &s)
{
    for (Entry &e : book)
        if (!e.obsolete && e.msgid == english) {
            e.msgstr = s;
            e.flags.erase("fuzzy");
            e.has_previous = false;
            e.previous.clear();
            bool changed = false;
            return write_if_changed(lang.path, po_text(lang.code, book), &changed);
        }
    return false;
}

/* The export file's line form: \ and new lines escaped. */
std::string escape_line(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c != '\r')
            out += c;
    }
    return out;
}

struct Item {
    size_t lang;                                /* index in langs / books */
    std::string id;
};

/* PC (2026-11-16, user request): "an option to export the open items to a
 * file to be edited, then given back to the CLI.  The format stays simple,
 * grouped by language.  Each line in a group is <english string> = <blank,
 * to be filled in>".  The file written (UTF-8, its notes as # lines), the
 * person edits and saves it and presses Enter; each translation filled in is
 * prepared and judged as a typed one, the ones taken written to their .po;
 * the ones not taken listed - fixed in the file, Enter reads it again
 * (skipall: on with the build). */
void export_and_read(const fs::path &file, const Template &tpl, const std::vector<Lang> &langs,
                     std::vector<std::vector<Entry>> &books, const std::vector<Item> &items)
{
    /* the file */
    std::string text = "\xEF\xBB\xBF"
                       "# GT3 PC - the interface's translations to write (tools/lang_sync, build.cmd).\r\n"
                       "# After each \" = \", write the translation of the English before it; leave it empty to skip it.\r\n"
                       "# \\n is a new line.  Keep the English's %s / %d / %.1f ... values, in the same order (a percent\r\n"
                       "# sign in such a string: %%).  An in-game menu's text is put in upper case.  # lines are notes.\r\n"
                       "# Save the file, then press Enter in the build's console.\r\n";
    std::vector<std::vector<std::pair<std::string, std::string>>> keys(langs.size());   /* (written, English) */
    size_t last = (size_t)-1;
    for (const Item &it : items) {
        if (it.lang != last) {
            text += "\r\n[" + std::string(english_name(langs[it.lang].code)) + " (" + langs[it.lang].code + ")]\r\n";
            last = it.lang;
        }
        for (const Entry &e : books[it.lang])
            if (!e.obsolete && e.msgid == it.id && e.flags.count("fuzzy") != 0 && e.has_previous)
                text += "# its English changed - it was \"" + escape_line(e.previous) + "\", translated \"" +
                        escape_line(e.msgstr) + "\"\r\n";
        text += escape_line(it.id) + " = \r\n";
        keys[it.lang].push_back({escape_line(it.id), it.id});
    }
    for (auto &k : keys)                        /* (the longest first: an English with " = " in it) */
        std::sort(k.begin(), k.end(), [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) {
            printf("\n%s could not be written: nothing exported.\n\n", file.u8string().c_str());
            return;
        }
    }
    printf("\nThe strings to be translated are at %s.\nUpdate the file then press Enter. ", file.u8string().c_str());
    fflush(stdout);

    const int widest = widest_panel_line(tpl);
    int taken = 0;
    std::set<std::pair<size_t, std::string>> done;      /* (taken already: a second reading skips them) */
    for (;;) {
        std::string answer;
        if (!read_line(answer) || trim_ascii(answer) == "skipall" || trim_ascii(answer) == "SKIPALL")
            break;
        bool ok = false;
        const std::string body = read_file(file, &ok);
        if (!ok) {
            printf("%s cannot be read. Press Enter to try again, or type skipall to finish the build. ",
                   file.u8string().c_str());
            fflush(stdout);
            continue;
        }
        std::vector<std::string> refused, unknown;
        int now = 0;
        long cur = -1;
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (trim_ascii(line).empty() || line[0] == '#')
                continue;
            if (line[0] == '[' && line.back() == ']') {
                cur = -1;
                const size_t o = line.rfind('('), c = line.rfind(')');
                const std::string code = o != std::string::npos && c > o ? line.substr(o + 1, c - o - 1) : "";
                for (size_t l = 0; l < langs.size(); l++)
                    if (langs[l].code == code)
                        cur = (long)l;
                if (cur < 0)
                    unknown.push_back(line + " (no language of this tree: its lines are left out)");
                continue;
            }
            if (cur < 0)
                continue;
            const std::pair<std::string, std::string> *key = nullptr;
            std::string value;
            for (const auto &k : keys[(size_t)cur]) {
                if (line.compare(0, k.first.size(), k.first) != 0)
                    continue;
                size_t i = k.first.size();
                while (i < line.size() && line[i] == ' ')
                    i++;
                if (i < line.size() && line[i] == '=') {
                    key = &k;
                    value = line.substr(i + 1);
                    if (!value.empty() && value[0] == ' ')
                        value.erase(0, 1);
                    break;
                }
            }
            if (key == nullptr) {
                unknown.push_back(line);
                continue;
            }
            if (trim_ascii(value).empty() || done.count({(size_t)cur, key->second}) != 0)
                continue;
            const Lang &lang = langs[(size_t)cur];
            const std::vector<Ref> &refs = tpl.refs.at(key->second);
            const std::string s = prepare(key->second, value, is_panel(refs, key->second));
            const Verdict v = judge(key->second, s, refs, widest);
            if (!v.errors.empty()) {
                refused.push_back(std::string(english_name(lang.code)) + ", \"" + shorten(key->second, 50) +
                                  "\": " + v.errors[0]);
                continue;
            }
            for (const std::string &n : v.notes)
                printf("  Note (%s, \"%s\"): %s\n", lang.code.c_str(), shorten(key->second, 40).c_str(), n.c_str());
            if (!apply(books[(size_t)cur], lang, key->second, s)) {
                refused.push_back(std::string(english_name(lang.code)) + ", \"" + shorten(key->second, 50) +
                                  "\": lang\\" + lang.code + ".po could not be written (read-only? open in an editor?)");
                continue;
            }
            done.insert({(size_t)cur, key->second});
            now++;
        }
        taken += now;
        printf("\nTranslated %d from the file (%d in all).\n", now, taken);
        for (const std::string &u : unknown)
            printf("  Not one of the strings to translate (left out): %s\n", shorten(u, 90).c_str());
        if (refused.empty())
            break;
        printf("Not taken:\n");
        for (const std::string &r : refused)
            printf("  %s\n", r.c_str());
        printf("Fix them in the file and press Enter to read it again, or type skipall to finish the build. ");
        fflush(stdout);
    }
    const int left = (int)items.size() - taken;
    printf("\n%d translated, %d left%s.\n\n", taken, left,
           left > 0 ? " (they show in English; the next build asks again)" : "");
}

/* PC (2026-11-16, user request): "During the build process, the console asks
 * for the user's translation of each open item directly ... User can also
 * skip all in the middle".  Each language's untranslated strings and those
 * whose English changed, one by one: a translation typed is checked (the
 * programs' rules: conversions, the panels' characters - else asked again)
 * and written to its .po at once; blank skips it, skipall the rest.  Or
 * (e) all of them exported to a file, edited, read back (export_and_read). */
int ask(const fs::path &root, const fs::path &out, const Template &tpl, const std::vector<Lang> &langs)
{
    (void)root;
    if (!console_input())
        return 0;                           /* (a tool or a script runs the build: nobody to ask) */
    std::vector<std::vector<Entry>> books(langs.size());
    std::vector<Item> items;
    std::string per;
    for (size_t l = 0; l < langs.size(); l++) {
        if (!fs::exists(langs[l].path))
            continue;
        books[l] = update(load(langs[l].path), tpl);
        int n = 0;
        for (const Entry &e : books[l])
            if (!e.obsolete && (e.msgstr.empty() || e.flags.count("fuzzy") != 0)) {
                items.push_back({l, e.msgid});
                n++;
            }
        if (n > 0)
            per += (per.empty() ? "" : ", ") + std::string(english_name(langs[l].code)) + " " + std::to_string(n);
    }
    if (items.empty())
        return 0;
    printf("\nThere are %d untranslated strings (%s). Do you want to translate them now?\n"
           "(y = yes, e = export all to a file, n / Enter = no) ",
           (int)items.size(), per.c_str());
    fflush(stdout);
    std::string answer;
    const bool got = read_line(answer);
    const std::string a = trim_ascii(answer);
    if (got && (a == "e" || a == "E")) {
        std::error_code ec;
        export_and_read(fs::absolute(out / "translate.txt", ec).lexically_normal(), tpl, langs, books, items);
        return 0;
    }
    if (!got || (a != "y" && a != "Y")) {
        printf("Not now: they show in English. The next build asks again (the list: build\\languages_todo.txt).\n\n");
        return 0;
    }
    printf("\nType each translation and press Enter.  Blank: skip this one.  skipall: skip the rest and finish\n"
           "the build.  \\n in your text: a new line there.  Keep the English's %%s / %%d / %%.1f ..., in order.\n");
    const int widest = widest_panel_line(tpl);
    int done = 0, skipped = 0;
    size_t k = 0;
    for (; k < items.size(); k++) {
        const Item &it = items[k];
        const Lang &lang = langs[it.lang];
        const Entry *e = nullptr;
        for (const Entry &x : books[it.lang])
            if (!x.obsolete && x.msgid == it.id)
                e = &x;
        if (e == nullptr)
            continue;
        const std::vector<Ref> &refs = tpl.refs.at(it.id);
        const bool panel = is_panel(refs, it.id);
        const size_t lines = split_lines(it.id).size();
        printf("\n[%d/%d] Enter the translation for %s %s ", (int)k + 1, (int)items.size(), where(refs).c_str(),
               english_name(lang.code));
        print_english(it.id, "    ");
        printf("\n");
        if (e->flags.count("fuzzy") != 0 && e->has_previous)
            printf("    (its English changed; it was \"%s\", translated \"%s\")\n", shorten(e->previous, 70).c_str(),
                   shorten(e->msgstr, 70).c_str());
        if (panel || lines > 1)
            printf("    (%s%s%s)\n", panel ? "an in-game menu's text: written in upper case" : "",
                   panel && lines > 1 ? "; " : "",
                   lines > 1 ? (std::to_string(lines) + " lines: \\n between them").c_str() : "");
        bool skipAll = false;
        for (;;) {
            printf("> ");
            fflush(stdout);
            std::string typed;
            if (!read_line(typed)) {
                skipAll = true;             /* (the input ended) */
                break;
            }
            const std::string t = trim_ascii(typed);
            if (t.empty()) {
                skipped++;
                break;
            }
            if (t == "skipall" || t == "SKIPALL") {
                skipAll = true;
                break;
            }
            const std::string s = prepare(it.id, typed, panel);
            const Verdict v = judge(it.id, s, refs, widest);
            for (const std::string &x : v.errors)
                printf("    Not taken: %s\n", x.c_str());
            for (const std::string &x : v.notes)
                printf("    Note: %s\n", x.c_str());
            if (!v.errors.empty()) {
                printf("    Type it again (blank: skip it).\n");
                continue;
            }
            if (!apply(books[it.lang], lang, it.id, s)) {
                printf("    lang/%s.po could not be written (read-only? open in an editor?): not kept.\n",
                       lang.code.c_str());
                skipped++;
            } else {
                done++;
            }
            break;
        }
        if (skipAll) {
            skipped += (int)(items.size() - k);
            break;
        }
    }
    printf("\nTranslated %d, skipped %d%s.\n\n", done, skipped,
           skipped > 0 ? " (they show in English; the next build asks again)" : "");
    return 0;
}

fs::path find_root(const char *given)
{
    if (given != nullptr)
        return fs::u8path(given);
    for (fs::path p = fs::current_path(); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "src" / "platform" / "ui_text.cpp"))
            return p;
        if (p == p.parent_path())
            break;
    }
    return fs::current_path();
}

int usage()
{
    printf("lang_sync [--root DIR] [--build BUILDDIR | --check | --ask | --status | --todo CODE [--json] |\n"
           "          --fill CODE FILE.json | --new CODE]\n"
           "(docs/languages.md \"The interface's languages\")\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char *rootArg = nullptr, *buildDir = nullptr, *todo = nullptr, *fillCode = nullptr, *fillFile = nullptr,
               *newCode = nullptr, *outArg = nullptr;
    bool checkOnly = false, json = false, quiet = false, askMode = false, statusMode = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--root" && i + 1 < argc)
            rootArg = argv[++i];
        else if (a == "--ask")
            askMode = true;
        else if (a == "--out" && i + 1 < argc)
            outArg = argv[++i];
        else if (a == "--status")
            statusMode = true;
        else if (a == "--build" && i + 1 < argc)
            buildDir = argv[++i];
        else if (a == "--check")
            checkOnly = true;
        else if (a == "--todo" && i + 1 < argc)
            todo = argv[++i];
        else if (a == "--json")
            json = true;
        else if (a == "--quiet")
            quiet = true;
        else if (a == "--fill" && i + 2 < argc) {
            fillCode = argv[++i];
            fillFile = argv[++i];
        } else if (a == "--new" && i + 1 < argc)
            newCode = argv[++i];
        else
            return usage();
    }
    const fs::path root = find_root(rootArg);
    if (!fs::exists(root / "src" / "platform" / "ui_text.cpp")) {
        printf("lang_sync: %s is not the GT3 PC tree (no src/platform/ui_text.cpp)\n", root.u8string().c_str());
        return buildDir != nullptr ? 0 : 2;
    }
    const Template tpl = extract(root);
    const std::vector<Lang> langs = languages(root);
    auto known = [&](const char *code) {
        for (const Lang &l : langs)
            if (l.code == code)
                return &l;
        return (const Lang *)nullptr;
    };
    std::error_code ec;
    fs::create_directories(root / "lang", ec);

    if (askMode)
        return ask(root, outArg != nullptr ? fs::u8path(outArg) : root / "build", tpl, langs);

    if (statusMode) {
        /* the end of build.cmd: what is still open, the errors - nothing when all is done */
        int open = 0;
        std::vector<Issue> issues;
        for (const Lang &l : langs) {
            if (!fs::exists(l.path))
                continue;
            const std::vector<Entry> entries = update(load(l.path), tpl);
            const Counts c = count(entries);
            open += c.missing + c.fuzzy;
            check(l.code, entries, tpl, issues);
        }
        int errors = 0;
        for (const Issue &i : issues)
            errors += i.error ? 1 : 0;
        if (open > 0)
            printf("\nInterface languages: %d translation(s) still to write - shown in English until then; the next "
                   "build asks for them (the list: build\\languages_todo.txt).\n",
                   open);
        if (errors > 0) {
            printf("\nInterface languages: %d translation(s) not taken - shown in English; fix them in lang\\<code>.po:\n",
                   errors);
            for (const Issue &i : issues)
                if (i.error)
                    printf("  %s\n", i.text.c_str());
        }
        return 0;
    }

    if (newCode != nullptr) {
        const Lang *l = known(newCode);
        if (l == nullptr) {
            printf("%s is not a language of src/platform/ui_text.cpp\n", newCode);
            return 1;
        }
        if (fs::exists(l->path)) {
            printf("lang/%s.po exists\n", newCode);
            return 1;
        }
        bool changed = false;
        if (!write_if_changed(l->path, po_text(l->code, update({}, tpl)), &changed)) {
            printf("lang/%s.po could not be written\n", newCode);
            return 1;
        }
        printf("lang/%s.po: %d strings to translate\n", newCode, (int)tpl.order.size());
        return 0;
    }

    if (todo != nullptr) {
        const Lang *l = known(todo);
        if (l == nullptr)
            return usage();
        const std::vector<Entry> entries = update(load(l->path), tpl);
        std::string out = json ? "{\"panel_line_limit\": " + std::to_string(widest_panel_line(tpl)) + ", \"items\": [\n"
                               : std::string();
        bool first = true;
        for (const Entry &e : entries) {
            if (e.obsolete || (!e.msgstr.empty() && e.flags.count("fuzzy") == 0))
                continue;
            const std::vector<Ref> &refs = tpl.refs.at(e.msgid);
            const bool panel = is_panel(refs, e.msgid);
            int width = 0;
            for (const std::string &l2 : split_lines(e.msgid))
                width = std::max(width, cells(l2));
            if (json) {
                out += std::string(first ? "" : ",\n") + "{\"en\": " + json_quote(e.msgid) + ", \"where\": " +
                       json_quote(where(refs)) + ", \"panel\": " + (panel ? "true" : "false") +
                       ", \"lines\": " + std::to_string(split_lines(e.msgid).size()) + ", \"width\": " +
                       std::to_string(width);
                if (e.has_previous)
                    out += ", \"was\": " + json_quote(e.previous) + ", \"old\": " + json_quote(e.msgstr);
                out += "}";
            } else {
                out += "#: " + where(refs) + (panel ? "  (panel)" : "") + "\n";
                if (e.has_previous)
                    out += "#| was: " + po_quote(e.previous) + "\n#| its translation: " + po_quote(e.msgstr) + "\n";
                out += "msgid " + po_quote(e.msgid) + "\n\n";
            }
            first = false;
        }
        if (json)
            out += "\n]}\n";
        fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }

    if (fillCode != nullptr) {
        const Lang *l = known(fillCode);
        if (l == nullptr)
            return usage();
        std::vector<std::pair<std::string, std::string>> given;
        std::string why;
        bool ok = false;
        const std::string text = read_file(fs::u8path(fillFile), &ok);
        if (!ok || !parse_json_object(text, given, why)) {
            printf("%s: %s\n", fillFile, ok ? why.c_str() : "cannot be read");
            return 1;
        }
        std::vector<Entry> entries = update(load(l->path), tpl);
        int filled = 0, unknown = 0;
        for (const auto &g : given) {
            bool found = false;
            for (Entry &e : entries)
                if (!e.obsolete && e.msgid == g.first) {
                    e.msgstr = g.second;
                    e.flags.erase("fuzzy");
                    e.has_previous = false;
                    e.previous.clear();
                    found = true;
                    filled++;
                }
            if (!found) {
                unknown++;
                printf("  not a string of the sources: \"%s\"\n", shorten(g.first).c_str());
            }
        }
        bool changed = false;
        if (!write_if_changed(l->path, po_text(l->code, entries), &changed)) {
            printf("lang/%s.po could not be written\n", fillCode);
            return 1;
        }
        std::vector<Issue> issues;
        check(l->code, entries, tpl, issues);
        int errors = 0;
        for (const Issue &i : issues) {
            printf("  %s %s\n", i.error ? "ERROR" : "warning", i.text.c_str());
            errors += i.error ? 1 : 0;
        }
        const Counts c = count(entries);
        printf("%s: %d filled now; %d of %d translated; %d error(s), %d warning(s)%s\n", fillCode, filled, c.done,
               c.done + c.fuzzy + c.missing, errors, (int)issues.size() - errors,
               unknown ? " (some English not found)" : "");
        return errors || unknown ? 1 : 0;
    }

    /* the sync (or only the check) */
    const bool write = !checkOnly;
    std::vector<std::string> notes;             /* what could not be done */
    bool anyChanged = false;
    if (write) {
        bool changed = false;
        if (!write_if_changed(root / "lang" / "template.pot", template_text(tpl), &changed))
            notes.push_back("lang/template.pot could not be written");
    }
    int panel = 0;
    for (const std::string &s : tpl.order)
        panel += is_panel(tpl.refs.at(s), s) ? 1 : 0;
    struct Result {
        const Lang *lang;
        bool present;
        Counts counts;
        std::vector<Entry> entries;
    };
    std::vector<Result> results;
    std::vector<Issue> issues;
    for (const Lang &l : langs) {
        Result r{&l, fs::exists(l.path), {}, {}};
        if (!r.present) {
            results.push_back(r);
            continue;
        }
        r.entries = update(load(l.path), tpl);
        if (write) {
            bool changed = false;
            if (!write_if_changed(l.path, po_text(l.code, r.entries), &changed))
                notes.push_back("lang/" + l.code + ".po could not be written (read-only? open in an editor?)");
            anyChanged = anyChanged || changed;
        }
        r.counts = count(r.entries);
        check(l.code, r.entries, tpl, issues);
        results.push_back(std::move(r));
    }
    /* a .po of no language ui_text.cpp knows: built in, never read */
    for (const auto &e : fs::directory_iterator(root / "lang", ec)) {
        const std::string name = e.path().filename().u8string();
        if (e.path().extension() == ".po" && known(e.path().stem().u8string().c_str()) == nullptr)
            notes.push_back("lang/" + name + " is no language of src/platform/ui_text.cpp (kLangs): never read");
    }
    int errors = 0;
    for (const Issue &i : issues)
        errors += i.error ? 1 : 0;

    std::string summary;                        /* the table */
    char line[256];
    snprintf(line, sizeof(line), "%d strings (%d of the game's panels, %d the launcher's)\n", (int)tpl.order.size(), panel,
             (int)tpl.order.size() - panel);
    summary += line;
    int open = 0;                               /* languages with strings to translate or look at */
    std::vector<std::string> openCodes;
    for (const Result &r : results) {
        if (!r.present) {
            snprintf(line, sizeof(line), "  %-6s no catalogue (lang/%s.po: lang_sync --new %s)\n", r.lang->code.c_str(),
                     r.lang->code.c_str(), r.lang->code.c_str());
        } else {
            snprintf(line, sizeof(line), "  %-6s %4d translated, %3d to look at (fuzzy), %3d missing, %3d obsolete\n",
                     r.lang->code.c_str(), r.counts.done, r.counts.fuzzy, r.counts.missing, r.counts.obsolete);
            if (r.counts.fuzzy + r.counts.missing > 0) {
                open++;
                openCodes.push_back(r.lang->code);
            }
        }
        summary += line;
    }

    if (buildDir == nullptr) {
        printf("%s", summary.c_str());
        for (const Issue &i : issues)
            if (!quiet || i.error)
                printf("    %s %s\n", i.error ? "ERROR" : "warning", i.text.c_str());
        for (const std::string &n : notes)
            printf("    %s\n", n.c_str());
        return checkOnly && errors ? 1 : 0;
    }

    /* --build: the reports, the stamp, a short summary */
    const fs::path out = fs::u8path(buildDir);
    fs::create_directories(out, ec);
    int toTranslate = 0;
    std::string todoText;
    for (const Result &r : results) {
        if (!r.present || r.counts.fuzzy + r.counts.missing == 0)
            continue;
        todoText += "== " + r.lang->code + " (lang/" + r.lang->code + ".po): " +
                    std::to_string(r.counts.fuzzy + r.counts.missing) + "\n";
        for (const Entry &e : r.entries) {
            if (e.obsolete || (!e.msgstr.empty() && e.flags.count("fuzzy") == 0))
                continue;
            toTranslate++;
            todoText += "  " + where(tpl.refs.at(e.msgid)) + (e.flags.count("fuzzy") ? "  (English changed)" : "") +
                        ": \"" + shorten(e.msgid, 100) + "\"\n";
        }
        todoText += "\n";
    }
    const bool attention = open > 0 || !issues.empty() || !notes.empty();
    std::string report = "GT3 PC - the interface's languages (lang\\*.po), as this build left them\n\n" + summary + "\n";
    if (!attention) {
        report += "Nothing to do: every string has its translation in every language.\n";
    } else {
        report += "What to do:\n";
        if (open > 0) {
            std::string list;
            for (const std::string &c : openCodes)
                list += (list.empty() ? "" : ", ") + c;
            report += "  - Strings without a (current) translation in " + list +
                      ": they show in English there until they get one. Nothing breaks; translate them when you can.\n"
                      "    The list: build\\languages_todo.txt; build\\tools\\lang_sync.exe --todo <code> prints a "
                      "language's.\n";
        }
        if (errors > 0)
            report += "  - Errors: translations the programs do not take (they show the English) or the game's panels "
                      "cannot draw - fix them (below).\n";
        if ((int)issues.size() > errors)
            report += "  - Warnings: translations to look at (below).\n";
        for (const std::string &n : notes)
            report += "  - " + n + "\n";
        report += "\nHow: lang\\<code>.po holds each string as msgid (the English, never edit it) and msgstr (the\n"
                  "translation). Write the msgstr; an entry marked \"#, fuzzy\" had its English changed: check its\n"
                  "msgstr, then delete the \"fuzzy\" line. Keep each %s / %d / %.1f of the English, in the same order.\n"
                  "A panel string (the in-game menus: upper case English) keeps to upper case and to the characters\n"
                  "the overlay's font has. Poedit (poedit.net) opens the files. The next build checks and builds\n"
                  "them in; set GT3PC_LANG_DIR=<this tree>\\lang to try an edited file without a build.\n"
                  "docs/languages.md \"The interface's languages\".\n";
        if (!issues.empty()) {
            report += "\nErrors and warnings:\n";
            for (const Issue &i : issues)
                report += std::string("  ") + (i.error ? "ERROR " : "warning ") + i.text + "\n";
        }
    }
    bool changed = false;
    write_if_changed(out / "languages.txt", report, &changed);
    write_if_changed(out / "languages_todo.txt",
                     todoText.empty() ? "Nothing to translate.\n" : "The strings to translate, per language:\n\n" + todoText,
                     &changed);
    /* the stamp the programs' resources depend on: touched when a catalogue changed */
    const fs::path stamp = out / "languages.stamp";
    if (anyChanged || !fs::exists(stamp)) {
        std::ofstream s(stamp, std::ios::binary | std::ios::trunc);
        s << "lang/*.po changed\n";
    }
    /* the summary in the build's output */
    if (!attention) {
        printf("Interface languages: %d strings, all translated in the %d languages.\n", (int)tpl.order.size(),
               (int)langs.size());
    } else {
        std::string what;
        if (open > 0) {
            int missing = 0, fuzzy = 0;
            for (const Result &r : results) {
                missing += r.counts.missing;
                fuzzy += r.counts.fuzzy;
            }
            what += std::to_string(missing) + " translation(s) to write";
            if (fuzzy > 0)
                what += ", " + std::to_string(fuzzy) + " to check (English changed)";
            what += " in " + std::to_string(open) + " language(s) - shown in English meanwhile";
        }
        if (errors > 0)
            what += (what.empty() ? "" : "; ") + std::to_string(errors) + " error(s)";
        if ((int)issues.size() > errors)
            what += (what.empty() ? "" : "; ") + std::to_string((int)issues.size() - errors) + " warning(s)";
        if (!notes.empty())
            what += (what.empty() ? "" : "; ") + notes[0];
        printf("Interface languages: %s - see build\\languages.txt\n", what.c_str());
    }
    return 0;
}
