/* main.cpp - gt3launcher.exe: the GT3 PC launcher.
 *
 * Without arguments it shows the launcher window (ui.cpp).  With --install
 * it installs the game files (the data folder) from a disc image without a
 * window, for scripts and tests; --unpack does the same from a gt3.vol (an
 * earlier version's install, or the development tree's
 * gt3_disc\files\gt3.vol):
 *
 *   gt3launcher.exe --install <image.iso> [--to <folder>]
 *   gt3launcher.exe --unpack <gt3.vol> [--to <folder>]
 *
 * (PC 2026-11-23: the image may be a CHD, chd_image.h.)
 *
 * PC (2026-11-14, engines.h): the window is preceded by the engine selector,
 * which starts this launcher's own window or another engine's launcher as
 *
 *   gt3launcher.exe --engine [--shared <folder>]
 *
 * --engine: no selector (the window at once); --shared: the folder whose
 * gt3pc.ini is read and written, the selector's launcher's.
 *
 * <folder> defaults to the launcher's folder.  The program is a GUI
 * program, so the shell does not wait for it: use "start /wait" in cmd, or
 * pipe its output in PowerShell (... | Out-Host).  Output goes to the
 * redirected standard output, else to the console of the parent; with
 * neither, the result is shown in a message box.  Ctrl+C in the console
 * cancels the install (the temporary folder is deleted).
 *
 * Exit codes: 0 installed, 1 failed, 2 not a Gran Turismo 3 engine disc, 3
 * cancelled, 4 bad command line. */
#include "engines.h"
#include "install.h"
#include "ui.h"
#include "util.h"
#include "platform/ui_text.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <string>
#include <vector>

namespace {

enum ExitCode { kOk = 0, kFailed = 1, kWrongDisc = 2, kCancelled = 3, kUsage = 4 };

const wchar_t kUsageText[] =
    L"Gran Turismo 3 A-Spec launcher\n"
    L"\n"
    L"  gt3launcher.exe\n"
    L"      the launcher window\n"
    L"  gt3launcher.exe --install <image.iso> [--to <folder>]\n"
    L"      installs the game files from a Gran Turismo 3 disc image (an .iso or a .chd) into <folder>\\data_US,\n"
    L"      data_EU or data_JP (any other Gran Turismo 3 engine disc - GT Concept, a demo -\n"
    L"      is extracted as data only into data_<boot file>), <folder> by default the\n"
    L"      launcher's, without a window\n"
    L"  gt3launcher.exe --unpack <gt3.vol> [--to <folder>]\n"
    L"      the same from the disc's GT3.VOL as a file (as an earlier version installed it;\n"
    L"      a CORE.GT3 and SYSTEM.CNF beside it name the release)\n"
    L"\n"
    L"Exit codes: 0 installed, 1 failed, 2 not a Gran Turismo 3 engine disc, 3 cancelled, 4 bad command line.\n";

/* Where the command line mode writes. */
class Output {
public:
    Output()
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
            h_ = h;
        } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            h_ = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
            if (h_ == INVALID_HANDLE_VALUE)
                h_ = nullptr;
            attached_ = h_ != nullptr;
        }
        DWORD mode;
        console_ = h_ && GetConsoleMode(h_, &mode);
        /* The shell printed its prompt already: start on a new line. */
        if (attached_)
            write(L"\n");
    }

    bool available() const { return h_ != nullptr; }
    bool console() const { return console_; }

    void write(const std::wstring &s)
    {
        if (!h_)
            return;
        DWORD n;
        if (console_) {
            WriteConsoleW(h_, s.data(), (DWORD)s.size(), &n, nullptr);
        } else {
            /* A pipe or a file: UTF-8 with CR LF line ends. */
            std::wstring crlf;
            for (wchar_t c : s) {
                if (c == L'\n')
                    crlf += L'\r';
                crlf += c;
            }
            std::string u = util::narrow(crlf);
            WriteFile(h_, u.data(), (DWORD)u.size(), &n, nullptr);
        }
    }

private:
    HANDLE h_ = nullptr;
    bool console_ = false;
    bool attached_ = false;
};

std::atomic<bool> g_cancel{false};

BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cancel = true;
        return TRUE;
    }
    return FALSE;
}

class ConsoleListener : public install::Listener {
public:
    explicit ConsoleListener(Output &out) : out_(out) {}

    void phase(const std::wstring &text) override
    {
        end_progress_line();
        out_.write(text + L"\n");
    }
    void progress(uint64_t done, uint64_t total) override
    {
        if (!total)
            return;
        int pct = (int)(done * 100 / total);
        /* A console line redrawn every percent; a log line every 10%. */
        if (out_.console()) {
            if (pct == last_)
                return;
            out_.write(L"\r  " + std::to_wstring(pct) + L"%  " + util::format_size(done) + L" of " + util::format_size(total) +
                       L"   ");
            in_line_ = true;
        } else {
            if (pct / 10 == last_ / 10 && last_ >= 0)
                return;
            out_.write(L"  " + std::to_wstring(pct) + L"%  " + util::format_size(done) + L" of " + util::format_size(total) +
                       L"\n");
        }
        last_ = pct;
    }
    bool cancel_requested() override { return g_cancel; }

    void end_progress_line()
    {
        if (in_line_)
            out_.write(L"\n");
        in_line_ = false;
    }

private:
    Output &out_;
    int last_ = -1;
    bool in_line_ = false;
};

int run_install(const std::wstring &source, bool from_volume, const std::wstring &folder)
{
    Output out;
    if (out.console())
        SetConsoleCtrlHandler(on_ctrl, TRUE);
    out.write(L"Installing the game files from " + source + L"\n  into " + folder + L"\n");

    ConsoleListener listener(out);
    std::wstring message;
    DWORD t0 = GetTickCount();
    install::Result r = from_volume ? install::install_from_volume(source, folder, listener, &message)
                                    : install::install_from_iso(source, folder, listener, &message);
    listener.end_progress_line();

    int code = kFailed;
    switch (r) {
    case install::Result::ok: {
        code = kOk;
        wchar_t secs[32];
        swprintf(secs, 32, L" in %.1f s.", (GetTickCount() - t0) / 1000.0);
        if (!message.empty() && message.back() == L'.')
            message.pop_back();
        message += secs;
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
    if (out.available())
        out.write(message + L"\n");
    else
        MessageBoxW(nullptr, message.c_str(), L"Gran Turismo 3 A-Spec", MB_OK | (code == kOk ? MB_ICONINFORMATION : MB_ICONERROR));
    return code;
}

/* PC (2026-11-16): the interface's language (platform/ui_text.h), [Interface]
 * Language of gt3pc.ini - the shared one's with --shared - for the selector
 * and the window (the command line modes stay English: scripts read them). */
void set_interface_language(const std::wstring &shared)
{
    const std::wstring ini = (shared.empty() ? util::exe_folder() : util::with_slash(shared)) + L"gt3pc.ini";
    wchar_t v[32] = L"";
    GetPrivateProfileStringW(L"Interface", L"Language", L"auto", v, 32, ini.c_str());
    ui_text::set_language(ui_text::resolve(ui_text::from_value(v)));
}

int usage(const std::wstring &error)
{
    Output out;
    std::wstring text = error.empty() ? std::wstring(kUsageText) : error + L"\n\n" + kUsageText;
    if (out.available())
        out.write(text);
    else
        MessageBoxW(nullptr, text.c_str(), L"Gran Turismo 3 A-Spec", MB_OK | (error.empty() ? MB_ICONINFORMATION : MB_ICONERROR));
    return error.empty() ? kOk : kUsage;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; i++)
        args.push_back(argv[i]);
    if (argv)
        LocalFree(argv);

    std::wstring iso, volume, folder, shared;
    bool engine = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::wstring &a = args[i];
        if (a == L"--help" || a == L"-h" || a == L"/?")
            return usage(std::wstring());
        /* PC (2026-11-14): started by an engine selector (engines.h) */
        if (a == L"--engine") {
            engine = true;
            continue;
        }
        if (a == L"--shared" && i + 1 < args.size()) {
            shared = args[++i];
            continue;
        }
        if ((a == L"--install" || a == L"--unpack" || a == L"--to") && i + 1 < args.size()) {
            (a == L"--install" ? iso : a == L"--unpack" ? volume : folder) = args[++i];
            continue;
        }
        return usage(L"Unknown or incomplete argument: " + a);
    }
    if (iso.empty() && volume.empty() && folder.empty()) {
        /* The window.  An engine started by a selector has none of its own
         * (a selector never leads to another); else the selector first: this
         * launcher's window, or another engine's launcher in its place. */
        set_interface_language(shared);
        if (!engine) {
            const std::wstring own = util::exe_folder();
            engines::migrate(own);
            for (;;) {
                std::wstring launcher, other, error;
                const engines::Choice c = engines::select(GetModuleHandleW(nullptr), own, show, &launcher, &other);
                if (c == engines::Choice::exit)
                    return 0;
                if (c == engines::Choice::own)
                    break;
                if (engines::start(launcher, other, own, &error))
                    return 0;
                MessageBoxW(nullptr,
                            (trf(L"The engine's launcher could not be started: %s.", error.c_str()) + L"\n\n" + launcher)
                                .c_str(),
                            L"Gran Turismo 3 A-Spec", MB_OK | MB_ICONERROR);
            }
            shared.clear();
        }
        return ui::run(show, shared);
    }
    if (iso.empty() == volume.empty())
        return usage(iso.empty() ? L"--to needs --install <image.iso> or --unpack <gt3.vol>."
                                 : L"--install and --unpack cannot be used together.");
    if (folder.empty())
        folder = util::exe_folder();
    return iso.empty() ? run_install(volume, true, folder) : run_install(iso, false, folder);
}
