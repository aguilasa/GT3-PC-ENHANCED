/* T483_car_information.h - CarInformation: a CarEquipments plus the parts
 * owned (one bit per part id) and the car's condition (CarSaveInformation),
 * translation unit T483, unit u108, 0x00237860..0x00238390: prices, the
 * ownership bits, loading a car, the 0x1FC-byte save block (0x1D0 of
 * CarSaveEquipments, then the 0x2C of CarSaveInformation).
 *
 * The condition wrappers (0x00238188..0x002382A8) are named after the
 * CarCondition methods they call (T280). */
#ifndef GAME_T483_CAR_INFORMATION_H
#define GAME_T483_CAR_INFORMATION_H

#include "game/game.h"
#include "game/data/T482_car_equipments.h"           /* base CarEquipments (CarSaveEquipments, CarDataBase) */
#include "game/car/physics/T280_car_condition.h"            /* CarCondition (held by value) */

class CarDataBase;                              /* T481_car_data_base.h (references only) */
struct CarSaveInformation;
class CarInformation;


/* The save block of the car's ownership, 0x2C bytes (cleared, copied, saved
 * and loaded as a whole).  Not polymorphic; it has a typeinfo because it is a
 * base of CarInformation. */
struct CarSaveInformation {
    u8           partsOwned[0xC];               /* 0x00 one bit per part id (0..0x56) */
    CarCondition condition;                     /* 0x0C */
};
EGCS_SIZE(CarSaveInformation, 0x2C);

/* vtable 0x00342388 (1 slot), in CarEquipments' CarDataBase vptr at +0x1DC
 * (this-delta -0x1D0).  typeinfo __tf14CarInformation 0x002382F0.  Bases:
 * CarEquipments at +0x000 (0x1E8 bytes), CarSaveInformation at +0x1E8: the
 * other units' partsOwned +0x1E8 and condition +0x1F4.  Size 0x218 (GarageCar
 * holds it at +0x10, its own data starts at +0x228).  The copy constructor is
 * the implicit one (inline, check_regulation_current_car T489). */
class CarInformation : public CarEquipments, public CarSaveInformation {
public:
    CarInformation();                           /* 0x00237860 clear() */
    CarInformation(const CarDataBase &db);      /* 0x002378D0 clear(), then the car database db */
    virtual ~CarInformation();                  /* [0] +0x0C 0x00237958 */
    void clear();                               /* 0x00237980 the car database g_002EBD88, nothing
                                                   owned, the condition zeroed */
    CarInformation &operator=(const CarInformation &src);   /* 0x002379C0 */
    void addPart(int part);                     /* 0x00237A08 owns part id `part` (< 0x57); 0x38..0x41
                                                   also own 0x43..0x4C */
    void removePart(int part);                  /* 0x00237B68 */
    int  hasPart(int part);                     /* 0x00237BA0 the bit of part id `part` */
    int  getPrice();                            /* 0x00237BC0 the car record's price (x100 in region 0) */
    int  getPartPrice(int part);                /* 0x00237C18 -1 if the car has no such part */
    int  getPartPrice(int table, u64 code);     /* 0x00237C80 the price of a record of a part table */
    int  loadCar(u64 code);                     /* 0x00237DB8 0 if there is no such car */
    void addFittedParts();                      /* 0x00237E18 owns the fitted parts */
    int  partExists(int part);                  /* 0x00238018 1 if part id `part` exists for this car */
    /* GarageCar::saveSize (T490, 0x0023B168) calls it with GarageCar's
     * `this' (its CarInformation base is at +0x10, not adjusted): static. */
    static int dataSize();                      /* 0x00238040 0x1FC */
    const u8 *deserialize(const void *data);    /* 0x00238048 data + 0x1FC */
    u8  *serialize(void *buf);                  /* 0x00238098 buf + 0x1FC */
    int  getPartSellPrice(int part);            /* 0x002380F0 getPartPrice(part) / 4 */
    int  getSellPrice();                        /* 0x00238120 getPrice() / 4 */
    int  loadCar(const char *name);             /* 0x00238150 loadCar(nameToCode(name)) */
    f32  getEngineMileage();                    /* 0x00238188 condition +0x04 */
    void setEngineMileage(f32 v);               /* 0x00238190 */
    u32  getOdometer();                         /* 0x002381B0 condition.odometerDistance() */
    void setOdometer(u32 odo);                  /* 0x002381D0 */
    void setDirt(f32 dirt);                     /* 0x002381F0 */
    f32  getDirt();                             /* 0x00238210 condition +0x0C */
    void changeOil();                           /* 0x00238218 */
    void setOilMileage(f32 v);                  /* 0x00238238 */
    f32  getOilDirt();                          /* 0x00238258 condition.oilDirtiness() */
    f32  getOilMileage();                       /* 0x00238278 condition +0x08 */
    void resetEngine();                         /* 0x00238280 condition.resetEngine() */
    CarCondition *getCondition();               /* 0x002382A0 */
    void setCondition(const CarCondition *c);   /* 0x002382A8 */
    /* PC: 0x214..0x218 is tail padding; on GCC / Clang i386 a member
     * there keeps a derived class's data at +0x218, as MSVC lays it out. */
    PC_TAIL_PAD(pcTail_214, 0x4)
};
EGCS_SIZE(CarInformation, 0x218);

#endif /* GAME_T483_CAR_INFORMATION_H */
