/* ui_text_test.cpp - checks the interface's languages off Windows
 * (src/platform/ui_text.cpp): the catalogues built in as the generated source
 * of cmake/embed_languages.cmake, the language of the session's locale
 * (SDL_GetPreferredLocales, or the same variables read without SDL) and the wide
 * formats' MSVC meaning (%s a wide string).
 *
 *   ui_text_test             with the generated catalogues linked in
 *   ui_text_test --none      a program without them (ui_text_none_test):
 *                            every language has no catalogue, tr gives the
 *                            English back
 *
 * Exits 0 when every check holds. */
#include "platform/ui_text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#ifndef GT3PC_UI_TEXT_NO_SDL
#include <SDL3/SDL_stdinc.h>
#endif

/* ui_text.cpp's log */
namespace pclog {
void vwrite(const char *, va_list) {}
void write(const char *, ...) {}
} // namespace pclog

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_failures++;
}

/* The locale variables as a session would have them. */
void locale(const char *language, const char *all, const char *lang)
{
    /* (SDL reads its own copy of the environment, taken once: both are set) */
    auto put = [](const char *name, const char *v) {
        if (v != nullptr)
            setenv(name, v, 1);
        else
            unsetenv(name);
#ifndef GT3PC_UI_TEXT_NO_SDL
        if (v != nullptr)
            SDL_setenv_unsafe(name, v, 1);
        else
            SDL_unsetenv_unsafe(name);
#endif
    };
    put("LANGUAGE", language);
    put("LC_ALL", all);
    put("LC_MESSAGES", nullptr);
    put("LANG", lang);
}

} // namespace

int main(int argc, char **argv)
{
    using namespace ui_text;
    const bool none = argc > 1 && strcmp(argv[1], "--none") == 0;
    unsetenv("GT3PC_LANG_DIR");

    if (none) {
        check(translated(kPortuguese) == -1, "without the generated source pt_BR has no catalogue");
        set_language(kPortuguese);
        check(strcmp(::tr("Language"), "Language") == 0, "  and tr(\"Language\") is the English");
        printf("%s\n", g_failures == 0 ? "ui_text_none_test: all checks passed" : "ui_text_none_test: FAILED");
        return g_failures == 0 ? 0 : 1;
    }

    /* the built-in catalogue */
    const int n = translated(kPortuguese);
    printf("      pt_BR: %d translations built in\n", n);
    check(n > 1000, "pt_BR's catalogue is built in");
    set_language(kPortuguese);
    check(language() == kPortuguese, "set_language(kPortuguese)");
    printf("      tr(\"Language\") = \"%s\"\n", ::tr("Language"));
    check(strcmp(::tr("Language"), "Idioma") == 0, "tr(\"Language\") is pt_BR's \"Idioma\"");
    check(wcscmp(::tr(L"Language"), L"Idioma") == 0, "tr(L\"Language\") is L\"Idioma\"");
    check(strcmp(::tr("no such string in any catalogue"), "no such string in any catalogue") == 0,
          "a string without a translation stays English");
    const std::string a = trf("the file could not be opened (%s)", "x.iso");
    printf("      trf = \"%s\"\n", a.c_str());
    check(a == "não foi possível abrir o arquivo (x.iso)", "trf: pt_BR's sentence, formatted");
    /* MSVC's wide %s: a wide string */
    const std::wstring w = trf(L"the file could not be opened (%s)", L"x.iso");
    check(w == L"não foi possível abrir o arquivo (x.iso)", "trf(L...): %s takes a wide string, as on Windows");
    check(trf(L"%d of %s and %hs, %c%%", 3, L"wide", "narrow", L'!') == L"3 of wide and narrow, !%",
          "trf(L...): %hs narrow, %c wide, %% kept");
    check(translated(kJapanese) > 1000 && translated(kRussian) > 1000 && translated(kPolish) > 1000,
          "ja, ru and pl are built in too");
    check(translated(kEnglish) == 0, "English has no catalogue (the code's own text)");
    set_language(kEnglish);
    check(strcmp(::tr("Language"), "Language") == 0, "English again: tr gives the English");

    /* the session's language */
    locale(nullptr, nullptr, "pt_BR.UTF-8");
    check(windows_language() == kPortuguese, "LANG=pt_BR.UTF-8: Portuguese");
    check(resolve(kAuto) == kPortuguese, "  resolve(kAuto) too");
    check(resolve(kFrench) == kFrench, "  a set language stays");
    /* (SDL's order on Unix: LANG, then LANGUAGE's list; LC_ALL not read) */
    locale("de:fr", "ru_RU.UTF-8", "pt_BR.UTF-8");
    check(windows_language() == kPortuguese, "LANG=pt_BR before LANGUAGE=de:fr, LC_ALL not read: Portuguese");
    locale("nl:ja:fr", nullptr, nullptr);
    check(windows_language() == kJapanese, "LANGUAGE=nl:ja:fr: the first one there is, Japanese");
    locale("fr", nullptr, "nl_NL.UTF-8");
    check(windows_language() == kFrench, "LANG=nl_NL, LANGUAGE=fr: French");
    locale(nullptr, nullptr, "pt_PT.UTF-8");
    check(windows_language() == kPortuguese, "LANG=pt_PT: Portuguese (the language part, as Windows' primary id)");
    locale(nullptr, nullptr, "nl_NL.UTF-8");
    check(windows_language() == kEnglish, "LANG=nl_NL: none of them, English");
    locale(nullptr, nullptr, "C");
    check(windows_language() == kEnglish, "LANG=C: English");

    printf("%s\n", g_failures == 0 ? "ui_text_test: all checks passed" : "ui_text_test: FAILED");
    return g_failures == 0 ? 0 : 1;
}
