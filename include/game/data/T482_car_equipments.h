/* T482_car_equipments.h - CarEquipments, a car and its fitted parts, over the car database (translation unit T482, units u106, u107, u108).
 *
 * A CarEquipments is the save block CarSaveEquipments (the car, its colour,
 * the code of the part fitted in each category, the settings of the
 * adjustable parts; 0x1D0 bytes) plus a CarDataBase (T481) that resolves a
 * code to its record: loading a car (its stock parts, or a table 0x23 preset
 * car), the default settings of a part, the getters of the fitted parts and
 * their names, the setting getters / setters, and getSpec, which builds the
 * car parameter block (CarSpec) from the records of all the parts.
 * T482 is one source file, src/game/data/T482_car_equipments.cpp, over the
 * addresses of three work units: u106 0x00231AB0..0x00234020 (loading, the
 * defaults, the spec), u107 ..0x00236600 (the getters, part ids and names),
 * u108 ..0x00237860 (tyres, the class-inline accessors and part setters,
 * typeinfo).
 *
 * The record tables of paramdb (T481 DataBlocks; every part record starts
 * with {u64 code; u64 car; u8 kind}, kind = the grade within the category):
 *   0 brake, 1 brake controller, 2 steer, 3 chassis, 4 lightweight,
 *   5 racing modify, 6 engine, 7 port polish, 8 engine balance,
 *   9 displacement, 0xA computer, 0xB NA tune, 0xC turbine kit,
 *   0xD drivetrain, 0xE flywheel, 0xF clutch, 0x10 propeller shaft,
 *   0x11 gear (transmission), 0x12 suspension, 0x13 intercooler,
 *   0x14 muffler, 0x15 LSD, 0x16 TCSC, 0x17 ASCC, 0x18 wheel, 0x19 tyre
 *   size, 0x1A tyre force vol, 0x1B tyre compound, 0x1C front tyres,
 *   0x1D rear tyres, 0x1E car, 0x1F / 0x23 preset car, 0x20 race (T484).
 * The parts of the game are numbered 0x00..0x56 (part ids; 0x58 = none;
 * the categories in alphabetical order, brake .. turbine, then ASCC and the
 * drive train, each by kind; PART_* below):
 * getPartCategory / getPartKind give a part id's table and kind,
 * get<Part>Part() the id of the fitted part, get<Part>Name() its name.
 *
 * Names: the accessors and setters are Lupo 2's T482 names (setBrake,
 * getSettingFrontCamber, IsPresetCar, ...).  u108 inferred the unnamed ones
 * between them (getSettingRearLSDparam3Max .. getMaxLevelParam4WD): the
 * min / max and level getters run in the reverse order of the setting
 * getters, each reads the record field of its setting, and the Lupo 2 names
 * among them fix the spelling; "? name" is left where that does not settle
 * it.  The other names are the first pass's.  The
 * record names follow u106 (the fullest), with the fields other units found
 * where u106 had padding.
 */
#ifndef GAME_T482_CAR_EQUIPMENTS_H
#define GAME_T482_CAR_EQUIPMENTS_H

#include "game/game.h"
#include "game/data/T481_car_data_base.h"        /* base CarDataBase */

struct CarSaveEquipments;
class CarEquipments;


/* ======================================================================== */
/* The records of the tables (overlays on paramdb, through getRecord).      */
/* Their sizes are the tables' elementSize: no EGCS_SIZE.                   */
/* ======================================================================== */

/* The head every part record shares (findPart searches car + kind; the
 * bytes after kind are the tables' own, see their records). */
struct PartRecord {
    u64 code;                               /* 0x00 the part's code (the search key) */
    u64 car;                                /* 0x08 code of the car it belongs to */
    u8  kind;                               /* 0x10 grade within the category */
    u8  unk_11;                             /* 0x11 (table 0x0C: turbo; 0x11: gears) */
    u8  unk_12;                             /* 0x12 */
    u8  unk_13;                             /* 0x13 */
    u8  unk_14;                             /* 0x14 (table 0x0D: layout) */
    u8  unk_15;                             /* 0x15 (table 0x0D: awdType) */
};

/* Value curves of the parts and of the spec (x[], y[] pairs). */
struct Curve4   { u8 x[4]; u8 y[4]; };
/* Curve6's x and y are records of their own: SetupCarSpec (T282, u076)
 * assigns them one at a time (lwl / lwr + 2 x lb from its local copies). */
struct Curve6Row { u8 v[6]; };
struct Curve6   { Curve6Row x; Curve6Row y; };
struct Curve8   { u8 x[8]; u8 y[8]; };
struct Curve6x3 { u8 v[3][6]; };            /* ? three rows of 6 */
EGCS_SIZE(Curve4, 8);
EGCS_SIZE(Curve6Row, 6);
EGCS_SIZE(Curve6, 12);
EGCS_SIZE(Curve8, 16);
EGCS_SIZE(Curve6x3, 18);

/* Table 0x00 (brake).  (u035 / u036 "BrakeRecord" is the brake controller's,
 * table 0x01: countF / countR at +0x11 / +0x15.)
 * The `price' members of the part records: CarInformation::getPartPrice
 * (T483, 0x00237C80) reads them by table (x 100 in region 0). */
struct BrakeRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  unk_11;                             /* 0x11 -> CarSpec.brakeTorque[0..2] */
    u8  unk_12;                             /* 0x12 */
    u8  unk_13;                             /* 0x13 */
    int price;                              /* 0x14 */
};

/* Table 0x01 (brake balance controller).  A setting = {count, from, to}:
 * interpolate() maps the level 1..count onto from..to. */
struct BrakeControllerRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  countF;                             /* 0x11 levels of frontABS (getMaxLevelFrontABS) */
    u8  fromF;                              /* 0x12 */
    u8  toF;                                /* 0x13 */
    u8  level;                              /* 0x14 the default of both */
    u8  countR;                             /* 0x15 levels of rearABS (getMaxLevelRearABS) */
    u8  fromR;                              /* 0x16 */
    u8  toR;                                /* 0x17 */
    u8  unk_18[4];                          /* 0x18 */
    int price;                              /* 0x1C */
};

/* Table 0x02 (steering). */
struct SteerRecord {
    u64    code;                            /* 0x00 */
    u64    car;                             /* 0x08 */
    u8     kind;                            /* 0x10 */
    u8     unk_11;                          /* 0x11 -> CarSpec.steerLock */
    Curve6 curve[3];                        /* 0x12 -> CarSpec.steer[2], [0], [1] */
    u8     unk_36[6];                       /* 0x36 */
    int    price;                           /* 0x3C */
};

/* Table 0x03 (chassis; no kind byte: it is not a part). */
struct ChassisRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  unk_10;                             /* 0x10 -> CarSpec.frontWeight */
    u8  unk_11;                             /* 0x11 */
    u8  unk_12;                             /* 0x12 -> CarSpec.axleGrip[0] (+ 124) */
    u8  unk_13;                             /* 0x13 */
    u16 length;                             /* 0x14 mm (getLength) */
    u16 height;                             /* 0x16 mm (getHeight) */
    u16 wheelBase;                          /* 0x18 mm (getWheelBase) */
    u16 weight;                             /* 0x1A kg */
    u8  unk_1C;                             /* 0x1C -> CarSpec.yawInertia */
    u8  unk_1D;                             /* 0x1D -> CarSpec.cgOffset */
    u8  unk_1E;                             /* 0x1E -> CarSpec.axleGrip[1] (+ 124) */
};

/* Table 0x04 (weight reduction). */
struct LightweightRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  rate;                               /* 0x11 % of CarSpec.yawInertia */
    u16 weightRate;                         /* 0x12 x/1000 of CarSpec.weight */
    int price;                              /* 0x14 */
};

/* Table 0x05 (racing modification). */
struct RacingModifyRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 0 = none (isRacingModified) */
    u8  aeroFMin;                           /* 0x11 getSettingClFMin */
    u8  aeroFMax;                           /* 0x12 getSettingClFMax */
    u8  aeroF;                              /* 0x13 the default of clF */
    int price;                              /* 0x14 (getPartPrice) */
    u64 model;                              /* 0x18 the car whose model / colours are used (getModelCode) */
    f32 unk_20;                             /* 0x20 -> CarSpec.bodyFront */
    f32 unk_24;                             /* 0x24 -> CarSpec.bodyRear */
    f32 unk_28;                             /* 0x28 -> CarSpec.width (u106: width) */
    f32 unk_2C;                             /* 0x2C -> CarSpec.height (u106: height) */
    u16 weightRate;                         /* 0x30 x/1000 of CarSpec.weight */
    u16 unk_32;                             /* 0x32 -> CarSpec.treadFront */
    u16 unk_34;                             /* 0x34 -> CarSpec.treadRear */
    u16 width;                              /* 0x36 mm (getWidth) */
    u8  rate;                               /* 0x38 % of CarSpec.yawInertia */
    u8  unk_39;                             /* 0x39 -> CarSpec.dragShare */
    u8  aeroRMin;                           /* 0x3A getSettingClRMin */
    u8  aeroRMax;                           /* 0x3B getSettingClRMax */
    u8  aeroR;                              /* 0x3C the default of clR */
    u8  unk_3D;                             /* 0x3D -> CarSpec.trackOffset[0] */
    u8  unk_3E;                             /* 0x3E -> CarSpec.trackOffset[1] */
    u8  unk_3F;                             /* 0x3F -> CarSpec.swingArm[0] */
    u8  unk_40;                             /* 0x40 -> CarSpec.swingArm[1] */
    u8  unk_41[3];                          /* 0x41 */
    f32 unk_44;                             /* 0x44 x 10000 -> CarSpec.frontalArea */
};

/* Table 0x06 (engine; no kind byte: +0x10 is a text index, lhu at
 * getEngineTypeText1 0x002342C4). */
struct EngineRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u16 typeText1;                          /* 0x10 uniStrings: engine type, first word */
    u16 typeText2;                          /* 0x12 uniStrings: engine type, second word */
    u16 aspirationText;                     /* 0x14 uniStrings */
    u16 soundCode;                          /* 0x16 the engine sound, "sound/engine/%05d" (getEngineSoundCode) */
    u16 curveTorque[16];                    /* 0x18 -> CarSpec.curveTorque */
    u16 displacementText;                   /* 0x38 strings */
    u16 power;                              /* 0x3A maximum power, ps */
    u16 powerRpmText;                       /* 0x3C uniStrings */
    u16 torque;                             /* 0x3E maximum torque, kgfm */
    u16 torqueRpmText;                      /* 0x40 uniStrings */
    u8  unk_42;                             /* 0x42 x 10 -> CarSpec.torqueGain */
    u8  idleRpm;                            /* 0x43 */
    u8  unk_44;                             /* 0x44 -> CarSpec.idle */
    u8  autoGearRpm;                        /* 0x45 x 100 rpm */
    u8  revLimit;                           /* 0x46 x 100 rpm */
    u8  curveRpm[16];                       /* 0x47 -> CarSpec.curveRpm */
    u8  curveCount;                         /* 0x57 */
};

/* Records of tables 0x07 (port polish), 0x09 (displacement), 0x0A
 * (computer), 0x13 (intercooler), 0x14 (muffler): power rates. */
struct PowerRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  powerRate;                          /* 0x11 % of CarSpec.torqueTop */
    u8  rate;                               /* 0x12 % of CarSpec.torqueBottom */
    u8  unk_13;                             /* 0x13 */
    int price;                              /* 0x14 */
};

/* Records of tables 0x08 (engine balance) and 0x0B (NA tune). */
struct EngineTuneRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  curveRpmAdd;                        /* 0x11 */
    u8  autoGearRpmAdd;                     /* 0x12 */
    u8  powerRate;                          /* 0x13 % */
    u8  rate;                               /* 0x14 % */
    u8  unk_15[7];                          /* 0x15 */
    int price;                              /* 0x1C */
};

/* Table 0x0C (turbine kit). */
struct TurbineKitRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  turbo;                              /* 0x11 non-zero: a turbo engine (isTurbo) */
    u8  setting[6];                         /* 0x12 the defaults of boost1 .. response2 */
    u8  autoGearRpmAdd;                     /* 0x18 */
    u8  revLimitAdd;                        /* 0x19 */
    u8  unk_1A;                             /* 0x1A */
    u8  rate;                               /* 0x1B % */
    u16 powerRate;                          /* 0x1C % */
    u8  unk_1E[6];                          /* 0x1E */
    int price;                              /* 0x24 */
};

/* Table 0x0D (drive train). */
struct DrivetrainRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  vcdMin;                             /* 0x11 getSettingParam4WDMin */
    u8  vcdMax;                             /* 0x12 getSettingParam4WDMax */
    u8  vcd;                                /* 0x13 the default of param4WD */
    u8  layout;                             /* 0x14 0 FR, 1 FF, 2 4WD, 3 MR, 4 RR (getDriveTrain) */
    u8  awdType;                            /* 0x15 */
    u16 unk_16;                             /* 0x16 -> CarSpec.torqueUnit */
    u16 unk_18[2];                          /* 0x18 -> CarSpec.unk_0D2 */
    u16 unk_1C[2];                          /* 0x1C -> CarSpec.unk_0CE */
    u16 flywheel;                           /* 0x20 -> CarSpec.flywheel */
    u8  unk_22[2];                          /* 0x22 */
    int price;                              /* 0x24 */
};

/* Records of tables 0x0E (flywheel) and 0x0F (clutch). */
struct InertiaRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  rate06C;                            /* 0x11 % of CarSpec.torqueUnit (+0x6C) */
    u8  rateFlywheel;                       /* 0x12 % of CarSpec.flywheel */
    u8  rate0D2[2];                         /* 0x13 % of CarSpec.unk_0D2 */
    u8  unk_15;                             /* 0x15 clutch only -> CarSpec.unk_0CC */
    u8  unk_16[6];                          /* 0x16 */
    int price;                              /* 0x1C */
};

/* Table 0x10 (propeller shaft). */
struct PropellerShaftRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  rate06C;                            /* 0x11 */
    u8  rate0D2[2];                         /* 0x12 */
    u8  rate0CE[2];                         /* 0x14 */
    u8  unk_16[6];                          /* 0x16 */
    int price;                              /* 0x1C */
};

/* Table 0x11 (transmission; u106 GearRecord).  Names: u108 (getSetting*,
 * getGearFlag); finalGearRatio / maxSpeed are u106's finalGear / autoLevel
 * defaults. */
struct TransmissionRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  gears;                              /* 0x11 forward gears (getGearNum) */
    u16 gearRatio[8];                       /* 0x12 the defaults of the ratios ([0] reverse) */
    u16 finalGearRatio;                     /* 0x22 default */
    u16 finalGearRatioMax;                  /* 0x24 */
    u16 finalGearRatioMin;                  /* 0x26 */
    u8  gearFlag;                           /* 0x28 != 0: the ratios follow the auto level (F_002369A8) */
    u8  maxSpeed;                           /* 0x29 the auto level's default */
    u8  maxSpeedMin;                        /* 0x2A */
    u8  maxSpeedMax;                        /* 0x2B */
    int price;                              /* 0x2C */
    /* The enhanced engine (ref/enhanced/INTERFACES.md): the gearbox type of
     * a widened GEAR table only, Lupo 2's layout (rows of 0x38 bytes, +0x31..
     * +0x37 zero).  GT3's rows are 0x30 bytes and end at price: read it
     * only when the table's element size is over 0x30 (else it is the next
     * row's code).  Writer: the paramdb data; reader: the physics area's
     * getSpec / specGear -> CarSpec.gearboxType (Lupo 2 0x00241BC0, GT3Hooks
     * GB_ApplyGear; docs/gearbox_dualnote.md section 2). */
    u8  gearboxType;                        /* 0x30 GEARBOX_* (T292), 0 = GT3's gearbox */
};

/* Table 0x12 (suspension).  A setting is {count, from, to, default} or
 * {min, max, default}; F1/F2 = the two damper stages, B / R = bound /
 * rebound (u108: front damper FB levels +0x2B, FR +0x32, rear +0x39 /
 * +0x40). */
struct SuspensionRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  camberFMin;                         /* 0x11 */
    u8  camberFMax;                         /* 0x12 */
    u8  camberF;                            /* 0x13 */
    u8  camberRMin;                         /* 0x14 */
    u8  camberRMax;                         /* 0x15 */
    u8  camberR;                            /* 0x16 */
    u8  toeFMin;                            /* 0x17 */
    u8  toeFMax;                            /* 0x18 */
    u8  toeF;                               /* 0x19 */
    u8  toeRMin;                            /* 0x1A */
    u8  toeRMax;                            /* 0x1B */
    u8  toeR;                               /* 0x1C */
    u8  unk_1D[4];                          /* 0x1D -> CarSpec.strokeLimit */
    u8  springRateFMin;                     /* 0x21 */
    u8  springRateFMax;                     /* 0x22 */
    u8  springRateF;                        /* 0x23 */
    u8  springRateRMin;                     /* 0x24 */
    u8  springRateRMax;                     /* 0x25 */
    u8  springRateR;                        /* 0x26 */
    u8  unk_27;                             /* 0x27 the default of CarSaveEquipments.unk_164 */
    u8  unk_28;                             /* 0x28 the default of unk_168 */
    u8  unk_29;                             /* 0x29 -> CarSpec.susp[0].bumpStop */
    u8  unk_2A;                             /* 0x2A -> CarSpec.susp[1].bumpStop */
    u8  damperFCount;                       /* 0x2B */
    u8  damperFFrom;                        /* 0x2C */
    u8  damperFTo;                          /* 0x2D */
    u8  damperF;                            /* 0x2E */
    u8  damperF2From;                       /* 0x2F */
    u8  damperF2To;                         /* 0x30 */
    u8  damperF2;                           /* 0x31 */
    u8  reboundFCount;                      /* 0x32 */
    u8  reboundFFrom;                       /* 0x33 */
    u8  reboundFTo;                         /* 0x34 */
    u8  reboundF;                           /* 0x35 */
    u8  reboundF2From;                      /* 0x36 */
    u8  reboundF2To;                        /* 0x37 */
    u8  reboundF2;                          /* 0x38 */
    u8  damperRCount;                       /* 0x39 */
    u8  damperRFrom;                        /* 0x3A */
    u8  damperRTo;                          /* 0x3B */
    u8  damperR;                            /* 0x3C */
    u8  damperR2From;                       /* 0x3D */
    u8  damperR2To;                         /* 0x3E */
    u8  damperR2;                           /* 0x3F */
    u8  reboundRCount;                      /* 0x40 */
    u8  reboundRFrom;                       /* 0x41 */
    u8  reboundRTo;                         /* 0x42 */
    u8  reboundR;                           /* 0x43 */
    u8  reboundR2From;                      /* 0x44 */
    u8  reboundR2To;                        /* 0x45 */
    u8  reboundR2;                          /* 0x46 */
    u8  stabilizerFCount;                   /* 0x47 */
    u8  stabilizerFFrom;                    /* 0x48 */
    u8  stabilizerFTo;                      /* 0x49 */
    u8  stabilizerF;                        /* 0x4A */
    u8  stabilizerRCount;                   /* 0x4B */
    u8  stabilizerRFrom;                    /* 0x4C */
    u8  stabilizerRTo;                      /* 0x4D */
    u8  stabilizerR;                        /* 0x4E */
    u8  unk_4F;                             /* 0x4F */
    u16 heightFMin;                         /* 0x50 */
    u16 heightFMax;                         /* 0x52 */
    u16 heightF;                            /* 0x54 */
    u16 heightRMin;                         /* 0x56 */
    u16 heightRMax;                         /* 0x58 */
    u16 heightR;                            /* 0x5A */
    u8  unk_5C[2];                          /* 0x5C -> CarSpec.susp[].unk_3 */
    u8  unk_5E[2];                          /* 0x5E -> CarSpec.camberGain */
    u8  unk_60[4];                          /* 0x60 -> CarSpec.camberX[0] */
    u8  unk_64[4];                          /* 0x64 -> CarSpec.camberY[0] */
    u8  unk_68[4];                          /* 0x68 -> CarSpec.camberX[1] */
    u8  unk_6C[4];                          /* 0x6C -> CarSpec.camberY[1] */
    u8  unk_70[4];                          /* 0x70 -> CarSpec.susp[0].bumpSlowSpeed, bumpFastSpeed, reboundSlowSpeed, reboundFastSpeed */
    u8  unk_74[4];                          /* 0x74 -> CarSpec.susp[1].bumpSlowSpeed, bumpFastSpeed, reboundSlowSpeed, reboundFastSpeed */
    u8  unk_78[2];                          /* 0x78 -> CarSpec.susp[].unk_C */
    u8  unk_7A[2];                          /* 0x7A -> CarSpec.susp[].unk_D */
    int price;                              /* 0x7C */
};

/* Table 0x15 (LSD): per parameter default, min, max (u108: front / rear
 * param, param2, param3 = initial, accel, decel). */
struct LsdRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  diffCodeF;                          /* 0x11 -> CarSpec.diffCode[0] */
    u8  initialF;                           /* 0x12 */
    u8  initialFMin;                        /* 0x13 */
    u8  initialFMax;                        /* 0x14 */
    u8  accelF;                             /* 0x15 */
    u8  accelFMin;                          /* 0x16 */
    u8  accelFMax;                          /* 0x17 */
    u8  decelF;                             /* 0x18 */
    u8  decelFMin;                          /* 0x19 */
    u8  decelFMax;                          /* 0x1A */
    u8  diffCodeR;                          /* 0x1B -> CarSpec.diffCode[1] */
    u8  initialR;                           /* 0x1C */
    u8  initialRMin;                        /* 0x1D */
    u8  initialRMax;                        /* 0x1E */
    u8  accelR;                             /* 0x1F */
    u8  accelRMin;                          /* 0x20 */
    u8  accelRMax;                          /* 0x21 */
    u8  decelR;                             /* 0x22 */
    u8  decelRMin;                          /* 0x23 */
    u8  decelRMax;                          /* 0x24 */
    u8  unk_25[7];                          /* 0x25 */
    int price;                              /* 0x2C */
};

/* Table 0x16 (TCSC, traction control; u106's TcscRecord), what
 * getTCSCRecord returns: a setting {count, from, to} with its default level
 * (interpolate() maps the level 1..count onto from..to, specTcsc). */
struct TcsRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 the car it belongs to */
    u8  kind;                               /* 0x10 grade within the category */
    u8  level;                              /* 0x11 the default of settingTCSC (resetTcsc) */
    /* count: the levels of the TCS setting (getMaxLevelTCSC); 1: not
     * adjustable (T084 can_adjust_tcs).  The pages show the level reversed,
     * count - settingTCSC (u033 / u035: levelMax, u036: fixed, u108:
     * tcscLevels). */
    u8  count;                              /* 0x12 */
    u8  unk_13;                             /* 0x13 -> CarSpec.tcsThrottle */
    u8  unk_14;                             /* 0x14 -> CarSpec.fadeGain */
    u8  from;                               /* 0x15 */
    u8  to;                                 /* 0x16 */
    u8  unk_17;                             /* 0x17 -> CarSpec.fadeStart */
    u8  unk_18[4];                          /* 0x18 */
    int price;                              /* 0x1C */
};

/* Table 0x17 (active stability control): four settings {count, from, to,
 * level} (u108's getMaxLevel: VSC param 1 +0x11, VSC param B +0x18, VUC
 * param 1 +0x1E, VUC param B +0x23). */
struct AsccRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 */
    u8  count0;                             /* 0x11 */
    u8  level0;                             /* 0x12 */
    u8  unk_13;                             /* 0x13 -> CarSpec.brakeSlip */
    u8  from0;                              /* 0x14 */
    u8  to0;                                /* 0x15 */
    u8  flag16;                             /* 0x16 -> CarSpec.ascFlags (2 bits each) */
    u8  flag17;                             /* 0x17 */
    u8  count1;                             /* 0x18 */
    u8  from1;                              /* 0x19 */
    u8  to1;                                /* 0x1A */
    u8  level1;                             /* 0x1B */
    u8  flag1C;                             /* 0x1C */
    u8  flag1D;                             /* 0x1D */
    u8  count2;                             /* 0x1E */
    u8  from2;                              /* 0x1F */
    u8  to2;                                /* 0x20 */
    u8  level2;                             /* 0x21 */
    u8  unk_22;                             /* 0x22 -> CarSpec.brakeSlip2 */
    u8  count3;                             /* 0x23 */
    u8  from3;                              /* 0x24 */
    u8  to3;                                /* 0x25 */
    u8  level3;                             /* 0x26 */
    u8  unk_27[5];                          /* 0x27 */
    int price;                              /* 0x2C */
};

/* Table 0x18 (wheel). */
struct WheelRecord {
    u64 code;                               /* 0x00 */
    int price;                              /* 0x08 (getPartPrice) */
    u16 nameText;                           /* 0x0C strings (getWheelName) */
};

/* Table 0x19 (tyre size). */
struct TireSizeRecord {
    u64 code;                               /* 0x00 */
    u8  rim;                                /* 0x08 inches */
    u8  aspect;                             /* 0x09 x 5 % */
    u8  width;                              /* 0x0A x 5 mm */
};

/* Table 0x1A (? tyre force volume). */
struct TireForceVolRecord {
    u64 code;                               /* 0x00 */
    u8  unk_08[10];                         /* 0x08 -> CarSpecTyre.unk_4B[0..9] */
    u8  unk_12;                             /* 0x12 -> unk_4B[12] */
    u8  unk_13;                             /* 0x13 */
    u8  unk_14;                             /* 0x14 */
};

/* Table 0x1B (tyre compound).  (getPartPrice's switch reads an int at +0x14
 * for this table too, inside curve0: no part id has this category.) */
struct TireCompoundRecord {
    u64      code;                          /* 0x00 */
    u8       grip;                          /* 0x08 -> CarSpecTyre.grip */
    u8       maxAngle;                      /* 0x09 -> CarSpecTyre.maxAngle */
    Curve4   curve3;                        /* 0x0A */
    Curve8   curve0;                        /* 0x12 */
    Curve6x3 curve1;                        /* 0x22 */
    Curve6x3 curve2;                        /* 0x34 */
    Curve6   curve4;                        /* 0x46 */
    u8       unk_52;                        /* 0x52 -> CarSpecTyre.unk_6B */
    /* The enhanced engine: Lupo 2's three compound flags (physics.md 8),
     * bytes of GT3's 0x58-byte rows that are 0 in all 17 GT3 compounds (and
     * in GT Concept's).  specTire copies them to CarSpecTyre +0x6C..+0x6E
     * (Lupo 2 L2 0x00242380..0x00242394). */
    u8       slipVelocityModel;             /* 0x53 1: the slip ratio is a slip velocity, the ABS blends (T296 / T356) */
    u8       latVelocityModel;              /* 0x54 != 0: the lateral force over the lateral velocity (T296) */
    u8       noFrictionCircle;              /* 0x55 1: no friction circle with the drag (T296) */
};

/* Tables 0x1C (front tyres) and 0x1D (rear tyres) (u108 TireRecord). */
struct TiresRecord {
    u64 code;                               /* 0x00 */
    u64 car;                                /* 0x08 */
    u8  kind;                               /* 0x10 0..0x18 (getFronttireKind; 13..24 the second set of 0..11) */
    u8  unk_11[3];                          /* 0x11 */
    int price;                              /* 0x14 (getPartPrice prices table 0x1C, not 0x1D) */
    u64 size;                               /* 0x18 table 0x19 */
    u64 compound[3];                        /* 0x20 table 0x1B */
    u8  unk_38[8];                          /* 0x38 */
    u64 forceVol[3];                        /* 0x40 table 0x1A */
};

/* The codes of the stock parts of a car record and of a preset car, in the
 * order of the tables (but lsd before gear).  The same names as the
 * CarSaveEquipments fields. */
struct CarParts {
    u64 brake;                              /* 0x00 table 0x00 */
    u64 brakeController;                    /* 0x08 0x01 */
    u64 steer;                              /* 0x10 0x02 */
    u64 chassis;                            /* 0x18 0x03 */
    u64 lightweight;                        /* 0x20 0x04 */
    u64 racingModify;                       /* 0x28 0x05 */
    u64 engine;                             /* 0x30 0x06 */
    u64 portPolish;                         /* 0x38 0x07 */
    u64 engineBalance;                      /* 0x40 0x08 */
    u64 displacement;                       /* 0x48 0x09 */
    u64 computer;                           /* 0x50 0x0A */
    u64 naTune;                             /* 0x58 0x0B */
    u64 turbine;                            /* 0x60 0x0C */
    u64 drivetrain;                         /* 0x68 0x0D */
    u64 flywheel;                           /* 0x70 0x0E */
    u64 clutch;                             /* 0x78 0x0F */
    u64 propellerShaft;                     /* 0x80 0x10 */
    u64 lsd;                                /* 0x88 0x15 */
    u64 gear;                               /* 0x90 0x11 */
    u64 suspension;                         /* 0x98 0x12 */
    u64 intercooler;                        /* 0xA0 0x13 */
    u64 muffler;                            /* 0xA8 0x14 */
    u64 frontTire;                          /* 0xB0 0x1C */
    u64 rearTire;                           /* 0xB8 0x1D */
    u64 ascc;                               /* 0xC0 0x17 */
    u64 tcsc;                               /* 0xC8 0x16 */
    u64 wheel;                              /* 0xD0 0x18 */
};
EGCS_SIZE(CarParts, 0xD8);

/* Table 0x1E (a car: its stock parts, maker and names).  The name fields
 * are string indexes (strings / uniStrings); R0 / R2 the names used in the
 * regions 0 / 2 (CarDataBase::isJapan / isUK: Japan, the UK). */
struct CarRecord {
    u64      code;                          /* 0x000 */
    CarParts parts;                         /* 0x008 */
    u16      maker;                         /* 0x0E0 index of g_002EFF20 (getMakerIndex) */
    u16      brand;                         /* 0x0E2 index of g_002EFFD8 (getBrandIndex) */
    u16      name1;                         /* 0x0E4 uniStrings: name, first part */
    u16      name2;                         /* 0x0E6 uniStrings: name, second part */
    u16      shortName;                     /* 0x0E8 strings: the car's short code name (getShortName; db_car_short_name;
                                               the arcade's "%s_mini" icons) */
    u8       unk_0EA[2];                    /* 0x0EA */
    int      price;                         /* 0x0EC (x 100 if isJapan) */
    u8       flags;                         /* 0x0F0 shop flags (getShopFlags; u009: bit 0 not sold by the dealer,
                                               bit 1 not at the used car shop, bit 2 wheels cannot be changed) */
    u8       unk_0F1;                       /* 0x0F1 -> CarSaveEquipments.carType */
    u16      name1R0;                       /* 0x0F2 name1 in region 0 */
    u16      name2R0;                       /* 0x0F4 */
    u8       steerType;                     /* 0x0F6 getSteerType */
    u8       regionLock;                    /* 0x0F7 0 none, 1..8 (F_0023AC80) */
    int      unk_0F8;                       /* 0x0F8 */
    int      unk_0FC;                       /* 0x0FC */
    u16      name1R2;                       /* 0x100 name1 in region 2 */
    u16      name2R2;                       /* 0x102 */
    u16      makerName;                     /* 0x104 uniStrings: the maker's name as shown (getMakerDisplayName) */
};

/* Tables 0x23 / 0x1F (a preset car: an entry car of a race, a demo car):
 * its parts (setPreset) and its settings (loadPresetSettings; u106
 * PresetRecord). */
struct PresetCarRecord {
    u64      code;                          /* 0x000 */
    u64      car;                           /* 0x008 the car's code */
    CarParts parts;                         /* 0x010 */
    u8       frontLSDparam;                 /* 0x0E8 */
    u8       frontLSDparam2;                /* 0x0E9 */
    u8       frontLSDparam3;                /* 0x0EA */
    u8       rearLSDparam;                  /* 0x0EB */
    u8       rearLSDparam2;                 /* 0x0EC */
    u8       rearLSDparam3;                 /* 0x0ED */
    u8       maxSpeed;                      /* 0x0EE */
    u8       unk_0EF;                       /* 0x0EF */
    u8       frontDamperF1B;                /* 0x0F0 */
    u8       frontDamperF2B;                /* 0x0F1 */
    u8       frontDamperF1R;                /* 0x0F2 */
    u8       frontDamperF2R;                /* 0x0F3 */
    u8       rearDamperF1B;                 /* 0x0F4 */
    u8       rearDamperF2B;                 /* 0x0F5 */
    u8       rearDamperF1R;                 /* 0x0F6 */
    u8       rearDamperF2R;                 /* 0x0F7 */
    u8       clF;                           /* 0x0F8 */
    u8       clR;                           /* 0x0F9 */
    u8       frontCamber;                   /* 0x0FA */
    u8       rearCamber;                    /* 0x0FB */
    u8       frontToe;                      /* 0x0FC */
    u8       rearToe;                       /* 0x0FD */
    u8       frontStabilizer;               /* 0x0FE */
    u8       rearStabilizer;                /* 0x0FF */
    u16      frontRideheight;               /* 0x100 */
    u16      rearRideheight;                /* 0x102 */
    u16      frontSpringrate;               /* 0x104 */
    u16      rearSpringrate;                /* 0x106 */
    u8       tcsc;                          /* 0x108 */
    u8       ascc;                          /* 0x109 */
    u16      unk_10A;                       /* 0x10A -> CarSaveEquipments.unk_1B8 */
    u16      finalGearRatio;                /* 0x10C */
};

/* ======================================================================== */
/* CarSpec: the car parameter block getSpec fills (u076, u077, u079 read it */
/* in the race / the setup; ? name, u035, u069, u076, u079 call it so).     */
/* ======================================================================== */

/* A counted curve: count 6, then the 6 + 6 values. */
struct CarSpecCurve {
    u8     count;                           /* 0x0 */
    Curve6 c;                               /* 0x1 */
};
EGCS_SIZE(CarSpecCurve, 13);

/* One axle's suspension values (CarSpec +0x3A front, +0x4A rear; u077
 * CarSuspSpec: +0x04 bound[4] = slow %, slow force, fast %, fast force,
 * +0x08 rebound[4]). */
struct CarSpecSuspension {
    u8 springRate;                          /* 0x00 the springrate setting */
    u8 stabilizer;                          /* 0x01 interpolated from the stabilizer setting */
    u8 bumpStop;                            /* 0x02 x 10000 kgf (u077): suspension +0x29 / +0x2A */
    u8 unsprungMass;                        /* 0x03 x 0.25 kg: suspension +0x5C / +0x5D */
    u8 bumpSlowSpeed;                       /* 0x04 % (the bump damper's slow speed): suspension +0x70 / +0x74 */
    u8 damper;                              /* 0x05 its force, x 5 kgf: interpolated from the F1B setting */
    u8 bumpFastSpeed;                       /* 0x06 % (the fast speed): suspension +0x71 / +0x75 */
    u8 damper2;                             /* 0x07 its force: interpolated from F2B */
    u8 reboundSlowSpeed;                    /* 0x08 % (the rebound damper's): suspension +0x72 / +0x76 */
    u8 rebound;                             /* 0x09 interpolated from F1R (front: F1B) */
    u8 reboundFastSpeed;                    /* 0x0A %: suspension +0x73 / +0x77 */
    u8 rebound2;                            /* 0x0B interpolated from F2R (front: F2B) */
    u8 bumpStopStroke;                      /* 0x0C mm: suspension +0x78 / +0x79 */
    u8 bumpStopDamp;                        /* 0x0D x 10 kgf: suspension +0x7A / +0x7B */
    u8 unk_E[2];                            /* 0x0E */
};
EGCS_SIZE(CarSpecSuspension, 0x10);

/* One of the six tyre blocks (CarSpec +0x142: front 0..2, rear 3..5, one
 * per compound of the tyre part; u077 CarTireSpec, u076 CarSpecBlock). */
struct CarSpecTyre {
    u8       grip;                          /* 0x00 % (0 = 100): compound +0x08 */
    u8       maxAngle;                      /* 0x01 degrees (0: 45, SetupCarSpec): compound +0x09 */
    u8       curve0Count;                   /* 0x02 8 */
    Curve8   curve0;                        /* 0x03 compound +0x12 */
    u8       curve1Count;                   /* 0x13 6 (negative side) */
    Curve6x3 curve1;                        /* 0x14 compound +0x22 */
    u8       curve2Count;                   /* 0x26 6 (positive side) */
    Curve6x3 curve2;                        /* 0x27 compound +0x34 */
    u8       curve3Count;                   /* 0x39 4 */
    Curve4   curve3;                        /* 0x3A compound +0x0A */
    u8       unk_42[9];                     /* 0x42 */
    u8       unk_4B[16];                    /* 0x4B % (0 = 100): force vol +0x08.. ([0..9], [12..14]) */
    u8       unk_5B;                        /* 0x5B 0 */
    u8       unk_5C;                        /* 0x5C 0 */
    u8       unk_5D;                        /* 0x5D 0 */
    u8       curve4Count;                   /* 0x5E 6 */
    Curve6   curve4;                        /* 0x5F compound +0x46 */
    u8       unk_6B;                        /* 0x6B %: compound +0x52 */
    /* The enhanced engine: Lupo 2's compound flags (physics.md 8; GT3's
     * unused bytes, 0 for every GT3 compound): compound +0x53..+0x55 here
     * (specTire), then TyreData (CarDynamics_init, L2 0x001DCACC). */
    u8       slipVelocityModel;             /* 0x6C compound +0x53 */
    u8       latVelocityModel;              /* 0x6D compound +0x54 */
    u8       noFrictionCircle;              /* 0x6E compound +0x55 */
    u8       unk_6F;                        /* 0x6F */
};
EGCS_SIZE(CarSpecTyre, 0x70);

/* 0x3E2 bytes (u085, u076; 0x3E4 in u069's GearChart, 0x3F0 on the
 * callers' stacks).  The comments name each field's source in getSpec;
 * "x%" = scaled by later parts. */
struct CarSpec {
    u16 catalogPower;                       /* 0x000 ps, the catalogue figure (0 = none) */
    u8  unk_002;                            /* 0x002 */
    u8  catalogPowerRpm;                    /* 0x003 rpm / 100 */
    u16 catalogTorque;                      /* 0x004 */
    u8  unk_006;                            /* 0x006 */
    u8  catalogTorqueRpm;                   /* 0x007 rpm / 100 */
    u16 catalogSpeed;                       /* 0x008 (? km/h) */
    u8  unk_00A[2];                         /* 0x00A */
    u8  handlingRating;                     /* 0x00C tenths, 0: 7 (MenuCarSpec's handling gauge, CircleGauge u007) */
    u8  accelRating;                        /* 0x00D tenths, 0: 7 (the acceleration gauge) */
    u8  unk_00E;                            /* 0x00E */
    u8  unk_00F;                            /* 0x00F 9 (? version) */
    u8  unk_010;                            /* 0x010 2 (u076) */
    /* The enhanced engine: Lupo 2's "special kind" (CAR_KIND_*, T279), a
     * byte no GT3 code reads or writes (getSpec and SetupCarSpec leave it 0:
     * SetupCarSpec clears the spec first).  Writer: the physics area's
     * SetupCarSpec from the car code (Lupo 2 L2 0x001DACDC..0x001DADB8, sb
     * 0x11); CarDynamics_init copies it to CarDynamics.specialKind (Lupo 2
     * L2 0x001DC7A4).  Lupo 2 CarSpec+0x11. */
    u8  specialKind;                        /* 0x011 */
    u8  separateRearTyres;                  /* 0x012 1 (u076); 0: tyre sets 3..5 reuse 0..2 (u077) */
    u8  unk_013[3];                         /* 0x013 */
    s16 frontalArea;                        /* 0x016 x 1/10000 m^2: racing modify +0x44 x 10000 (0: width x height, u077) */
    u8  caster;                             /* 0x018 degrees, a setting 0..90 (casterCamber = sin; ShowRoomCar::getCaster) */
    u8  unk_019;                            /* 0x019 */
    s16 length;                             /* 0x01A chassis length, mm */
    s16 bodyFront;                          /* 0x01C racing modify +0x20: the body ahead of the wheelbase centre, mm (0: length / 2) */
    s16 bodyRear;                           /* 0x01E racing modify +0x24: the body behind it, mm (0: length / 2) */
    s16 width;                              /* 0x020 racing modify +0x28 (? width, mm) */
    s16 height;                             /* 0x022 racing modify +0x2C (? height, mm) */
    /* s16: CarDynamics_init reads the three with lh (u077) */
    s16 wheelBase;                          /* 0x024 chassis, mm */
    s16 treadFront;                         /* 0x026 racing modify +0x32, mm (halfTrack = (tread - 2 trackOffset) / 2, u077) */
    s16 treadRear;                          /* 0x028 racing modify +0x34, mm */
    s16 weight;                             /* 0x02A chassis, then x/1000 (lightweight, racing modify) */
    u8  frontWeight;                        /* 0x02C chassis +0x10, % (u077) */
    u8  unk_02D;                            /* 0x02D (u076: isRacingModified) */
    u8  yawInertia;                         /* 0x02E x 100 (0: 6000; u077): chassis +0x1C, then x% */
    u8  pitchInertia;                       /* 0x02F x 100 (0: 4000): = yawInertia */
    u8  rollInertia;                        /* 0x030 x 100 (0: 2000): = yawInertia / 2 */
    u8  cgOffset;                           /* 0x031 mm (u077): chassis +0x1D */
    u8  dragShare;                          /* 0x032 % (u077): racing modify +0x39 */
    u8  aeroF;                              /* 0x033 the clF setting */
    u8  aeroR;                              /* 0x034 the clR setting */
    u8  steerLock;                          /* 0x035 degrees (u077): steer +0x11 */
    u8  axleGrip[2];                        /* 0x036 per mille - 900 (u077): chassis +0x12, +0x1E, + 124 */
    u8  swingArm[2];                        /* 0x038 % (ShowRoomCar::getSwingArm): racing modify +0x3F, +0x40 */
    CarSpecSuspension susp[2];              /* 0x03A front, rear */
    u8  camber[2];                          /* 0x05A */
    u8  rideHeight[2];                      /* 0x05C */
    u8  suspRate[2];                        /* 0x05E % of the axle's spring rate; its damper speeds x 100 / it
                                               (SuspParam_init, u077): CarSaveEquipments.unk_164, unk_168 */
    u8  toe[2];                             /* 0x060 */
    s8  camberGain[2];                      /* 0x062 degrees, -100..100 (ShowRoomCar::getCamberGain): suspension +0x5E, +0x5F */
    u8  strokeLimit[2];                     /* 0x064 mm: the bump travel per axle (ShowRoomCar::getBumpTravel):
                                               suspension +0x1D, +0x1E */
    u8  strokeExt[2];                       /* 0x066 mm: the extension per axle (SuspParam_initStroke, u077:
                                               its own array, retail indexes it as (spec + axle) + 0x66):
                                               suspension +0x1F, +0x20 */
    s8  trackOffset[2];                     /* 0x068 mm, -100..100, x 2 off the tread (ShowRoomCar::getTrackOffset):
                                               racing modify +0x3D, +0x3E */
    u16 flywheel;                           /* 0x06A drivetrain +0x20, then x% (flywheel, clutch) */
    /* drivetrain +0x16, then x% (flywheel, clutch, shaft); engine_setup
     * (T291, u078): x 0.1 x 9.8 / the last curve rpm = CarEngine.torqueScale */
    u16 torqueUnit;                         /* 0x06C */
    u8  idle;                               /* 0x06E engine +0x44; x 10 rpm: the engine's idle (engine_setup) */
    /* engine, x 100 rpm (+ balance, NA tune, turbine): the top gear's rpm of
     * the auto gears (T294); engine_setup's redline (0: the last curve
     * point; u078 redline) */
    u8  autoGearRpm;                        /* 0x06F */
    /* engine, x 100 rpm (+ turbine); GearCalc.revLimit (T292); the
     * engine's redline is at least revLimit x 100 + 200 (u078 minRedline) */
    u8  revLimit;                           /* 0x070 */
    u8  curveCount;                         /* 0x071 engine: the points used below (u078 points) */
    u8  curveRpm[16];                       /* 0x072 engine (0xFF past curveCount) */
    /* engine (0 past curveCount).  s16: engine_setup (T291, u078) tests a
     * point with lh and puts values below -1 back to unsigned (+0x10000) */
    s16 curveTorque[16];                    /* 0x082 */
    /* The torque curve's factors (engine_setup, T291; u078's names): the
     * gain, and the factors at the last / first point, interpolated along
     * the curve (0: 1). */
    u16 torqueGain;                         /* 0x0A2 x 0.001: engine +0x42 x 10, or CarSaveEquipments.unk_1B8 */
    u16 torqueTop;                          /* 0x0A4 x 0.001: 1000, x% by the engine parts (u106 powerRate) */
    u8  torqueBottom;                       /* 0x0A6 x 0.01: 100, x% by the engine parts */
    u8  turboMax;                           /* 0x0A7 x 0.1, the boost cap: turbine kit +0x11 (0 = none; u106 turbo) */
    u8  turboType[2];                       /* 0x0A8 per stage, 0..2 (turbo_setup) */
    /* The two turbo stages (turbo_setup), from the settings peekRPM1 / 2,
     * boost1 / 2, response1 / 2 (u106 turboSetting[6]). */
    u8  turboRpm[2];                        /* 0x0AA x 100 rpm */
    u8  turboBoost[2];                      /* 0x0AC x 0.1 */
    u8  turboRate[2];                       /* 0x0AE x 0.1 */
    /* PC (2026-11-08): the GEAR record's kind (its category: 0 stock, 1
     * close, 2 super close, 3 fully customisable), a byte no GT3 code reads
     * or writes (getSpec's memset leaves it 0).  Writer: specGear; gear_calc
     * copies it to GearCalc.gearKind (game/app/pc_shift.h). */
    u8  gearKind;                           /* 0x0B0 */
    u8  unk_0B1;                            /* 0x0B1 */
    u8  layout;                             /* 0x0B2 drivetrain: 0 FR, 1 FF, 2 4WD, 3 MR, 4 RR */
    u8  awdType;                            /* 0x0B3 drivetrain (4WD kind 0..7) */
    u8  awdSplit;                           /* 0x0B4 drivetrain's VCD default (x 0.01) */
    u8  awdParam;                           /* 0x0B5 (u079: ? 4WD coupling torque; 0 = default) */
    s16 gearRatio[8];                       /* 0x0B6 x 1000: [0] reverse, [1..gearCount] forward (u079); -1 past them */
    s16 finalGear;                          /* 0x0C6 x 1000 */
    u8  gearCount;                          /* 0x0C8 transmission gears */
    u8  idleRpm;                            /* 0x0C9 engine */
    u8  autoGear;                           /* 0x0CA 0; != 0: the ratios come from the auto setting */
    u8  autoGearLevel;                      /* 0x0CB */
    u8  unk_0CC;                            /* 0x0CC clutch +0x15 */
    u8  unk_0CD;                            /* 0x0CD (u079: x 9.8) */
    s16 unk_0CE[2];                         /* 0x0CE drivetrain +0x1C, +0x1E, then x% (shaft) */
    s16 unk_0D2[2];                         /* 0x0D2 drivetrain +0x18, +0x1A, then x% */
    u8  diffCode[2];                        /* 0x0D6 LSD +0x11, +0x1B (an index or a letter of "frnmhvyt") */
    u8  diffInitial[2];                     /* 0x0D8 */
    u8  diffAccel[2];                       /* 0x0DA */
    u8  diffDecel[2];                       /* 0x0DC */
    u8  brakeTorque[3];                     /* 0x0DE x 10 kgf, front / rear / handbrake (u077): brake +0x11..+0x13 */
    /* The enhanced engine: the gearbox type (GEARBOX_*, T292), a byte no GT3
     * code reads or writes (0 for every GT3 car).  Writer: the physics
     * area's getSpec / specGear from the GEAR row (TransmissionRecord.
     * gearboxType, widened tables) or its list of row ids; gear_calc copies
     * it to GearCalc.gearboxType.  Lupo 2 CarSpec+0xE1 (docs/
     * gearbox_dualnote.md section 4: the same two bytes). */
    u8  gearboxType;                        /* 0x0E1 */
    CarSpecCurve steer[3];                  /* 0x0E2 steer +0x1E, +0x2A, +0x12 */
    u8  ascFlags;                           /* 0x109 ASCC +0x17, +0x16, +0x1D, +0x1C (2 bits each) */
    u8  brakeBalance[2];                    /* 0x10A interpolated from frontABS, rearABS */
    u8  asc0;                               /* 0x10C interpolated from settingASCC */
    u8  brakeSlip;                          /* 0x10D x 0.01 (TcsParams, u077): ASCC +0x13 */
    u8  fadeGain;                           /* 0x10E x 0.1: TCSC +0x14 */
    u8  fadeStart;                          /* 0x10F x 0.01: TCSC +0x17 */
    u8  tcsThrottle;                        /* 0x110 x 0.1, TcsParams.throttleGain (0: no throttle cut): TCSC +0x13 */
    u8  asc1;                               /* 0x111 interpolated from asccParam[0] */
    u8  brakeK[2];                          /* 0x112 per axle, x 0.01 (u077; u076: default 100) */
    u8  asc2;                               /* 0x114 interpolated from asccParam[1] */
    u8  brakeSlip2;                         /* 0x115 x 0.01 (TcsParams, u077): ASCC +0x22 */
    u8  asc3;                               /* 0x116 interpolated from asccParam[2] */
    u8  tcs;                                /* 0x117 interpolated from settingTCSC */
    u8  unk_118[2];                         /* 0x118 */
    u8  tyreWidth[2];                       /* 0x11A tyre size +0x0A, x 5 mm */
    u8  tyreRim[2];                         /* 0x11C tyre size +0x08, inches */
    u8  tyreAspect[2];                      /* 0x11E tyre size +0x09, x 5 % */
    u8  camberPoints[2];                    /* 0x120 4: the points of each axle's camber grip curve (u077) */
    u8  camberX[2][4];                      /* 0x122 [axle]: x = (0x80 - it) / 8 degrees, backwards: suspension +0x60, +0x68 */
    u8  camberY[2][4];                      /* 0x12A [axle]: y = it / 200: suspension +0x64, +0x6C */
    u8  tyreSet[16];                        /* 0x132 the tyre set (tyre[]) of each surface slot (u077) */
    CarSpecTyre tyre[6];                    /* 0x142 front 0..2, rear 3..5 */
};
EGCS_SIZE(CarSpec, 0x3E2);

/* ======================================================================== */
/* Names for the numbers (u106, u107, u108)                                 */
/* ======================================================================== */

/* The record tables of paramdb (CarDataBase::getRecord / getDataBlock).  A
 * part category is the table of its records; the part tables' records start
 * with {u64 code; u64 car; u8 kind} (PartRecord); T484 reads 0x1F .. 0x22. */
enum {
    CARDB_BRAKE            = 0x00,      /* BrakeRecord */
    CARDB_BRAKE_CONTROLLER = 0x01,      /* BrakeControllerRecord */
    CARDB_STEER            = 0x02,      /* SteerRecord */
    CARDB_CHASSIS          = 0x03,      /* ChassisRecord */
    CARDB_LIGHTWEIGHT      = 0x04,      /* LightweightRecord */
    CARDB_RACING_MODIFY    = 0x05,      /* RacingModifyRecord */
    CARDB_ENGINE           = 0x06,      /* EngineRecord */
    CARDB_PORT_POLISH      = 0x07,      /* PowerRecord */
    CARDB_ENGINE_BALANCE   = 0x08,      /* EngineTuneRecord */
    CARDB_DISPLACEMENT     = 0x09,      /* PowerRecord */
    CARDB_COMPUTER         = 0x0A,      /* PowerRecord */
    CARDB_NA_TUNE          = 0x0B,      /* EngineTuneRecord */
    CARDB_TURBINE          = 0x0C,      /* TurbineKitRecord */
    CARDB_DRIVETRAIN       = 0x0D,      /* DrivetrainRecord */
    CARDB_FLYWHEEL         = 0x0E,      /* InertiaRecord */
    CARDB_CLUTCH           = 0x0F,      /* InertiaRecord */
    CARDB_PROPELLER_SHAFT  = 0x10,      /* PropellerShaftRecord */
    CARDB_GEAR             = 0x11,      /* TransmissionRecord */
    CARDB_SUSPENSION       = 0x12,      /* SuspensionRecord */
    CARDB_INTERCOOLER      = 0x13,      /* PowerRecord */
    CARDB_MUFFLER          = 0x14,      /* PowerRecord */
    CARDB_LSD              = 0x15,      /* LsdRecord */
    CARDB_TCSC             = 0x16,      /* TcsRecord */
    CARDB_ASCC             = 0x17,      /* AsccRecord */
    CARDB_WHEEL            = 0x18,      /* WheelRecord */
    CARDB_TIRE_SIZE        = 0x19,      /* TireSizeRecord */
    CARDB_TIRE_FORCE_VOL   = 0x1A,      /* TireForceVolRecord */
    CARDB_TIRE_COMPOUND    = 0x1B,      /* TireCompoundRecord */
    CARDB_FRONT_TIRE       = 0x1C,      /* TiresRecord */
    CARDB_REAR_TIRE        = 0x1D,      /* TiresRecord */
    CARDB_CAR              = 0x1E,      /* CarRecord */
    CARDB_ENTRY_CAR        = 0x1F,      /* PresetCarRecord: the entry cars of the races */
    CARDB_RACE             = 0x20,      /* RaceEvent */
    CARDB_CAR_LIST         = 0x21,      /* CarListRecord */
    CARDB_COURSE           = 0x22,      /* CourseRecord */
    CARDB_PRESET_CAR       = 0x23       /* PresetCarRecord */
};

/* The part ids (fitPart, getPartName, T083's set_* actions, the shops): the
 * categories in alphabetical order (brake, brake controller, clutch,
 * computer, displacement, engine balance, flywheel, gear, intercooler,
 * lightweight, LSD, muffler, NA tune, port polish, propeller shaft, racing
 * modify, suspension, TCSC, tyres front / rear, turbine), then the two added
 * last (ASCC, drive train); within a category by kind (getPartKind), kind 0
 * the car's base part.  The names translate the full names
 * (getPartFullName, g_002EC470 -> g_002F1048). */
enum {
    PART_BRAKE                   = 0x00,    /* kind 0 normal */
    PART_BRAKE_RACING            = 0x01,    /* racing kit */
    PART_BRAKE_CONTROLLER_NONE   = 0x02,
    PART_BRAKE_CONTROLLER        = 0x03,    /* brake balance controller */
    PART_CLUTCH                  = 0x04,
    PART_CLUTCH_SPORTS           = 0x05,
    PART_CLUTCH_TWIN             = 0x06,    /* twin plate */
    PART_CLUTCH_TRIPLE           = 0x07,    /* triple plate */
    PART_COMPUTER                = 0x08,    /* (short name "normal", full name "none") */
    PART_COMPUTER_ROM            = 0x09,    /* tuned ROM */
    PART_DISPLACEMENT_NONE       = 0x0A,
    PART_DISPLACEMENT_UP         = 0x0B,    /* displacement up */
    PART_DISPLACEMENT_2          = 0x0C,    /* (kind 2: short name "done", full name "none") */
    PART_ENGINE_BALANCE_NONE     = 0x0D,
    PART_ENGINE_BALANCE          = 0x0E,    /* engine balance tune */
    PART_FLYWHEEL                = 0x0F,
    PART_FLYWHEEL_SPORTS         = 0x10,
    PART_FLYWHEEL_SEMI_RACING    = 0x11,
    PART_FLYWHEEL_RACING         = 0x12,
    PART_GEAR                    = 0x13,    /* the transmission */
    PART_GEAR_CLOSE              = 0x14,
    PART_GEAR_SUPER_CLOSE        = 0x15,
    PART_GEAR_FULL_CUSTOM        = 0x16,
    PART_INTERCOOLER             = 0x17,
    PART_INTERCOOLER_SPORTS      = 0x18,
    PART_INTERCOOLER_RACING      = 0x19,    /* racing big */
    PART_LIGHTWEIGHT_NONE        = 0x1A,
    PART_LIGHTWEIGHT_STAGE1      = 0x1B,
    PART_LIGHTWEIGHT_STAGE2      = 0x1C,
    PART_LIGHTWEIGHT_STAGE3      = 0x1D,
    PART_LSD                     = 0x1E,
    PART_LSD_1WAY                = 0x1F,
    PART_LSD_2WAY                = 0x20,
    PART_LSD_1_5WAY              = 0x21,
    PART_LSD_FULL_CUSTOM         = 0x22,
    PART_LSD_AYC                 = 0x23,    /* AYC controller (kind 5) */
    PART_MUFFLER                 = 0x24,
    PART_MUFFLER_SPORTS          = 0x25,
    PART_MUFFLER_SEMI_RACING     = 0x26,
    PART_MUFFLER_RACING          = 0x27,
    PART_NA_TUNE_NONE            = 0x28,
    PART_NA_TUNE_STAGE1          = 0x29,
    PART_NA_TUNE_STAGE2          = 0x2A,
    PART_NA_TUNE_STAGE3          = 0x2B,
    PART_PORT_POLISH_NONE        = 0x2C,
    PART_PORT_POLISH             = 0x2D,
    PART_PROPELLER_SHAFT         = 0x2E,
    PART_PROPELLER_SHAFT_CARBON  = 0x2F,
    PART_RACING_MODIFY_NONE      = 0x30,
    PART_RACING_MODIFY           = 0x31,
    PART_SUSPENSION              = 0x32,
    PART_SUSPENSION_SPORTS       = 0x33,
    PART_SUSPENSION_SEMI_RACING  = 0x34,
    PART_SUSPENSION_FULL_CUSTOM  = 0x35,
    PART_TCSC_NONE               = 0x36,
    PART_TCSC                    = 0x37,    /* TCS controller */
    PART_FRONT_TIRE              = 0x38,    /* 0x38..0x42: tyre kinds 0..9, 0xC */
    PART_FRONT_TIRE_DIRT         = 0x42,
    PART_REAR_TIRE               = 0x43,    /* 0x43..0x4D: tyre kinds 0..9, 0xC */
    PART_REAR_TIRE_DIRT          = 0x4D,
    PART_TURBINE                 = 0x4E,    /* kind 0 (getTurbineName: "なし" (none) unless
                                               the car is a turbo in stock form) */
    PART_TURBINE_STAGE1          = 0x4F,
    PART_TURBINE_STAGE2          = 0x50,
    PART_TURBINE_STAGE3          = 0x51,
    PART_TURBINE_STAGE4          = 0x52,
    PART_ASCC_NONE               = 0x53,
    PART_ASCC                    = 0x54,    /* active stability management system */
    PART_DRIVETRAIN              = 0x55,
    PART_DRIVETRAIN_VCD          = 0x56,    /* variable centre differential */
    PART_COUNT                   = 0x57,    /* the number of part ids (the jump tables of
                                               getPartCategory / Kind cover 0..0x57, 0x57 = none) */
    PART_NONE                    = 0x58     /* no part: the category is absent ("-") */
};

/* Tyre kinds (TiresRecord.kind; getFronttireKind): the part ids of the
 * fitted tyres are PART_FRONT_TIRE / PART_REAR_TIRE + kind (0..9, 0xC).
 * 0..9 named after the "set_tir_*" actions / "attach_tir_<f|r>_<kind>"
 * hooks (u034, u036). */
enum {
    TIRE_KIND_NORMAL        = 0x00,
    TIRE_KIND_SPORTS        = 0x01,
    TIRE_KIND_RACING_SH     = 0x02,     /* racing super hard ("racing_sh") */
    TIRE_KIND_RACING_H      = 0x03,     /* hard */
    TIRE_KIND_RACING_MH     = 0x04,     /* medium hard */
    TIRE_KIND_RACING_M      = 0x05,     /* medium */
    TIRE_KIND_RACING_MS     = 0x06,     /* medium soft */
    TIRE_KIND_RACING_S      = 0x07,     /* soft */
    TIRE_KIND_RACING_SS     = 0x08,     /* super soft */
    TIRE_KIND_CONTROL       = 0x09,     /* "control" */
    TIRE_KIND_ARCADE        = 0x0A,     /* ? arcade tyres: T000 MenuArgs.tyre1 == 0 (F_00236718) */
    TIRE_KIND_ARCADE_RACING = 0x0B,     /* ? MenuArgs.tyre1 == 1, ArcadeControl racing1 (F_00236758) */
    TIRE_KIND_DIRT          = 0x0C,     /* rally races (RaceBase::virtual_39, race kind 2) */
    TIRE_KIND_SECOND_SET    = 0x0D      /* 13..24: the second set of 0..11 (race kind 1, F_00236798) */
};

/* Drive train layouts (DrivetrainRecord.layout, getDriveTrain). */
enum {
    LAYOUT_FR  = 0,
    LAYOUT_FF  = 1,
    LAYOUT_4WD = 2,
    LAYOUT_MR  = 3,
    LAYOUT_RR  = 4
};

/* ======================================================================== */
/* CarSaveEquipments / CarEquipments                                         */
/* ======================================================================== */

/* The save block of a car (dataSize 0x1D0; serialize / deserialize), the
 * first base of CarEquipments.  Not polymorphic; typeinfo
 * __tf17CarSaveEquipments 0x00237820 (__user_type_info at 0x003534A0,
 * gp-0x4550).  The part fields hold the codes of the fitted parts (0 =
 * none); the setting names are the Lupo 2 accessors' (u106's names in
 * parentheses). */
struct CarSaveEquipments {
    u64 carCode;                            /* 0x000 the car (nameToCode; u106 code) */
    u32 colorCode;                          /* 0x008 the colour id, -1 = none (getColorCode).  u32:
                                               retail compares and stores -1 as the unsigned
                                               0xFFFFFFFF (lui/ori: reset, setColorCode, T193) */
    int colorIndex;                         /* 0x00C -1 = none (getColorIndex) */
    u16 carType;                            /* 0x010 car record +0xF1 (2: two more parts owned; 3 preset) */
    u16 restrictorLimit;                    /* 0x012 */
    u8  unk_014[4];                         /* 0x014 */
    u64 brake;                              /* 0x018 table 0x00 */
    u64 brakeController;                    /* 0x020 0x01 */
    u64 chassis;                            /* 0x028 0x03 */
    u64 engine;                             /* 0x030 0x06 */
    u64 drivetrain;                         /* 0x038 0x0D */
    u64 gear;                               /* 0x040 0x11 (the transmission) */
    u64 suspension;                         /* 0x048 0x12 */
    u64 lsd;                                /* 0x050 0x15 */
    u64 frontTire;                          /* 0x058 0x1C */
    u64 rearTire;                           /* 0x060 0x1D */
    u64 steer;                              /* 0x068 0x02 (getSpec; no setter; reset() keeps it) */
    u64 lightweight;                        /* 0x070 0x04 */
    u64 racingModify;                       /* 0x078 0x05 */
    u64 portPolish;                         /* 0x080 0x07 */
    u64 engineBalance;                      /* 0x088 0x08 */
    u64 displacement;                       /* 0x090 0x09 */
    u64 computer;                           /* 0x098 0x0A */
    u64 naTune;                             /* 0x0A0 0x0B */
    u64 turbine;                            /* 0x0A8 0x0C (setTurbo) */
    u64 flywheel;                           /* 0x0B0 0x0E */
    u64 clutch;                             /* 0x0B8 0x0F */
    u64 propellerShaft;                     /* 0x0C0 0x10 */
    u64 muffler;                            /* 0x0C8 0x14 */
    u64 intercooler;                        /* 0x0D0 0x13 */
    u64 ascc;                               /* 0x0D8 0x17 */
    u64 tcsc;                               /* 0x0E0 0x16 */
    u64 wheel;                              /* 0x0E8 0x18 (0 = the default wheel) */
    int gearRatio[8];                       /* 0x0F0 */
    int finalGearRatio;                     /* 0x110 (gearRatioFinal) */
    int maxSpeed;                           /* 0x114 the auto level (gearAutoLevel) */
    int param4WD;                           /* 0x118 (vcd) */
    int frontABS;                           /* 0x11C (brakeF) */
    int rearABS;                            /* 0x120 (brakeR) */
    int clF;                                /* 0x124 (aeroF) */
    int clR;                                /* 0x128 (aeroR) */
    int boost1;                             /* 0x12C (turbo[0..5]: turbine kit +0x12..) */
    int peekRPM1;                           /* 0x130 */
    int response1;                          /* 0x134 */
    int boost2;                             /* 0x138 */
    int peekRPM2;                           /* 0x13C */
    int response2;                          /* 0x140 */
    int frontCamber;                        /* 0x144 */
    int rearCamber;                         /* 0x148 */
    int frontRideheight;                    /* 0x14C */
    int rearRideheight;                     /* 0x150 */
    int frontToe;                           /* 0x154 */
    int rearToe;                            /* 0x158 */
    int frontSpringrate;                    /* 0x15C */
    int rearSpringrate;                     /* 0x160 */
    int unk_164;                            /* 0x164 suspension +0x27 (copied with the suspension) */
    int unk_168;                            /* 0x168 suspension +0x28 */
    int frontDamperF1B;                     /* 0x16C (damperF[0]) */
    int frontDamperF2B;                     /* 0x170 (damperF[1]) */
    int frontDamperF1R;                     /* 0x174 (reboundF[0]) */
    int frontDamperF2R;                     /* 0x178 (reboundF[1]) */
    int rearDamperF1B;                      /* 0x17C */
    int rearDamperF2B;                      /* 0x180 */
    int rearDamperF1R;                      /* 0x184 */
    int rearDamperF2R;                      /* 0x188 */
    int frontStabilizer;                    /* 0x18C */
    int rearStabilizer;                     /* 0x190 */
    int frontLSDparam;                      /* 0x194 (lsdInitialF) */
    int rearLSDparam;                       /* 0x198 */
    int frontLSDparam2;                     /* 0x19C (lsdAccelF) */
    int rearLSDparam2;                      /* 0x1A0 */
    int frontLSDparam3;                     /* 0x1A4 (lsdDecelF) */
    int rearLSDparam3;                      /* 0x1A8 */
    int lastFinalGearRatio;                 /* 0x1AC finalGearRatio as the transmission sets it */
    int settingTCSC;                        /* 0x1B0 */
    int unk_1B4;                            /* 0x1B4 */
    int unk_1B8;                            /* 0x1B8 non-zero: CarSpec.torqueGain (a preset car's +0x10A) */
    int settingASCC;                        /* 0x1BC (asc[0]) */
    int asccParam[3];                       /* 0x1C0 (asc[1..3]; copied with the ASCC) */
    int unk_1CC;                            /* 0x1CC */
};
EGCS_SIZE(CarSaveEquipments, 0x1D0);

/* Bases CarSaveEquipments at +0x000, CarDataBase at +0x1D0 (typeinfo
 * __tf13CarEquipments 0x002377C0: __class_type_info at 0x004B2A40, base
 * list 0x003422E0).  vtable 0x003422C0 (1 slot), the secondary one in the
 * CarDataBase base (this_delta -0x1D0, vptr +0x1DC).  Size 0x1E8 (operator=
 * copies 0x1E8 bytes; CarInformation (T483) derives from it).  egcs lays the
 * non-polymorphic first base out first; GCC 14 puts CarDataBase first.
 * The CarDataBase base holds copies of g_002EBD88's file pointers (copy). */
class CarEquipments : public CarSaveEquipments, public CarDataBase {
public:
    /* 0x1E0 IsPresetCar / setPresetCar(bool) (u106 unk1E0).  A bool: egcs's
     * bool is 4 bytes, and its own alias set shows in retail (u017: T054's
     * `lw args->color' is scheduled ahead of the `presetCar = 1' store, as
     * T052's; with an int field the store must stay first).  GCC 14 keeps
     * an int (its bool is 1 byte; the values are 0 / 1). */
    int presetCar;
    /* 0x1E4: tail padding, no member (u016): the class is 8-aligned
     * (CarSaveEquipments' u64), and the implicit copy constructor (T052's
     * setEquipments arguments, T151, T489) copies the members up to +0x1E4
     * only; a u8[4] member there was copied byte by byte.
     * PC: on GCC / Clang i386 a member there keeps CarInformation's
     * CarSaveInformation base at +0x1E8. */
    PC_TAIL_PAD(pcTail_1E4, 4)

    CarEquipments();                             /* 0x00231AB0 reset(), then copy(g_002EBD88) */
    virtual ~CarEquipments();                    /* [0] +0x0C 0x00231B38 */
    CarEquipments &operator=(const CarEquipments &src); /* 0x00231B90 memcpy of the 0x1E8 bytes, the vptr included */
    void reset();                                /* 0x00231BC0 the car, the colour and the parts cleared (not steer, +0x68) */
    int  setPreset(const PresetCarRecord *rec);  /* 0x00231C50 a preset car (table 0x23): its parts, their default settings, its car; 1 */
    int  setCar(u64 code);                       /* 0x00231DB0 a preset of that code, else the car of table 0x1E with its stock parts and colour index 0; 0 if none or no colour 0 */
    int  loadPreset(u64 code);                   /* 0x00231F60 table 0x23 record `code' (setPreset, loadPresetSettings); 0 if none */
    void resetGear();                            /* 0x00231FC8 the default settings of the fitted transmission */
    void resetDrivetrain();                      /* 0x00232080 ... of the drive train */
    void resetBrakeController();                 /* 0x002320C8 ... of the brake controller */
    void resetRacingModify();                    /* 0x00232138 ... of the racing modification */
    void resetTurbineKit();                      /* 0x002321A8 ... of the turbine kit */
    void resetSuspension();                      /* 0x00232208 ... of the suspension */
    void resetLsd();                             /* 0x00232400 ... of the LSD */
    void resetAscc();                            /* 0x002324D0 ... of the ASCC */
    void resetTcsc();                            /* 0x00232570 ... of the TCSC */
    void getSpec(CarSpec *spec);                 /* 0x002325B8 the car parameter block from the records of all the fitted parts */
    void specBrake(CarSpec *spec, const BrakeRecord *rec); /* 0x00232A68 */
    void specBrakeController(CarSpec *spec, const BrakeControllerRecord *rec); /* 0x00232A88 */
    void specSteer(CarSpec *spec, const SteerRecord *rec); /* 0x00232AF8 the record's curves go to the spec's curves 2, 0, 1 */
    void specChassis(CarSpec *spec, const ChassisRecord *rec); /* 0x00232C38 */
    void specEngine(CarSpec *spec, const EngineRecord *rec); /* 0x00232C88 */
    void specDrivetrain(CarSpec *spec, const DrivetrainRecord *rec); /* 0x00232DC8 awdSplit = the record's VCD default */
    void specGear(CarSpec *spec, const TransmissionRecord *rec); /* 0x00232E18 */
    void specSuspension(CarSpec *spec, const SuspensionRecord *rec); /* 0x00232E68 quirk: the front rebound uses the front bound settings */
    void specLsd(CarSpec *spec, const LsdRecord *rec); /* 0x00233150 */
    void specTiresFront(CarSpec *spec, const TiresRecord *rec); /* 0x00233198 tyre blocks 0..2 (the three compounds), the size */
    void specTiresRear(CarSpec *spec, const TiresRecord *rec); /* 0x002332A0 tyre blocks 3..5, the size */
    void specTire(CarSpecTyre *tyre, const TireCompoundRecord *compound, const TireForceVolRecord *forceVol); /* 0x002333A0 */
    void specLightweight(CarSpec *spec, const LightweightRecord *rec); /* 0x00233690 */
    void specRacingModify(CarSpec *spec, const RacingModifyRecord *rec); /* 0x002336D8 */
    void specPortPolish(CarSpec *spec, const PowerRecord *rec); /* 0x002337C0 */
    void specEngineBalance(CarSpec *spec, const EngineTuneRecord *rec); /* 0x00233808 */
    void specDisplacement(CarSpec *spec, const PowerRecord *rec); /* 0x00233960 */
    void specComputer(CarSpec *spec, const PowerRecord *rec); /* 0x002339A8 */
    void specNaTune(CarSpec *spec, const EngineTuneRecord *rec); /* 0x002339F0 */
    void specTurbineKit(CarSpec *spec, const TurbineKitRecord *rec); /* 0x00233B48 */
    void specFlywheel(CarSpec *spec, const InertiaRecord *rec); /* 0x00233BE8 */
    void specClutch(CarSpec *spec, const InertiaRecord *rec); /* 0x00233C68 */
    void specPropellerShaft(CarSpec *spec, const PropellerShaftRecord *rec); /* 0x00233CF0 */
    void specMuffler(CarSpec *spec, const PowerRecord *rec); /* 0x00233D88 */
    void specIntercooler(CarSpec *spec, const PowerRecord *rec); /* 0x00233DD0 */
    void specAscc(CarSpec *spec, const AsccRecord *rec); /* 0x00233E18 */
    void specTcsc(CarSpec *spec, const TcsRecord *rec); /* 0x00233F08 */
    void specWheel(CarSpec *spec, const WheelRecord *rec); /* 0x00233F58 (empty; table 0x18 has no car / kind) */
    int  interpolate(int from, int to, int count, int level); /* 0x00233F60 level 1..count onto from..to (from when count < 2) */
    u64  findPart(int table, int kind);          /* 0x00233F98 this car's part of `table' with `kind' (linear search); its code, 0 if none */
    u64  getModelCode();                         /* 0x00234020 the racing modification record's model (no NULL check) */
    int  isTurbo();                              /* 0x00234048 the fitted turbine kit makes a turbo engine (record +0x11) */
    int  isTurboByDefault();                     /* 0x00234080 the same for the car's stock turbine kit */
    u16  getEngineSoundCode();                           /* 0x002340D0 engine record +0x16 (no NULL check): the engine sound code (CarSound::load) */
    int  getExhaustSoundKind();                           /* 0x002340F8 the exhaust sound: the muffler's kind, + 4 for a turbo engine (CarSound::load's kind: "normal0".."turbo3") */
    void loadPresetSettings(const PresetCarRecord *rec); /* 0x00234150 the settings of a preset car (table 0x23 / 0x1F record) */
    const char *getAspirationText();             /* 0x00234258 engine texts (string block uniStrings) */
    const char *getEngineTypeText1();            /* 0x00234298 */
    const char *getEngineTypeText2();            /* 0x002342D8 */
    CarRecord *getCarRecord();                   /* 0x00234318 the car's own record (table 0x1E) */
    u16  getMakerIndex();                        /* 0x00234340 */
    u16  getBrandIndex();                        /* 0x00234360 */
    int  getShopFlags();                         /* 0x00234380 the car record's flags (+0xF0); int: every
                                                    caller shifts the result with sra (u009, u010, u066,
                                                    u108), a u8 return gives srl */
    const char *getName1();                      /* 0x002343A0 the two parts of the car's name, by region */
    const char *getName2();                      /* 0x00234420 */
    const char *getShortName();                  /* 0x002344A0 car record shortName (strings): the car's short code name */
    const char *getMakerDisplayName();           /* 0x002344D0 car record makerName (uniStrings): the garage's maker column
                                                    and maker sort (u066; getMakerName is the g_002EFF20 name) */
    u8   getDriveTrain();                        /* 0x00234500 the drive train record's layout: 0 FR, 1 FF, 2 4WD, 3 MR, 4 RR */
    u8   getAwdType();                           /* 0x00234528 the drive train record's awdType (no caller) */
    u8   getGearCount();                         /* 0x00234550 the transmission's forward gears (no caller) */
    /* The enhanced engine: Lupo 2's CVT test for the menus, getGearCount()
     * == 2 (L2 0x002433B0; menus_b.md 1).  No caller yet. */
    int  isCVT();
    int  isRacingModified();                     /* 0x00234578 the racing modification record's kind != 0 */
    int  hasDirtTires();                         /* 0x002345A8 both tyres are dirt tyres (kind 0xC) */
    int  getPartCategory(int part);              /* 0x00234608 the category (record table) of part id `part', -1 if none */
    int  getPartKind(int part);                  /* 0x002346F0 the kind of part id `part' within its category, -1 if none */
    u64  findPartCode(int part);                 /* 0x00234778 the code of this car's version of part id `part', 0 if none */
    u64  getDefaultPartCode(int category);       /* 0x002347D0 the code of the car's stock part of `category', 0 if none */
    EngineRecord *getEngineRecord();             /* 0x002348E8 table 0x06 */
    SuspensionRecord *getSuspensionRecord();     /* 0x00234918 table 0x12 */
    BrakeControllerRecord *getBrakecontrollerRecord(); /* 0x00234948 table 0x01 */
    TransmissionRecord *getGearRecord();         /* 0x00234978 table 0x11 */
    DrivetrainRecord *getDrivetrainRecord();     /* 0x002349A8 table 0x0D */
    RacingModifyRecord *getRacingmodifyRecord(); /* 0x002349D8 table 0x05 */
    ChassisRecord *getChassisRecord();           /* 0x00234A08 table 0x03 */
    LsdRecord *getLSDRecord();                   /* 0x00234A30 table 0x15 */
    AsccRecord *getASCCRecord();                 /* 0x00234A60 table 0x17 */
    TcsRecord *getTCSCRecord();                  /* 0x00234A90 table 0x16 */
    BrakeRecord *getBrakeRecord();               /* 0x00234AC0 table 0x00 */
    LightweightRecord *getLightweightRecord();   /* 0x00234AF0 table 0x04 */
    PowerRecord *getPortpolishRecord();          /* 0x00234B20 table 0x07 */
    EngineTuneRecord *getEnginebalanceRecord();  /* 0x00234B50 table 0x08 */
    PowerRecord *getDisplacementRecord();        /* 0x00234B80 table 0x09 */
    PowerRecord *getComputerRecord();            /* 0x00234BB0 table 0x0A */
    EngineTuneRecord *getNATuneRecord();         /* 0x00234BE0 table 0x0B */
    TurbineKitRecord *getTurbineRecord();        /* 0x00234C10 table 0x0C */
    InertiaRecord *getFlywheelRecord();          /* 0x00234C40 table 0x0E */
    InertiaRecord *getClutchRecord();            /* 0x00234C70 table 0x0F */
    PropellerShaftRecord *getPropellershaftRecord(); /* 0x00234CA0 table 0x10 */
    PowerRecord *getIntercoolerRecord();         /* 0x00234CD0 table 0x13 */
    PowerRecord *getMufflerRecord();             /* 0x00234D00 table 0x14 */
    WheelRecord *getWheelRecord();               /* 0x00234D30 table 0x18 */
    TiresRecord *getFronttireRecord();           /* 0x00234D60 table 0x1C */
    TiresRecord *getReartireRecord();            /* 0x00234D90 table 0x1D */
    u8   getFronttireKind();                     /* 0x00234DC0 0 normal, 1 sports, 2..8 racing, 9 control, 0xA / 0xB, 0xC dirt */
    u8   getReartireKind();                      /* 0x00234DE0 */
    int  getBrakePart();                         /* 0x00234E00 the part ids (0x00..0x56, 0x58 none) of the fitted parts */
    int  getBrakecontrollerPart();               /* 0x00234E40 */
    int  getDrivetrainPart();                    /* 0x00234E80 */
    int  getGearPart();                          /* 0x00234EC0 */
    int  getSuspensionPart();                    /* 0x00234F00 */
    int  getFronttirePart();                     /* 0x00234F40 */
    int  getReartirePart();                      /* 0x00234F80 */
    int  getLSDPart();                           /* 0x00234FC0 */
    int  getRacingmodifyPart();                  /* 0x00235000 */
    int  getASCCPart();                          /* 0x00235040 */
    int  getTCSCPart();                          /* 0x00235080 */
    int  getLightweightPart();                   /* 0x002350C0 */
    int  getPortpolishPart();                    /* 0x00235100 */
    int  getEnginebalancePart();                 /* 0x00235140 */
    int  getDisplacementPart();                  /* 0x00235180 */
    int  getComputerPart();                      /* 0x002351C0 */
    int  getNATunePart();                        /* 0x00235200 */
    int  getTurbinePart();                       /* 0x00235240 */
    int  getFlywheelPart();                      /* 0x00235280 */
    int  getClutchPart();                        /* 0x002352C0 */
    int  getPropellershaftPart();                /* 0x00235300 0x58 (none) for FF, MR and RR */
    int  getIntercoolerPart();                   /* 0x00235378 0x58 (none) unless a turbo in stock form */
    int  getMufflerPart();                       /* 0x002353D0 */
    const char *getDriveTrainText();             /* 0x00235410 */
    u16  getWheelBase();                         /* 0x00235498 */
    int  getMaxPower();                          /* 0x002354B8 the engine getters do not check for a NULL record */
    const char *getMaxPowerRpmText();            /* 0x002354F8 */
    int  getMaxTorque();                         /* 0x00235528 */
    const char *getMaxTorqueRpmText();           /* 0x00235568 */
    const char *getDisplacementText();           /* 0x00235598 */
    u16  getLength();                            /* 0x002355C8 */
    u16  getHeight();                            /* 0x002355E8 */
    u16  getWidth();                             /* 0x00235608 the racing modification record's width (no NULL check) */
    const char *getCategoryName(int part);       /* 0x00235628 the EUC-JP name of the category of part id `part' (no caller) */
    static const char *getPartFullName(int part); /* 0x00235780 no `this' */
    static const char *getPartName(int part);    /* 0x002357B0 no `this'; 0x58 reads past g_002EC3C0 */
    const char *getBrakeName();                  /* 0x002357E0 the EUC-JP names of the fitted parts */
    const char *getBrakecontrollerName();        /* 0x00235808 */
    const char *getClutchName();                 /* 0x00235830 */
    const char *getComputerName();               /* 0x00235858 */
    const char *getDisplacementName();           /* 0x00235880 */
    const char *getEnginebalanceName();          /* 0x002358A8 */
    const char *getFlywheelName();               /* 0x002358D0 */
    const char *getGearName();                   /* 0x002358F8 */
    const char *getIntercoolerName();            /* 0x00235920 */
    const char *getLightweightName();            /* 0x00235968 */
    const char *getLSDName();                    /* 0x00235990 */
    const char *getMufflerName();                /* 0x002359B8 */
    const char *getNATuneName();                 /* 0x002359E0 */
    const char *getPortpolishName();             /* 0x00235A08 */
    const char *getPropellershaftName();         /* 0x00235A30 */
    const char *getRacingmodifyName();           /* 0x00235A78 */
    const char *getSuspensionName();             /* 0x00235AA0 */
    const char *getTCSCName();                   /* 0x00235AC8 */
    const char *getFronttireName();              /* 0x00235AF0 */
    const char *getReartireName();               /* 0x00235B18 */
    const char *getTurbineName();                /* 0x00235B40 */
    const char *getASCCName();                   /* 0x00235BA0 */
    const char *getDrivetrainName();             /* 0x00235BC8 */
    int  F_00235BF0(int category);               /* 0x00235BF0 1 for the categories 0x01, 0x02, 0x0C, 0x16, 0x17 (no caller) */
    int  F_00235C28(int category);               /* 0x00235C28 1 for the categories a car may have no part of (?) */
    u64  getPartCodeToFit(int part);             /* 0x00235C60 this car's version of part id `part', else the stock part of its category */
    void fitPart(int part);                      /* 0x00235CF8 fits part id `part', then the settings follow the part */
    int  getAutoGearRpm();                           /* 0x00235EC8 engine record autoGearRpm x 100 (no caller) */
    int  getRevLimitRpm();                           /* 0x00235EF0 engine record revLimit x 100 (no caller) */
    int  clamp(int lo, int v, int hi);           /* 0x00235F18 v limited to lo..hi (`this' unused) */
    void setColorCode(int code);                 /* 0x00235F30 colorCode = code; unless -1 also the matching colour index */
    int  setColorIndex(int index);               /* 0x00235F88 colorIndex = index, colorCode its colour id; 1 if the model has it */
    const char *getMakerName();                  /* 0x00235FD8 */
    const char *getBrandName();                  /* 0x00236008 */
    int  isDefaultWheel();                       /* 0x00236038 */
    const char *getWheelName();                  /* 0x00236048 the fitted wheel's name, NULL for the default wheel */
    int  hasStockParts();                        /* 0x00236088 every part is the car's stock part (not the tyres and the wheel) */
    int  isTuned();                              /* 0x002362B8 a part differs from the stock one or is not its category's base part */
    int  hasArcadeTires();                           /* 0x002364E0 both tyres are of kind 0xA (? the arcade tyres, MenuArgs.tyre1 0) */
    int  hasArcadeRacingTires();                           /* 0x00236540 both tyres are of kind 0xB (? the arcade racing tyres, MenuArgs.tyre1 1) */
    int  hasFirstSetTires();                           /* 0x002365A0 neither tyre is of a kind >= 0xD */
    int  F_00236600();                           /* 0x00236600 both tyres are of the second set (kinds 13..24) */
    int  F_00236668(int kind);                   /* 0x00236668 fits the car's tyres of kind `kind' (tables 0x1C, 0x1D); 0 if none */
    int  F_002366D8();                           /* 0x002366D8 tyres of kind 0xC unless both already are (1 then) */
    int  F_00236718();                           /* 0x00236718 tyres of kind 0xA unless both already are */
    int  F_00236758();                           /* 0x00236758 tyres of kind 0xB unless both already are */
    int  F_00236798();                           /* 0x00236798 the second-set tyres matching the front tyre (0..11 -> 13..24) */
    int  F_00236868();                           /* 0x00236868 the first-set tyres matching the front tyre (13..24 -> 0..11) */
    u8  *serialize(void *buf);                  /* 0x00236940 the 0x1D0-byte save block into buf; buf + 0x1D0 */
    const u8 *deserialize(const void *data);     /* 0x00236978 the save block from data; data + 0x1D0 */
    void F_002369A8();                           /* 0x002369A8 the gear ratios of the auto level (maxSpeed x 10) when the transmission's gearFlag is set */
    int  F_00236A38();                           /* 0x00236A38 F_0023AC80 of the car record's regionLock */
    int  F_00236A60(const char *name);           /* 0x00236A60 loadPreset(nameToCode(name)) */
    u64  F_00236A98(int table, int kind);        /* 0x00236A98 findPart(table, kind) */
    int  F_00236AB8(const char *name);           /* 0x00236AB8 setCar(nameToCode(name)) */
    /* ---- Class-inline members (Lupo 2's accessors and part setters, and
     * the unnamed ones among them).  Their out-of-line copies
     * 0x00236AF0..0x002377A0 have no caller anywhere: egcs emits every
     * inline member of the class in the unit of its key function
     * (~CarEquipments, T482), after the unit's own functions and before
     * __tf13CarEquipments, in the reverse of their definition order; so they
     * are defined here from the last address (getSteerType) back to the
     * first (getSettingRearLSDparam3Max).  The record getters' results are
     * not checked for NULL; the part setters with settings store the code,
     * then reset the settings to the part's defaults (resetGear ..
     * resetTcsc).  Callers outside T482 expand them (the `this' of an
     * inline member: the CarEquipments base kept in a register). ---- */
    u8   getSteerType() { return getCarRecord()->steerType; }                        /* 0x002377A0 the car record's steerType */
    int  IsPresetCar() { return presetCar; }                                         /* 0x00237798 */
    void setPresetCar(bool v) { presetCar = v; }                                     /* 0x00237790 */
    int  dataSize() { return sizeof(CarSaveEquipments); }                            /* 0x00237788 0x1D0, the save block */
    u16  getRestrictorLimit() { return restrictorLimit; }                            /* 0x00237780 */
    void setRestrictorLimit(int v) { restrictorLimit = (u16)v; }                     /* 0x00237778 */
    u16  getCarType() { return carType; }                                            /* 0x00237770 */
    CarEquipments &F_00237750(const CarEquipments &src) { return operator=(src); }   /* 0x00237750 operator= (the 0x1E8 bytes) */
    const char *F_00237730() { return getWheelName(); }                              /* 0x00237730 getWheelName() */
    /* bit 0 (? not sold by the dealer) */
    int  F_00237710() { return getShopFlags() & 1; }                                 /* 0x00237710 */
    /* bit 1 (? not sold at the used car shop) */
    int  F_002376E8() { return (getShopFlags() >> 1) & 1; }                          /* 0x002376E8 */
    /* bit 2 of the car record's flags (? the wheels cannot be changed) */
    int  F_002376C0() { return (getShopFlags() >> 2) & 1; }                          /* 0x002376C0 */
    int  getColorCode() { return colorCode; }                                        /* 0x002376B8 */
    int  getColorIndex() { return colorIndex; }                                      /* 0x002376B0 */
    u64  getCarCode() const { return carCode; }                                      /* 0x002376A8 */
    void setBrake(u64 code) { brake = code; }                                        /* 0x002376A0 */
    void setChassis(u64 code) { chassis = code; }                                    /* 0x00237698 */
    void setEngine(u64 code) { engine = code; }                                      /* 0x00237690 */
    void setFronttire(u64 code) { frontTire = code; }                                /* 0x00237688 */
    void setReartire(u64 code) { rearTire = code; }                                  /* 0x00237680 */
    void setLightweight(u64 code) { lightweight = code; }                            /* 0x00237678 */
    void setPortpolish(u64 code) { portPolish = code; }                              /* 0x00237670 */
    void setEnginebalance(u64 code) { engineBalance = code; }                        /* 0x00237668 */
    void setDisplacement(u64 code) { displacement = code; }                          /* 0x00237660 */
    void setComputer(u64 code) { computer = code; }                                  /* 0x00237658 */
    void setFlywheel(u64 code) { flywheel = code; }                                  /* 0x00237650 */
    void setClutch(u64 code) { clutch = code; }                                      /* 0x00237648 */
    void setPropellershaft(u64 code) { propellerShaft = code; }                      /* 0x00237640 */
    void setMuffler(u64 code) { muffler = code; }                                    /* 0x00237638 */
    void setIntercooler(u64 code) { intercooler = code; }                            /* 0x00237630 */
    void setWheel(u64 code) { wheel = code; }                                        /* 0x00237628 */
    void setDefaultWheel() { wheel = 0; }                                            /* 0x00237620 */
    /* ? name (no Lupo 2 symbol); turbine = the car's stock turbine kit
     * (getDefaultPartCode(0x0C)), resetTurbineKit, then naTune = code */
    void setNATune(u64 code) {                                                      /* 0x002375D8 */
        turbine = getDefaultPartCode(CARDB_TURBINE);
        resetTurbineKit();
        naTune = code;
    }
    void setLSD(u64 code) { lsd = code; resetLsd(); }                                /* 0x002375B8 */
    void setSuspension(u64 code) { suspension = code; resetSuspension(); }           /* 0x00237598 */
    /* ? name (no Lupo 2 symbol: setTurbo or setTurbine); turbine = code, naTune =
     * the car's stock NA tune (getDefaultPartCode(0x0B)), then resetTurbineKit */
    void setTurbo(u64 code) {                                                       /* 0x00237550 */
        u64 stockNaTune;

        stockNaTune = getDefaultPartCode(CARDB_NA_TUNE);
        turbine     = code;
        naTune      = stockNaTune;
        resetTurbineKit();
    }
    void setRacingmodify(u64 code) { racingModify = code; resetRacingModify(); }     /* 0x00237530 */
    void setBrakecontroller(u64 code) { brakeController = code; resetBrakeController(); } /* 0x00237510 */
    void setGear(u64 code) { gear = code; resetGear(); }                             /* 0x002374F0 */
    void setDrivetrain(u64 code) { drivetrain = code; resetDrivetrain(); }           /* 0x002374D0 */
    void setASCC(u64 code) { ascc = code; resetAscc(); }                             /* 0x002374B0 */
    void setTCSC(u64 code) { tcsc = code; resetTcsc(); }                             /* 0x00237490 */
    /* ? name (the order says Param4WD, but it reads the drive train record's
     * vcdMax +0x12, as getSettingParam4WDMax, not a level count) */
    u8   getMaxLevelParam4WD() { return getDrivetrainRecord()->vcdMax; }             /* 0x00237470 */
    u8   getMaxLevelFrontABS() { return getBrakecontrollerRecord()->countF; }        /* 0x00237450 */
    u8   getMaxLevelRearABS() { return getBrakecontrollerRecord()->countR; }         /* 0x00237430 */
    u8   getMaxLevelFrontDamperFB() { return getSuspensionRecord()->damperFCount; }  /* 0x00237410 */
    u8   getMaxLevelFrontDamperFR() { return getSuspensionRecord()->reboundFCount; } /* 0x002373F0 */
    u8   getMaxLevelRearDamperFB() { return getSuspensionRecord()->damperRCount; }   /* 0x002373D0 */
    u8   getMaxLevelRearDamperFR() { return getSuspensionRecord()->reboundRCount; }  /* 0x002373B0 */
    u8   getMaxLevelFrontStabilizer() { return getSuspensionRecord()->stabilizerFCount; } /* 0x00237390 */
    u8   getMaxLevelRearStabilizer() { return getSuspensionRecord()->stabilizerRCount; } /* 0x00237370 */
    u8   getMaxLevelTCSC() { return getTCSCRecord()->count; }                        /* 0x00237350 */
    u8   getMaxLevelVSCparam1() { return getASCCRecord()->count0; }                  /* 0x00237330 */
    u8   getMaxLevelVSCparamB() { return getASCCRecord()->count1; }                  /* 0x00237310 */
    u8   getMaxLevelVUCparam1() { return getASCCRecord()->count2; }                  /* 0x002372F0 */
    u8   getMaxLevelVUCparamB() { return getASCCRecord()->count3; }                  /* 0x002372D0 */
    void setSettingGearRatio(int i, int v) { gearRatio[i] = v; }                     /* 0x002372C0 gear ratio i */
    void setSettingFinalGearRatio(int v) { finalGearRatio = v; }                     /* 0x002372B8 */
    void setSettingMaxSpeed(int v) { maxSpeed = v; }                                 /* 0x002372B0 */
    void setSettingLastFinalGearRatio(int v) { lastFinalGearRatio = v; }             /* 0x002372A8 */
    void setSettingParam4WD(int v) { param4WD = v; }                                 /* 0x002372A0 */
    void setSettingFrontABS(int v) { frontABS = v; }                                 /* 0x00237298 */
    void setSettingRearABS(int v) { rearABS = v; }                                   /* 0x00237290 */
    void setSettingClF(int v) { clF = v; }                                           /* 0x00237288 */
    void setSettingClR(int v) { clR = v; }                                           /* 0x00237280 */
    void setSettingBoost1(int v) { boost1 = v; }                                     /* 0x00237278 */
    void setSettingPeekRPM1(int v) { peekRPM1 = v; }                                 /* 0x00237270 */
    void setSettingResponse1(int v) { response1 = v; }                               /* 0x00237268 */
    void setSettingBoost2(int v) { boost2 = v; }                                     /* 0x00237260 */
    void setSettingPeekRPM2(int v) { peekRPM2 = v; }                                 /* 0x00237258 */
    void setSettingResponse2(int v) { response2 = v; }                               /* 0x00237250 */
    void setSettingFrontDamperFB(int v) { frontDamperF1B = v; frontDamperF2B = v; }  /* 0x00237240 */
    void setSettingFrontDamperF1B(int v) { frontDamperF1B = v; }                     /* 0x00237238 */
    void setSettingFrontDamperF2B(int v) { frontDamperF2B = v; }                     /* 0x00237230 */
    void setSettingFrontDamperFR(int v) { frontDamperF1R = v; frontDamperF2R = v; }  /* 0x00237220 */
    void setSettingFrontDamperF1R(int v) { frontDamperF1R = v; }                     /* 0x00237218 */
    void setSettingFrontDamperF2R(int v) { frontDamperF2R = v; }                     /* 0x00237210 */
    void setSettingRearDamperFB(int v) { rearDamperF1B = v; rearDamperF2B = v; }     /* 0x00237200 */
    void setSettingRearDamperF1B(int v) { rearDamperF1B = v; }                       /* 0x002371F8 */
    void setSettingRearDamperF2B(int v) { rearDamperF2B = v; }                       /* 0x002371F0 */
    void setSettingRearDamperFR(int v) { rearDamperF1R = v; rearDamperF2R = v; }     /* 0x002371E0 */
    void setSettingRearDamperF1R(int v) { rearDamperF1R = v; }                       /* 0x002371D8 */
    void setSettingRearDamperF2R(int v) { rearDamperF2R = v; }                       /* 0x002371D0 */
    void setSettingFrontCamber(int v) { frontCamber = v; }                           /* 0x002371C8 */
    void setSettingRearCamber(int v) { rearCamber = v; }                             /* 0x002371C0 */
    void setSettingFrontToe(int v) { frontToe = v; }                                 /* 0x002371B8 */
    void setSettingRearToe(int v) { rearToe = v; }                                   /* 0x002371B0 */
    void setSettingFrontRideheight(int v) { frontRideheight = v; }                   /* 0x002371A8 */
    void setSettingRearRideheight(int v) { rearRideheight = v; }                     /* 0x002371A0 */
    void setSettingFrontSpringrate(int v) { frontSpringrate = v; }                   /* 0x00237198 */
    void setSettingRearSpringrate(int v) { rearSpringrate = v; }                     /* 0x00237190 */
    void setSettingFrontStabilizer(int v) { frontStabilizer = v; }                   /* 0x00237188 */
    void setSettingRearStabilizer(int v) { rearStabilizer = v; }                     /* 0x00237180 */
    void setSettingFrontLSDparam(int v) { frontLSDparam = v; }                       /* 0x00237178 */
    void setSettingFrontLSDparam2(int v) { frontLSDparam2 = v; }                     /* 0x00237170 */
    void setSettingFrontLSDparam3(int v) { frontLSDparam3 = v; }                     /* 0x00237168 */
    void setSettingRearLSDparam(int v) { rearLSDparam = v; }                         /* 0x00237160 */
    void setSettingRearLSDparam2(int v) { rearLSDparam2 = v; }                       /* 0x00237158 */
    void setSettingRearLSDparam3(int v) { rearLSDparam3 = v; }                       /* 0x00237150 */
    void setSettingTCSC(int v) { settingTCSC = v; }                                  /* 0x00237148 */
    void setSettingASCC(int v) { settingASCC = v; }                                  /* 0x00237140 */
    /* the transmission's forward gears */
    u8   getGearNum() { return getGearRecord()->gears; }                             /* 0x00237120 */
    int  getSettingGearRatio(int i) { return gearRatio[i]; }                         /* 0x00237110 gear ratio i */
    int  getSettingFinalGearRatio() { return finalGearRatio; }                       /* 0x00237108 */
    int  getSettingMaxSpeed() { return maxSpeed; }                                   /* 0x00237100 */
    /* the transmission record's gearFlag (not checked for NULL) */
    u8   getGearFlag() { return getGearRecord()->gearFlag; }                         /* 0x002370E0 */
    int  getSettingLastFinalGearRatio() { return lastFinalGearRatio; }               /* 0x002370D8 */
    int  getSettingParam4WD() { return param4WD; }                                   /* 0x002370D0 */
    int  getSettingFrontABS() { return frontABS; }                                   /* 0x002370C8 */
    int  getSettingRearABS() { return rearABS; }                                     /* 0x002370C0 */
    int  getSettingClF() { return clF; }                                             /* 0x002370B8 */
    int  getSettingClR() { return clR; }                                             /* 0x002370B0 */
    int  getSettingBoost1() { return boost1; }                                       /* 0x002370A8 */
    int  getSettingPeekRPM1() { return peekRPM1; }                                   /* 0x002370A0 */
    int  getSettingResponse1() { return response1; }                                 /* 0x00237098 */
    int  getSettingBoost2() { return boost2; }                                       /* 0x00237090 */
    int  getSettingPeekRPM2() { return peekRPM2; }                                   /* 0x00237088 */
    int  getSettingResponse2() { return response2; }                                 /* 0x00237080 */
    int  getSettingFrontDamperF1B() { return frontDamperF1B; }                       /* 0x00237078 */
    int  getSettingFrontDamperF2B() { return frontDamperF2B; }                       /* 0x00237070 */
    int  getSettingFrontDamperF1R() { return frontDamperF1R; }                       /* 0x00237068 */
    int  getSettingFrontDamperF2R() { return frontDamperF2R; }                       /* 0x00237060 */
    int  getSettingRearDamperF1B() { return rearDamperF1B; }                         /* 0x00237058 */
    int  getSettingRearDamperF2B() { return rearDamperF2B; }                         /* 0x00237050 */
    int  getSettingRearDamperF1R() { return rearDamperF1R; }                         /* 0x00237048 */
    int  getSettingRearDamperF2R() { return rearDamperF2R; }                         /* 0x00237040 */
    int  getSettingFrontCamber() { return frontCamber; }                             /* 0x00237038 */
    int  getSettingRearCamber() { return rearCamber; }                               /* 0x00237030 */
    int  getSettingFrontToe() { return frontToe; }                                   /* 0x00237028 */
    int  getSettingRearToe() { return rearToe; }                                     /* 0x00237020 */
    int  getSettingFrontRideheight() { return frontRideheight; }                     /* 0x00237018 */
    int  getSettingRearRideheight() { return rearRideheight; }                       /* 0x00237010 */
    int  getSettingFrontSpringrate() { return frontSpringrate; }                     /* 0x00237008 */
    int  getSettingRearSpringrate() { return rearSpringrate; }                       /* 0x00237000 */
    int  getSettingFrontStabilizer() { return frontStabilizer; }                     /* 0x00236FF8 */
    int  getSettingRearStabilizer() { return rearStabilizer; }                       /* 0x00236FF0 */
    int  getSettingFrontLSDparam() { return frontLSDparam; }                         /* 0x00236FE8 */
    int  getSettingFrontLSDparam2() { return frontLSDparam2; }                       /* 0x00236FE0 */
    int  getSettingFrontLSDparam3() { return frontLSDparam3; }                       /* 0x00236FD8 */
    int  getSettingRearLSDparam() { return rearLSDparam; }                           /* 0x00236FD0 */
    int  getSettingRearLSDparam2() { return rearLSDparam2; }                         /* 0x00236FC8 */
    int  getSettingRearLSDparam3() { return rearLSDparam3; }                         /* 0x00236FC0 */
    int  getSettingTCSC() { return settingTCSC; }                                    /* 0x00236FB8 */
    int  getSettingASCC() { return settingASCC; }                                    /* 0x00236FB0 */
    u16  getSettingFinalGearRatioMin() { return getGearRecord()->finalGearRatioMin; } /* 0x00236F90 */
    u16  getSettingFinalGearRatioMax() { return getGearRecord()->finalGearRatioMax; } /* 0x00236F70 */
    u8   getSettingMaxSpeedMin() { return getGearRecord()->maxSpeedMin; }            /* 0x00236F50 */
    u8   getSettingMaxSpeedMax() { return getGearRecord()->maxSpeedMax; }            /* 0x00236F30 */
    u8   getSettingParam4WDMin() { return getDrivetrainRecord()->vcdMin; }           /* 0x00236F10 */
    u8   getSettingParam4WDMax() { return getDrivetrainRecord()->vcdMax; }           /* 0x00236EF0 */
    u8   getSettingClFMin() { return getRacingmodifyRecord()->aeroFMin; }            /* 0x00236ED0 */
    u8   getSettingClFMax() { return getRacingmodifyRecord()->aeroFMax; }            /* 0x00236EB0 */
    u8   getSettingClRMin() { return getRacingmodifyRecord()->aeroRMin; }            /* 0x00236E90 */
    u8   getSettingClRMax() { return getRacingmodifyRecord()->aeroRMax; }            /* 0x00236E70 */
    u8   getSettingFrontCamberMin() { return getSuspensionRecord()->camberFMin; }    /* 0x00236E50 */
    u8   getSettingFrontCamberMax() { return getSuspensionRecord()->camberFMax; }    /* 0x00236E30 */
    u8   getSettingRearCamberMin() { return getSuspensionRecord()->camberRMin; }     /* 0x00236E10 */
    u8   getSettingRearCamberMax() { return getSuspensionRecord()->camberRMax; }     /* 0x00236DF0 */
    u8   getSettingFrontToeMin() { return getSuspensionRecord()->toeFMin; }          /* 0x00236DD0 */
    u8   getSettingFrontToeMax() { return getSuspensionRecord()->toeFMax; }          /* 0x00236DB0 */
    u8   getSettingRearToeMin() { return getSuspensionRecord()->toeRMin; }           /* 0x00236D90 */
    u8   getSettingRearToeMax() { return getSuspensionRecord()->toeRMax; }           /* 0x00236D70 */
    u16  getSettingFrontRideheightMin() { return getSuspensionRecord()->heightFMin; } /* 0x00236D50 */
    u16  getSettingFrontRideheightMax() { return getSuspensionRecord()->heightFMax; } /* 0x00236D30 */
    u16  getSettingRearRideheightMin() { return getSuspensionRecord()->heightRMin; } /* 0x00236D10 */
    u16  getSettingRearRideheightMax() { return getSuspensionRecord()->heightRMax; } /* 0x00236CF0 */
    u8   getSettingFrontSpringrateMin() { return getSuspensionRecord()->springRateFMin; } /* 0x00236CD0 */
    u8   getSettingFrontSpringrateMax() { return getSuspensionRecord()->springRateFMax; } /* 0x00236CB0 */
    u8   getSettingRearSpringrateMin() { return getSuspensionRecord()->springRateRMin; } /* 0x00236C90 */
    u8   getSettingRearSpringrateMax() { return getSuspensionRecord()->springRateRMax; } /* 0x00236C70 */
    u8   getSettingFrontLSDparamMin() { return getLSDRecord()->initialFMin; }        /* 0x00236C50 */
    u8   getSettingFrontLSDparamMax() { return getLSDRecord()->initialFMax; }        /* 0x00236C30 */
    u8   getSettingFrontLSDparam2Min() { return getLSDRecord()->accelFMin; }         /* 0x00236C10 */
    u8   getSettingFrontLSDparam2Max() { return getLSDRecord()->accelFMax; }         /* 0x00236BF0 */
    u8   getSettingFrontLSDparam3Min() { return getLSDRecord()->decelFMin; }         /* 0x00236BD0 */
    u8   getSettingFrontLSDparam3Max() { return getLSDRecord()->decelFMax; }         /* 0x00236BB0 */
    u8   getSettingRearLSDparamMin() { return getLSDRecord()->initialRMin; }         /* 0x00236B90 */
    u8   getSettingRearLSDparamMax() { return getLSDRecord()->initialRMax; }         /* 0x00236B70 */
    u8   getSettingRearLSDparam2Min() { return getLSDRecord()->accelRMin; }          /* 0x00236B50 */
    u8   getSettingRearLSDparam2Max() { return getLSDRecord()->accelRMax; }          /* 0x00236B30 */
    u8   getSettingRearLSDparam3Min() { return getLSDRecord()->decelRMin; }          /* 0x00236B10 */
    u8   getSettingRearLSDparam3Max() { return getLSDRecord()->decelRMax; }          /* 0x00236AF0 */
};
EGCS_SIZE(CarEquipments, 0x1E8);

#endif /* GAME_T482_CAR_EQUIPMENTS_H */
