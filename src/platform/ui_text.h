/* ui_text.h - GT3 PC's own interface in other languages: the launcher and
 * the game's PC panels (the F2 settings menu, the Esc menu, F10, the panels
 * and notices drawn over the picture) - not the game itself, whose language
 * is [Game] Language (platform/language.h).  PC (2026-11-16), user request:
 * "options to support more languages in the launcher, and F2 menu, not the
 * game itself though" - with the strings translated by a tool's catalogues,
 * not written in every language in the code.
 *
 * English is the only language written in the code.  tr("English") returns
 * the chosen language's text for it, or the English itself when the language
 * has none (a new or changed string shows in English until its translation is
 * made; nothing breaks).  N_("English") only marks a string for the catalogue
 * (a table's entry, translated where it is shown: tr(entry)).
 *
 * The catalogues: lang/<code>.po (gettext's format; msgid the English, msgstr
 * the translation), built into gt3launcher.exe and gt3pc.exe as resources of
 * the type GT3LANG named as the code upper case ("FR", "PT_BR":
 * cmake/languages.cmake); off Windows into the executables as byte arrays of
 * a generated source (cmake/embed_languages.cmake).  tools/lang_sync, built and run by every build,
 * extracts the tr / trf / N_ strings from the sources into
 * lang/template.pot, brings every .po up to date (new strings empty, changed
 * ones marked fuzzy with their old translation, gone ones kept as obsolete),
 * checks them (the same printf conversions in the same order, only
 * characters the overlay font draws in the game's panels' strings, their
 * help lines' widths) and reports what is left to translate
 * (build\languages.txt).  docs/languages.md "The interface's languages".
 *
 * A translation is taken only when its printf conversions are the English's
 * (ui_text_check.h; else the English, logged: a wrong one would read the
 * wrong arguments) and
 * it is not marked fuzzy.  GT3PC_LANG_DIR=<folder> reads <folder>\<code>.po
 * instead of the resource (a translator's aid: an edited file without a
 * build).  No Windows headers. */
#pragma once

#include <stddef.h>
#include <string>

namespace ui_text {

/* The languages, in the settings' order (the ini values: value()). */
enum Language {
    kEnglish,
    kFrench,
    kGerman,
    kItalian,
    kSpanish,
    kJapanese,
    kRussian,
    kPortuguese,                                /* Brazilian Portuguese */
    kPolish,
    kCount
};
constexpr int kAuto = -1;                       /* the setting: Windows' display language's */

/* The ini value ("auto", "english", "french" .. "portuguese_br", "polish"). */
const char *value(int lang);
/* The setting of an ini value: a language, or kAuto (also for an unknown one). */
int from_value(const char *v);
int from_value(const wchar_t *v);
/* The catalogue's code ("en", "fr" .. "pt_BR", "pl": lang/<code>.po). */
const char *code(int lang);
/* The language's own name, as its speakers write it: "Français", "日本語"
 * (UTF-8); upper case for the overlay's panels ("FRANÇAIS"); UTF-16. */
const char *native_name(int lang);
const char *native_upper(int lang);
const wchar_t *native_name_w(int lang);

/* Windows' display language as one of these (kEnglish when it is none);
 * elsewhere the first of the session's preferred languages that is one of
 * these (SDL_GetPreferredLocales: LANG, then LANGUAGE's list; the language
 * part only, as Windows' primary language id). */
int windows_language();
/* The language a setting stands for (kAuto: windows_language()). */
int resolve(int setting);

/* This process's language from now on (its catalogue read once; any
 * thread - a string tr returned earlier stays valid). */
void set_language(int lang);
int language();

/* The text for the English one (UTF-8 / UTF-16), or the English itself. */
const char *tr(const char *english);
const wchar_t *tr(const wchar_t *english);

/* The catalogue's state of a language (the launcher's list): its translated
 * strings, -1 when it has no catalogue. */
int translated(int lang);

} // namespace ui_text

/* tr("English"): the chosen language's text.  N_("English"): a mark for
 * tools/lang_sync only (the string is translated where it is shown).
 * trf("English %s", ...): a sentence with printf conversions, translated and
 * then formatted (a translation keeps the English's conversions in their
 * order - the tool checks it; a wide format's %s is a wide string, MSVC's
 * swprintf).  A sentence is one string, never made of translated pieces (the
 * word order differs between languages). */
inline const char *tr(const char *english) { return ui_text::tr(english); }
inline const wchar_t *tr(const wchar_t *english) { return ui_text::tr(english); }
std::string trf(const char *english, ...);
std::wstring trf(const wchar_t *english, ...);
#define N_(s) s
