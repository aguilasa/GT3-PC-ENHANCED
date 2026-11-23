/* save_folders.h - where an engine's memory cards are: its save folders.
 *
 * PC (2026-11-15, user request: "Save manager ... Each engine gets their own
 * save manager config.  2 Slots for the virtual card ports: Slots are
 * assigned a folder ... Folder list is populated by the per-engine folder,
 * what's in there is what it sees ... memcard folder locations: "saves"
 * folder \ "<*engine*>""<inputted name>" \ GAMEDATA,replay,etc."; "F2 menu
 * gets selectors for slot 1 and 2, options are populated from the engine's
 * save folder").
 *
 * A save folder is one memory card: saves\<engine>\<name>\ in the folder of
 * the shared gt3pc.ini (the launcher the user starts, src/launcher/engines.h),
 * the card's saves its folders (BASCUS-97102GAMEDATA\ ...).  <engine> is the
 * engine's name - the first line of engine.txt in the engine's folder, else
 * "Gran Turismo 3 A-Spec" for the launcher the user starts and the folder's
 * own name for an engine started from another launcher's selector (as the
 * selector names them) - with what a folder name cannot have, or the ANSI
 * code page cannot keep, as '_'.  Which folder is in which slot: the engine's
 * gt3pc_engine.ini [Saves] Slot1, Slot2 (a folder's name; none: no card in
 * that slot).  The memcard1\ and memcard2\ folders of earlier versions are
 * not read any more (left where they are: the user's choice).
 *
 * Shared by the launcher (its save manager, src/launcher/save_manager.h) and
 * the game (platform/memcard.h; the F2 menu's MEMORY CARDS): wchar_t text
 * (UTF-16 on Windows, UTF-32 elsewhere), the files through platform/os/fs.h. */
#pragma once

#include <string>
#include <vector>

namespace save_folders {

constexpr const wchar_t *kFolder = L"saves";
constexpr const wchar_t *kSection = L"Saves";           /* gt3pc_engine.ini: Slot1, Slot2 */
constexpr const wchar_t *kOwnName = L"Gran Turismo 3 A-Spec";
constexpr int kSlots = 2;
constexpr int kMaxName = 64;                            /* a save folder's name, characters */

/* The engine's name (engines.h): engine.txt's first line in `engineFolder`,
 * else kOwnName (`shared` false: the launcher the user starts) or the
 * folder's name. */
std::wstring engine_name(const std::wstring &engineFolder, bool shared);

/* The engine's saves: <sharedFolder>saves\<engine>\, with a trailing '\'.
 * sharedFolder: the folder of the gt3pc.ini in use; engineFolder: the
 * engine's own (the executable's).  Not made here. */
std::wstring saves_dir(const std::wstring &sharedFolder, const std::wstring &engineFolder);

/* Why `name` cannot be a save folder's name, for the user (nullptr: it can):
 * empty, longer than kMaxName, a character a file name cannot have or the
 * ANSI code page (gt3pc_engine.ini, the game's file calls) cannot keep, a
 * name ending in a dot or a space, a device name (CON, NUL, COM1 ...). */
const wchar_t *name_problem(const std::wstring &name);

/* The folders in `savesDir`, by name (sorted, case not counted). */
std::vector<std::wstring> list(const std::wstring &savesDir);

/* PC (2026-11-15, user request: "The launcher needs to create 2 default
 * folders if there's nothing: (default1, default2)"): with no folder in
 * `savesDir` (or no savesDir), makes it and kDefaults - default1, default2 -
 * and puts each in its slot (1, 2): with no folder there, neither slot had a
 * card.  True when it made them.  The launcher's: at its start and at Play. */
constexpr const wchar_t *kDefaults[kSlots] = {L"default1", L"default2"};
bool make_defaults(const std::wstring &savesDir, const std::wstring &engineIni);

/* The folder in slot (0, 1) of `engineIni` [Saves], "" none. */
std::wstring slot(const std::wstring &engineIni, int slot);
/* Writes it ("": the key removed - no card). */
void set_slot(const std::wstring &engineIni, int slot, const std::wstring &name);

/* wchar_t text <-> the ANSI code page (the game's paths; UTF-8 off Windows). */
std::wstring from_ansi(const std::string &s);
std::string to_ansi(const std::wstring &s);

} // namespace save_folders
