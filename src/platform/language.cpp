/* language.cpp - the game's language in GT3 PC (language.h). */
#include "platform/language.h"
#include "platform/host_file.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/settings.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): the interface's languages (N_) */

#include <string.h>

#include <mutex>
#include <string>
#ifndef _WIN32
#include "platform/os/win_compat.h"         /* _stricmp */
#endif

namespace language {
namespace {

const char *const kCodes[kCount] = {"JP", "US", "BR", "FR", "GR", "IT", "SP"};
/* (the F2 menu's names: English, the log's too; translated where shown) */
const char *const kNames[kCount] = {N_("JAPANESE"), N_("ENGLISH (US)"), N_("ENGLISH (UK)"), N_("FRENCH"),
                                    N_("GERMAN"),   N_("ITALIAN"),      N_("SPANISH")};

bool present(const std::string &path)
{
    return host_file_present(("data/" + path).c_str()) != 0;
}

/* The language of a country code ("BR" -> 2), -1 if none. */
int of_code(const std::string &c)
{
    for (int i = 0; i < kCount; i++)
        if (_stricmp(c.c_str(), kCodes[i]) == 0)
            return i;
    return -1;
}

/* Taken once (the first call of current(), by game_init). */
std::once_flag g_once;
int g_current = kEnglishUS;
std::string g_db[3], g_param, g_gcf;

/* A file of the language's region: Japanese "<base>.<ext>", English (US)
 * "<base>_us.<ext>", the European languages "<base>_eu.<ext>". */
std::string region_file(int lang, const char *base, const char *ext)
{
    const char *suffix = lang == kJapanese ? "" : lang == kEnglishUS ? "_us" : "_eu";
    return std::string(base) + suffix + ext;
}

void take()
{
    g_current = chosen();
    /* the databases: the language's, else the disc's own, else the US
     * build's (an earlier launcher's install: the US disc) */
    static const char *const kBases[3] = {"database/paramdb", "database/paramstr", "database/paramunistr"};
    static const char *const kKeys[3] = {"database", "strings", "unistrings"};
    /* converted (paramdb<r>.sqlite3): the first of the three with its
     * database or its files decides */
    std::string disc = paths::install_info("database");
    if (disc.size() > 3 && _stricmp(disc.c_str() + disc.size() - 3, ".db") == 0)
        disc.resize(disc.size() - 3);
    else
        disc.clear();
    for (const std::string &base : {region_file(g_current, kBases[0], ""), disc, std::string("database/paramdb_us")}) {
        if (base.empty())
            continue;
        if (present(base + ".sqlite3")) {
            g_param = base + ".sqlite3";
            break;
        }
        if (present(base + ".db"))
            break;
    }
    for (int k = 0; k < 3; k++) {
        std::string f = region_file(g_current, kBases[k], ".db");
        if (!present(f)) {
            const std::string disc = paths::install_info(kKeys[k]);
            f = !disc.empty() && present(disc) ? disc : std::string(kBases[k]) + "_us.db";
        }
        g_db[k] = f;
    }
    g_gcf = "gameconf/product.gcf";
    if (g_current != kJapanese) {
        /* (or its database, gameconf/product_xx.sqlite3: game/app/pc_product_config.h) */
        const std::string f = region_file(g_current, "gameconf/product", ".gcf");
        if (present(f) || present(f.substr(0, f.size() - 4) + ".sqlite3"))
            g_gcf = f;
    }
    if (!g_param.empty())
        pclog::write("language: %s (%s), %s, %s", kNames[g_current], kCodes[g_current], g_param.c_str(),
                     g_gcf.c_str());
    else
        pclog::write("language: %s (%s), %s, %s, %s, %s", kNames[g_current], kCodes[g_current], g_db[0].c_str(),
                     g_db[1].c_str(), g_db[2].c_str(), g_gcf.c_str());
}

} // namespace

const char *code(int lang)
{
    return lang >= 0 && lang < kCount ? kCodes[lang] : "US";
}

const char *name(int lang)
{
    return lang >= 0 && lang < kCount ? kNames[lang] : "?";
}

bool installed(int lang)
{
    /* probed once (the settings menu asks every frame) */
    static std::once_flag once;
    static bool has[kCount];
    std::call_once(once, [] {
        for (int i = 0; i < kCount; i++)
            has[i] = present(std::string("menu/") + kCodes[i] + "/gt_mode301.pmb");
    });
    return lang >= 0 && lang < kCount && has[lang];
}

int disc_language()
{
    const int l = of_code(paths::install_info("language"));
    return l >= 0 ? l : kEnglishUS;
}

int chosen()
{
    const int s = settings::language();
    if (s != settings::kLanguageAuto && installed(s))
        return s;
    return disc_language();
}

int current()
{
    std::call_once(g_once, take);
    return g_current;
}

const char *car_database(int kind)
{
    current();
    return kind >= 0 && kind < 3 ? g_db[kind].c_str() : g_db[0].c_str();
}

const char *param_database()
{
    current();
    return g_param.c_str();
}

const char *product_config()
{
    current();
    return g_gcf.c_str();
}

} // namespace language
