/* host_file.h - "host:" files of GT3 PC.
 *
 * On the PS2 "host:" was the development PC's file system, reached through
 * the debugger (and HostFS patches).  In the port a host path names a file
 * in the data folder (platform/paths.h): "host:data/foo" or "data/foo" ->
 * <data folder>\data\foo.  Handles are small integers; reads are
 * positional and thread-safe.  The game reads all its files this way: the
 * volume is mounted as the loose host:data/ files (engine/io/T503 vol_init,
 * type 3), which the launcher unpacks from the disc's GT3.VOL.
 *
 * Mod folders (gt3pc_engine.ini [Mods] - PC 2026-11-14: the engine's own
 * file, were gt3pc.ini's: paths.h engine_ini_path - the launcher's Mods window: Count, ModN
 * a folder, ModNOn on / off; N = 1 the first) are laid out as the data
 * folder: an enabled one's file is read in place of the data folder's of
 * the same path (any case; the first folder holding it wins), and its new
 * files are added (opened, and listed by host_file_toc).  "<name>.gz", the
 * packed copy the game reads in place of <name>, is missing while a mod has
 * <name> but no .gz of it.  The folders are read once, at the first file
 * the game opens; names the ANSI code page cannot spell are left out
 * (logged), as are mod.txt and mod.png at a folder's top (the launcher's).
 * Files written (host_file_create) go to the data folder.
 *
 * PC (2026-11-14): the game started on a disc image (the launcher's Play tab
 * > Disc; platform/disc_image.h, paths.h): the data folder is the image's
 * small one, and a file neither a mod folder nor the data folder has is the
 * image's volume's - opened, probed (host_file_present), listed
 * (host_file_list, host_file_toc) and read as a file of the data folder,
 * unpacked (a packed one is inflated at its first read).  It has no Windows
 * path: host_file_full_path gives the data folder's, where no file is.
 *
 * A Windows path given whole ("D:\Music\x.mp3", "\\server\music\x.mp3": a
 * song of bgm/ads.txt anywhere on the machine, docs/sound.md 2.4) is read
 * where it is, by host_file_open, _present and _full_path(_w), no mod folder
 * in the way: UTF-8, which spells any file's name, else (typed by hand) the
 * ANSI code page's.  Off Windows such a path starts at the root ("/home/...",
 * not "/data/...").
 *
 * The files are reached through the platform's file layer
 * (platform/os/fs.h): off Windows a path's parts are found in any case, as
 * the game's data names them in the disc's. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  host_file_open(const char *path);                  /* -1 if missing */
/* The last host_file_open of this thread met a file another program holds -
 * one open for writing there, which host_file_open's sharing refuses (an
 * SQLite editor's database is such a file, which is why the game reads its
 * databases through SQLite itself: platform/sqlite_file.h) - not a missing
 * one: the Windows error, 0 when it did not.  A mod's file held: the open may
 * still have given the data folder's copy, which a caller that must have the
 * mod's closes. */
unsigned host_file_held(void);
/* The Windows path (the ANSI code page's) of a host path, into out (size
 * bytes): 0, or -1 when it does not fit.  Whether the file exists is not
 * checked.  A mod's file: its path (the data folder's when the code page
 * cannot spell it).  Off Windows: the system's path (UTF-8, '/'), the parts
 * that exist in their real case. */
int  host_file_full_path(const char *path, char *out, uint32_t size);
#ifdef _WIN32
/* The same in UTF-16 (size: characters), which any file's name fits. */
int  host_file_full_path_w(const char *path, wchar_t *out, uint32_t size);
#endif
/* The same in UTF-8 (size: bytes), which any file's name fits: what SQLite
 * and the other libraries take.  Off Windows it is host_file_full_path's,
 * the parts that exist in their real case (platform/os/fs.h). */
int  host_file_full_path_utf8(const char *path, char *out, uint32_t size);
/* The file exists (a quiet probe: not logged as missing; not the game's
 * host_file_exists, T505). */
int  host_file_present(const char *path);
/* fn(name, arg) for each file directly in the folder `path` ("host:data/stream"):
 * the mod folders' and the data folder's, a name once (a mod's case first); no
 * sub-folders, nothing hidden.  Any thread. */
void host_file_list(const char *path, void (*fn)(const char *name, void *arg), void *arg);
int  host_file_create(const char *path);                /* for writing; -1 on error */
/* With gt3pc.ini [Debug] FileLog=on the opens are logged ("file: ..."; the
 * debug console shows them): each file read with its size, each one
 * written, a file missing.  Probes are not (file_resolve's io_getstat: the
 * size, the packed copy): on, the calling thread's opens are quiet. */
void host_file_quiet(int on);
void host_file_close(int fd);
int64_t host_file_size(int fd);
int  host_file_read_at(int fd, uint64_t offset, void *buf, uint32_t bytes);   /* bytes read */
int  host_file_write_at(int fd, uint64_t offset, const void *buf, uint32_t bytes);

/* A "RoFS" table of contents (the layout of include/engine/io/T500_vol_toc.h)
 * listing the folder `path` ("host:data") and everything under it, built in
 * a buffer from alloc(size); *size gets its size.  Entries are sorted by
 * name (byte order) as the volume's are; a file's record has its size, no
 * sector.  Left out: hidden and system files (desktop.ini, Thumbs.db),
 * "*.gz" (the packed form of a file next to it) and the install marker.
 * "host:data" lists the mod folders' files too.  NULL when the folder
 * cannot be listed. */
uint8_t *host_file_toc(const char *path, void *(*alloc)(uint32_t size), uint32_t *size);

#ifdef __cplusplus
}
#endif
