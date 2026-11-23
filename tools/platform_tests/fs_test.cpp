/* fs_test.cpp - checks the POSIX file layer (src/platform/os/fs_posix.cpp)
 * against what the platform layer has on Windows: names found without case
 * (the game's data asks for race/brake.bin, the disc has RACE/BRAKE.BIN),
 * '/' and '\' as separators, listing, positional reads and writes, making
 * and removing, rename (never over another file) and rename_replace (over
 * it, in one step).  Works in a fresh folder under $TMPDIR (or /tmp), removed
 * at the end; exits 0 when every check holds. */
#include "platform/os/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_failures++;
}

/* A file made with the plain POSIX calls (not through the layer). */
void put(const std::string &path, const std::string &bytes)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        perror(path.c_str());
        exit(2);
    }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
}

std::string get(const std::string &path)
{
    std::string s;
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return "<missing>";
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}

std::vector<std::string> names(const std::string &dir)
{
    std::vector<fs::Entry> e;
    std::vector<std::string> out;
    if (!fs::list(dir, &e))
        out.push_back("<cannot list>");
    for (const fs::Entry &x : e)
        out.push_back(x.name + (x.st.dir ? "/" : ""));
    std::sort(out.begin(), out.end());
    return out;
}

std::string joined(const std::vector<std::string> &v)
{
    std::string s;
    for (const std::string &x : v)
        s += (s.empty() ? "" : " ") + x;
    return s;
}

void remove_tree(const std::string &dir)
{
    std::vector<fs::Entry> e;
    fs::list(dir, &e);
    for (const fs::Entry &x : e) {
        const std::string p = dir + "/" + x.name;
        if (x.st.dir)
            remove_tree(p);
        else
            unlink(p.c_str());
    }
    rmdir(dir.c_str());
}

} // namespace

int main()
{
    const char *tmp = getenv("TMPDIR");
    std::string base = std::string(tmp != nullptr && tmp[0] ? tmp : "/tmp") + "/fs_test.XXXXXX";
    if (mkdtemp(&base[0]) == nullptr) {
        perror("mkdtemp");
        return 2;
    }
    const std::string root = base;
    printf("in %s\n", root.c_str());

    /* the disc's case: RACE/BRAKE.BIN, CARS/DAY/AM0006 */
    mkdir((root + "/RACE").c_str(), 0777);
    mkdir((root + "/CARS").c_str(), 0777);
    mkdir((root + "/CARS/DAY").c_str(), 0777);
    put(root + "/RACE/BRAKE.BIN", "brake-bytes");
    put(root + "/CARS/DAY/AM0006", "car");

    /* -- open by another case -- */
    {
        fs::File f;
        check(f.open(root + "/race/brake.bin", fs::kRead), "open race/brake.bin finds RACE/BRAKE.BIN");
        char buf[32] = {};
        check(f.is_open() && f.size() == 11, "  its size is RACE/BRAKE.BIN's (11)");
        check(f.is_open() && f.read_at(6, buf, 5) == 5 && memcmp(buf, "bytes", 5) == 0,
              "  read_at 6 gives its bytes 6..10");
        check(f.is_open() && f.read_at(11, buf, 4) == 0, "  read_at its end gives 0");
    }
    {
        fs::File f;
        check(f.open(root + "/Race/Brake.Bin", fs::kRead), "open Race/Brake.Bin (mixed case) too");
    }
    check(fs::system_path(root + "/race/brake.bin") == root + "/RACE/BRAKE.BIN",
          "system_path(race/brake.bin) is the real RACE/BRAKE.BIN");
    check(fs::system_path(root + "/cars/day/am0006") == root + "/CARS/DAY/AM0006",
          "system_path resolves each part (cars/day/am0006)");
    check(fs::system_path(root + "/race/missing/x.bin") == root + "/RACE/missing/x.bin",
          "a part found in no case: the rest as given");

    /* -- separators -- */
    {
        fs::File f;
        check(f.open(root + "\\race\\brake.bin", fs::kRead), "open with '\\' separators");
        fs::File g;
        check(g.open(root + "/race\\BRAKE.bin", fs::kRead), "open with '/' and '\\' mixed");
    }
    check(fs::is_file(root + "\\cars\\day\\am0006"), "is_file with '\\' and another case");
    check(fs::is_dir(root + "/cars\\DAY"), "is_dir with mixed separators and case");
    check(!fs::exists(root + "/race/clutch.bin"), "a file not there in any case is missing");
    {
        fs::File f;
        check(!f.open(root + "/race/clutch.bin", fs::kRead) && fs::error_not_found(fs::last_error()),
              "  open fails with not found");
        check(!f.open(root + "/race", fs::kRead), "a folder does not open as a file");
    }
    {
        fs::Stat st;
        check(fs::stat(root + "/race/brake.bin", &st) && !st.dir && st.size == 11 && st.modify > 0,
              "stat by another case: size, time");
    }

    /* -- listing -- */
    check(joined(names(root)) == "CARS/ RACE/", "list the root: CARS/ RACE/");
    check(joined(names(root + "/race")) == "BRAKE.BIN", "list race (another case): BRAKE.BIN");
    check(joined(names(root + "\\Cars\\day")) == "AM0006", "list Cars\\day: AM0006");
    {
        std::vector<fs::Entry> e;
        check(!fs::list(root + "/nothing", &e), "list of a folder not there fails");
    }

    /* -- writing in a folder named in another case -- */
    {
        fs::File f;
        check(f.open(root + "/race/new.bin", fs::kWrite, fs::Create::create_always), "create race/new.bin");
        check(f.write_at(0, "hello", 5) == 5 && f.write_at(5, " world", 6) == 6, "  write_at twice");
    }
    check(get(root + "/RACE/new.bin") == "hello world", "  it is RACE/new.bin, with its bytes");
    {
        fs::File f;
        check(f.open(root + "/RACE/NEW.BIN", fs::kRead | fs::kWrite, fs::Create::open_existing),
              "open RACE/NEW.BIN read-write (made as new.bin)");
        check(f.seek(0, 2) == 11 && f.write("!", 1) == 1 && f.seek(0, 0) == 0, "  seek to the end, write, back");
        char buf[16] = {};
        check(f.read(buf, 16) == 12 && memcmp(buf, "hello world!", 12) == 0, "  read it all sequentially");
    }
    {
        /* create_always over an existing file in another case: that file, emptied (Windows keeps its name) */
        fs::File f;
        check(f.open(root + "/race/NEW.bin", fs::kWrite, fs::Create::create_always) && f.write("x", 1) == 1,
              "create_always NEW.bin over new.bin");
    }
    check(get(root + "/RACE/new.bin") == "x" && joined(names(root + "/RACE")) == "BRAKE.BIN new.bin",
          "  the same file, emptied: no second one");

    /* -- folders -- */
    check(fs::make_dir(root + "/race/sub"), "make_dir race/sub");
    check(fs::is_dir(root + "/RACE/sub"), "  RACE/sub is there");
    check(!fs::make_dir(root + "/RACE/SUB") && fs::error_exists(fs::last_error()),
          "make_dir RACE/SUB again (another case) fails: it is there");
    check(fs::make_dirs(root + "/a/b\\c/"), "make_dirs a/b\\c/");
    check(fs::is_dir(root + "/a/b/c"), "  a/b/c is there");
    check(fs::remove_dir(root + "/A/B/C") && !fs::exists(root + "/a/b/c"), "remove_dir A/B/C");

    /* -- rename (never over another file) -- */
    put(root + "/RACE/old.txt", "old");
    put(root + "/RACE/other.txt", "other");
    check(!fs::rename(root + "/race/old.txt", root + "/race/OTHER.TXT") && fs::error_exists(fs::last_error()),
          "rename over another file (another case) fails");
    check(get(root + "/RACE/other.txt") == "other" && get(root + "/RACE/old.txt") == "old", "  both kept");
    check(fs::rename(root + "/race/OLD.TXT", root + "/race/moved.txt"), "rename OLD.TXT to moved.txt");
    check(get(root + "/RACE/moved.txt") == "old" && !fs::exists(root + "/RACE/old.txt"), "  moved");
    check(fs::rename(root + "/race/moved.txt", root + "/race/MOVED.txt"), "rename to its own name in another case");
    check(joined(names(root + "/RACE")) == "BRAKE.BIN MOVED.txt new.bin other.txt sub/", "  the new case kept");

    /* -- rename_replace (over it, in one step) -- */
    put(root + "/RACE/save.tmp", "new contents");
    check(fs::rename_replace(root + "/race/save.tmp", root + "/race/OTHER.txt"),
          "rename_replace save.tmp over OTHER.txt (other.txt)");
    check(get(root + "/RACE/other.txt") == "new contents" && !fs::exists(root + "/RACE/save.tmp"),
          "  other.txt has the new contents, save.tmp gone");
    check(fs::rename_replace(root + "/race/other.txt", root + "/race/fresh.txt") &&
              get(root + "/RACE/fresh.txt") == "new contents",
          "rename_replace to a name not there");

    /* -- times -- */
    {
        const fs::Time t = fs::from_unix(1000000000);       /* 2001-09-09 01:46:40 UTC */
        fs::Stat st;
        check(fs::set_times(root + "/race/fresh.txt", nullptr, &t) && fs::stat(root + "/RACE/fresh.txt", &st) &&
                  st.modify == t,
              "set_times modify, read back");
        check(fs::to_unix(fs::from_unix(-5)) == -5 && fs::to_unix(fs::from_unix(1234567890)) == 1234567890,
              "Unix seconds <-> file times");
    }

    /* -- removing -- */
    check(fs::remove_file(root + "/race/FRESH.TXT") && !fs::exists(root + "/RACE/fresh.txt"),
          "remove_file FRESH.TXT");
    check(!fs::remove_file(root + "/race/sub"), "remove_file of a folder fails");

    /* -- names and text -- */
    check(!fs::exe_path().empty() && fs::exe_path()[0] == '/', "exe_path is absolute");
    {
        char cwd[4096];
        const bool ok = getcwd(cwd, sizeof(cwd)) != nullptr && chdir(root.c_str()) == 0;
        check(ok && fs::full_path("race/./x/../brake.bin") == root + "/RACE/BRAKE.BIN",
              "full_path: relative, '.' and '..' and case");
        if (ok && chdir(cwd) != 0)
            perror("chdir");
    }
    check(fs::valid_utf8("Citro\xC3\xABn") && !fs::valid_utf8("Citro\xEBn") && !fs::valid_utf8("\xED\xA0\x80"),
          "valid_utf8: UTF-8 yes; Latin-1 and a surrogate no");
    check(fs::narrow(fs::widen("\xE3\x82\xA2 \xF0\x9F\x98\x80")) == "\xE3\x82\xA2 \xF0\x9F\x98\x80",
          "widen / narrow round trip (BMP and beyond)");

    remove_tree(root);
    printf("%s: %d failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
