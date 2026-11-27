/* paths.cpp - where GT3 PC finds its files (paths.h). */
#include "platform/paths.h"
#include "platform/disc_image.h"
#include "platform/log.h"
#include "platform/save_folders.h"             /* PC (2026-11-15) */
#include "platform/os/fs.h"
#include "platform/os/win_compat.h"
#ifndef _WIN32
#include "platform/posix/user_dir.h"
#endif

#include <stdio.h>
#include <string.h>

#include <utility>
#include <vector>

namespace paths {
namespace {

std::string g_gameDir, g_dataDir, g_dataFolder, g_initError;
std::string g_ini, g_engineIni;                 /* PC (2026-11-14): paths.h ini_path, engine_ini_path */
std::string g_savesDir;                         /* PC (2026-11-15): paths.h saves_dir */
std::string g_screenshotsDir;                   /* PC (2026-11-15): paths.h screenshots_dir */
std::vector<std::pair<std::string, std::string>> g_marker;     /* the install marker's "key value" lines */

/* The marker's lines with a key and a value (the first line, the release's
 * description, has spaces too: its "key" is just never asked for). */
void read_marker(const std::string &path)
{
    std::string text;
    if (!fs::read_file(fs::native_to_utf8(path), &text))
        return;
    size_t at = 0;
    while (at < text.size()) {
        /* (a line as fgets gave it: up to its '\n', at most 1023 bytes) */
        size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end + 1;
        if (end - at > 1023)
            end = at + 1023;
        std::string line = text.substr(at, end - at);
        at = end;
        line = line.substr(0, line.find('\0'));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t space = line.find(' ');
        if (space == std::string::npos || space == 0)
            continue;
        g_marker.emplace_back(line.substr(0, space), line.substr(space + 1));
    }
}

bool file_exists(const std::string &p)
{
    return fs::is_file(fs::native_to_utf8(p));
}

std::string with_slash(std::string s)
{
    if (!s.empty() && s.back() != '\\' && s.back() != '/')
        s += fs::kSep;
    return s;
}

/* (the native text: fs.h) */
std::string full_path(const std::string &p)
{
    return fs::utf8_to_native(fs::full_path(fs::native_to_utf8(p)));
}

/* A folder's name with its separator. */
std::string folder(const std::string &name)
{
    return name + fs::kSep;
}

} // namespace

bool init(int argc, char **argv)
{
    std::string exe = fs::utf8_to_native(fs::exe_path());
    const size_t slash = exe.find_last_of(fs::kSep);
    if (slash != std::string::npos)
        exe.resize(slash + 1);
    g_gameDir = exe;
#ifndef _WIN32
    /* GT3PC_HOME, or the executable's folder read-only: the user's
     * (platform/posix/user_dir.h) */
    g_gameDir = pcrun::user_dir(exe);
    if (g_gameDir != exe)
        pclog::write("files: the settings, saves, caches and game files are in %s (not %s: %s)", g_gameDir.c_str(),
                     exe.c_str(), getenv("GT3PC_HOME") != nullptr && getenv("GT3PC_HOME")[0] != 0 ? "GT3PC_HOME"
                                                                                                 : "read-only");
#endif

    /* PC (2026-11-14): the settings file - another launcher's with --shared
     * (this engine was started from its selector) - and the engine's own */
    std::string iniDir = g_gameDir;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--shared") == 0)
            iniDir = with_slash(full_path(argv[i + 1]));
    const bool shared = _stricmp(iniDir.c_str(), g_gameDir.c_str()) != 0;
    g_ini = iniDir + "gt3pc.ini";
    g_engineIni = g_gameDir + kEngineIni;
    if (!shared && !file_exists(g_engineIni))
        g_engineIni = g_ini;                /* (the launcher has not moved them yet) */
    if (shared)
        pclog::write("settings: %s (shared: --shared); this engine's own: %s", g_ini.c_str(), g_engineIni.c_str());
    /* PC (2026-11-15): the memory cards, the engine's save folders
     * (save_folders.h) beside the gt3pc.ini in use */
    g_savesDir = save_folders::to_ansi(
        save_folders::saves_dir(save_folders::from_ansi(iniDir), save_folders::from_ansi(g_gameDir)));
    /* PC (2026-11-15): the screenshots, beside it too (platform/screenshot.h) */
    g_screenshotsDir = iniDir + folder(kScreenshotsFolder);

    /* the data folders tried, each a folder with the marker: --data's (the
     * data folder, or a folder holding data\), the [Game] Version's
     * (data_US ...: the launcher's last Play), the versions', an earlier
     * launcher's data\, the source tree's extracted disc */
    std::vector<std::string> candidates;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--data") == 0) {
            candidates.push_back(with_slash(argv[i + 1]));
            candidates.push_back(with_slash(argv[i + 1]) + folder(kDataFolder));
        }
    /* PC (2026-11-14): the disc image last played from the launcher (its
     * folder in discs\) */
    char disc[128] = "";
    GetPrivateProfileStringA("Game", "Disc", "", disc, sizeof(disc), g_engineIni.c_str());
    if (disc[0] != 0 && strpbrk(disc, "\\/:") == nullptr)
        candidates.push_back(g_gameDir + folder(kDiscsFolder) + folder(disc));
    char version[16] = "";
    GetPrivateProfileStringA("Game", "Version", "", version, sizeof(version), g_engineIni.c_str());
    if (version[0] != 0)
        candidates.push_back(g_gameDir + kDataFolder + "_" + folder(version));
    for (const char *v : {"US", "EU", "JP"})
        candidates.push_back(g_gameDir + kDataFolder + "_" + folder(v));
    candidates.push_back(g_gameDir + folder(kDataFolder));
    candidates.push_back(with_slash(full_path(exe + folder("..") + folder("..") + folder("gt3_disc") +
                                              folder("files") + kDataFolder)));
    for (const std::string &c : candidates)
        if (file_exists(c + kInstallMarker)) {
            g_dataFolder = with_slash(full_path(c));
            g_dataDir = g_dataFolder.substr(0, g_dataFolder.find_last_of(fs::kSep, g_dataFolder.size() - 2) + 1);
            pclog::write("data: %s", g_dataFolder.c_str());
            read_marker(g_dataFolder + kInstallMarker);
            const std::string release = install_info("release");
            pclog::write("data: %s", release.empty() ? "Gran Turismo 3 A-Spec (USA) (an earlier launcher's install)"
                                                      : release.c_str());
            /* PC (2026-11-14): a disc image's folder: its files are the
             * image's volume's, read in place (platform/disc_image.h) */
            const std::string image = install_info("image");
            if (!image.empty()) {
                std::string error;
                if (!disc_image::mount(image, install_info("volume"), install_info("language"), &error)) {
                    pclog::write("data: the disc image %s cannot be used: %s", image.c_str(), error.c_str());
                    g_initError = "The disc image\n\n" + image + "\n\ncannot be used: " + error +
                                  ".\n\nStart it again from the launcher's Play tab (Disc), or play an installed "
                                  "version there.";
                    return false;
                }
            }
            return true;
        }
    pclog::write("data: no installed game files (%s%c%s_US, _EU, _JP with %s) in %s (run the launcher to install "
                 "them)", g_gameDir.c_str(), fs::kSep, kDataFolder, kInstallMarker, g_gameDir.c_str());
    return false;
}

const std::string &init_error() { return g_initError; }
const std::string &ini_path() { return g_ini; }
const std::string &engine_ini_path() { return g_engineIni; }
const std::string &game_dir() { return g_gameDir; }
const std::string &saves_dir() { return g_savesDir; }  /* PC (2026-11-15) */
const std::string &screenshots_dir() { return g_screenshotsDir; }  /* PC (2026-11-15) */
const std::string &data_dir() { return g_dataDir; }
const std::string &data_folder() { return g_dataFolder; }

std::string install_info(const char *key)
{
    for (const auto &kv : g_marker)
        if (kv.first == key)
            return kv.second;
    return std::string();
}

} // namespace paths
