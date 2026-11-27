/* pc_ffb.cpp - the wheels' force feedback, the game's side (pc_ffb.h). */
#include "game/app/pc_ffb.h"
#include "game/save/T428_game_options.h"
#include "game/save/T429_game_status.h"
#include "libc.h"                               /* printf, snprintf */
#include "platform/settings_menu.h"             /* notice */
#include "platform/ui_text.h"                   /* the interface's languages */
#include "platform/wheel.h"                     /* set_game_ffb */

void pc_ffb_sync(void)
{
    gt2k_game::GameStatus *gs = getGameStatus();
    if (gs == NULL)
        return;                                 /* (not made yet: the option counts as on) */
    const ::GameOptions *o = gs->getGameOptions();
    wheel::set_game_ffb(0, o->forceFeedback1p != 0);
    wheel::set_game_ffb(1, o->forceFeedback2p != 0);
}

void pc_ffb_notice(int player)
{
    char text[640];
    snprintf(text, sizeof(text), "%s\n%s", trf("FORCE FEEDBACK IS OFF (PLAYER %d)", player + 1).c_str(),
             tr("THE GAME'S OWN FORCE FEEDBACK OPTION IS OFF: THE WHEEL\nGETS NO STEERING FORCE. TURN IT ON IN THE "
                "GAME'S OPTIONS."));
    settings_menu::notice(text, 8.0);
    printf("PC: force feedback: player %d plays with the wheel and the game's own Force Feedback option is off: no "
           "steering force (the notice shown)\n",
           player + 1);
}
