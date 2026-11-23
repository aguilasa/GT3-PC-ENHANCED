/* win_ini_probe.cpp - writes window_test_win.ini on Windows: Controller 1's
 * profile with Cross on the key A (`Cross=button:south;key:A'), through
 * the Windows build's own input_map::save (WritePrivateProfileStringA).
 * window_test reads it on Linux (the binding the same, VK 0x41).
 *
 * Not part of this folder's CMake project (a Windows program).  Built with
 * the Windows cross toolchain (cmake/toolchains/windows-clang-cl.cmake's
 * clang-cl and xwin), in bash, in an empty folder (R: the repository):
 *
 *   X=$HOME/opt/xwin; I=; for d in crt/include sdk/include/ucrt sdk/include/um sdk/include/shared; do
 *     I="$I /imsvc $X/$d"; done
 *   /usr/lib/llvm-18/bin/clang-cl --target=i686-pc-windows-msvc /nologo /O1 /EHsc /std:c++17 -msse2 \
 *     /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH $I /I$R/src /c $R/tools/platform_tests/win_ini_probe.cpp \
 *     $R/src/platform/input_map.cpp
 *   /usr/lib/llvm-18/bin/lld-link /nologo /subsystem:console /out:win_ini_probe.exe win_ini_probe.obj \
 *     input_map.obj /libpath:$X/crt/lib/x86 /libpath:$X/sdk/lib/um/x86 /libpath:$X/sdk/lib/ucrt/x86 \
 *     kernel32.lib user32.lib
 *
 * and run on Windows (or Wine) as `win_ini_probe.exe C:\some\gt3pc.ini'. */
#include "platform/input_map.h"

#include <windows.h>
#include <stdio.h>

namespace ui_text {
/* (the labels are not written: no catalogues needed) */
const char *tr(const char *english)
{
    return english;
}
const wchar_t *tr(const wchar_t *english)
{
    return english;
}
} // namespace ui_text

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    GetFullPathNameA(argc > 1 ? argv[1] : "gt3pc.ini", MAX_PATH, path, nullptr);
    input_map::Profile p;
    input_map::defaults(input_map::kController1, &p);
    input_map::Binding a;
    a.source = input_map::Source::key;
    a.index = 'A';                          /* the virtual key VK 0x41 */
    p.map[input_map::PAD_CROSS][1] = a;
    input_map::save(path, input_map::kController1, p);
    char line[400];
    GetPrivateProfileStringA("Controller1", "Cross", "", line, sizeof line, path);
    printf("wrote %s: Cross=%s\n", path, line);
    return 0;
}
