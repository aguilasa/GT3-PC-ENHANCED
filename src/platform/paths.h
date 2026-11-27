/* paths.h - where GT3 PC finds its files.
 *
 * The game folder is the folder of the executable.  The game files are
 * loose in a data folder of it, one per version - data_US (the USA disc and
 * the bundle), data_EU, data_JP - which the launcher unpacks there from the
 * user's disc image (the volume GT3.VOL, src/launcher/install.h) and marks
 * whole with gt3pc_install.txt, written last.  The game reads them as the
 * original's loose host:data/ files (platform/host_file.h: "data/" is the
 * data folder).  The folder: --data <folder> (the launcher's Play: the data
 * folder itself, or a folder holding data\), else [Game] Version's (the
 * launcher's last Play), else data_US, data_EU, data_JP, an earlier
 * launcher's data\, and for development the extracted disc of the source
 * tree (pc_enhanced/gt3_disc/files/data, unpacked by the launcher's
 * --unpack).
 *
 * PC (2026-11-14): a data folder may be a disc image's (the launcher's Play
 * tab > Disc, "Play from disc...": discs\<boot file>_<id>, src/launcher/
 * install.h prepare_disc): its marker names the image ("image <path>",
 * UTF-8, and "volume <its volume in the image>"), which init opens - the
 * disc's files are then read straight from it, under the folder's own
 * (platform/disc_image.h, host_file.h).  [Game] Disc names the one last
 * played (its folder in discs\; the key is removed by an installed
 * version's Play): the game started without --data takes it first. */
#pragma once

#include <string>

namespace paths {

/* The folder of the game files, and the file the launcher writes into it
 * last (a data folder without it is an install that did not finish). */
constexpr const char *kDataFolder = "data";
constexpr const char *kInstallMarker = "gt3pc_install.txt";
/* The disc images' data folders are in this folder of the game's. */
constexpr const char *kDiscsFolder = "discs";

/* Resolves the folders from the command line; false (with a message in
 * the log) when no installed data folder is found. */
bool init(int argc, char **argv);
/* Why init failed, for the user ("": no installed data folder was found):
 * a disc image's data folder whose image could not be used (UTF-8). */
const std::string &init_error();

/* The executable's folder, with a trailing '\'.  Off Windows, when that
 * folder is read-only, the user's instead (platform/posix/user_dir.h). */
const std::string &game_dir();
/* PC (2026-11-14, user request: the executable plugin system; src/launcher/
 * engines.h).  gt3pc.ini, the settings: the executable's folder's, or with
 * --shared <folder> that folder's - an engine started from another
 * launcher's selector reads and writes that launcher's file, one for every
 * engine.  What is an engine's own stays in the executable's folder: the
 * memory cards, the data folders, the caches, and gt3pc_engine.ini - its mod
 * folders ([Mods]) and what was played last ([Game] Version, Disc).  Until
 * its launcher has made that file (and without --shared), those keys are
 * read where they were: engine_ini_path() is then gt3pc.ini. */
const std::string &ini_path();
const std::string &engine_ini_path();
constexpr const char *kEngineIni = "gt3pc_engine.ini";
const std::string &data_folder();   /* the data folder (data_US\ ...), with a trailing '\' */
const std::string &data_dir();      /* the folder holding it, with a trailing '\' */
/* A line of the install marker, "<key> <value>": the value ("language BR"
 * -> "BR"; the launcher's keys: src/launcher/install.h), or "" when it has
 * no such line (an earlier launcher's marker: Gran Turismo 3 A-Spec (USA)). */
std::string install_info(const char *key);
/* The memory cards were game_dir() + "memcard1\" and "memcard2\"
 * (platform/memcard.cpp).  PC (2026-11-15): they are save folders,
 * saves\<engine>\<name>\ in ini_path()'s folder, gt3pc_engine.ini [Saves]
 * naming the one in each slot (platform/save_folders.h); this is
 * saves\<engine>\, with its '\' (ANSI). */
const std::string &saves_dir();
/* PC (2026-11-15): the screenshots (platform/screenshot.h): screenshots\ in
 * ini_path()'s folder (every engine's in one place), with its '\' (ANSI;
 * made by the first screenshot). */
constexpr const char *kScreenshotsFolder = "screenshots";
const std::string &screenshots_dir();

} // namespace paths
