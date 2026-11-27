/* pc_car_shop.cpp - PC (2026-11-14, no original): the cars the game does not
 * sell bought as any other (pc_car_shop.h). */
#include "game/app/pc_car_shop.h"
#include "game/data/T482_car_equipments.h"      /* PC (2026-11-23): CarEquipments, TcsRecord */
#include "platform/settings.h"

#include <stdio.h>

int pc_buy_not_for_sale(void)
{
    return settings::buy_not_for_sale() ? 1 : 0;
}

int pc_buy_flags(int shopFlags)
{
    return pc_buy_not_for_sale() ? (shopFlags & ~3) : shopFlags;
}

/* PC (2026-11-23) */
void pc_new_car_assists(CarEquipments *car)
{
    if (car == NULL || !settings::new_car_assists_off())
        return;
    const TcsRecord *tcs = car->getTCSCRecord();
    const int tcsWas = car->settingTCSC, asmWas = car->settingASCC;
    if (tcs != NULL)
        car->settingTCSC = tcs->count;      /* (the page's level: count - the setting) */
    car->settingASCC = 1;                   /* (the page's level: the setting - 1) */
    printf("PC: new car without assists: TCS %d -> 0, ASM %d -> 0\n", tcs != NULL ? tcs->count - tcsWas : 0,
           asmWas - 1);
}
