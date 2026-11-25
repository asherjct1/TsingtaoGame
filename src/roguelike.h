#ifndef TSINGTAO_ROGUELIKE_H
#define TSINGTAO_ROGUELIKE_H

#include <stdint.h>

#define RL_MAX_COASTERS 5
#define RL_NUM_COASTERS 20
#define RL_MAX_TRICKS   2
#define RL_NUM_TRICKS   6
#define RL_NUM_BOSSES   8
#define RL_WIN_NIGHT    8
#define RL_SCORE_CAP    999999999u
#define RL_SERVE_CAP    99999999u

typedef enum {
    RARITY_COMMON   = 0,
    RARITY_UNCOMMON = 1,
    RARITY_RARE     = 2
} CoasterRarity;

typedef enum {
    COASTER_COLD_FOAM      = 0,
    COASTER_GREEN_GLASS    = 1,
    COASTER_PLASTIC_HUSTLE = 2,
    COASTER_TAP_RHYTHM     = 3,
    COASTER_MIXTAPE_POUR   = 4,
    COASTER_GLASS_JUGGLER  = 5,
    COASTER_TIP_JAR        = 6,
    COASTER_TOP_SHELF      = 7,
    COASTER_STREET_SIDE    = 8,
    COASTER_BIG_APPETITE   = 9,
    COASTER_QINGDAO_TRIO   = 10,
    COASTER_VIP_VELVET     = 11,
    COASTER_FROST_BITE     = 12,
    COASTER_CLEAN_APRON    = 13,
    COASTER_PIGGY_BANK     = 14,
    COASTER_SPEED_SNEAKERS = 15,
    COASTER_GOLDEN_TAP     = 16,
    COASTER_ICE_KING       = 17,
    COASTER_BAG_SPLASH     = 18,
    COASTER_GLASS_CANNON   = 19
} CoasterId;

/* Coaster category bitflags */
#define RL_FLAG_BASE  (1u << 0)
#define RL_FLAG_MULT  (1u << 1)
#define RL_FLAG_XMULT (1u << 2)
#define RL_FLAG_ECON  (1u << 3)
#define RL_FLAG_UTIL  (1u << 4)

typedef enum {
    TRICK_NONE       = -1,
    TRICK_COLD_MUG   = 0,
    TRICK_HAPPY_BELL = 1,
    TRICK_BOUNCER    = 2,
    TRICK_ICE_BUCKET = 3,
    TRICK_FOAM_CATCH = 4,
    TRICK_VIP_COUPON = 5
} TrickId;

typedef enum {
    BOSS_NONE             = 0,
    BOSS_BOTTLENECK       = 1,
    BOSS_TOUGH_CROWD      = 2,
    BOSS_SLIPPERY_BAR     = 3,
    BOSS_BLACKOUT         = 4,
    BOSS_NO_TIPS          = 5,
    BOSS_BAG_FESTIVAL     = 6,
    BOSS_DOUBLE_SHIFT     = 7,
    BOSS_HEALTH_INSPECTOR = 8
} BossType;

typedef enum {
    SHIFT_REGULAR    = 0,
    SHIFT_HAPPY_HOUR = 1,
    SHIFT_BOSS       = 2
} ShiftKind;

typedef struct {
    uint8_t     id;
    uint8_t     rarity;
    uint8_t     cost;
    uint8_t     flags;
    const char *name;
    const char *desc;
} CoasterDef;

typedef struct {
    uint8_t     id;
    uint8_t     cost;
    const char *name;
    const char *desc;
} TrickDef;

typedef struct {
    uint8_t     id;
    const char *name;
    const char *desc;
} BossDef;

typedef struct {
    uint32_t    base;
    uint32_t    mult_x10;
    uint32_t    total_pts;
    uint8_t     triggered_mask;
    uint8_t     splash_triggered;
    const char *style_label;
} ServeScoreBreakdown;

typedef struct {
    uint16_t night;
    uint8_t  shift_in_night;
    uint8_t  endless_mode;
    uint8_t  won_night8;
    uint8_t  boss_order[8];
    int8_t   lives;
    int16_t  cash;
    uint32_t run_score;
    uint32_t high_score;
    uint32_t served_total;
    uint8_t  drink_level[3];
    uint8_t  coasters[RL_MAX_COASTERS];
    uint8_t  coaster_count;
    uint8_t  coaster_ping[RL_MAX_COASTERS];
    uint8_t  clean_apron_stacks;
    int8_t   tricks[RL_MAX_TRICKS];
    uint32_t rng_state;
    uint8_t  last_drinks[3];
    uint8_t  last_drink_count;
    uint8_t  empty_catch_buff;
    int16_t  tip_buff_timer;
    uint8_t  shift_spills;
} RunState;

typedef struct {
    uint8_t choices[3];
    int8_t  cursor;
    int16_t flip_timer;
    int8_t  picked;
} DraftState;

typedef struct {
    int16_t  base_pay;
    int16_t  lives_bonus;
    int16_t  interest;
    int16_t  overkill_bonus;
    int16_t  tips_earned;
    int16_t  total_cash;
    uint32_t shift_score;
    uint32_t shift_target;
    uint16_t timer;
    uint8_t  step;
} PayoutState;

typedef struct {
    uint8_t coaster_offers[2];
    uint8_t coaster_sold[2];
    uint8_t drink_offer;
    uint8_t drink_sold;
    int8_t  trick_offer;
    uint8_t trick_sold;
    int16_t reroll_cost;
    int8_t  zone;
    int8_t  cursor;
    int16_t card_flip[4];
    char    toast[36];
    int16_t toast_timer;
} ShopState;

/* Run initialization, target curve & Boss Twist rotation */
void     rl_run_init(RunState *run, uint32_t seed, uint32_t high_score);
uint32_t rl_shift_target(uint16_t night, uint8_t shift_in_night);
int16_t  rl_shift_duration_frames(uint8_t shift_in_night);
BossType rl_current_boss(const RunState *run);
BossType rl_night_boss(const RunState *run, uint16_t night);
void     rl_advance_shift(RunState *run);

/* Coaster, Trick & Boss inventory / lookup helpers */
int               rl_has_coaster(const RunState *run, CoasterId id);
int               rl_add_coaster(RunState *run, CoasterId id);
int               rl_add_trick(RunState *run, TrickId id);
CoasterId         rl_roll_rare_coaster(RunState *run);
const CoasterDef *rl_coaster_def(uint8_t id);
const TrickDef   *rl_trick_def(int8_t id);
const BossDef    *rl_boss_def(uint8_t id);

/* Pure-integer Balatro serve scoring & event hooks */
void rl_score_serve(RunState *run,
                    uint8_t drink_type,
                    uint8_t lane,
                    uint8_t cust_type,
                    uint8_t cust_hp_before,
                    uint16_t combo,
                    int16_t frost,
                    uint8_t is_rush,
                    uint8_t adjacent_occupied,
                    uint8_t happy_bell_active,
                    ServeScoreBreakdown *out);

void rl_on_empty_caught(RunState *run);
void rl_on_tip_collected(RunState *run);
void rl_on_spill(RunState *run);
void rl_tick_timers(RunState *run);

/* Interactive Starter Draft UI */
void rl_draft_init(DraftState *ds, RunState *run);
int  rl_draft_update(DraftState *ds, RunState *run);
void rl_draft_draw(const DraftState *ds, const RunState *run);

/* Interactive Shift Payout UI */
void rl_payout_init(PayoutState *ps, RunState *run,
                    uint32_t shift_score, uint32_t shift_target,
                    int16_t tips_earned);
int  rl_payout_update(PayoutState *ps, RunState *run);
void rl_payout_draw(const PayoutState *ps, const RunState *run);

/* Interactive Night-Market Shop UI */
void rl_shop_init(ShopState *ss, RunState *run);
int  rl_shop_update(ShopState *ss, RunState *run);
void rl_shop_draw(const ShopState *ss, const RunState *run);

#endif /* TSINGTAO_ROGUELIKE_H */
