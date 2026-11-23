/* ini_test: the POSIX private-profile calls (src/platform/os/ini_posix.cpp)
 * against the Windows behaviour the settings code relies on.  Exits 0 when
 * every check passes, 1 otherwise (each failure is printed). */
#include "platform/os/win_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

namespace {

int g_failures;
std::string g_dir;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond);        \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

#define CHECK_STR(got, want)                                                        \
    do {                                                                            \
        const std::string g_ = (got), w_ = (want);                                  \
        if (g_ != w_) {                                                             \
            fprintf(stderr, "%s:%d: FAIL: %s\n  got:  [%s]\n  want: [%s]\n",       \
                    __FILE__, __LINE__, #got, g_.c_str(), w_.c_str());              \
            g_failures++;                                                           \
        }                                                                           \
    } while (0)

std::string path(const char *name)
{
    return g_dir + "/" + name;
}

void put(const std::string &file, const std::string &text)
{
    FILE *fp = fopen(file.c_str(), "wb");
    if (fp == nullptr) {
        perror(file.c_str());
        exit(2);
    }
    fwrite(text.data(), 1, text.size(), fp);
    fclose(fp);
}

std::string get(const std::string &file)
{
    std::string s;
    FILE *fp = fopen(file.c_str(), "rb");
    if (fp == nullptr)
        return "<missing>";
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        s.append(buf, n);
    fclose(fp);
    return s;
}

/* a NUL-separated list as "a|b|c", up to its double NUL */
std::string list(const char *buf, DWORD got)
{
    std::string s;
    for (const char *p = buf; p < buf + got && *p; p += strlen(p) + 1) {
        if (!s.empty())
            s += '|';
        s += p;
    }
    return s;
}

std::string read(const std::string &file, const char *sec, const char *key, const char *def)
{
    char buf[256];
    memset(buf, 'x', sizeof(buf));
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), file.c_str());
    return buf;
}

const char kSample[] =
    "; gt3pc.ini sample\n"
    "\n"
    "[Graphics]\n"
    "; the resolution scale\n"
    "RenderScale = 150\n"
    "Lod=on\n"
    "Quoted=\"  spaced  \"\n"
    "Negative=-5\n"
    "NotANumber=abc\n"
    "Empty=\n"
    "\n"
    "[CarKinds]\n"
    "# a hash line\n"
    "c001 = race\n"
    "c002=road\n"
    "\n"
    "[Audio]\n"
    "Output=wasapi\n";

void test_missing_gives_default()
{
    const std::string f = path("missing.ini");
    put(f, kSample);
    CHECK_STR(read(f, "Graphics", "NoSuchKey", "dflt"), "dflt");
    CHECK_STR(read(f, "NoSuchSection", "Lod", "dflt"), "dflt");
    CHECK_STR(read(path("no-such-file.ini"), "Graphics", "Lod", "dflt"), "dflt");
    CHECK_STR(read(f, "Graphics", "NoSuchKey", "trail   "), "trail");   /* trailing blanks dropped */
    CHECK_STR(read(f, "Graphics", "NoSuchKey", nullptr), "");
    char buf[16];
    CHECK(GetPrivateProfileStringA("Graphics", "NoSuchKey", "abc", buf, sizeof(buf), f.c_str()) == 3);
    CHECK(GetPrivateProfileIntA("Graphics", "NoSuchKey", 77, f.c_str()) == 77);
    CHECK(GetPrivateProfileIntA("Graphics", "Empty", 77, f.c_str()) == 77);
    CHECK(GetPrivateProfileIntA("Graphics", "RenderScale", 77, f.c_str()) == 150);
    CHECK(GetPrivateProfileIntA("Graphics", "Negative", 77, f.c_str()) == 0);
    CHECK(GetPrivateProfileIntA("Graphics", "NotANumber", 77, f.c_str()) == 0);
}

void test_values()
{
    const std::string f = path("values.ini");
    put(f, kSample);
    CHECK_STR(read(f, "Graphics", "RenderScale", ""), "150");          /* trimmed around '=' */
    CHECK_STR(read(f, "Graphics", "Quoted", ""), "  spaced  ");        /* one pair of quotes off */
    CHECK_STR(read(f, "Graphics", "Empty", "dflt"), "");               /* present but empty */
    char small[4];
    CHECK(GetPrivateProfileStringA("Audio", "Output", "", small, sizeof(small), f.c_str()) == 3);
    CHECK_STR(small, "was");                                           /* cut: size - 1 */
}

void test_case_insensitive()
{
    const std::string f = path("case.ini");
    put(f, kSample);
    CHECK_STR(read(f, "graphics", "LOD", "dflt"), "on");
    CHECK_STR(read(f, "GRAPHICS", "renderscale", "dflt"), "150");
    CHECK(GetPrivateProfileIntA("gRaPhIcS", "RENDERSCALE", 0, f.c_str()) == 150);
    /* a write with another spelling changes the same key, keeping the file's */
    CHECK(WritePrivateProfileStringA("AUDIO", "output", "sdl", f.c_str()));
    CHECK_STR(read(f, "Audio", "Output", ""), "sdl");
    CHECK(get(f).find("Output=sdl\n") != std::string::npos);
    CHECK(get(f).find("output") == std::string::npos);
    CHECK(get(f).find("[AUDIO]") == std::string::npos);
}

void test_lists_double_nul()
{
    const std::string f = path("lists.ini");
    put(f, kSample);
    char buf[256];
    memset(buf, 'x', sizeof(buf));
    DWORD got = GetPrivateProfileSectionA("carkinds", buf, sizeof(buf), f.c_str());
    const char want[] = "# a hash line\0c001=race\0c002=road\0";  /* + the final NUL */
    CHECK(got == sizeof(want) - 1);
    CHECK(memcmp(buf, want, sizeof(want)) == 0);
    CHECK(buf[got] == '\0' && buf[got - 1] == '\0');

    memset(buf, 'x', sizeof(buf));
    got = GetPrivateProfileSectionNamesA(buf, sizeof(buf), f.c_str());
    CHECK_STR(list(buf, got), "Graphics|CarKinds|Audio");
    CHECK(buf[got] == '\0' && buf[got - 1] == '\0');

    memset(buf, 'x', sizeof(buf));
    got = GetPrivateProfileStringA("Graphics", nullptr, "", buf, sizeof(buf), f.c_str());
    CHECK_STR(list(buf, got), "RenderScale|Lod|Quoted|Negative|NotANumber|Empty");
    CHECK(buf[got] == '\0' && buf[got - 1] == '\0');

    /* cut by the buffer: size - 2, two NULs at the end */
    char small[8];
    memset(small, 'x', sizeof(small));
    got = GetPrivateProfileSectionA("CarKinds", small, sizeof(small), f.c_str());
    CHECK(got == sizeof(small) - 2);
    CHECK(small[6] == '\0' && small[7] == '\0');

    /* a missing section: 0 and an empty list */
    memset(buf, 'x', sizeof(buf));
    got = GetPrivateProfileSectionA("Nothing", buf, sizeof(buf), f.c_str());
    CHECK(got == 0);
    CHECK(buf[0] == '\0' && buf[1] == '\0');
}

void test_write_keeps_comments_and_order()
{
    const std::string f = path("write.ini");
    put(f, kSample);
    CHECK(WritePrivateProfileStringA("Graphics", "Lod", "off", f.c_str()));         /* in place */
    CHECK(WritePrivateProfileStringA("Graphics", "NewKey", "1", f.c_str()));        /* end of section */
    CHECK(WritePrivateProfileStringA("Stats", "PlayTime", "3600", f.c_str()));      /* new section */
    CHECK_STR(get(f),
              "; gt3pc.ini sample\n"
              "\n"
              "[Graphics]\n"
              "; the resolution scale\n"
              "RenderScale = 150\n"
              "Lod=off\n"
              "Quoted=\"  spaced  \"\n"
              "Negative=-5\n"
              "NotANumber=abc\n"
              "Empty=\n"
              "NewKey=1\n"
              "\n"
              "[CarKinds]\n"
              "# a hash line\n"
              "c001 = race\n"
              "c002=road\n"
              "\n"
              "[Audio]\n"
              "Output=wasapi\n"
              "\n"
              "[Stats]\n"
              "PlayTime=3600\n");

    /* a new file; CRLF and a missing last line ending kept */
    const std::string g = path("new.ini");
    unlink(g.c_str());
    CHECK(WritePrivateProfileStringA("A", "k", "v", g.c_str()));
    CHECK_STR(get(g), "[A]\nk=v\n");
    const std::string h = path("crlf.ini");
    put(h, "[A]\r\n;c\r\nk=v");
    CHECK(WritePrivateProfileStringA("A", "j", "w", h.c_str()));
    CHECK_STR(get(h), "[A]\r\n;c\r\nk=v\r\nj=w\r\n");
}

void test_null_deletes()
{
    const std::string f = path("delete.ini");
    put(f, kSample);
    CHECK(WritePrivateProfileStringA("Graphics", "lod", nullptr, f.c_str()));       /* the key */
    CHECK_STR(read(f, "Graphics", "Lod", "gone"), "gone");
    CHECK(WritePrivateProfileStringA("Graphics", "NoSuchKey", nullptr, f.c_str())); /* nothing to do */
    CHECK(WritePrivateProfileStringA("carkinds", nullptr, nullptr, f.c_str()));     /* the section */
    CHECK_STR(get(f),
              "; gt3pc.ini sample\n"
              "\n"
              "[Graphics]\n"
              "; the resolution scale\n"
              "RenderScale = 150\n"
              "Quoted=\"  spaced  \"\n"
              "Negative=-5\n"
              "NotANumber=abc\n"
              "Empty=\n"
              "\n"
              "[Audio]\n"
              "Output=wasapi\n");
}

void test_write_section()
{
    const std::string f = path("section.ini");
    put(f, kSample);
    CHECK(WritePrivateProfileSectionA("CarKinds", "c009=race\0c010=road\0", f.c_str()));
    char buf[128];
    const DWORD got = GetPrivateProfileSectionA("CarKinds", buf, sizeof(buf), f.c_str());
    CHECK_STR(list(buf, got), "c009=race|c010=road");
    CHECK_STR(read(f, "Audio", "Output", ""), "wasapi");
}

void test_other_process_change_seen()
{
    const std::string f = path("shared.ini");
    put(f, "[Stats]\nPlayTime=1\n");
    CHECK(GetPrivateProfileIntA("Stats", "PlayTime", 0, f.c_str()) == 1);
    put(f, "[Stats]\nPlayTime=2\n");                  /* as another process would */
    CHECK(GetPrivateProfileIntA("Stats", "PlayTime", 0, f.c_str()) == 2);
}

} // namespace

int main()
{
    const char *tmp = getenv("TMPDIR");
    std::string tmpl = std::string(tmp != nullptr && *tmp ? tmp : "/tmp") + "/ini_test.XXXXXX";
    if (mkdtemp(&tmpl[0]) == nullptr) {
        perror("mkdtemp");
        return 2;
    }
    g_dir = tmpl;

    test_missing_gives_default();
    test_values();
    test_case_insensitive();
    test_lists_double_nul();
    test_write_keeps_comments_and_order();
    test_null_deletes();
    test_write_section();
    test_other_process_change_seen();

    std::string rm = "rm -rf '" + g_dir + "'";
    if (system(rm.c_str()) != 0)
        fprintf(stderr, "could not remove %s\n", g_dir.c_str());
    if (g_failures != 0) {
        fprintf(stderr, "ini_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("ini_test: all checks passed\n");
    return 0;
}
