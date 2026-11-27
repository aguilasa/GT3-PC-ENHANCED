/* joy_names.cpp - the names joysticks go by in gt3pc.ini (joy_names.h). */
#include "platform/joy_names.h"

#include <algorithm>
#include <map>
#include <string.h>
#include <vector>
#ifndef _WIN32
#include "platform/os/win_compat.h"         /* _stricmp */
#endif

namespace joy_names {
namespace {

/* Whether a joystick's force feedback opens, found out once (SDL's thread
 * alone).  SDL_IsJoystickHaptic is not enough: it said yes for the part of
 * the user's Fanatec base that then failed to open ("Haptic:
 * SDL_SYS_HapticOpenFromJoystick failed").  Asked only of joysticks that
 * share their name with another one: opening a wheel's force feedback takes
 * its own centring away until it is closed again. */
std::map<SDL_JoystickID, bool> g_ffb;

bool ffb_opens(SDL_JoystickID id)
{
    const auto it = g_ffb.find(id);
    if (it != g_ffb.end())
        return it->second;
    bool opens = false;
    SDL_Joystick *joy = SDL_OpenJoystick(id);   /* (one more reference when it is open already) */
    if (joy != nullptr) {
        if (SDL_WasInit(SDL_INIT_HAPTIC) != 0 && SDL_IsJoystickHaptic(joy)) {
            SDL_Haptic *h = SDL_OpenHapticFromJoystick(joy);
            opens = h != nullptr;
            if (h != nullptr)
                SDL_CloseHaptic(h);
        }
        SDL_CloseJoystick(joy);
    }
    g_ffb[id] = opens;
    return opens;
}

struct Same {
    SDL_JoystickID id;
    bool ffb;
    std::string path;
};

/* The connected joysticks of id's SDL name, in their order (joy_names.h);
 * empty when SDL has no name for id.  One joystick: not probed. */
std::vector<Same> same_named(SDL_JoystickID id, std::string *name)
{
    std::vector<Same> same;
    const char *own = SDL_GetJoystickNameForID(id);
    if (own == nullptr)
        return same;
    *name = own;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; ids != nullptr && i < n; i++) {
        const char *other = SDL_GetJoystickNameForID(ids[i]);
        if (other == nullptr || _stricmp(other, name->c_str()) != 0)
            continue;
        const char *path = SDL_GetJoystickPathForID(ids[i]);
        same.push_back({ids[i], false, path != nullptr ? path : ""});
    }
    SDL_free(ids);
    if (same.size() < 2)
        return same;
    for (Same &s : same)
        s.ffb = ffb_opens(s.id);
    std::sort(same.begin(), same.end(), [](const Same &a, const Same &b) {
        if (a.ffb != b.ffb)
            return a.ffb;
        const int c = _stricmp(a.path.c_str(), b.path.c_str());
        return c != 0 ? c < 0 : a.id < b.id;
    });
    return same;
}

} // namespace

std::string of(SDL_JoystickID id)
{
    std::string name;
    const std::vector<Same> same = same_named(id, &name);
    for (size_t i = 1; i < same.size(); i++)
        if (same[i].id == id)
            return name + " #" + std::to_string(i + 1);
    return name;
}

bool lesser(SDL_JoystickID id)
{
    std::string name;
    const std::vector<Same> same = same_named(id, &name);
    if (same.size() < 2)
        return false;
    const Uint16 vendor = SDL_GetJoystickVendorForID(id), product = SDL_GetJoystickProductForID(id);
    bool own = false, sibling = false;
    for (const Same &s : same) {
        if (s.id == id)
            own = s.ffb;
        else if (s.ffb && SDL_GetJoystickVendorForID(s.id) == vendor && SDL_GetJoystickProductForID(s.id) == product)
            sibling = true;
    }
    return sibling && !own;
}

bool haptic(SDL_JoystickID id)
{
    static std::map<SDL_JoystickID, bool> known;
    const auto it = known.find(id);
    if (it != known.end())
        return it->second;
    bool yes = false;
    SDL_Joystick *joy = SDL_OpenJoystick(id);   /* (one more reference when it is open already) */
    if (joy != nullptr) {
        yes = SDL_WasInit(SDL_INIT_HAPTIC) != 0 && SDL_IsJoystickHaptic(joy);
        SDL_CloseJoystick(joy);
    }
    if (known.size() > 256)
        known.clear();                          /* (ids are not reused: years of plugging) */
    known[id] = yes;
    return yes;
}

void forget(SDL_JoystickID id)
{
    g_ffb.erase(id);
}

} // namespace joy_names
