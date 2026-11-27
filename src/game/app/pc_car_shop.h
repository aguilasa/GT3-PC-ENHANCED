/* pc_car_shop.h - PC (2026-11-14, no original): the cars the game does not
 * sell bought as any other ([Game] BuyNotForSale, the settings menu's GAME >
 * EXTRAS > BUY CARS NOT FOR SALE, the launcher's Game > Extras).
 *
 * A car record's shop flags (CarEquipments::getShopFlags, the database's CAR
 * ShopFlags): bit 0, not sold by the dealers - the dealer pages show its
 * price as "----" (make_goods_car_price on the brand's list, text_car_buy_price
 * on the car's page, T036) and the Buy button's check refuses it with
 * "00cmn/not_for_buy" (check_cd_buy_sure, T045); bit 1, not traded - the
 * trade's buy from a guest garage (hm/hm_car_trade) refuses it the same way
 * (check_hm_buy_sure) and the car has no sell price.  Every car of the US
 * database's 244 with bit 1 has bit 0 too (53 with bit 0); 26 of them are on
 * the US disc's dealer pages: the race and rally cars (787B, GT-One, R390,
 * ZZII, the WRC cars, the cup and touring cars ...), the concept cars (RX-8,
 * Z, Viper GTS-R concept), the Nismo GT-R LM, the Vanquish, the Cerbera Speed
 * 12, the regional S2000s.  Each has a price of its own in its record (the
 * one the game pays a quarter of when it is sold).
 *
 * On, those prices are shown and the two buy checks let the cars through:
 * bought as any dealer car, at the record's price (x 100 in Japanese, as
 * every price), new engine, the colour chosen.  Selling is the game's own:
 * a car with bit 1 still cannot be sold (no sell price), one with bit 0 only
 * is sold for a quarter as a prize car is.  The cars the dealer pages do not
 * list (the F1 cars, the samples, the _ext prize versions ...) stay out of
 * reach: nothing here adds a car to a page. */
#ifndef GAME_APP_PC_CAR_SHOP_H
#define GAME_APP_PC_CAR_SHOP_H

#include "game/game.h"

/* The setting: 1 on. */
int pc_buy_not_for_sale(void);

/* A car record's shop flags as the buying side reads them: bits 0 and 1
 * cleared with the setting on (the dealer's "----" and its refusal, the
 * trade's refusal), the flags themselves off. */
int pc_buy_flags(int shopFlags);

/* PC (2026-11-23, user: "An option to toggle driving assists for new cars
 * added to the garage ... While the setting is on, cars that get added have
 * those 2 settings [TCS, ASM] set to 0 / off"; [Game] NewCarAssistsOff, GAME
 * > EXTRAS > NEW CARS WITHOUT ASSISTS, the launcher's Game > Extras):
 * GameStatus::addCar (T429) - every car that joins the garage, bought new
 * or used, won, the debug fill's.  With the setting on the car's two
 * settings go to the ends the setting page shows as 0: settingASCC 1 (the
 * ASM slider's least, T082 setting_slider_elec_ascc), settingTCSC the TCS
 * record's count (its slider is reversed: count + 1 - the level).  The parts
 * stay fitted: the page can turn them up again. */
class CarEquipments;
void pc_new_car_assists(CarEquipments *car);

#endif /* GAME_APP_PC_CAR_SHOP_H */
