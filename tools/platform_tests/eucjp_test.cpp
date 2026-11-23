/* eucjp_test.cpp - checks the EUC-JP codec (src/platform/os/eucjp.cpp), which
 * param_db uses off Windows, against Windows' code page 20932:
 *
 *  1. the samples (eucjp_samples.txt, argument 1): strings of the game's
 *     databases as the Windows build stored them - the text it decoded (each
 *     must decode the same and encode back to its bytes) and the bytes it
 *     could not decode (each must fail here too: NEC's row 13, "Ⅱ");
 *  2. a few written here: JIS X 0212 in code page 20932's two-byte form, the
 *     EUC-JP three-byte form 0x8F ... (not valid there), a lead byte at the
 *     end, the characters that decode one way only;
 *  3. every byte, pair and character against the code page's NLS file
 *     (argument 2, c_20932.nls; "-" skips it): MultiByteToWideChar with
 *     MB_ERR_INVALID_CHARS and WideCharToMultiByte with WC_NO_BEST_FIT_CHARS
 *     done from the file's tables;
 *  4. glibc's iconv "EUC-JP" as a second reference where the two are meant to
 *     agree: JIS X 0208 (rows 1..84) and JIS X 0212 (code page 20932's pair
 *     <-> EUC-JP's 0x8F triple).  The differences are listed and must be the
 *     known ones (kKnownIconv below).
 * Exits 0 when every check holds. */
#include "platform/os/eucjp.h"

#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string &what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        g_failures++;
}

std::string unhex(const std::string &h)
{
    std::string b;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        b += (char)strtoul(h.substr(i, 2).c_str(), nullptr, 16);
    return b;
}

std::string hex(const std::string &b)
{
    std::string h;
    char x[4];
    for (unsigned char c : b) {
        snprintf(x, sizeof(x), "%02X", c);
        h += x;
    }
    return h;
}

std::u32string from_utf8(const std::string &s)
{
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        int k = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t v = k == 1 ? c : k == 2 ? (c & 0x1F) : k == 3 ? (c & 0x0F) : (c & 0x07);
        for (int j = 1; j < k && i + j < s.size(); j++)
            v = (v << 6) | ((unsigned char)s[i + j] & 0x3F);
        out.push_back(v);
        i += (size_t)k;
    }
    return out;
}

std::string u32hex(const std::u32string &s)
{
    std::string h;
    char x[12];
    for (char32_t c : s) {
        snprintf(x, sizeof(x), "%sU+%04X", h.empty() ? "" : " ", (unsigned)c);
        h += x;
    }
    return h;
}

/* ---- 1. the samples ---- */
void samples(const char *path)
{
    std::ifstream in(path);
    check(in.good(), std::string("samples file ") + path);
    std::string line;
    int texts = 0, blobs = 0, bad = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string kind, h;
        ls >> kind >> h;
        const std::string bytes = unhex(h);
        std::u32string got;
        if (kind == "T") {
            std::string text;
            std::getline(ls, text);
            text.erase(0, 1);
            texts++;
            const std::u32string want = from_utf8(text);
            bool lossy = true;
            const bool ok = eucjp::decode(bytes.data(), bytes.size(), &got) && got == want &&
                            eucjp::encode(want, &lossy) == bytes && !lossy;
            if (!ok) {
                bad++;
                printf("FAIL  T %s %s: decoded %s\n", h.c_str(), text.c_str(), u32hex(got).c_str());
            }
        } else if (kind == "B") {
            blobs++;
            if (eucjp::decode(bytes.data(), bytes.size(), &got)) {
                bad++;
                printf("FAIL  B %s decodes (%s): Windows did not\n", h.c_str(), u32hex(got).c_str());
            }
        }
    }
    check(texts > 0 && blobs > 0 && bad == 0,
          "samples of the Windows build's databases: " + std::to_string(texts) + " texts decode and encode back, " +
              std::to_string(blobs) + " byte strings do not decode (" + std::to_string(bad) + " wrong)");
}

/* ---- 2. written here ---- */
void written()
{
    std::u32string got;
    bool lossy;
    /* JIS X 0212 as code page 20932 has it: 0x2B33 e-diaeresis as 0xAB 0x33 */
    check(eucjp::decode("\xAB\x33\xAB\x54", 4, &got) && got == U"ëô",
          "JIS X 0212 pairs 0xAB33 0xAB54 decode to U+00EB U+00F4");
    check(eucjp::encode(U"ë¡", &lossy) == "\xAB\x33\xA2\x42" && !lossy,
          "U+00EB U+00A1 encode as JIS X 0212 pairs 0xAB33 0xA242");
    check(eucjp::decode("\xA2\x37", 2, &got) && got == U"~" && eucjp::encode(U"~", &lossy) == "~",
          "JIS X 0212 0xA237 decodes to U+007E, which encodes as 0x7E");
    /* EUC-JP's three-byte JIS X 0212: 0x8F is a single byte (U+008F) here */
    check(!eucjp::decode("\x8F\xA2\xB7", 3, &got), "0x8F 0xA2 0xB7 (EUC-JP's JIS X 0212 tilde) does not decode");
    check(eucjp::decode("\x8F\xB0\xA1", 3, &got) && got == U"\u008F亜",
          "0x8F 0xB0 0xA1 decodes as U+008F and JIS X 0208 0xB0A1 (U+4E9C)");
    check(eucjp::encode(U"\u008F", &lossy) == "?" && lossy, "U+008F does not encode back ('?')");
    check(!eucjp::decode("\xA5\xA2\xA5", 3, &got) && got.empty(), "a lead byte at the end does not decode");
    check(!eucjp::decode("\xAD\xB6", 2, &got), "NEC row 13 (0xADB6) does not decode");
    check(eucjp::decode("\x8E\xB1\xA1\xC1\xA1\xDD", 6, &got) && got == U"ｱ〜−",
          "half-width katakana, WAVE DASH, MINUS SIGN");
    check(eucjp::decode("\xA0\xFF", 2, &got) && got == U"", "0xA0, 0xFF decode to U+F8F0, U+F8F3");
    check(eucjp::encode(U"A\U0001F600", &lossy) == "A?" && lossy, "a character beyond U+FFFF is '?'");
}

/* ---- 3. the NLS file ---- */
struct Nls {
    uint16_t def = 0, unidef = 0, transuni = 0;
    std::vector<uint16_t> mb, wc;
    std::map<int, std::vector<uint16_t>> dbcs;
};

bool load_nls(const char *path, Nls *n)
{
    std::ifstream in(path, std::ios::binary);
    std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 1024)
        return false;
    std::vector<uint16_t> w(d.size() / 2);
    memcpy(w.data(), d.data(), w.size() * 2);
    if (w[1] != 20932 || w[2] != 2)
        return false;
    n->def = w[3];
    n->unidef = w[4];
    n->transuni = w[6];
    size_t p = w[0];
    const size_t wcAt = p + w[p] + 1;
    p++;
    n->mb.assign(w.begin() + p, w.begin() + p + 256);
    p += 256;
    if (w[p++] != 0)
        p += 256;
    p++;
    const size_t base = p;
    for (int l = 0; l < 256; l++)
        if (w[base + l] != 0)
            n->dbcs[l].assign(w.begin() + base + w[base + l], w.begin() + base + w[base + l] + 256);
    if (wcAt + 0x10000 > w.size())
        return false;
    n->wc.assign(w.begin() + wcAt, w.begin() + wcAt + 0x10000);
    return true;
}

void against_nls(const char *path)
{
    if (strcmp(path, "-") == 0) {
        printf("skip  the NLS file (none given)\n");
        return;
    }
    Nls n;
    check(load_nls(path, &n), std::string("code page 20932's NLS file ") + path);
    if (n.wc.empty())
        return;
    int bad = 0, pairs = 0;
    std::u32string got;
    for (int b = 0; b < 256; b++) {
        if (n.dbcs.count(b)) {
            const std::vector<uint16_t> &t = n.dbcs[b];
            for (int k = 0; k < 256; k++) {
                const bool valid = t[k] != n.unidef || ((b << 8) | k) == n.transuni;
                const char s[2] = {(char)b, (char)k};
                const bool ok = eucjp::decode(s, 2, &got);
                pairs++;
                if (ok != valid || (ok && (got.size() != 1 || got[0] != t[k]))) {
                    if (bad++ < 10)
                        printf("FAIL  %02X%02X: the file %s U+%04X, here %s\n", b, k, valid ? "has" : "lacks", t[k],
                               ok ? u32hex(got).c_str() : "no character");
                }
            }
            const char s[1] = {(char)b};
            if (eucjp::decode(s, 1, &got)) {
                if (bad++ < 10)
                    printf("FAIL  lead byte %02X alone decodes\n", b);
            }
        } else {
            const char s[1] = {(char)b};
            const bool ok = eucjp::decode(s, 1, &got);
            if (!ok || got.size() != 1 || got[0] != n.mb[b]) {
                if (bad++ < 10)
                    printf("FAIL  %02X: the file has U+%04X\n", b, n.mb[b]);
            }
        }
    }
    check(bad == 0, "MultiByteToWideChar(20932, MB_ERR_INVALID_CHARS): every byte and " + std::to_string(pairs) +
                        " pairs as the NLS file has them (" + std::to_string(bad) + " differ)");
    bad = 0;
    for (uint32_t u = 0; u < 0x10000; u++) {
        const uint16_t code = n.wc[u];
        std::string want;
        if (!(code == n.def && u != n.def)) {
            if (code < 0x100)
                want = std::string(1, (char)code);
            else
                want = std::string{(char)(code >> 8), (char)(code & 0xFF)};
        }
        char b[2];
        const int k = eucjp::encode_char((char32_t)u, b);
        const std::string here(b, (size_t)k);
        if (here != want && bad++ < 10)
            printf("FAIL  U+%04X: the file encodes %s, here %s\n", u, want.empty() ? "none" : hex(want).c_str(),
                   here.empty() ? "none" : hex(here).c_str());
    }
    check(bad == 0, "WideCharToMultiByte(20932, WC_NO_BEST_FIT_CHARS): U+0000..U+FFFF as the NLS file has them (" +
                        std::to_string(bad) + " differ)");
}

/* ---- 4. glibc's iconv ---- */

/* The known differences from iconv, where both are meant to agree: JIS X 0212
 * 0x2237 TILDE, U+007E in code page 20932 (as Unicode's JIS0212.TXT has it),
 * U+FF5E FULLWIDTH TILDE in glibc.  (Rows 85..94, 0xF5..0xFE, are left out:
 * code page 20932 has the private use area there, iconv nothing.) */
const char *const kKnownIconv[] = {
    "0212 A2B7: code page 20932 U+007E, iconv U+FF5E",
};

bool iconv_decode(iconv_t cd, const std::string &in, char32_t *out)
{
    char32_t buf[4];
    char *ip = (char *)in.data();
    size_t il = in.size();
    char *op = (char *)buf;
    size_t ol = sizeof(buf);
    iconv(cd, nullptr, nullptr, nullptr, nullptr);
    if (iconv(cd, &ip, &il, &op, &ol) == (size_t)-1 || il != 0 || sizeof(buf) - ol != 4)
        return false;
    *out = buf[0];
    return true;
}

void against_iconv()
{
    iconv_t cd = iconv_open("UTF-32LE", "EUC-JP");
    check(cd != (iconv_t)-1, "glibc iconv EUC-JP");
    if (cd == (iconv_t)-1)
        return;
    /* The known differences: (set, iconv's, code page 20932's) by kind; a
     * character only one of them has is "none" on the other side. */
    std::map<std::string, int> diffs;
    std::vector<std::string> shown;
    int same0208 = 0, same0212 = 0;
    for (int l = 0xA1; l <= 0xFE; l++)
        for (int t = 0x21; t <= 0xFE; t++) {
            if (t > 0x7E && t < 0xA1)
                continue;
            const bool x0212 = t < 0x80;
            if (l > 0xF4)
                continue;                   /* (rows 85..94: the private use area, kKnownIconv) */
            const std::string ours{(char)l, (char)t};
            const std::string theirs = x0212 ? std::string{'\x8F', (char)l, (char)(t | 0x80)} : ours;
            std::u32string a;
            char32_t b = 0;
            const bool ha = eucjp::decode(ours.data(), 2, &a);
            const bool hb = iconv_decode(cd, theirs, &b);
            if (ha == hb && (!ha || a[0] == b)) {
                (x0212 ? same0212 : same0208) += ha ? 1 : 0;
                continue;
            }
            std::string kind = std::string(x0212 ? "JIS X 0212" : "JIS X 0208") +
                               (ha && hb ? ": another character" : ha ? ": only code page 20932" : ": only iconv");
            diffs[kind]++;
            char line[160];
            snprintf(line, sizeof(line), "  %s %02X%02X: code page 20932 %s, iconv %s", x0212 ? "0212" : "0208", l,
                     t | (x0212 ? 0x80 : 0), ha ? u32hex(a).c_str() : "none",
                     hb ? u32hex(std::u32string(1, b)).c_str() : "none");
            shown.push_back(line);
        }
    iconv_close(cd);
    printf("      iconv agrees on %d JIS X 0208 and %d JIS X 0212 characters\n", same0208, same0212);
    for (const std::string &s : shown)
        printf("%s\n", s.c_str());
    std::string found;
    for (const auto &d : diffs)
        found += (found.empty() ? "" : "; ") + d.first + " " + std::to_string(d.second);
    printf("      differences: %s\n", found.empty() ? "none" : found.c_str());
    const std::vector<std::string> known(kKnownIconv, kKnownIconv + sizeof(kKnownIconv) / sizeof(kKnownIconv[0]));
    std::vector<std::string> lines;
    for (const std::string &s : shown)
        lines.push_back(s.substr(2));
    check(lines == known, "the differences from iconv are the known ones (" + std::to_string(known.size()) + ")");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: eucjp_test <eucjp_samples.txt> <c_20932.nls | ->\n");
        return 2;
    }
    samples(argv[1]);
    written();
    against_nls(argv[2]);
    against_iconv();
    printf("%s: %d failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
