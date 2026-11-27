/* main_posix.cpp - gt3launcher off Windows: the launcher's command line
 * modes (main.cpp's), without its window.  --install installs the game files
 * (the data folder) from a disc image; --unpack does the same from a gt3.vol
 * (an earlier version's install, or a disc's files extracted):
 *
 *   gt3launcher --install <image.iso> [--to <folder>]
 *   gt3launcher --unpack <gt3.vol> [--to <folder>]
 *
 * <folder> defaults to pcrun::user_dir's folder (platform/posix/user_dir.h):
 * $GT3PC_HOME when it is set, else the launcher's folder, else, when that
 * folder is read-only, $XDG_DATA_HOME/gt3pc (~/.local/share/gt3pc).  Output
 * goes to the standard output, UTF-8: on a terminal the progress line is
 * redrawn every percent, else a line every 10%.  Ctrl+C (SIGINT) or SIGTERM
 * cancels the install (the temporary folder is deleted).
 *
 * Exit codes (main.cpp's): 0 installed, 1 failed, 2 not a Gran Turismo 3
 * engine disc, 3 cancelled, 4 bad command line - also without arguments: the
 * launcher's window is Windows' only. */
#include "install.h"
#include "util.h"
#include "platform/posix/user_dir.h"

#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

enum ExitCode { kOk = 0, kFailed = 1, kWrongDisc = 2, kCancelled = 3, kUsage = 4 };

const char kUsageText[] =
    "Gran Turismo 3 A-Spec launcher\n"
    "\n"
    "  gt3launcher --install <image.iso> [--to <folder>]\n"
    "      installs the game files from a Gran Turismo 3 disc image into <folder>/data_US,\n"
    "      data_EU or data_JP (any other Gran Turismo 3 engine disc - GT Concept, a demo -\n"
    "      is extracted as data only into data_<boot file>); <folder> by default\n"
    "      $GT3PC_HOME, else the launcher's folder, else, when that one is read-only,\n"
    "      $XDG_DATA_HOME/gt3pc (~/.local/share/gt3pc)\n"
    "  gt3launcher --unpack <gt3.vol> [--to <folder>]\n"
    "      the same from the disc's GT3.VOL as a file (as an earlier version installed it;\n"
    "      a CORE.GT3 and SYSTEM.CNF beside it name the release)\n"
    "\n"
    "The launcher's window is only on Windows.\n"
    "\n"
    "Exit codes: 0 installed, 1 failed, 2 not a Gran Turismo 3 engine disc, 3 cancelled, 4 bad command line.\n";

void out(const std::wstring &s)
{
    const std::string u = util::narrow(s);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
}

volatile sig_atomic_t g_cancel = 0;

void on_signal(int)
{
    g_cancel = 1;
}

double seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

class ConsoleListener : public install::Listener {
public:
    explicit ConsoleListener(bool terminal) : terminal_(terminal) {}

    void phase(const std::wstring &text) override
    {
        end_progress_line();
        out(text + L"\n");
    }
    void progress(uint64_t done, uint64_t total) override
    {
        if (!total)
            return;
        int pct = (int)(done * 100 / total);
        /* A terminal line redrawn every percent; a log line every 10%. */
        if (terminal_) {
            if (pct == last_)
                return;
            out(L"\r  " + std::to_wstring(pct) + L"%  " + util::format_size(done) + L" of " + util::format_size(total) +
                L"   ");
            in_line_ = true;
        } else {
            if (pct / 10 == last_ / 10 && last_ >= 0)
                return;
            out(L"  " + std::to_wstring(pct) + L"%  " + util::format_size(done) + L" of " + util::format_size(total) +
                L"\n");
        }
        last_ = pct;
    }
    bool cancel_requested() override { return g_cancel != 0; }

    void end_progress_line()
    {
        if (in_line_)
            out(L"\n");
        in_line_ = false;
    }

private:
    bool terminal_;
    int last_ = -1;
    bool in_line_ = false;
};

int run_install(const std::wstring &source, bool from_volume, const std::wstring &folder)
{
    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    out(L"Installing the game files from " + source + L"\n  into " + folder + L"\n");

    ConsoleListener listener(isatty(STDOUT_FILENO) != 0);
    std::wstring message;
    const double t0 = seconds();
    install::Result r = from_volume ? install::install_from_volume(source, folder, listener, &message)
                                    : install::install_from_iso(source, folder, listener, &message);
    listener.end_progress_line();

    int code = kFailed;
    switch (r) {
    case install::Result::ok: {
        code = kOk;
        char secs[32];
        snprintf(secs, sizeof(secs), " in %.1f s.", seconds() - t0);
        if (!message.empty() && message.back() == L'.')
            message.pop_back();
        message += util::widen(secs);
        break;
    }
    case install::Result::cancelled:
        code = kCancelled;
        break;
    case install::Result::wrong_disc:
        code = kWrongDisc;
        break;
    default:
        break;
    }
    out(message + L"\n");
    return code;
}

int usage(const std::string &error)
{
    const std::string text = error.empty() ? std::string(kUsageText) : error + "\n\n" + kUsageText;
    fputs(text.c_str(), error.empty() ? stdout : stderr);
    return error.empty() ? kOk : kUsage;
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++)
        args.push_back(argv[i]);

    std::string iso, volume, folder;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "--help" || a == "-h")
            return usage(std::string());
        if ((a == "--install" || a == "--unpack" || a == "--to") && i + 1 < args.size()) {
            (a == "--install" ? iso : a == "--unpack" ? volume : folder) = args[++i];
            continue;
        }
        return usage("Unknown or incomplete argument: " + a);
    }
    if (iso.empty() && volume.empty() && folder.empty())
        return usage("No --install or --unpack: the launcher's window is only on Windows.");
    if (iso.empty() == volume.empty())
        return usage(iso.empty() ? "--to needs --install <image.iso> or --unpack <gt3.vol>."
                                 : "--install and --unpack cannot be used together.");
    /* (no --to: the folder the game looks in, user_dir.h) */
    const std::wstring to =
        folder.empty() ? util::widen(pcrun::user_dir(util::narrow(util::exe_folder()))) : util::widen(folder);
    return iso.empty() ? run_install(util::widen(volume), true, to) : run_install(util::widen(iso), false, to);
}
