/* util.cpp - small helpers shared by the launcher's files (util.h). */
#include "util.h"
#include "platform/ui_text.h"                   /* PC (2026-11-16): the interface's languages */
#include "platform/os/fs.h"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

#include <stdio.h>
#include <wchar.h>

namespace util {

std::wstring system_error_text(unsigned long code)
{
#ifndef _WIN32
    return fs::widen(fs::error_text(code));
#else
    wchar_t *text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
    std::wstring s;
    if (n && text)
        s.assign(text, n);
    if (text)
        LocalFree(text);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    if (s.empty()) {
        wchar_t buf[32];
        swprintf(buf, 32, L"error %lu", code);
        s = buf;
    }
    return s;
#endif
}

/* (PC 2026-11-16: the units in the interface's language - French Go / Mo / Ko;
 * the command line mode sets none: English) */
std::wstring format_size(uint64_t bytes)
{
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
    if (bytes >= (uint64_t)gb)
        return trf(L"%.2f GB", bytes / gb);
    if (bytes >= (uint64_t)mb)
        return trf(L"%.0f MB", bytes / mb);
    if (bytes >= (uint64_t)kb)
        return trf(L"%.0f KB", bytes / kb);
    return trf(L"%u bytes", (unsigned)bytes);
}

std::wstring exe_folder()
{
    const std::wstring path = fs::widen(fs::exe_path());
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(1, L'.') + (wchar_t)fs::kSep : path.substr(0, slash + 1);
}

std::wstring with_slash(std::wstring folder)
{
    if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
        folder += (wchar_t)fs::kSep;
    return folder;
}

bool make_folders(const std::wstring &folder)
{
    return fs::make_dirs(fs::narrow(folder));
}

std::wstring widen(const std::string &s)
{
    return fs::widen(s);
}

std::string narrow(const std::wstring &w)
{
    return fs::narrow(w);
}

#ifdef _WIN32
std::wstring choose_folder(void *owner, const wchar_t *title, const wchar_t *ok, const std::wstring &start)
{
    std::wstring out;
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
        return out;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    d->SetTitle(title);
    d->SetOkButtonLabel(ok);
    IShellItem *at = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&at)))) {
        d->SetFolder(at);
        at->Release();
    }
    IShellItem *it = nullptr;
    if (SUCCEEDED(d->Show((HWND)owner)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out = p;
            CoTaskMemFree(p);
        }
        it->Release();
    }
    d->Release();
    return out;
}
#endif

} // namespace util
