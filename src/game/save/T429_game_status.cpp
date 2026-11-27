/* T429 - gt2k_game::GameStatus: the game's save data (0x002185D8..0x0021AB08).
 *
 * One instance (g_00352A94, gp-0x4F5C), created by createGameStatus /
 * T004.  The garage: 200 GarageCar slots (cars[], free when !valid) of
 * which the used ones are linked in carList (new cars at the head), and 200
 * GarageWheel slots linked in wheelList.  runningCarIndex is the position
 * of the car the player drives in carList (-1 none); editableCarIndex the
 * position of the race car of MenuStatus in carList (-1 when it is not a
 * garage car).  As a Serialize::Base it saves the 0x130 bytes from +0x08,
 * the listed cars and wheels and the record blocks (version 7.1, class
 * "GameStatus").
 *
 * Money: buying a car costs its price, selling gives a quarter of it; the
 * wheels cost the "wheel" service price (a quarter back when sold).
 *
 * The end of the unit is code egcs emits for inline / template functions:
 * the accessors of GameStatus (inline in T429_game_status.h), the
 * List<GarageWheel> / List<GarageCar> destructors (menu_list.h), the
 * typeinfo functions and destructors of the two lists, and unreferenced
 * copies of GarageCar's (T036_garage.h).
 */
/* .rodata: <new>'s "bad_alloc" (0x00340588) opens the file's data (nothing
 * references it); then the literals, the vtables and the type names of what
 * the original saw: the GameStatus (with the garage), then the race classes
 * in the order of T000 / T007 (UnitArena<64>, RaceCameraBase, the race
 * displays, the menu classes, RaceMenuButton, the windows, the pause menus,
 * the result bars), none of which it uses. */
// DATA_START 0x00340588
#include <new>
#include "game/save/game_status_internal.h"
#include "libc.h"                               /* memcpy, strcpy */
#include "game/save/T429_game_status.h"
#include "game/menu/memorycard/T108_game_thumbnail.h"           /* gt2k_game::GameThumbnail (describe) */
#include "game/data/T482_car_equipments.h"           /* CarEquipments (newCar's probe) */
#include "game/car/physics/T280_car_condition.h"            /* CarCondition::resetEngine */
#include "game/race/core/T414_race_random.h"              /* raceRandomGetSeed / raceRandomSetSeed */
#include "game/data/T435_product_config.h"           /* product_config, ProductConfig */
#include "game/data/T481_car_data_base.h"            /* nameToCode */
#include "game/common/T494_text_utils.h"               /* str_equal */
#include "game/data/T516_country.h"                  /* g_00352FD8 (the language) */
/* (no use: the classes the original saw; UnitArena<64> instantiated by
 * T180's g_002C2F50, with its type name, T153_race_course.h leaves it out
 * unless asked; T154's displays bring T157's "" at 0x00352A90, the first
 * item of the file's .sdata) */
#define GAME_T153_EMITS_TYPEINFO
#include "game/race/core/T153_race_course.h"
#include "game/car/model/T180_race_car_model.h"
#include "game/camera/T383_race_camera_base.h"         /* RaceCameraBase */
#include "game/race/display/T154_race_display.h"             /* the race displays */
#include "game/menu/framework/T216_menu_transitive.h"          /* the menu classes */
#include "game/menu/race_menu/T198_race_menu_button.h"
#include "game/menu/race_menu/T199_race_menu_layer.h"
#include "game/race/pause/T169_race_pause.h"
#include "game/race/pause/T170_replay_pause.h"
#include "game/menu/records/T200_result_board.h"

#include "game/save/pc_save_ext.h"              /* PC: the 8.0 stream's keyed lists */
#include "game/app/pc_new_game.h"               /* PC: a new game's money, garage and unlocks (init) */
#include "game/app/pc_challenge.h"              /* PC (2026-11-14): the money decay's licence pause */
#include "game/app/pc_car_shop.h"               /* PC (2026-11-23): pc_new_car_assists (addCar) */
#include <vector>                               /* PC: the garage's heap blocks */

using namespace gt2k_game;

extern const Serialize::Factory g_00352A98;     /* (external linkage; defined below) */

/* ------------------------------------------------------------------------ */
/* PC: the garage past cars[200] / wheels[200]                              */
/* ------------------------------------------------------------------------ */

/* PC (ROADMAP 18): blocks of GARAGE_BLOCK more slots on the heap, added
 * when a slot is needed (a car or wheel bought, won or loaded) and kept
 * while the GameStatus lives.  Their addresses do not move: carList's cells,
 * the running car and MenuStatus's race cars point at the slots.  Every walk
 * over the slots goes through car_slot / wheel_slot.  (The GameStatus is
 * never copied: one instance, created once.) */
struct GarageBlocks {
    const GameStatus          *owner;
    std::vector<GarageCar *>   cars;
    std::vector<GarageWheel *> wheels;
};
static std::vector<GarageBlocks> g_garageBlocks;

static GarageBlocks *garage_blocks(const GameStatus *gs, bool create)
{
    for (size_t i = 0; i < g_garageBlocks.size(); i++)
        if (g_garageBlocks[i].owner == gs)
            return &g_garageBlocks[i];
    if (!create)
        return NULL;
    GarageBlocks b;
    b.owner = gs;
    g_garageBlocks.push_back(b);
    return &g_garageBlocks.back();
}

static int car_slot_count(const GameStatus *gs)
{
    GarageBlocks *b = garage_blocks(gs, false);
    return GARAGE_CARS + (b != NULL ? (int)b->cars.size() * GARAGE_BLOCK : 0);
}

static GarageCar *car_slot(GameStatus *gs, int slot)
{
    if (slot < GARAGE_CARS)
        return &gs->cars[slot];
    slot -= GARAGE_CARS;
    return &garage_blocks(gs, false)->cars[slot / GARAGE_BLOCK][slot % GARAGE_BLOCK];
}

/* A new block of car slots (empty: GarageCar()); NULL at GARAGE_LIMIT. */
static GarageCar *car_block_add(GameStatus *gs)
{
    if (car_slot_count(gs) + GARAGE_BLOCK > GARAGE_LIMIT)
        return NULL;
    GarageCar *block = new GarageCar[GARAGE_BLOCK];
    garage_blocks(gs, true)->cars.push_back(block);
    printf("PC: GameStatus: the garage grows to %d car slots\n", car_slot_count(gs));
    return block;
}

static int wheel_slot_count(const GameStatus *gs)
{
    GarageBlocks *b = garage_blocks(gs, false);
    return GARAGE_WHEELS + (b != NULL ? (int)b->wheels.size() * GARAGE_BLOCK : 0);
}

static GarageWheel *wheel_slot(GameStatus *gs, int slot)
{
    if (slot < GARAGE_WHEELS)
        return &gs->wheels[slot];
    slot -= GARAGE_WHEELS;
    return &garage_blocks(gs, false)->wheels[slot / GARAGE_BLOCK][slot % GARAGE_BLOCK];
}

static GarageWheel *wheel_block_add(GameStatus *gs)
{
    if (wheel_slot_count(gs) + GARAGE_BLOCK > GARAGE_LIMIT)
        return NULL;
    GarageWheel *block = new GarageWheel[GARAGE_BLOCK];
    garage_blocks(gs, true)->wheels.push_back(block);
    printf("PC: GameStatus: the garage grows to %d wheel slots\n", wheel_slot_count(gs));
    return block;
}

/* The heap blocks freed (the lists unlinked first). */
static void garage_blocks_free(const GameStatus *gs)
{
    for (size_t i = 0; i < g_garageBlocks.size(); i++)
        if (g_garageBlocks[i].owner == gs) {
            for (size_t k = 0; k < g_garageBlocks[i].cars.size(); k++)
                delete[] g_garageBlocks[i].cars[k];
            for (size_t k = 0; k < g_garageBlocks[i].wheels.size(); k++)
                delete[] g_garageBlocks[i].wheels[k];
            g_garageBlocks.erase(g_garageBlocks.begin() + i);
            return;
        }
}

/* The registration (.data, before the unit's frame block): defined here, egcs
 * generates its static initialiser and destructor (T429_static_init / fini;
 * a placement new in a hand-written function adds a NULL test retail lacks). */
Serialize::ClassInfo g_002E34B8("GameStatus", &g_00352A98);     /* 0x002E34B8 */

/* .sdata: the instance and the "GameStatus" factory, after T157's "" (the
 * inline RaceMessageDisplay::clear's, from T154_race_display.h: retail's
 * 0x00352A90, unreferenced). */
// DATA_START 0x00352A90
GameStatus *g_00352A94 = NULL;                  /* 0x00352A94 gp-0x4F5C (getGameStatus) */
const Serialize::Factory g_00352A98 = {         /* 0x00352A98 gp-0x4F58 */
    game_status_get, game_status_release
};

// FUNC 0x002185D8 gt2k_game::GameStatus::getClassInfo
//   Its registration (0x002E34B8 "GameStatus").
const Serialize::ClassInfo *GameStatus::getClassInfo()
{
    return &g_002E34B8;
}

// FUNC 0x002185E8 createGameStatus
//   Creates the instance once (operator new 0x35210; the landing pad gives
//   the block back with operator delete if GameStatus() throws), then
//   applies the game options (GameOptions::apply).  Caller: game_init.
void createGameStatus(void)
{
    if (g_00352A94 == NULL) {
        g_00352A94 = new GameStatus;         /* implicit: operator new, operator delete (landing pad) */
        GameOptions::apply();
    }
}

// FUNC 0x00218660 getGameStatus
//   The instance (677 callers).
GameStatus *getGameStatus(void)
{
    return g_00352A94;
}

// FUNC 0x00218668 gt2k_game::GameStatus::GameStatus
//   Serialize::Base(), the vptr, then the members in order: GarageCar() on
//   the 200 cars, the inline GarageCarList() (its vptr, head = tail = 0),
//   GarageWheel() on the 200 wheels, GarageWheelList(), LicenseRecords(),
//   EventResults(), CourseBestTimes(), ArcadeRecords(), TimeTrialRecords(),
//   FreeRunRecords(), MachineTestRecords(), PrizeSetFlags(),
//   ChampionshipSave(), GameOptions(), MenuStatus(); then init().  The
//   landing pads destroy what was built: the arrays through their virtual
//   destructors, the lists with ~pList, the inline destructors of
//   ChampionshipSave (~CarEquipments of its 8 entries) and MenuStatus
//   (~CarInformation and ~pCell of its two GarageCar buffers).
GameStatus::GameStatus()
{
    /* implicit: MachineTestRecords, FreeRunRecords, TimeTrialRecords, LicenseRecords,
     * PrizeSetFlags, ChampionshipSave, ArcadeRecords, CourseBestTimes, EventResults, MenuStatus,
     * GameOptions, ~CarEquipments, ~CarInformation, GarageCar, GarageWheel, ~pCell, ~pList, Base,
     * ~Base (compiler-generated: bases, members, deleting form) */
    init();
}

// FUNC 0x00218AD0 gt2k_game::GameStatus::~GameStatus
//   The vptr, both lists emptied, then the members in reverse order: the
//   inline destructors of MenuStatus (its two GarageCar buffers:
//   ~CarInformation, ~pCell) and ChampionshipSave (~CarEquipments of its 8
//   entries), the wheel list (inline: List<GarageWheel>'s vptr, ~pList),
//   the wheels through their virtual destructors (flags 0), the car list
//   (~pList), the cars; then ~Base with the caller's flags.
GameStatus::~GameStatus()
{
    /* implicit: ~CarEquipments, ~CarInformation, ~pCell, ~pList, ~Base (compiler-generated: bases,
     * members, deleting form) */
    carList.unlinkAll();
    wheelList.unlinkAll();
    garage_blocks_free(this);                   /* PC: the heap slots */
    pc_challenge_forget(this);                  /* PC (2026-11-14): its money decay pause */
}

// FUNC 0x00218CF0 gt2k_game::GameStatus::init
//   A new game: day 1, the starting money ("initial"), the statistics, the
//   garage emptied (with the car's own wheel added), the record blocks
//   reset (GameOptions is not).  Callers: the constructor, T004.
void GameStatus::init()
{
    battleNum = 0;
    orderSum = 0;
    winnerNum = 0;
    day = 1;
    /* PC: the setting's starting credits, else the product config's
     * (game/app/pc_new_game.h) */
    money = pc_new_game_money(product_config()->servicePrice(SERVICE_INITIAL_MONEY));
    language = g_00352FD8.value;
    totalPrize = 0;
    distance = 0;
    trophyNum = 0;
    easyEnding = 0;
    normalEnding = 0;
    presentNum = 0;
    largeTrophy = 0;
    smallTrophy = 0;
    carSerial = 0;
    carNum = 0;
    wheelNum = 0;
    randomSeed = 0;
    unk_40 = 0;
    arcadeLevel = 0;
    for (int level = 2 * ARCADE_LEVELS - 1; level >= 0; level--)
        arcadeClear[level] = 0;

    carList.unlinkAll();
    for (int slot = 0; slot < car_slot_count(this); slot++)      /* PC: every slot (was 200) */
        car_slot(this, slot)->clear();
    runningCarIndex = -1;
    editableCarIndex = -1;

    wheelList.unlinkAll();
    for (int slot = 0; slot < wheel_slot_count(this); slot++)    /* PC: every slot */
        wheel_slot(this, slot)->clear();
    addWheel(NULL);

    lastInputedName[0] = 0;
    for (int i = (int)(sizeof(unk_D8) / sizeof(unk_D8[0])) - 1; i >= 0; i--)
        unk_D8[i] = 0;

    licences.reset();
    events.clear();
    bestTimes.clear();
    arcade.clear();
    timeTrial.reset();
    freeRun.init();
    machineTest.init();
    prizeSets.clear();
    championship.clear();
    menu.reset();
    /* PC: a new game's garage and unlocks (the debug settings,
     * game/app/pc_new_game.h), the game's own status only: createGameStatus
     * builds it before g_00352A94 is set; the guest garage's (T004) is
     * another */
    if (g_00352A94 == NULL || g_00352A94 == this)
        pc_new_game(this);
    /* PC (2026-11-14): no money decay pause in a new game; the challenge
     * panel's day (game/app/pc_challenge.h; the game's status only) */
    pc_challenge_forget(this);
    pc_challenge_refresh(this);
}

// FUNC 0x00218F08 gt2k_game::GameStatus::setRunningCarIndex
//   The running car is car `index' of the list (-1 none); MenuStatus's race
//   car follows (setRunningCar).  Callers: check_ride_sure, action_ride_car.
void GameStatus::setRunningCarIndex(int index)
{
    runningCarIndex = index;
    setRunningCar(getRunningCar());
}

// FUNC 0x00218F40 gt2k_game::GameStatus::getRunningCar
//   The running car, NULL if none (89 callers).
GarageCar *GameStatus::getRunningCar()
{
    GarageCar *car = NULL;
    if (runningCarIndex >= 0)
        car = getCar(runningCarIndex);
    return car;
}

// FUNC 0x00218F68 gt2k_game::GameStatus::clearRunningCar
//   No running car (no callers).
void GameStatus::clearRunningCar()
{
    setRunningCarIndex(-1);
}

/* (? form) The slot test of findFreeCar / addWheel: retail computes the
 * slot's address first (an inline function's argument, a fresh pseudo) and
 * reads valid through it; `cars[slot].valid' folds the offset instead.  Not
 * inline members of GarageCar / GarageWheel: egcs would emit a copy of a
 * GarageWheel member in T491 (its key function's unit), which retail lacks. */
static inline int slotUsed(const GarageCar *car) { return car->valid; }
static inline int slotUsed(const GarageWheel *wheel) { return wheel->valid; }

// FUNC 0x00218F88 gt2k_game::GameStatus::findFreeCar
//   The first unused car slot, NULL if all 200 are used.  PC: past the 200,
//   a new block of slots; NULL only at GARAGE_LIMIT.
GarageCar *GameStatus::findFreeCar()
{
    GarageCar *car = NULL;
    for (int slot = 0; slot < car_slot_count(this); slot++) {
        if (!slotUsed(car_slot(this, slot))) {
            car = car_slot(this, slot);
            break;
        }
    }
    if (car == NULL)
        car = car_block_add(this);              /* PC */
    if (car == NULL) {
        static int logged = 0;
        if (logged == 0) {
            logged = 1;
            printf("PC: GameStatus: the garage is full (%d cars): car not added\n", GARAGE_LIMIT);
        }
    }
    return car;
}

// FUNC 0x00218FE0 gt2k_game::GameStatus::addCar
//   The car gets the next serial number and goes at the head of the list,
//   so the running car (when there is one) moves down one place.
void GameStatus::addCar(GarageCar *car)
{
    pc_new_car_assists(car);                /* PC (2026-11-23): [Game] NewCarAssistsOff (game/app/pc_car_shop.h) */
    car->index = carSerial;
    carSerial++;
    carList.prepend(car);
    carNum++;
    if (runningCarIndex >= 0)
        runningCarIndex++;
}

// FUNC 0x00219048 gt2k_game::GameStatus::newCar
//   A free slot becomes car `code' (only if the car's data loads: a stack
//   CarEquipments, destroyed with ~CarEquipments) with colour index
//   `colorIndex', and is added; NULL if there is no free slot or no such
//   car.
GarageCar *GameStatus::newCar(const char *code, int colorIndex)
{
    GarageCar *car = findFreeCar();
    if (car == NULL)
        return NULL;
    {
        CarEquipments probe;                    /* sp+0x000 */
        if (probe.setCar(nameToCode(code)) == 0)
            return NULL;
    }
    car->setCar(code);
    car->setColorIndex(colorIndex);
    addCar(car);
    return car;
}

// FUNC 0x00219128 gt2k_game::GameStatus::newCarCopy
//   A free slot becomes a copy of src, without its wheel (0: the car's
//   own), and is added; NULL if there is no free slot.
GarageCar *GameStatus::newCarCopy(const GarageCar *src)
{
    GarageCar *car = findFreeCar();
    if (car == NULL)
        return NULL;
    car->assign(*src);
    car->wheel = 0;
    addCar(car);
    return car;
}

// FUNC 0x00219190 gt2k_game::GameStatus::removeCar
//   Takes car out of the list: the running car is dropped if it is that
//   one, or moves up one place if it was below it; editableCarIndex takes
//   the new running car index.  Then the slot is emptied.
void GameStatus::removeCar(GarageCar *car)
{
    if (car == NULL)
        return;
    int pos = carList.indexOf(car);
    if (pos == runningCarIndex)
        runningCarIndex = -1;
    else if (pos < runningCarIndex)
        runningCarIndex--;
    editableCarIndex = runningCarIndex;
    carList.unlink(car);
    carNum--;
    for (int slot = 0; slot < car_slot_count(this); slot++) {   /* PC: every slot */
        if (car_slot(this, slot) == car) {
            car->clear();
            break;
        }
    }
}

// FUNC 0x00219250 gt2k_game::GameStatus::buyCar
//   A new car: its price is paid and its engine is new
//   (CarCondition::resetEngine).  Caller: action_buy_car.
void GameStatus::buyCar(const char *code, int colorIndex)
{
    GarageCar *car = newCar(code, colorIndex);
    if (car != NULL) {
        money = money - car->getPrice();
        car->condition.resetEngine();
    }
}

// FUNC 0x002192A8 gt2k_game::GameStatus::buyUsedCar
//   A copy of src (a used car): its price is paid.  Caller:
//   action_buy_guest_car.
void GameStatus::buyUsedCar(const GarageCar *src)
{
    GarageCar *car = newCarCopy(src);
    if (car != NULL)
        money = money - car->getPrice();
}

// FUNC 0x002192E8 gt2k_game::GameStatus::presentCar
//   A prize car: a free slot becomes car `code' with colour id `colorCode'
//   (setColorCode: no check, nothing paid), counted in presentNum.  Caller:
//   car_get_prizes.
void GameStatus::presentCar(const char *code, int colorCode)
{
    GarageCar *car = findFreeCar();
    if (car != NULL) {
        car->setCar(code);
        car->setColorCode(colorCode);
        presentNum++;
        addCar(car);
    }
}

// FUNC 0x00219368 gt2k_game::GameStatus::sellCar
//   Car `index' of the list (pList::at itself, not getCar): a quarter of
//   its price (int division) is paid back, then it is removed.  Caller:
//   action_sell_car.
void GameStatus::sellCar(int index)
{
    GarageCar *car = static_cast<GarageCar *>(static_cast<gt2k_menu::Cell<GarageCar> *>(carList.at(index)));
    money = car->getPrice() / 4 + money;
    removeCar(car);
}

// FUNC 0x002193D8 gt2k_game::GameStatus::getAverageOrder
//   ? name.  orderSum * 100 / battleNum (signed), 0 before the first race
//   (no callers).
int GameStatus::getAverageOrder()
{
    if (battleNum != 0)
        return orderSum * 100 / battleNum;
    return 0;
}

// FUNC 0x00219410 gt2k_game::GameStatus::getWinRate
//   winnerNum * 100 / battleNum (signed), 0 before the first race.
int GameStatus::getWinRate()
{
    if (battleNum != 0)
        return winnerNum * 100 / battleNum;
    return 0;
}

// FUNC 0x00219448 gt2k_game::GameStatus::getCarNum
int GameStatus::getCarNum()
{
    return carNum;
}

/* PC (no original; the header): the garage's real limit, not the PS2's 200 */
int GameStatus::pcGarageFull()
{
    return carNum >= GARAGE_LIMIT;
}

// FUNC 0x00219450 gt2k_game::GameStatus::getCarList
GarageCarList *GameStatus::getCarList()
{
    return &carList;
}

// FUNC 0x00219460 gt2k_game::GameStatus::getCar
//   Car `index' of the list, NULL past the end.
GarageCar *GameStatus::getCar(int index)
{
    return static_cast<GarageCar *>(static_cast<gt2k_menu::Cell<GarageCar> *>(carList.at(index)));
}

// FUNC 0x00219488 gt2k_game::GameStatus::getTotalCarPrice
//   The sum of the prices of the listed cars.  Caller:
//   text_status_ttl_car_price.
s64 GameStatus::getTotalCarPrice()
{
    s64 total = 0;
    gt2k_menu::ListIterator<GarageCar> it(&carList);    /* sp+0x0 */
    GarageCar *car;
    while ((car = it.next()) != NULL)
        total += car->getPrice();
    return total;
}

// FUNC 0x00219500 gt2k_game::GameStatus::getWheelNum
//   The number of listed wheels (the list's count, not wheelNum).
int GameStatus::getWheelNum()
{
    return wheelList.count();
}

// FUNC 0x00219528 gt2k_game::GameStatus::getWheelList
GarageWheelList *GameStatus::getWheelList()
{
    return &wheelList;
}

// FUNC 0x00219538 gt2k_game::GameStatus::getWheel
//   Wheel `index' of the list, NULL past the end.
GarageWheel *GameStatus::getWheel(int index)
{
    return static_cast<GarageWheel *>(static_cast<gt2k_menu::Cell<GarageWheel> *>(wheelList.at(index)));
}

// FUNC 0x00219560 gt2k_game::GameStatus::addWheel
//   The first free wheel slot becomes wheel `name' (NULL: the car's own)
//   and goes at the head of the list; NULL if all 200 are used.
GarageWheel *GameStatus::addWheel(const char *name)
{
    GarageWheel *wheel = NULL;
    for (int slot = 0; slot < wheel_slot_count(this); slot++) {     /* PC: every slot */
        if (!slotUsed(wheel_slot(this, slot))) {
            wheel = wheel_slot(this, slot);
            break;
        }
    }
    if (wheel == NULL)
        wheel = wheel_block_add(this);          /* PC: past the 200 */
    if (wheel == NULL)
        return NULL;
    wheel->setName(name);
    wheelList.prepend(wheel);
    wheelNum++;
    return wheel;
}

// FUNC 0x00219620 gt2k_game::GameStatus::removeWheel
//   Takes the wheel out of the list and empties its slot.  Bug kept:
//   wheelNum is incremented, not decremented.
void GameStatus::removeWheel(GarageWheel *wheel)
{
    if (wheel == NULL)
        return;
    wheelList.unlink(wheel);
    wheelNum++;                                 /* (sic) */
    for (int slot = 0; slot < wheel_slot_count(this); slot++) {     /* PC: every slot */
        if (wheel_slot(this, slot) == wheel) {
            wheel->clear();
            break;
        }
    }
}

// FUNC 0x002196A8 gt2k_game::GameStatus::buyWheel
//   Adds wheel `name' and pays the "wheel" price.  Caller: action_buy_wheel.
void GameStatus::buyWheel(const char *name)
{
    if (addWheel(name) != NULL)
        money = money - product_config()->servicePrice(SERVICE_WHEEL);
}

// FUNC 0x002196F8 gt2k_game::GameStatus::sellWheel
//   Wheel `index' of the list (pList::at itself, not getWheel): a quarter of
//   the "wheel" price is paid back, then it is removed (no callers).
void GameStatus::sellWheel(int index)
{
    GarageWheel *wheel =
        static_cast<GarageWheel *>(static_cast<gt2k_menu::Cell<GarageWheel> *>(wheelList.at(index)));
    money = product_config()->servicePrice(SERVICE_WHEEL) / 4 + money;
    removeWheel(wheel);
}

// FUNC 0x00219778 gt2k_game::GameStatus::hasWheel
//   1 if a listed wheel is called `name'.  Callers: check_ws_buy_sure,
//   WheelShopButton::virtual_2.
int GameStatus::hasWheel(const char *name)
{
    gt2k_menu::ListIterator<GarageWheel> it(&wheelList);    /* sp+0x0 */
    GarageWheel *wheel;
    while ((wheel = it.next()) != NULL) {
        const char *wheelName = wheel->getName();
        if (wheelName != NULL && str_equal(wheelName, name) != 0)
            return 1;
    }
    return 0;
}

// FUNC 0x00219800 gt2k_game::GameStatus::setRunningCarByCode
//   Player 1's race car becomes car `code' (MenuStatus's buffer): not a
//   garage car.  Caller: gt2k_menu::ArcadeControl::finish.
void GameStatus::setRunningCarByCode(const char *code)
{
    menu.setRaceCarByCode(code, 1);
    editableCarIndex = -1;
}

// FUNC 0x00219840 gt2k_game::GameStatus::setRunningCar
//   Player 1's race car is `car' itself; editableCarIndex its position in
//   the list (the count when it is not there), -1 for NULL.
void GameStatus::setRunningCar(GarageCar *car)
{
    menu.setRaceCar(car, 1);
    if (car != NULL) {
        int pos = 0;
        gt2k_menu::ListIterator<GarageCar> it(&carList);    /* sp+0x0 */
        GarageCar *listed;
        while ((listed = it.next()) != NULL && listed != car)
            pos++;
        editableCarIndex = pos;
    } else {
        editableCarIndex = -1;
    }
}

// FUNC 0x002198D8 gt2k_game::GameStatus::setRunningCarCopy
//   Player 1's race car is a copy of `car' (MenuStatus's buffer): not a
//   garage car.  Caller: gt2k_menu::ArcadeControl::finish.
void GameStatus::setRunningCarCopy(const GarageCar *car)
{
    menu.setRaceCarCopy(car, 1);
    editableCarIndex = -1;
}

// FUNC 0x00219918 gt2k_game::GameStatus::getCompletionRatio
//   The game completion: (events won + arcade score + time trial first
//   places + licence tests passed) / (their maximums); 1.0 only when they
//   are all equal, else at most 0.99.
f32 GameStatus::getCompletionRatio()
{
    int done  = events.wins();
    int total = events.count();
    done  += arcade.score();
    total += arcade.maxScore();
    done  += timeTrial.countFirstPlaces();
    total += timeTrial.numCourses();
    done  += licences.countPassed();
    total += licences.numTests();
    if (total == done)
        return 1.0f;
    f32 ratio = (f32)done / (f32)total;
    if (0.99f < ratio)
        ratio = 0.99f;
    return ratio;
}

// FUNC 0x00219A28 gt2k_game::GameStatus::allArcadeRacesWon
//   1 if every "SINGLE_RACE" course of arcade level `level' has been won
//   with race code `race' (the difficulty): getPlace() == 0.
int GameStatus::allArcadeRacesWon(const char *race, int level)
{
    const char *list = ARCADE_COURSE_LIST;      /* one register (s3) for the three calls */
    int courses = product_config()->courseCount(list);
    int i;
    for (i = 0; i < courses; i++) {
        if (product_config()->courseLevel(i, list) != level)
            continue;
        if (arcade.getPlace(race, nameToCode(product_config()->courseName(i, list))) != PLACE_WON)
            break;
    }
    return !(i < courses);
}

// FUNC 0x00219B20 gt2k_game::GameStatus::updateArcadeProgress
//   The arcade level goes up (to the highest level + 1 at most) past every
//   level whose courses have each been won at one difficulty (E, N or H)
//   at least; it stops at the first level with a course won at none.  Then
//   arcadeClear[level] = 1 for the levels (0..5) won entirely at N, and
//   arcadeClear[6 + level] = arcadeClear[level] = 1 for those won at H.
//   Caller: ArcadeRecords::updatePlace.
void GameStatus::updateArcadeProgress()
{
    /* list: as in allArcadeRacesWon; here it gets no register (all nine
     * s-registers are taken) and reload loads the constant at each use. */
    const char *list = ARCADE_COURSE_LIST;
    int courses = product_config()->courseCount(list);       /* sp+0x0 */
    int end = product_config()->F_0021D568(list) + 1;        /* sp+0x4 */
    int level;

    for (level = arcadeLevel; level < end; level++) {
        int i;
        for (i = 0; i < courses; i++) {
            if (product_config()->courseLevel(i, list) != level)
                continue;
            const char *course = product_config()->courseName(i, list);
            int easy   = arcade.getPlace(ARCADE_RACE_EASY, nameToCode(course));
            int normal = arcade.getPlace(ARCADE_RACE_NORMAL, nameToCode(course));
            int hard   = arcade.getPlace(ARCADE_RACE_HARD, nameToCode(course));
            if (easy != PLACE_WON && normal != PLACE_WON && hard != PLACE_WON)
                break;                          /* a course won at no difficulty */
        }
        if (i < courses)
            break;
    }
    arcadeLevel = level;

    for (int i = 0; i < ARCADE_LEVELS; i++) {
        if (allArcadeRacesWon(ARCADE_RACE_NORMAL, i))
            arcadeClear[i] = 1;
    }
    for (int i = 0; i < ARCADE_LEVELS; i++) {
        if (allArcadeRacesWon(ARCADE_RACE_HARD, i)) {
            arcadeClear[ARCADE_LEVELS + i] = 1;
            arcadeClear[i] = 1;
        }
    }
}

// FUNC 0x00219D60 gt2k_game::GameStatus::isArcadeLevelOpen
//   Level 0 always; level n when arcadeClear[n - 1] (level n - 1 won at N).
//   Caller: SmartCarIndex::virtual_2.
int GameStatus::isArcadeLevelOpen(int level)
{
    if (level == 0)
        return 1;
    /* PC: a level outside arcadeClear's 12 (product.gcf's levels go to 12:
     * 7..12 read the H flags) read past it: closed */
    if (level < 1 || level > (int)(sizeof(arcadeClear) / sizeof(arcadeClear[0]))) {
        static int logged = 0;
        if (logged == 0) {
            logged = 1;
            printf("PC: GameStatus: arcade level %d outside the save's %d level flags: closed\n",
                   level, (int)(sizeof(arcadeClear) / sizeof(arcadeClear[0])));
        }
        return 0;
    }
    return arcadeClear[level - 1];
}

// FUNC 0x00219D80 gt2k_game::GameStatus::isLicenseSPassed
//   (? name) 1 if every test of licence 4 (S) has a medal.  Callers: the
//   demo selection of main() (0x00100A78) and four menu units.
int GameStatus::isLicenseSPassed()
{
    return licences.isPassed(LICENSE_S, 0) != 0;
}

// FUNC 0x00219DB0 game_status_get
//   ? name.  Factory::get of "GameStatus" (g_00352A98): the instance.
Serialize::Base *game_status_get(void)
{
    return g_00352A94;
}

// FUNC 0x00219DB8 game_status_release
//   ? name.  Factory::release of "GameStatus": nothing.
void game_status_release(Serialize::Base *obj)
{
}

// FUNC 0x00219DC0 gt2k_game::GameStatus::dataSize
//   The size of the saved data with `cars' cars and `wheels' wheels: the
//   0x130-byte block, the cars, the wheels and the record blocks.  PC: the
//   7.1 stream's (positional records); this build writes 8.0, whose size is
//   getDataSize's.
int GameStatus::dataSize(int cars, int wheels)
{
    int size = GarageCar::saveSize() * cars + GAME_STATUS_BLOCK_SIZE;
    size += GarageWheel::saveSize() * wheels;
    size += LicenseRecords::getSize();
    size += EventResults::getSize();
    size += CourseBestTimes::getSize();
    size += ArcadeRecords::getSize();
    size += TimeTrialRecords::getSize();
    size += FreeRunRecords::size();
    size += MachineTestRecords::size();
    size += PrizeSetFlags::getSize();
    size += ChampionshipSave::getSize();
    return size + GameOptions::getSize();
}

// FUNC 0x00219E60 gt2k_game::GameStatus::getDataSize
//   dataSize(carNum, wheelNum).  PC: the 8.0 stream's exact size, from
//   pcWriteBody itself (counting only): the keyed lists' length depends on
//   the records held and their names.  Callers: Serialize::Base::saveSize
//   (the memory-card menus' buffer and free-space checks) and writeHeader.
int GameStatus::getDataSize()
{
    PcSaveWriter w;

    w.p = NULL;
    w.size = 0;
    pcWriteBody(&w);
    return (int)w.size;
}

// FUNC 0x00219E80 gt2k_game::GameStatus::maxDataSize
//   ? name.  dataSize(200, 200) (no callers).  PC: the 7.1 layout with the
//   garage's limit (GARAGE_LIMIT); an 8.0 stream has no fixed maximum (its
//   keyed lists grow with the data).
int GameStatus::maxDataSize()
{
    return dataSize(GARAGE_LIMIT, GARAGE_LIMIT);
}

// FUNC 0x00219EA0 gt2k_game::GameStatus::minDataSize
//   ? name.  dataSize(0, 0) (no callers).  (PC: the 7.1 layout's.)
int GameStatus::minDataSize()
{
    return dataSize(0, 0);
}

// FUNC 0x00219EC0 gt2k_game::GameStatus::saveRandomSeed
//   The race random generator's seed is saved with the game.
void GameStatus::saveRandomSeed()
{
    randomSeed = raceRandomGetSeed();
}

// FUNC 0x00219EE8 gt2k_game::GameStatus::restoreRandomSeed
//   The saved seed is restored, except from saves of a version below 7 or of
//   version 7.0 exactly (they have no seed).
void GameStatus::restoreRandomSeed(const Serialize::Header *hdr)
{
    int noSeed = 0;
    u32 seed = randomSeed;                      /* read first: retail loads it in the first branch's slot */
    /* (the second test is one compare of the word at +8 with 0x00070000) */
    if (hdr->version < GAME_STATUS_VERSION || (hdr->subVersion == 0 && hdr->version == GAME_STATUS_VERSION))
        noSeed = 1;
    if (noSeed == 0)
        raceRandomSetSeed(seed);
}

// FUNC 0x00219F38 gt2k_game::GameStatus::getVersion
int GameStatus::getVersion()
{
    return GAME_STATUS_PC_VERSION;              /* PC: 8 (was GAME_STATUS_VERSION, 7) */
}

// FUNC 0x00219F40 gt2k_game::GameStatus::getSubVersion
int GameStatus::getSubVersion()
{
    return GAME_STATUS_PC_SUBVERSION;           /* PC: 0 (was GAME_STATUS_SUBVERSION, 1) */
}

/* PC (no original: Base::checkHeader in retail's slot 5): 0 for this
 * build's 8.0; 1 for the PS2's 7.x ("the sub-version differs": the
 * memory-card menus and load take it, positionally); 2 for anything else
 * (another version: refused). */
int GameStatus::checkHeader(const void *data, void *dest)
{
    Serialize::Header hdr;

    readHeader(&hdr, data);
    if (hdr.version == GAME_STATUS_PC_VERSION && hdr.subVersion == GAME_STATUS_PC_SUBVERSION)
        return 0;
    if (hdr.version == GAME_STATUS_VERSION)
        return 1;
    return CHECK_HEADER_BAD_VERSION;
}

/* PC: a fixed-size block of the stream: written by `expr' (the record's
 * save, at w->p), counted by `bytes' (its getSize) when only counting. */
#define PC_SAVE_BLOCK(w, bytes, expr)   \
    do {                                \
        if ((w)->p != NULL)             \
            (w)->p = (expr);            \
        (w)->size += (u32)(bytes);      \
    } while (0)

/* PC: the 8.0 stream after the header; with w->p NULL only its size
 * (getDataSize), so the two always agree.  As 7.1 but for the records
 * product.gcf and the BGM list index, written as name-keyed lists
 * (pc_save_ext.h): a mod adding or removing an event, course, prize set or
 * song does not move the others' records.
 *   0x130 block (day .. unk_D8)            as 7.1
 *   the listed cars (GarageCar, 0x204)     as 7.1 (carNum of them)
 *   the listed wheels (GarageWheel, 0x44)  as 7.1 (wheelNum)
 *   LicenseRecords (0x3FC0)                as 7.1
 *   EventResults       keyed: event name, BestPlace (4)
 *   CourseBestTimes    keyed: table 0x6 name, BestTime (4)
 *   ArcadeRecords      keyed: SINGLE_RACE course name, difficulty (1), ArcadeRecord (0x14)
 *   TimeTrialRecords (0x1A68)              as 7.1 (the 10 fixed AT races)
 *   FreeRunRecords     keyed: TIME_ATTACK course name, FreeRunTable (0x300)
 *   MachineTestRecords (0x900)             as 7.1
 *   PrizeSetFlags      keyed: prize set name, its flag (1)
 *   ChampionshipSave (0xF2C)               as 7.1
 *   GameOptions (0x638 + 0x250)            as 7.1 (the play list's inline part)
 *   the play list      keyed: song code (8), played (1), enabled (1), order (2)
 *   the sections       {u32 tag, u32 bytes, payload} (pc_save_ext.h), each
 *                      only when it has data: PC_EXT_CHAMPIONSHIP, the
 *                      suspended championship's entrants past 8 (ROADMAP 35);
 *                      PC_EXT_CHALLENGE, the money decay's licence pause
 *                      (2026-11-14, game/app/pc_challenge.h)
 * Each keyed list: u32 count, then {u16 key length, key, record}: the
 * entries holding data, then those a load kept (names the data lacked). */
void GameStatus::pcWriteBody(PcSaveWriter *w)
{
    pc_put(w, &day, GAME_STATUS_BLOCK_SIZE);
    {
        gt2k_menu::ListIterator<GarageCar> it(&carList);
        GarageCar *car;
        while ((car = it.next()) != NULL)
            PC_SAVE_BLOCK(w, GarageCar::saveSize(), car->save(w->p));
    }
    {
        gt2k_menu::ListIterator<GarageWheel> it(&wheelList);
        GarageWheel *wheel;
        while ((wheel = it.next()) != NULL)
            PC_SAVE_BLOCK(w, GarageWheel::saveSize(), wheel->save(w->p));
    }
    PC_SAVE_BLOCK(w, LicenseRecords::getSize(), licences.save(w->p));
    events.saveKeyed(w);
    bestTimes.saveKeyed(w);
    arcade.saveKeyed(w);
    PC_SAVE_BLOCK(w, TimeTrialRecords::getSize(), timeTrial.save(w->p));
    freeRun.saveKeyed(w);
    PC_SAVE_BLOCK(w, MachineTestRecords::size(), machineTest.save(w->p));
    prizeSets.saveKeyed(w);
    PC_SAVE_BLOCK(w, ChampionshipSave::getSize(), championship.save(w->p));
    PC_SAVE_BLOCK(w, GameOptions::getSize(), options.save(w->p));
    options.music.saveKeyed(w);
    championship.pcSaveSection(w);              /* PC (ROADMAP 35): its entrants past 8, if any */
    pc_challenge_save_section(this, w);         /* PC (2026-11-14): the money decay's licence pause, if ahead */
}

/* PC (ROADMAP 35): the sections after the 8.0 body (pcWriteBody), from src
 * to the data's end: each known tag to its record (PC_EXT_CHAMPIONSHIP:
 * ChampionshipSave::pcLoadSection), an unknown one skipped; one that runs
 * past the data ends them (logged).  A save without sections (any older
 * one) has none.  Returns the end of what was read. */
static const u8 *pc_load_sections(gt2k_game::GameStatus *status, const u8 *src, const u8 *end)
{
    while (src != NULL && end - src >= 8) {
        u32 tag, bytes;
        memcpy(&tag, src, 4);
        memcpy(&bytes, src + 4, 4);
        if ((u32)(end - src - 8) < bytes) {
            printf("PC: GameStatus::load: section %u of %u bytes runs past the data: ignored\n", tag, bytes);
            return src;
        }
        src += 8;
        switch (tag) {
        case PC_EXT_CHAMPIONSHIP:
            status->getChampionshipSave()->pcLoadSection(src, bytes);
            break;
        case PC_EXT_CHALLENGE:                  /* PC (2026-11-14): game/app/pc_challenge.h */
            pc_challenge_load_section(status, src, bytes);
            break;
        default:
            printf("PC: GameStatus::load: section %u (%u bytes) unknown: skipped\n", tag, bytes);
            break;
        }
        src += bytes;
    }
    return src;
}

// FUNC 0x00219F48 gt2k_game::GameStatus::load
//   With dest the data goes to that object instead (its load, dest NULL).
//   The header is read and checked (a save of another version is refused:
//   NULL); the 0x130-byte block is copied to +0x08, the lists are rebuilt
//   from the saved cars (carNum of them, in slot order) and wheels
//   (wheelNum), then the record blocks load in turn (each but GameOptions
//   with arg 0); MenuStatus is reset and the running car set again (twice
//   when a championship is suspended), the random seed restored.  Returns
//   the end of the data.
const void *GameStatus::load(const void *data, void *dest)
{
    if (dest != NULL)
        return static_cast<Serialize::Base *>(dest)->load(data, NULL);

    Serialize::Header hdr;                      /* sp+0x00 */
    /* data's register is reused for the body (as T150's loads, u049) */
    const void *top = data;
    data = readHeader(&hdr, data);
    if (checkHeader(top, dest) == CHECK_HEADER_BAD_VERSION)     /* dest: NULL here ($a2 = 0) */
        return NULL;
    /* PC: 8.0 (this build's) has the keyed lists; 7.x loads positionally,
     * with the current product.gcf and BGM list, as before.  dataEnd: the
     * end of the data (restore checked the header's sizes) */
    int keyed = hdr.version == GAME_STATUS_PC_VERSION;
    const u8 *dataEnd = (const u8 *)data + hdr.dataSize;
    const u8 *end;

    /* PC: the saved car / wheel counts were not checked (200 slots); now
     * the garage grows to them, but a count past GARAGE_LIMIT is taken for
     * a damaged save; and an 8.0 stream's keyed lists must end within the
     * data.  Else refused (NULL) before anything is copied, so the game in
     * memory is kept. */
    {
        int savedCars, savedWheels;
        memcpy(&savedCars, (const u8 *)data + ((u8 *)&carNum - (u8 *)&day), sizeof(savedCars));
        memcpy(&savedWheels, (const u8 *)data + ((u8 *)&wheelNum - (u8 *)&day), sizeof(savedWheels));
        if ((u32)savedCars > GARAGE_LIMIT || (u32)savedWheels > GARAGE_LIMIT) {
            printf("PC: GameStatus::load: %d cars / %d wheels, at most %d each: refused (damaged data)\n",
                   savedCars, savedWheels, GARAGE_LIMIT);
            return NULL;
        }
        if (keyed) {
            const u8 *p = (const u8 *)data + GAME_STATUS_BLOCK_SIZE;
            u32 fixed = (u32)savedCars * GarageCar::saveSize() + (u32)savedWheels * GarageWheel::saveSize()
                        + LicenseRecords::getSize();
            p = hdr.dataSize >= GAME_STATUS_BLOCK_SIZE && (u32)(dataEnd - p) >= fixed ? p + fixed : NULL;
            p = pc_keyed_skip(p, dataEnd, PC_REC_EVENT);
            p = pc_keyed_skip(p, dataEnd, PC_REC_BEST_TIME);
            p = pc_keyed_skip(p, dataEnd, PC_REC_ARCADE);
            p = p != NULL && (u32)(dataEnd - p) >= (u32)TimeTrialRecords::getSize() ? p + TimeTrialRecords::getSize() : NULL;
            p = pc_keyed_skip(p, dataEnd, PC_REC_FREE_RUN);
            p = p != NULL && (u32)(dataEnd - p) >= (u32)MachineTestRecords::size() ? p + MachineTestRecords::size() : NULL;
            p = pc_keyed_skip(p, dataEnd, PC_REC_PRIZE_SET);
            fixed = ChampionshipSave::getSize() + GameOptions::getSize();
            p = p != NULL && (u32)(dataEnd - p) >= fixed ? p + fixed : NULL;
            p = pc_keyed_skip(p, dataEnd, PC_REC_MUSIC);
            if (p == NULL) {
                printf("PC: GameStatus::load: the 8.0 record lists run past the data: refused (damaged data)\n");
                return NULL;
            }
        }
        while (car_slot_count(this) < savedCars && car_block_add(this) != NULL)
            ;
        while (wheel_slot_count(this) < savedWheels && wheel_block_add(this) != NULL)
            ;
    }

    u32 size = GAME_STATUS_BLOCK_SIZE;          /* a variable: retail calls memcpy */
    memcpy(&day, data, size);
    data = (const u8 *)data + size;

    /* PC: the slots through car_slot / wheel_slot (the heap blocks past the
     * 200, added above) */
    carList.unlinkAll();
    for (int slot = 0; slot < car_slot_count(this); slot++)
        car_slot(this, slot)->clear();
    for (int i = 0; i < carNum; i++) {
        data = car_slot(this, i)->load((const u8 *)data);
        carList.append(car_slot(this, i));
    }

    wheelList.unlinkAll();
    for (int slot = 0; slot < wheel_slot_count(this); slot++)
        wheel_slot(this, slot)->clear();
    for (int i = 0; i < wheelNum; i++) {
        data = wheel_slot(this, i)->load((const u8 *)data);
        wheelList.append(wheel_slot(this, i));
    }

    data = licences.load((const u8 *)data, 0);
    if (keyed) {
        /* PC: the keyed lists (pcWriteBody; checked above) */
        data = events.loadKeyed((const u8 *)data, dataEnd);
        data = bestTimes.loadKeyed((const u8 *)data, dataEnd);
        data = arcade.loadKeyed((const u8 *)data, dataEnd);
        data = timeTrial.load((const u8 *)data, 0);
        data = freeRun.loadKeyed((const u8 *)data, dataEnd);
        data = machineTest.load((const u8 *)data, 0);
        data = prizeSets.loadKeyed((const u8 *)data, dataEnd);
    } else {
        data = events.load((const u8 *)data, 0);
        data = bestTimes.load((const u8 *)data, 0);
        data = arcade.load((const u8 *)data, 0);
        data = timeTrial.load((const u8 *)data, 0);
        data = freeRun.load((const u8 *)data, 0);
        data = machineTest.load((const u8 *)data, 0);
        data = prizeSets.load((const u8 *)data, 0);
    }
    data = championship.load((const u8 *)data, 0);
    end = options.load((const u8 *)data);
    /* PC: the play list rebuilt on the current BGM list: from the keyed
     * list (8.0) or from GameOptions' own 64 entries (7.x) */
    if (keyed)
        end = options.music.loadKeyed(end, dataEnd);
    else
        options.music.rebuildSaved();
    pc_challenge_forget(this);                  /* PC (2026-11-14): no licence pause but its section's */
    if (keyed)                                  /* PC (ROADMAP 35): the sections after the body */
        end = pc_load_sections(this, end, dataEnd);

    menu.reset();
    setRunningCar(getRunningCar());
    /* the accessor: retail reads active through the register that held
     * &championship for its load (an inline call's this, CSE'd) */
    if (getChampionshipSave()->active != 0)
        setRunningCar(getRunningCar());
    restoreRandomSeed(&hdr);
    pc_challenge_refresh(this);                 /* PC (2026-11-14): the challenge panel's day (the game's status) */
    return end;
}

// FUNC 0x0021A258 gt2k_game::GameStatus::save
//   The header, the language and the random seed into their fields, the
//   0x130-byte block from +0x08, the listed cars and wheels in list order,
//   then the record blocks.  Returns the end of what it wrote.
void *GameStatus::save(void *buf)
{
    u8 *dst = writeHeader(buf);
    language = g_00352FD8.value;
    saveRandomSeed();
    /* PC: the 8.0 stream (pcWriteBody; writeHeader's dataSize is its
     * count).  Was the 7.1 one: the 0x130 block, the cars, the wheels,
     * then every record block positionally. */
    PcSaveWriter w;
    w.p = dst;
    w.size = 0;
    pcWriteBody(&w);
    if (w.size != ((Serialize::Header *)buf)->dataSize)
        printf("PC: GameStatus::save: wrote %u bytes, the header says %u\n", w.size,
               ((Serialize::Header *)buf)->dataSize);
    return w.p;
}

// FUNC 0x0021A410 gt2k_game::GameStatus::describe
//   The thumbnail of a game data save: type 0, the money, the number of
//   cars, the highest licence, the day and the completion in %.
void GameStatus::describe(gt2k_game::GameThumbnail *thumb)
{
    thumb->type = THUMBNAIL_GAME_DATA;
    thumb->game.money = money;
    thumb->game.cars = getCarNum();
    thumb->game.licence = licences.highestPassed();
    thumb->game.days = day;
    thumb->game.achievement = getCompletionRatio() * 100.0f;
}

// FUNC 0x0021A488 T429_static_fini (_GLOBAL_$D$g_002E34B8)
//   egcs's static destructor (.dtors entry 0x0028F244): ~ClassInfo of the
//   registration (flags 2).
/* implicit: ~ClassInfo */

// FUNC 0x0021A4B0 T429_static_init (_GLOBAL_$I$g_002E34B8)
//   egcs's static initialiser (.ctors entry 0x0028F170): the registration
//   "GameStatus" with its factory (gp-0x4F58).
/* implicit: ClassInfo (the constructor of g_002E34B8) */

// FUNC 0x0021A4E0 gt2k_menu::List<GarageWheel>::~List (_$_Q29gt2k_menut4List1Z11GarageWheel)
/* implicit: ~pList; inline in game/menu/framework/menu_list.h; egcs emits this copy (the vptr 0x00340650,
 * then ~pList with the caller's flags).  Instantiated in GameStatus() (its
 * cleanup of wheelList), after the accessors: first among the copies. */

// FUNC 0x0021A508 gt2k_menu::List<GarageCar>::~List (_$_Q29gt2k_menut4List1Z9GarageCar)
/* implicit: ~pList; inline in game/menu/framework/menu_list.h; egcs emits this copy (the vptr 0x00340680,
 * then ~pList with the caller's flags).  Instantiated in GameStatus() (its
 * cleanup of carList). */

/* The accessors 0x0021A530..0x0021A7E0 are inline in
 * game/save/T429_game_status.h (egcs emits these copies; none has callers). */

// FUNC 0x0021A530 gt2k_game::GameStatus::getMenuStatus
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A540 gt2k_game::GameStatus::getGameOptions
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A550 gt2k_game::GameStatus::getChampionshipSave
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A560 gt2k_game::GameStatus::getPrizeSetFlags
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A570 gt2k_game::GameStatus::getMachineTestRecords
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A580 gt2k_game::GameStatus::getFreeRunRecords
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A590 gt2k_game::GameStatus::getTimeTrialRecords
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5A0 gt2k_game::GameStatus::getArcadeRecords
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5B0 gt2k_game::GameStatus::getCourseBestTimes
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5C0 gt2k_game::GameStatus::getEventResults
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5D0 gt2k_game::GameStatus::getLicenseRecords
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5E0 gt2k_game::GameStatus::getLanguage
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5E8 gt2k_game::GameStatus::setSmallTrophy
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A5F8 gt2k_game::GameStatus::hasSmallTrophy
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A608 gt2k_game::GameStatus::setLargeTrophy
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A618 gt2k_game::GameStatus::hasLargeTrophy
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A628 gt2k_game::GameStatus::setNormalEndingFlag
/* inline in game/save/T429_game_status.h; egcs emits this copy (the bool is
 * stored as a word) */

// FUNC 0x0021A630 gt2k_game::GameStatus::setEasyEndingFlag
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A638 gt2k_game::GameStatus::hasSeenNormalEnding
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A640 gt2k_game::GameStatus::hasSeenEasyEnding
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A648 gt2k_game::GameStatus::getLastInputedName
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A650 gt2k_game::GameStatus::setLastInputedName
/* implicit: strcpy; inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A670 gt2k_game::GameStatus::isRunningCarEditable
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A680 gt2k_game::GameStatus::getRaceCar
/* inline in game/save/T429_game_status.h; egcs emits this copy (player 1's race
 * car, MenuStatus+0x540) */

// FUNC 0x0021A698 gt2k_game::GameStatus::getArcadeProgressLevel
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6A0 gt2k_game::GameStatus::getPresentNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6A8 gt2k_game::GameStatus::getTrophyNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6B0 gt2k_game::GameStatus::getDistance
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6B8 gt2k_game::GameStatus::getTotalPrize
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6C0 gt2k_game::GameStatus::getWinnerNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6C8 gt2k_game::GameStatus::getBattleNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6D0 gt2k_game::GameStatus::getMoney
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6D8 gt2k_game::GameStatus::getDay
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6E0 gt2k_game::GameStatus::incTrophyNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A6F0 gt2k_game::GameStatus::incDistance
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A700 gt2k_game::GameStatus::incWinnerNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A710 gt2k_game::GameStatus::incPrize
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A720 gt2k_game::GameStatus::incOrder
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A730 gt2k_game::GameStatus::incBattleNum
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A740 gt2k_game::GameStatus::incMoney
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A750 gt2k_game::GameStatus::incDay
/* inline in game/save/T429_game_status.h; egcs emits this copy */

// FUNC 0x0021A760 gt2k_game::GameStatus::setRaceCar2Copy
/* implicit: inline in game/save/T429_game_status.h; egcs emits this copy
 * (MenuStatus::setRaceCarCopy(car, 0)) */

// FUNC 0x0021A788 gt2k_game::GameStatus::setRaceCar2
/* implicit: inline in game/save/T429_game_status.h; egcs emits this copy
 * (MenuStatus::setRaceCar(car, 0)) */

// FUNC 0x0021A7B0 gt2k_game::GameStatus::setRaceCar2ByCode
/* implicit: inline in game/save/T429_game_status.h; egcs emits this copy
 * (MenuStatus::setRaceCarByCode(code, 0)) */

// FUNC 0x0021A7D8 gt2k_game::GameStatus::setMoney
/* inline in game/save/T429_game_status.h; egcs emits this copy (a 64-bit
 * store) */

// FUNC 0x0021A7E0 gt2k_game::GameStatus::setDay
/* inline in game/save/T429_game_status.h; egcs emits this copy */


// FUNC 0x0021A888 GarageWheelList::~GarageWheelList
/* implicit: ~pList; the implicit destructor (game/save/T429_garage_wheel_list.h); egcs emits
 * this copy (the one the vtable uses): List<GarageWheel>'s vptr, then ~pList with
 * the caller's flags */


// FUNC 0x0021A950 GarageCarList::~GarageCarList
/* implicit: ~pList; the implicit destructor (game/save/T429_garage_car_list.h); egcs emits
 * this copy (the one the vtable uses): List<GarageCar>'s vptr, then ~pList with
 * the caller's flags */


/* T429's unreferenced copies of GarageCar's inline code (T036_garage.h; the
 * copies the vtables use are T036's). */

// FUNC 0x0021A9C8 gt2k_menu::Cell<GarageCar>::~Cell
/* implicit: ~pCell; inline in game/menu/framework/menu_list.h; egcs emits this copy (the vptr 0x00311C50,
 * then ~pCell with the caller's flags; T036's is 0x00125A10) */


// FUNC 0x0021AA50 GarageCar::~GarageCar
/* implicit: ~CarInformation, ~pCell; inline in game/menu/gtmode/T036_garage.h; egcs emits this copy (both vptrs,
 * 0x00311C20 at +0x1EC and 0x00311C30 at +0x8, ~CarInformation on +0x10
 * with flags 0, Cell<GarageCar>'s vptr, ~pCell with the caller's flags;
 * T036's is 0x00125A98) */

