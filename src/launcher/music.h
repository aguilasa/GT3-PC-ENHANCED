/* music.h - the streamed music converted for the PC (docs/sound.md 2.4).
 *
 * Each bgm\*.ads of a data folder becomes a .wav beside it - 16-bit PCM at
 * its own rate, the title and artist bgm\ads.inf gives it as its tags (an
 * intro, i_<name>, its song's) - and the .ads is deleted; bgm\ads.inf is
 * written out as bgm\ads.txt, the plain-text list the game reads instead
 * (engine/sound/native/bgm_files.h).  ads.inf stays: without ads.txt the
 * game reads it again, its .ads names finding the .wav of the same name;
 * an ads.txt already there (edited, a mod's) is never replaced.  An install
 * converts as its last step; the launcher converts an installed version
 * that still has an .ads, or ads.inf without ads.txt, when it starts.
 * Stopped or failed half-way, it carries on with what is left next time.
 *
 * PC (2026-11-15): a disc image's data folder (the Play tab's Disc sub-tab:
 * install.h prepare_disc, disc_files.h) is never converted; what its bgm\
 * does not have - ads.inf, the songs (.ads) - is the image's, read in place
 * by the functions below as the game reads it, so the song manager works on
 * it as on an installed version.  Its bgm\ads.txt is written into the
 * folder (bgm\ made then). */
#pragma once

#include "install.h"

#include "engine/sound/native/bgm_files.h"

#include <cstdint>
#include <string>
#include <vector>

namespace music {

/* The data folder has music to convert: a bgm\*.ads with an .ads head, or
 * bgm\ads.inf without bgm\ads.txt. */
bool needs_conversion(const std::wstring &data);

/* Converts the data folder's music; the listener gets the progress (bytes
 * of .ads converted) and is asked for cancellation between files.
 * `message` gets a one-line summary (ok), or what could not be converted
 * (failed: the rest was). */
install::Result convert(const std::wstring &data, install::Listener &listener, std::wstring *message);

/* The bytes the conversion adds to a data folder whose .ads files have
 * `ads_bytes` (a .wav is about 3.5 times its .ads, which goes). */
uint64_t growth(uint64_t ads_bytes);

/* ---- the music files and the list (the song manager: song_manager.h) ---- */

/* A music file (.wav, .mp3) of bgm\, or anywhere on the machine (probe_song). */
struct MusicFile {
    std::wstring name;          /* in bgm\; else its full path */
    std::string bytes;          /* as the list keeps it: its name as the game opens it (host_file: the
                                   ANSI code page), else its full path in UTF-8 (track_file) */
    bool nameOk = false;        /* the code page has every character of its name (a full path: true) */
    bool found = false;         /* it is there */
    bool pending = false;       /* not looked at yet (the song manager's) */
    bool playable = false;      /* a format the game plays (bgm_probe) */
    bool disc = false;          /* PC (2026-11-15): a song of a disc image's bgm\ (disc_music) */
    spu2::BgmFileInfo info;
};
/* The .wav and .mp3 files of the data folder's bgm\, by name. */
std::vector<MusicFile> music_files(const std::wstring &data);
/* PC (2026-11-15): a disc image's data folder: the music files of the
 * image's bgm\ (its .ads songs, intros and jingles) that the folder's bgm\
 * does not have (neither the file nor its .wav / .ads twin), by name; none
 * for an installed version's folder.  The game plays them from the image. */
std::vector<MusicFile> disc_music(const std::wstring &data);
/* What the list keeps for the song at `path' (bgm_system_path): a file of the
 * data folder's bgm\ its name, as ever (unless the code page cannot spell it);
 * any other its full path, in UTF-8.  Nothing is copied: it plays from there. */
std::string track_file(const std::wstring &data, const std::wstring &path);
/* A list's track file as Windows names it: a full path as such, else the name
 * in bgm\. */
std::wstring shown_file(const std::string &file);
/* What the song at a full path is, its tags too, `bytes' left to the caller.
 * Its head and tags are read, never all of it (bgm_probe's estimate), so a
 * folder of songs takes moments.  Any thread. */
MusicFile probe_song(const std::wstring &path);
/* The .wav and .mp3 files in `folder' and the folders under it, by path. */
std::vector<std::wstring> folder_songs(const std::wstring &folder);
/* The list the game reads: bgm\ads.txt, else as written from ads.inf (as
 * the conversion writes it), else a new one. */
std::string list_text(const std::wstring &data);
/* ads.txt written (to .part, then put in place); false with the reason. */
bool write_list(const std::wstring &data, const std::string &text, std::wstring *message);
/* ads.inf as a list (the disc's: the slots' originals); false without one. */
bool original_list(const std::wstring &data, spu2::AdsList *out);
/* The beat markers of an ads.txt the conversion wrote before 2026-10-26,
 * which misread ads.inf's (spu2::ads_inf_read, legacyMarkers: channel 1 the
 * track table's bytes, 2 .. 4 slices of the beats - the replay cameras cut
 * off the music): each track whose markers are exactly that misreading of
 * its ads.inf track (by file name) gets ads.inf's as the game reads them;
 * the rest of the list as it is.  The number of tracks repaired (0: none
 * needed it, no ads.txt or no ads.inf); -1 when ads.txt could not be
 * written (message: why). */
int repair_markers(const std::wstring &data, std::wstring *message);
/* What bgm\<file> (or its .wav / .ads twin) or the file at a full path is, as
 * ads_list_text and ads_list_image take it. */
spu2::AdsListProbe prober(const std::wstring &data);
/* The samples of bgm\<file> (or its .wav / .ads twin), or of the file at a
 * full path, as one channel (the
 * channels' mean) at the file's rate, for the beat markers (beats.h): a
 * .wav, an .ads, an .mp3 (as the game: Media Foundation on Windows, FFmpeg
 * elsewhere).  Any thread;
 * false with the reason. */
bool song_samples(const std::wstring &data, const std::string &file, std::vector<float> *mono, int *rate,
                  std::wstring *why);
/* Names and text between the game's bytes (the ANSI code page) and the
 * launcher's; ansi: false when a character has no byte ('?' then). */
bool ansi(const std::wstring &w, std::string *out);
std::wstring from_ansi(const std::string &s);

/* ---- new songs ----
 * A .wav or .mp3 put into bgm\ that bgm\ads.txt does not name - neither as
 * a track (nor its .ads / .wav twin), as an intro of one (i_<track>), nor on
 * a commented-out track line ("# track FILE": kept out) - is a new song.
 * The launcher looks when it starts and before Play and adds them to group
 * 2, the race songs, without beat markers (the European disc's race songs
 * have none either; the song manager makes them from a tempo typed in,
 * beats.h): the title and artist of the file's tags, else its name and
 * "unknown", which the song manager edits (the Audio tab says so). */
struct NewSong {
    std::wstring file;          /* in bgm\ */
    std::string fileBytes;      /* its name as the game opens it */
    std::wstring title, artist; /* the file's tags, else its name and "unknown" */
};
/* The new songs of a data folder, by name.  unplayable: the files in a
 * format the game does not play, or whose names it cannot open (characters
 * outside the ANSI code page): not new songs, reported. */
std::vector<NewSong> find_new_songs(const std::wstring &data, std::vector<std::wstring> *unplayable);
/* Adds them to group 2 of bgm\ads.txt (made from ads.inf first, or new,
 * when there is none); false with the reason. */
bool add_songs(const std::wstring &data, const std::vector<NewSong> &songs, std::wstring *message);

} // namespace music
