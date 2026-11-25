#ifndef TSINGTAO_GAME_H
#define TSINGTAO_GAME_H

#include <stdint.h>
#include "roguelike.h"

/* Fixed-point scale (24.8 signed integer, ZERO floats on PS1 R3000A) */
#define FP_SHIFT          8
#define FP_ONE            (1 << FP_SHIFT)
#define TO_FP(x)          ((int32_t)(x) << FP_SHIFT)
#define FROM_FP(x)        ((int32_t)((x) >> FP_SHIFT))

/* Core gameplay layout constants (PAL 320x256 @ 50 Hz) */
#define NUM_LANES         4
#define MAX_CUSTOMERS     20
#define MAX_DRINKS        16
#define MAX_EMPTIES       16
#define MAX_TIPS          8
#define MAX_PARTICLES     64
#define MAX_POPUPS        16

#define FROST_MAX         100
#define INITIAL_LIVES     3
#define MAX_LIVES         5

/* Logical counter bar_x bounds (0 = left doorway, 256 = right brass tap) */
#define BAR_LEFT_X        0
#define BAR_RIGHT_X       256
#define BARTENDER_MIN_X   16
#define BARTENDER_POUR_X  248
#define BARTENDER_TAP_X   256

/* Hard caps on counter cash tips & non-tip buffs per shift */
#define SHIFT_MAX_TIPS_SPAWNED  6
#define SHIFT_MAX_TIPS_CASH     10
#define SHIFT_MAX_BUFFS_SPAWNED 3

/* Counter pickup categories (all beneficial Gold/Cyan/Emerald pickups) */
typedef enum {
    BUFF_GOLD_TIP    = 0,
    BUFF_FROST_ORB   = 1,
    BUFF_STAR_MULT   = 2,
    BUFF_FOAM_SHIELD = 3
} BuffKind;

/* Drink types matching PS1 face buttons: X=Can, O=Bottle, []=Bag */
typedef enum {
    DRINK_CAN    = 0,
    DRINK_BOTTLE = 1,
    DRINK_BAG    = 2
} DrinkType;

/* 5 Distinct Qingdao night-market customer archetypes */
typedef enum {
    CUST_REGULAR    = 0, /* Wants 1 Tsingtao Can */
    CUST_FOODIE     = 1, /* Wants 2 Tsingtao Cans */
    CUST_BOTTLE_VIP = 2, /* Fast VIP wanting Green Bottle */
    CUST_BAG_VIP    = 3, /* VIP wanting Qingdao Plastic Beer Bag (+big Frost) */
    CUST_RUSHER     = 4  /* Sprinter wanting Can */
} CustomerType;

typedef enum {
    CSTATE_WALKING   = 0,
    CSTATE_DRINKING  = 1, /* Knocked back briefly after non-lethal drink hit */
    CSTATE_ANGRY     = 2, /* Received wrong drink (1.45x charge toward tap) */
    CSTATE_SATISFIED = 3  /* Sliding joyfully out the left door */
} CustomerState;

typedef enum {
    GAME_STEP_PLAYING   = 0,
    GAME_STEP_PAUSE     = 1,
    GAME_STEP_BONUS     = 2,
    GAME_STEP_GAME_OVER = 3,
    GAME_STEP_SHIFT_WIN = 4
} GameStepResult;

typedef struct {
    uint8_t  active;
    uint8_t  lane;
    uint8_t  type;         /* CustomerType */
    uint8_t  state;        /* CustomerState */
    uint8_t  wanted_drink; /* DrinkType */
    uint8_t  hp;           /* Drinks remaining */
    int32_t  x_fp;         /* Logical counter bar_x (0..256) in 24.8 fixed-point */
    int32_t  vx_fp;        /* Fixed-point X velocity */
    int16_t  state_timer;  /* Frames in current state */
    int16_t  stun_timer;   /* Frames stunned by Bouncer pocket trick */
    uint16_t anim_tick;
    uint8_t  hit_flash;    /* Frames remaining for white-gold impact flash */
} Customer;

typedef struct {
    uint8_t active;
    uint8_t lane;
    uint8_t type;          /* DrinkType */
    uint8_t is_rush;       /* 1 = Ice-Cold Rush super wave */
    int32_t x_fp;          /* Logical counter bar_x (0..256) in 24.8 fixed-point */
    int32_t vx_fp;
} SlidingDrink;

typedef struct {
    uint8_t  active;
    uint8_t  lane;
    uint8_t  type;         /* DRINK_CAN or DRINK_BOTTLE */
    int32_t  x_fp;         /* Logical counter bar_x (0..256) in 24.8 fixed-point */
    int32_t  vx_fp;
    uint16_t spin_tick;
} ReturningEmpty;

typedef struct {
    uint8_t active;
    uint8_t lane;
    uint8_t buff_kind;     /* BuffKind (0=Gold Tip, 1=Frost Orb, 2=2x Star, 3=Shield) */
    int16_t x;             /* Logical counter bar_x (0..256) */
    int16_t cash_val;      /* Yuan cash value */
    int16_t score_val;     /* Score points */
    int16_t life_timer;    /* Frames remaining before disappearing */
} CashTip;

typedef struct {
    uint8_t active;
    uint8_t kind;          /* 0=foam, 1=ice/sparkle, 2=shard, 3=mini sparkle, 4=danger ember */
    int32_t x_fp;          /* Screen X in 24.8 fixed-point */
    int32_t y_fp;          /* Screen Y in 24.8 fixed-point */
    int32_t vx_fp;
    int32_t vy_fp;
    int16_t life;
    uint8_t r, g, b;
} Particle;

typedef struct {
    uint8_t active;
    int16_t x;
    int16_t y;
    int16_t life;
    uint8_t r, g, b;
    char    text[16];
} ScorePopup;

typedef struct {
    /* Shift score & countdown clock */
    uint32_t shift_score;
    uint32_t shift_target;
    int16_t  shift_frames_left;
    uint16_t combo;
    uint16_t combo_timer;      /* Frames left before combo resets */
    uint16_t served_total;     /* Patrons satisfied in this shift */

    /* Shift economy caps & state */
    uint8_t  shift_tips_spawned;
    int16_t  shift_tips_cash;
    uint8_t  shift_buffs_spawned; /* Anti-exploit cap < 3 non-tip buffs per shift */
    uint8_t  rush_active;         /* 1 while any Rush drink is in flight */
    uint8_t  closing_bell_sfx;    /* Edge latch for closing bell SFX */

    /* Frost, Bar Buff & Pocket Trick timers */
    int16_t  frost;               /* 0 .. FROST_MAX */
    uint8_t  frost_ready_sfx;     /* Edge latch so SFX_RUSH_READY fires once */
    int16_t  happy_bell_timer;    /* 400f 2x Mult buff */
    int16_t  star_mult_timer;     /* 250f (5s) 2x Mult pickup buff */
    int16_t  foam_catch_timer;    /* 400f magnetic empty catch buff */
    uint8_t  foam_shield_charges; /* Max 1 active auto-catch shield */
    uint8_t  vip_coupon_spawns;   /* Next N spawns forced VIP */

    /* Last serve breakdown for HUD Coaster Strip */
    uint32_t last_serve_base;
    uint32_t last_serve_mult_x10;
    uint32_t last_serve_pts;
    char     last_style_label[16];

    /* Spawner & flow timers */
    int16_t  spawn_timer;
    int16_t  shift_banner_timer;
    int16_t  shift_end_timer;  /* Brief clear/fail pause at closing bell */
    uint8_t  shift_end_result; /* 0=none, 1=win, 2=fail */
    int16_t  life_lost_timer;  /* Brief freeze/flash after losing a life */
    int16_t  flash_timer;      /* Screen flash overlay timer */
    uint8_t  flash_r, flash_g, flash_b;
    uint8_t  last_spawn_lane;

    /* Bartender state (bar_x 0..256) */
    int8_t   lane;             /* 0..3 */
    int32_t  bartender_x_fp;   /* Logical counter bar_x in 24.8 fixed-point */
    int16_t  pour_timer;       /* 8-frame tap-pull cadence */
    int8_t   pour_buf_drink;   /* -1 or DrinkType queued during pour_timer */
    int16_t  pour_buf_timer;   /* 8-frame pour input buffer countdown */
    int16_t  celebrate_timer;  /* Victory pose timer */
    uint8_t  is_dashing;
    int8_t   dash_dir;         /* -1=left, +1=right, 0=idle */
    int8_t   buffered_drink;   /* -1 or DrinkType while sprinting back to tap */
    uint8_t  is_auto_sprinting;
    uint8_t  last_drink_served;

    /* Deterministic PRNG state (xorshift32) */
    uint32_t rng_state;

    /* Entity pools */
    Customer       customers[MAX_CUSTOMERS];
    SlidingDrink   drinks[MAX_DRINKS];
    ReturningEmpty empties[MAX_EMPTIES];
    CashTip        tips[MAX_TIPS];
    Particle       particles[MAX_PARTICLES];
    ScorePopup     popups[MAX_POPUPS];

    uint8_t        tutorial_mode;
    uint8_t        tutorial_step;
    int16_t        tutorial_step_timer;
} GameState;

/* Initialize a shift against the current RunState (resets shift counters & clock) */
void game_init_shift(GameState *gs, RunState *run);

/* Step 1 frame at 50 Hz PAL. Returns next top-level transition if any. */
GameStepResult game_update(GameState *gs, RunState *run);

/* Render the 2.5D Qingdao street bar scene, depth-sorted entities, and safe-zone HUD */
void game_draw(const GameState *gs, const RunState *run);

/* Initialize, update, and render the 5-step interactive guided tutorial */
void game_init_tutorial(GameState *gs, RunState *run);
GameStepResult game_update_tutorial(GameState *gs, RunState *run);
void game_draw_tutorial(const GameState *gs, const RunState *run);

#endif /* TSINGTAO_GAME_H */
