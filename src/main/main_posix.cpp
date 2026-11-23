/* main_posix.cpp - the entry point of GT3 PC off Windows (main.cpp's
 * WinMain, step by step).
 *
 * main opens the log next to the program (the last run's kept), installs
 * the crash report, reserves the game heap, finds the game files, opens
 * the window, starts the platform services and runs the game's main
 * (gt3_main, src/game/app/T000_main.cpp) on the first game thread, then
 * runs the window's event loop until it closes.  The Esc menu's Restart
 * replaces the process with a new run (platform/posix/run.h). */
#include "pc_static_init.h"
#include "engine/media/native/still_cache.h"
#include "platform/audio.h"
#include "platform/crash.h"
#include "platform/gamepad.h"
#include "platform/gpu.h"
#include "platform/heap_arena.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/play_time.h"
#include "platform/posix/run.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_messagebox.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>

int gt3_main(int argc, char **argv);

namespace {

/* The main thread's priority before the game lowers it (T002 drops it to
 * 9 once the render thread, 7, is up). */
constexpr int kMainThreadPriority = 1;
/* The PS2 main thread had a 1 MB stack (crt0 _stack_size); some game
 * functions keep large arrays on it (play_saved_replay 0x41980 bytes), and
 * these frames are not the egcs ones: room to spare. */
constexpr int kMainThreadStack = 16 * 1024 * 1024;

/* What the bootstrap passed to CORE.GT3. */
char kArg0[] = "cdrom0:\\CORE.GT3;1";
char *g_gameArgv[] = {kArg0, nullptr};

void game_main_thread(void *)
{
    pc_run_static_inits();
    int r = gt3_main(1, g_gameArgv);
    pclog::write("game: main returned %d", r);
    /* the window's loop ends as when it is closed */
    SDL_Event quit = {};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

void error_box(const char *text)
{
    pclog::write("%s", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gran Turismo 3", text, nullptr);
}

} // namespace

int main(int argc, char **argv)
{
    pcrun::wait_for_previous_run(argc, argv);

    const std::string dir = pcrun::exe_dir();
    pclog::open(pcrun::rotate_log(dir).c_str());
    pclog::write("Gran Turismo 3 A-Spec (PC)");
    if (pcrun::new_game_run(argc, argv)) {
        system_menu::set_new_game_run();
        pclog::write("restart: a new game: no autoload");
    }
    crash::install();
    {
        uint32_t heapSize;
        heap_arena(&heapSize);  /* (before the libraries split the address space up) */
    }

    if (!paths::init(argc, argv)) {
        /* a disc image that could not be used (its path is UTF-8: any file's name) */
        if (!paths::init_error().empty()) {
            error_box(paths::init_error().c_str());
            return 1;
        }
        error_box("The game files were not found (the data folder next to gt3pc).\n\n"
                  "Run the GT3 installer to install them from your Gran Turismo 3 A-Spec disc image.");
        return 1;
    }

    settings::load();   /* gt3pc.ini: the display mode */
    play_time::start(); /* gt3pc.ini [Stats] PlayTime: counted from here */
    if (settings::debug_console())
        pclog::open_console();
    gt3media::still_cache_prepare();    /* the arcade menus' stills: no disk or decoder in the menus */
    if (!window::create("Gran Turismo 3 A-Spec"))
        pclog::fatal("could not create the window");
    if (!gpu::init(window::handle()))
        pclog::fatal("Direct3D 11 could not start on this PC");

    gamepad::init();    /* the game controllers (SDL3); keyboard only without them */
    sched::init();
    audio::init();      /* the native SPU2 (snd_native_init) and the sound output (on sched's clock) */
    sched::start_main_thread(game_main_thread, nullptr, kMainThreadPriority, kMainThreadStack);

    int code = window::run_message_loop();
    pclog::write("window closed");
    /* where the game was (a softlock closed with the window: its evidence) */
    sched::log_threads("the window closed");
    gamepad::shutdown();    /* the motors stop */
    audio::shutdown();      /* the output stopped and silent, the dump closed */
    play_time::stop();      /* the count written */
    const bool restart = system_menu::restart_requested();
    if (restart)
        pclog::write("restart: this process is replaced by a new run");
    pclog::close();
    if (restart)
        pcrun::restart(argc, argv, system_menu::restart_as_new_game());
    /* The game threads never end by themselves: no static destructors under
     * them (as ExitProcess). */
    fflush(nullptr);
    _exit(code);
}
