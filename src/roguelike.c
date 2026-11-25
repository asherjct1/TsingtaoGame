#include "roguelike.h"
#include "gfx.h"
#include "room3d.h"
#include "input.h"
#include "audio.h"
#include "assets_layout.h"
#include "sfx_table.h"

#include <stddef.h>
#include <string.h>

/* =========================================================================
 * Deterministic PRNG & Integer Formatting Helpers (ZERO floats, ZERO libc)
 * ========================================================================= */

static uint32_t rl_rand(RunState *run)
{
    uint32_t x = run ? run->rng_state : 0x96A51996u;
    if (x == 0u) {
        x = 0x96A51996u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (run) {
        run->rng_state = x;
    }
    return x;
}

static int rl_rand_range(RunState *run, int min_v, int max_v)
{
    if (max_v <= min_v) {
        return min_v;
    }
    return min_v + (int)(rl_rand(run) % (uint32_t)(max_v - min_v + 1));
}

static char *buf_append_str(char *dst, char *end, const char *src)
{
    if (!src) return dst;
    while (*src && dst < end) {
        *dst++ = *src++;
    }
    *dst = '\0';
    return dst;
}

static char *buf_append_uint(char *dst, char *end, uint32_t val)
{
    char rev[12];
    int n = 0;

    do {
        rev[n++] = (char)('0' + (val % 10u));
        val /= 10u;
    } while (val > 0u && n < 12);

    while (n > 0 && dst < end) {
        *dst++ = rev[--n];
    }
    *dst = '\0';
    return dst;
}

static char *buf_append_int(char *dst, char *end, int32_t val)
{
    if (val < 0) {
        if (dst < end) {
            *dst++ = '-';
        }
        return buf_append_uint(dst, end, (uint32_t)(-(val + 1)) + 1u);
    }
    return buf_append_uint(dst, end, (uint32_t)val);
}

/* Smooth integer triangle-wave oscillation in [-amp, +amp] for 3D card tilt */
static int integer_osc(uint32_t tick, uint32_t period, int amp)
{
    uint32_t half;
    uint32_t phase;

    if (period < 2u || amp <= 0) return 0;
    half = period >> 1;
    phase = tick % period;
    if (phase < half) {
        return -amp + (int)((phase * (uint32_t)(amp * 2)) / half);
    }
    return amp - (int)(((phase - half) * (uint32_t)(amp * 2)) / (period - half));
}

/* =========================================================================
 * Static Catalogs: 20 Coasters, 6 Pocket Tricks, 8 Boss Twists
 * ========================================================================= */

static const CoasterDef k_coasters[RL_NUM_COASTERS] = {
    { COASTER_COLD_FOAM,      RARITY_COMMON,   3, RL_FLAG_BASE,
      "COLD FOAM",      "+30 BASE ON EVERY SERVE" },
    { COASTER_GREEN_GLASS,    RARITY_COMMON,   4, RL_FLAG_BASE | RL_FLAG_MULT,
      "GREEN GLASS",    "BOTTLE: +40 BASE & +1.5 MULT" },
    { COASTER_PLASTIC_HUSTLE, RARITY_COMMON,   4, RL_FLAG_BASE | RL_FLAG_MULT,
      "PLASTIC HUSTLE", "BAG: +45 BASE & +1.5 MULT" },
    { COASTER_TAP_RHYTHM,     RARITY_COMMON,   4, RL_FLAG_MULT,
      "TAP RHYTHM",     "+0.3 MULT PER COMBO STREAK" },
    { COASTER_MIXTAPE_POUR,   RARITY_UNCOMMON, 5, RL_FLAG_MULT,
      "MIXTAPE POUR",   "+2.5 MULT IF DRINK DIFFERS" },
    { COASTER_GLASS_JUGGLER,  RARITY_UNCOMMON, 5, RL_FLAG_BASE | RL_FLAG_MULT,
      "GLASS JUGGLER",  "CATCH EMPTY: +80 BASE +2 MULT" },
    { COASTER_TIP_JAR,        RARITY_COMMON,   4, RL_FLAG_MULT | RL_FLAG_ECON,
      "TIP JAR",        "ON TIP: +3.0 MULT FOR 7 SEC" },
    { COASTER_TOP_SHELF,      RARITY_RARE,     7, RL_FLAG_XMULT,
      "TOP SHELF",      "*2.0 MULT IN TOP LANE 1" },
    { COASTER_STREET_SIDE,    RARITY_UNCOMMON, 5, RL_FLAG_BASE | RL_FLAG_MULT,
      "STREET SIDE",    "LANE 4: +60 BASE & +1.5 MULT" },
    { COASTER_BIG_APPETITE,   RARITY_UNCOMMON, 5, RL_FLAG_XMULT,
      "BIG APPETITE",   "*1.8 MULT VS MULTI-HP PATRON" },
    { COASTER_QINGDAO_TRIO,   RARITY_RARE,     7, RL_FLAG_XMULT,
      "QINGDAO TRIO",   "CAN+BTL+BAG SET: *3.0 MULT" },
    { COASTER_VIP_VELVET,     RARITY_RARE,     6, RL_FLAG_XMULT,
      "VIP VELVET",     "*2.0 MULT ON BOTTLE/BAG VIPS" },
    { COASTER_FROST_BITE,     RARITY_UNCOMMON, 5, RL_FLAG_BASE | RL_FLAG_XMULT,
      "FROST BITE",     "*2.0 AT 75+ FROST (+30 ELSE)" },
    { COASTER_CLEAN_APRON,    RARITY_UNCOMMON, 5, RL_FLAG_MULT,
      "CLEAN APRON",    "+3 MULT/CLEAN SHIFT (0 SPILL)" },
    { COASTER_PIGGY_BANK,     RARITY_COMMON,   4, RL_FLAG_ECON,
      "PIGGY BANK",     "+$1 PAY & $8 INTEREST CAP" },
    { COASTER_SPEED_SNEAKERS, RARITY_COMMON,   4, RL_FLAG_UTIL,
      "SPEED SNEAKERS", "+35% BARTENDER DASH SPEED" },
    { COASTER_GOLDEN_TAP,     RARITY_RARE,     7, RL_FLAG_MULT | RL_FLAG_ECON,
      "GOLDEN TAP",     "+1.5 MULT PER $5 IN BANK" },
    { COASTER_ICE_KING,       RARITY_RARE,     7, RL_FLAG_XMULT,
      "ICE KING",       "*2.5 MULT IN ICE-COLD RUSH" },
    { COASTER_BAG_SPLASH,     RARITY_UNCOMMON, 5, RL_FLAG_BASE | RL_FLAG_UTIL,
      "BAG SPLASH",     "BAG: +30 BASE & 50% ADJ HIT" },
    { COASTER_GLASS_CANNON,   RARITY_RARE,     7, RL_FLAG_XMULT,
      "GLASS CANNON",   "*3.0 MULT; SPILLS COST 1 LIFE!" }
};

static const TrickDef k_tricks[RL_NUM_TRICKS] = {
    { TRICK_COLD_MUG,   4, "COLD MUG",   "+1 LIFE (MAX 5) WHEN USED" },
    { TRICK_HAPPY_BELL, 4, "HAPPY BELL", "*2.0 MULT FOR 8 SECONDS" },
    { TRICK_BOUNCER,    3, "BOUNCER",    "SHOVE & STUN ALL PATRONS" },
    { TRICK_ICE_BUCKET, 4, "ICE BUCKET", "FILL FROST GAUGE TO 100%" },
    { TRICK_FOAM_CATCH, 3, "FOAM CATCH", "AUTO-CATCH EMPTIES 8 SEC" },
    { TRICK_VIP_COUPON, 3, "VIP COUPON", "NEXT 3 SPAWNS ARE VIPS" }
};

static const BossDef k_bosses[RL_NUM_BOSSES + 1] = {
    { BOSS_NONE,             "NO TWIST",         "STANDARD QINGDAO SHIFT" },
    { BOSS_BOTTLENECK,       "BOTTLENECK",       "ALL PATRONS ORDER BOTTLES" },
    { BOSS_TOUGH_CROWD,      "TOUGH CROWD",      "EVERY PATRON GETS +1 HP" },
    { BOSS_SLIPPERY_BAR,     "SLIPPERY BAR",     "EMPTIES SLIDE 32% FASTER" },
    { BOSS_BLACKOUT,         "BLACKOUT",         "POWER OUTAGE DIM LIGHTING" },
    { BOSS_NO_TIPS,          "NO TIPS",          "ZERO CASH TIPS THIS SHIFT" },
    { BOSS_BAG_FESTIVAL,     "BAG FESTIVAL",     "70% OF PATRONS ARE BAG VIPS" },
    { BOSS_DOUBLE_SHIFT,     "DOUBLE SHIFT",     "FAST WALKERS & FAST SPAWNS" },
    { BOSS_HEALTH_INSPECTOR, "HEALTH INSPECTOR", "2* SPILL FEE & ZERO FROST!" }
};

static const char *const k_drink_names[3] = {
    "TSINGTAO CAN",
    "GREEN BOTTLE",
    "BEER BAG"
};

static const char *const k_rarity_names[3] = {
    "COMMON",
    "UNCOMMON",
    "RARE"
};

/* =========================================================================
 * Run Initialization, Shift Target Curve & Boss Rotation
 * ========================================================================= */

static void shuffle_boss_subset(RunState *run)
{
    int i;
    for (i = 0; i < 7; i++) {
        run->boss_order[i] = (uint8_t)(i + 1);
    }
    for (i = 6; i > 0; i--) {
        int j = rl_rand_range(run, 0, i);
        uint8_t tmp = run->boss_order[i];
        run->boss_order[i] = run->boss_order[j];
        run->boss_order[j] = tmp;
    }
    run->boss_order[7] = (uint8_t)BOSS_HEALTH_INSPECTOR;
}

void rl_run_init(RunState *run, uint32_t seed, uint32_t high_score)
{
    int i;
    if (!run) return;

    memset(run, 0, sizeof(*run));
    run->night          = 1u;
    run->shift_in_night = (uint8_t)SHIFT_REGULAR;
    run->endless_mode   = 0u;
    run->won_night8     = 0u;
    run->lives          = 4;
    run->cash           = 4;
    run->run_score      = 0u;
    run->high_score     = high_score;
    run->served_total   = 0u;
    run->rng_state      = (seed != 0u) ? seed : 0x19960815u;

    for (i = 0; i < 3; i++) {
        run->drink_level[i] = 1u;
    }
    for (i = 0; i < RL_MAX_TRICKS; i++) {
        run->tricks[i] = (int8_t)TRICK_NONE;
    }

    shuffle_boss_subset(run);
}

uint32_t rl_shift_target(uint16_t night, uint8_t shift_in_night)
{
    /* Calibrated Night 1..8 targets + True Arcade Endless plateau (Night 9..12+) */
    static const uint32_t k_night_targets[12][3] = {
        {   1200u,   2800u,   5200u }, /* Night 1 (welcoming for any starter coaster) */
        {   9000u,  16000u,  26000u }, /* Night 2 */
        {  30000u,  48000u,  74000u }, /* Night 3 */
        {  72000u, 110000u, 165000u }, /* Night 4 */
        { 140000u, 205000u, 295000u }, /* Night 5 */
        { 225000u, 320000u, 445000u }, /* Night 6 */
        { 335000u, 465000u, 630000u }, /* Night 7 */
        { 460000u, 620000u, 820000u }, /* Night 8 */
        { 515000u, 690000u, 905000u }, /* Endless Night 9 */
        { 565000u, 750000u, 985000u }, /* Endless Night 10 */
        { 605000u, 800000u, 1050000u }, /* Endless Night 11 */
        { 640000u, 845000u, 1110000u }  /* Endless Night 12+ Plateau */
    };
    uint8_t s = (shift_in_night <= SHIFT_BOSS) ? shift_in_night : (uint8_t)SHIFT_BOSS;
    uint16_t n = (night >= 1u) ? night : 1u;

    if (n > 12u) {
        n = 12u;
    }
    return k_night_targets[n - 1u][s];
}

int16_t rl_shift_duration_frames(uint8_t shift_in_night)
{
    switch (shift_in_night) {
        case SHIFT_HAPPY_HOUR: return 2500; /* 50s @ 50Hz */
        case SHIFT_BOSS:       return 2750; /* 55s @ 50Hz */
        default:               return 2250; /* 45s @ 50Hz */
    }
}

BossType rl_night_boss(const RunState *run, uint16_t night)
{
    uint16_t n = (night >= 1u) ? night : 1u;
    if (!run) {
        return (n == RL_WIN_NIGHT) ? BOSS_HEALTH_INSPECTOR : (BossType)(((n - 1u) % 7u) + 1u);
    }
    if (n == RL_WIN_NIGHT) {
        return BOSS_HEALTH_INSPECTOR;
    }
    if (n < RL_WIN_NIGHT) {
        uint8_t b = run->boss_order[n - 1u];
        if (b >= 1u && b <= 7u) return (BossType)b;
        return (BossType)n;
    }
    /* Endless Night 9+: non-repeating shuffled 1..7 across 7-night cycles */
    {
        uint8_t idx = (uint8_t)((n - (RL_WIN_NIGHT + 1u)) % 7u);
        uint8_t b = run->boss_order[idx];
        if (b >= 1u && b <= 7u) return (BossType)b;
        return (BossType)(idx + 1u);
    }
}

BossType rl_current_boss(const RunState *run)
{
    if (!run || run->shift_in_night != (uint8_t)SHIFT_BOSS) {
        return BOSS_NONE;
    }
    return rl_night_boss(run, run->night);
}

void rl_advance_shift(RunState *run)
{
    if (!run) return;

    if (rl_has_coaster(run, COASTER_CLEAN_APRON) && run->shift_spills == 0u) {
        if (run->clean_apron_stacks < 20u) {
            run->clean_apron_stacks++;
        }
    }

    run->shift_spills     = 0u;
    run->empty_catch_buff = 0u;
    run->tip_buff_timer   = 0;

    if (run->shift_in_night < (uint8_t)SHIFT_BOSS) {
        run->shift_in_night++;
    } else {
        run->shift_in_night = (uint8_t)SHIFT_REGULAR;
        run->night++;
        /* Reshuffle Boss 1..7 at start of every 7-night Endless cycle */
        if (run->night > RL_WIN_NIGHT &&
            ((run->night - (RL_WIN_NIGHT + 1u)) % 7u) == 0u) {
            shuffle_boss_subset(run);
        }
    }
}

/* =========================================================================
 * Coaster, Trick & Boss Inventory Helpers
 * ========================================================================= */

int rl_has_coaster(const RunState *run, CoasterId id)
{
    uint8_t i;
    if (!run) return 0;
    for (i = 0; i < run->coaster_count && i < RL_MAX_COASTERS; i++) {
        if (run->coasters[i] == (uint8_t)id) {
            return 1;
        }
    }
    return 0;
}

int rl_add_coaster(RunState *run, CoasterId id)
{
    if (!run || (uint8_t)id >= RL_NUM_COASTERS) {
        return 0;
    }
    if (run->coaster_count >= RL_MAX_COASTERS) {
        return 0;
    }
    if (rl_has_coaster(run, id)) {
        return 0;
    }
    run->coasters[run->coaster_count]     = (uint8_t)id;
    run->coaster_ping[run->coaster_count] = 0u;
    run->coaster_count++;
    return 1;
}

int rl_add_trick(RunState *run, TrickId id)
{
    int i;
    if (!run || id < 0 || id >= RL_NUM_TRICKS) {
        return 0;
    }
    for (i = 0; i < RL_MAX_TRICKS; i++) {
        if (run->tricks[i] == (int8_t)TRICK_NONE) {
            run->tricks[i] = (int8_t)id;
            return 1;
        }
    }
    return 0;
}

CoasterId rl_roll_rare_coaster(RunState *run)
{
    uint8_t pool[RL_NUM_COASTERS];
    int count = 0;
    int i;

    for (i = 0; i < RL_NUM_COASTERS; i++) {
        if (k_coasters[i].rarity == RARITY_RARE && !rl_has_coaster(run, (CoasterId)i)) {
            pool[count++] = (uint8_t)i;
        }
    }
    if (count == 0) {
        for (i = 0; i < RL_NUM_COASTERS; i++) {
            if (!rl_has_coaster(run, (CoasterId)i)) {
                pool[count++] = (uint8_t)i;
            }
        }
    }
    if (count == 0) {
        return COASTER_GOLDEN_TAP;
    }
    return (CoasterId)pool[rl_rand_range(run, 0, count - 1)];
}

const CoasterDef *rl_coaster_def(uint8_t id)
{
    if (id >= RL_NUM_COASTERS) {
        return &k_coasters[0];
    }
    return &k_coasters[id];
}

const TrickDef *rl_trick_def(int8_t id)
{
    if (id < 0 || id >= RL_NUM_TRICKS) {
        return NULL;
    }
    return &k_tricks[id];
}

const BossDef *rl_boss_def(uint8_t id)
{
    if (id > RL_NUM_BOSSES) {
        return &k_bosses[0];
    }
    return &k_bosses[id];
}

/* =========================================================================
 * Serve Scoring Pipeline (Left-to-Right Coasters, Pure uint64_t Math)
 * ========================================================================= */

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
                    ServeScoreBreakdown *out)
{
    static const uint32_t k_base0[3] = { 40u, 70u, 85u };
    static const uint32_t k_step[3]  = { 25u, 35u, 40u };
    uint8_t dt = (drink_type < 3u) ? drink_type : 0u;
    uint8_t lv;
    uint64_t base;
    uint64_t mult_x10;
    uint8_t triggered_mask = 0u;
    uint8_t splash_triggered = 0u;
    const char *label = k_drink_names[dt];
    int drink_differs = 0;
    int is_trio = 0;
    int trio_consumed = 0;
    int juggler_consumed = 0;
    int ping_count = 0;
    uint8_t i;

    if (!run || !out) return;

    lv = run->drink_level[dt];
    if (lv < 1u) lv = 1u;
    if (lv > 9u) lv = 9u;

    base = (uint64_t)k_base0[dt] + (uint64_t)(lv - 1u) * (uint64_t)k_step[dt];
    mult_x10 = (uint64_t)lv * 10u;

    /* Modest combo streak contribution (+0.1x per step above 1, capped at +1.5x) */
    if (combo > 1u) {
        uint64_t cbonus = (uint64_t)(combo - 1u);
        if (cbonus > 15u) cbonus = 15u;
        mult_x10 += cbonus;
    }

    /* Evaluate sequential drink history before appending current serve */
    if (run->last_drink_count > 0u &&
        run->last_drinks[run->last_drink_count - 1u] != dt) {
        drink_differs = 1;
    }
    if (run->last_drink_count >= 2u) {
        uint8_t a = run->last_drinks[run->last_drink_count - 2u];
        uint8_t b = run->last_drinks[run->last_drink_count - 1u];
        uint8_t mask = (uint8_t)((1u << (a & 3u)) | (1u << (b & 3u)) | (1u << (dt & 3u)));
        if (mask == 0x07u) {
            is_trio = 1;
        }
    }

    /* Left-to-right Coaster execution (no Coaster can retrigger another) */
    for (i = 0; i < run->coaster_count && i < RL_MAX_COASTERS; i++) {
        uint8_t cid = run->coasters[i];
        uint64_t add_base = 0u;
        uint64_t add_mult_x10 = 0u;
        uint64_t mul_mult_x10 = 10u;
        int fired = 0;

        switch (cid) {
            case COASTER_COLD_FOAM:
                add_base = 30u;
                fired = 1;
                break;

            case COASTER_GREEN_GLASS:
                if (dt == 1u) { /* DRINK_BOTTLE */
                    add_base = 40u;
                    add_mult_x10 = 15u;
                    fired = 1;
                }
                break;

            case COASTER_PLASTIC_HUSTLE:
                if (dt == 2u) { /* DRINK_BAG */
                    add_base = 45u;
                    add_mult_x10 = 15u;
                    fired = 1;
                }
                break;

            case COASTER_TAP_RHYTHM:
                if (combo > 0u) {
                    add_mult_x10 = (uint64_t)combo * 3u;
                    fired = 1;
                }
                break;

            case COASTER_MIXTAPE_POUR:
                if (drink_differs) {
                    add_mult_x10 = 25u;
                    fired = 1;
                }
                break;

            case COASTER_GLASS_JUGGLER:
                if (run->empty_catch_buff > 0u) {
                    add_base = 80u;
                    add_mult_x10 = 20u;
                    juggler_consumed = 1;
                    fired = 1;
                }
                break;

            case COASTER_TIP_JAR:
                if (run->tip_buff_timer > 0) {
                    add_mult_x10 = 30u;
                    fired = 1;
                }
                break;

            case COASTER_TOP_SHELF:
                if (lane == 0u) {
                    mul_mult_x10 = 20u; /* x2.0 */
                    fired = 1;
                }
                break;

            case COASTER_STREET_SIDE:
                if (lane == 3u) {
                    add_base = 60u;
                    add_mult_x10 = 15u;
                    fired = 1;
                }
                break;

            case COASTER_BIG_APPETITE:
                if (cust_type == 1u || cust_hp_before > 1u) {
                    mul_mult_x10 = 18u; /* x1.8 */
                    fired = 1;
                    label = "BIG APPETITE!";
                }
                break;

            case COASTER_QINGDAO_TRIO:
                if (is_trio) {
                    mul_mult_x10 = 30u; /* x3.0 */
                    trio_consumed = 1;
                    fired = 1;
                    label = "QINGDAO TRIO!";
                }
                break;

            case COASTER_VIP_VELVET:
                if (cust_type == 2u || cust_type == 3u) {
                    mul_mult_x10 = 20u; /* x2.0 */
                    fired = 1;
                    label = "VIP VELVET!";
                }
                break;

            case COASTER_FROST_BITE:
                if (frost >= 75 || is_rush != 0u) {
                    mul_mult_x10 = 20u; /* x2.0 */
                } else {
                    add_base = 30u;
                }
                fired = 1;
                break;

            case COASTER_CLEAN_APRON:
                if (run->clean_apron_stacks > 0u) {
                    add_mult_x10 = (uint64_t)run->clean_apron_stacks * 30u;
                    fired = 1;
                }
                break;

            case COASTER_PIGGY_BANK:
            case COASTER_SPEED_SNEAKERS:
                /* Passive economy & mobility coasters */
                break;

            case COASTER_GOLDEN_TAP:
                if (run->cash >= 5) {
                    add_mult_x10 = (uint64_t)(run->cash / 5) * 15u;
                    fired = 1;
                }
                break;

            case COASTER_ICE_KING:
                if (is_rush != 0u) {
                    mul_mult_x10 = 25u; /* x2.5 */
                    fired = 1;
                }
                break;

            case COASTER_BAG_SPLASH:
                if (dt == 2u && adjacent_occupied != 0u) {
                    add_base = 30u;
                    splash_triggered = 1u;
                    fired = 1;
                    label = "BAG SPLASH!";
                }
                break;

            case COASTER_GLASS_CANNON:
                mul_mult_x10 = 30u; /* x3.0 */
                fired = 1;
                label = "GLASS CANNON!";
                break;

            default:
                break;
        }

        if (fired) {
            /* Strict execution order per Coaster: +Base -> +Mult -> xMult */
            base += add_base;
            if (base > 100000000ull) base = 100000000ull;

            mult_x10 += add_mult_x10;
            if (mult_x10 > 100000000ull) mult_x10 = 100000000ull;

            if (mul_mult_x10 != 10u) {
                mult_x10 = (mult_x10 * mul_mult_x10) / 10ull;
                if (mult_x10 > 100000000ull) mult_x10 = 100000000ull;
            }

            triggered_mask |= (uint8_t)(1u << i);
            run->coaster_ping[i] = 16u;

            {
                int pitch = ping_count * 160;
                if (pitch > 1200) pitch = 1200;
                audio_play_sfx(SFX_MULT_PING, 160, pitch);
            }
            ping_count++;
        }
    }

    if (juggler_consumed) {
        run->empty_catch_buff = 0u;
    }

    /* Update rolling last-3 drinks history */
    if (trio_consumed) {
        run->last_drinks[0]   = dt;
        run->last_drink_count = 1u;
    } else if (run->last_drink_count < 3u) {
        run->last_drinks[run->last_drink_count++] = dt;
    } else {
        run->last_drinks[0] = run->last_drinks[1];
        run->last_drinks[1] = run->last_drinks[2];
        run->last_drinks[2] = dt;
    }

    if (happy_bell_active != 0u) {
        mult_x10 *= 2ull;
        if (mult_x10 > 100000000ull) mult_x10 = 100000000ull;
    }

    if (is_rush != 0u) {
        label = "ICE-COLD RUSH!";
    }

    {
        uint64_t total = (base * mult_x10) / 10ull;
        if (total > (uint64_t)RL_SERVE_CAP) {
            total = (uint64_t)RL_SERVE_CAP;
        }
        out->base             = (base > (uint64_t)RL_SERVE_CAP) ? RL_SERVE_CAP : (uint32_t)base;
        out->mult_x10         = (mult_x10 > 99999999u) ? 99999999u : (uint32_t)mult_x10;
        out->total_pts        = (uint32_t)total;
        out->triggered_mask   = triggered_mask;
        out->splash_triggered = splash_triggered;
        out->style_label      = label;
    }
}

void rl_on_empty_caught(RunState *run)
{
    if (!run) return;
    if (rl_has_coaster(run, COASTER_GLASS_JUGGLER)) {
        run->empty_catch_buff = 1u;
    }
}

void rl_on_tip_collected(RunState *run)
{
    if (!run) return;
    if (rl_has_coaster(run, COASTER_TIP_JAR)) {
        run->tip_buff_timer = 350; /* 7 seconds @ 50Hz */
    }
}

void rl_on_spill(RunState *run)
{
    if (!run) return;
    run->shift_spills++;
    run->clean_apron_stacks = 0u;
}

void rl_tick_timers(RunState *run)
{
    uint8_t i;
    if (!run) return;
    for (i = 0; i < RL_MAX_COASTERS; i++) {
        if (run->coaster_ping[i] > 0u) {
            run->coaster_ping[i]--;
        }
    }
    if (run->tip_buff_timer > 0) {
        run->tip_buff_timer--;
    }
}

/* =========================================================================
 * Starter Draft UI (3D Cards over Qingdao Harbor)
 * ========================================================================= */

void rl_draft_init(DraftState *ds, RunState *run)
{
    uint8_t pool[RL_NUM_COASTERS];
    int pool_len = 0;
    int i;

    if (!ds || !run) return;

    memset(ds, 0, sizeof(*ds));
    ds->cursor = 0;
    ds->picked = -1;

    /* Build starter pool from Common & Uncommon coasters (skip Glass Cannon for fair N1 start) */
    for (i = 0; i < RL_NUM_COASTERS; i++) {
        if (i == (int)COASTER_GLASS_CANNON) continue;
        if (k_coasters[i].rarity <= RARITY_UNCOMMON && !rl_has_coaster(run, (CoasterId)i)) {
            pool[pool_len++] = (uint8_t)i;
        }
    }

    /* Fisher-Yates pick 3 distinct choices */
    for (i = 0; i < 3 && pool_len > 0; i++) {
        int idx = rl_rand_range(run, 0, pool_len - 1);
        ds->choices[i] = pool[idx];
        pool[idx] = pool[pool_len - 1];
        pool_len--;
    }
}

int rl_draft_update(DraftState *ds, RunState *run)
{
    if (!ds || !run) return 1;

    if (ds->flip_timer < 30000) {
        ds->flip_timer++;
    }
    if (ds->picked >= 0) {
        return 1;
    }

    if (input_pressed(BTN_LEFT)) {
        ds->cursor = (int8_t)((ds->cursor + 2) % 3);
        audio_play_sfx(SFX_CARD_FLIP, 110 + ds->cursor * 50, 40);
    } else if (input_pressed(BTN_RIGHT)) {
        ds->cursor = (int8_t)((ds->cursor + 1) % 3);
        audio_play_sfx(SFX_CARD_FLIP, 110 + ds->cursor * 50, 80);
    } else if (ds->flip_timer >= 18 &&
               (input_pressed(BTN_CROSS) || input_pressed(BTN_START))) {
        ds->picked = ds->cursor;
        rl_add_coaster(run, (CoasterId)ds->choices[ds->picked]);
        audio_play_sfx(SFX_SHOP_BUY, 160, 0);
        return 1;
    }
    return 0;
}

void rl_draft_draw(const DraftState *ds, const RunState *run)
{
    static const int16_t k_cx[3] = { 58, 160, 262 };
    uint32_t tick = gfx_frame_count();
    const CoasterDef *sel;
    char line[40];
    char *p;
    int i;

    if (!ds || !run) return;

    gfx_set_shake_apply(0);
    room3d_draw_barroom(run->night, tick, -1, 0, 0, 0);

    /* Top Title Banner inside PAL safe margins (y >= 8) */
    gfx_draw_rect(GFX_Z_HUD_BG, 24, 10, 272, 28, 10, 16, 30, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 24, 10, 272, 1, 255, 224, 90, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 24, 37, 272, 1, 80, 220, 160, GFX_TRANS_NONE);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 14, "STARTER COASTER DRAFT", 255, 236, 96);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 26, "CHOOSE 1 FOR NIGHT 1 SHIFT", 150, 240, 255);

    /* 3 Floating 3D Holographic Coaster Cards */
    for (i = 0; i < 3; i++) {
        const CoasterDef *def = rl_coaster_def(ds->choices[i]);
        int selected = (i == (int)ds->cursor);
        int local_t = (int)ds->flip_timer - i * 5;
        int yaw = 0;
        int pitch = 0;
        int cy = selected ? 92 : 98;
        uint8_t rcol = (def->rarity == RARITY_RARE) ? 255 :
                       (def->rarity == RARITY_UNCOMMON ? 120 : 210);
        uint8_t gcol = (def->rarity == RARITY_RARE) ? 215 :
                       (def->rarity == RARITY_UNCOMMON ? 255 : 225);
        uint8_t bcol = (def->rarity == RARITY_RARE) ? 90 :
                       (def->rarity == RARITY_UNCOMMON ? 160 : 240);

        if (local_t < 0) {
            yaw = 180;
        } else if (local_t < 12) {
            yaw = 180 - (local_t * 180) / 12;
        } else if (selected) {
            yaw   = integer_osc(tick + (uint32_t)(i * 19u), 90u, 14);
            pitch = integer_osc(tick + 23u, 110u, 8);
        }

        room3d_draw_card(GFX_Z_POPUP, k_cx[i], cy, 52, 70,
                         yaw, pitch, (int)def->rarity,
                         CARD_KIND_COASTER, (int)def->id,
                         selected, selected || (def->rarity == RARITY_RARE), tick);

        if (local_t >= 6) {
            int tag_w = (int)strlen(k_rarity_names[def->rarity]) * 8;
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, k_cx[i] - tag_w / 2, 138,
                                 k_rarity_names[def->rarity], rcol, gcol, bcol);
        }
    }

    /* Highlighted Coaster Inspection Box */
    sel = rl_coaster_def(ds->choices[ds->cursor]);
    gfx_draw_rect(GFX_Z_HUD_BG, 20, 156, 280, 58, 12, 20, 36, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 20, 156, 280, 1, 255, 224, 90, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 20, 213, 280, 1, 255, 224, 90, GFX_TRANS_NONE);

    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 165, sel->name, 255, 240, 100);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 181, sel->desc, 225, 245, 255);

    p = line;
    p = buf_append_str(p, line + sizeof(line) - 1, "RARITY: ");
    p = buf_append_str(p, line + sizeof(line) - 1, k_rarity_names[sel->rarity]);
    p = buf_append_str(p, line + sizeof(line) - 1, "   VALUE: $");
    p = buf_append_uint(p, line + sizeof(line) - 1, sel->cost);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 198, line, 160, 255, 175);

    /* Safe-zone Bottom Prompt (y = 228..244) */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 228, GFX_SCREEN_W, 18, 10, 14, 24, GFX_TRANS_HALF);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 16, 230, GLYPH_DPAD);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 33, 233, "CHOOSE", 190, 225, 255);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 180, 230, GLYPH_CROSS);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 197, 233, "EQUIP COASTER", 160, 255, 170);
}

/* =========================================================================
 * Shift Payout Ledger UI
 * ========================================================================= */

void rl_payout_init(PayoutState *ps, RunState *run,
                    uint32_t shift_score, uint32_t shift_target,
                    int16_t tips_earned)
{
    int has_piggy;
    int16_t cash_for_int;
    int16_t int_cap;

    if (!ps || !run) return;

    memset(ps, 0, sizeof(*ps));
    has_piggy = rl_has_coaster(run, COASTER_PIGGY_BANK);

    ps->base_pay = (int16_t)(4 + (int16_t)run->shift_in_night + (has_piggy ? 1 : 0));
    ps->lives_bonus = (run->lives >= 3) ? 1 : 0;

    cash_for_int = (run->cash > 0) ? run->cash : 0;
    int_cap = has_piggy ? 8 : 5;
    ps->interest = (int16_t)(cash_for_int / 5);
    if (ps->interest > int_cap) {
        ps->interest = int_cap;
    }

    ps->overkill_bonus = ((uint64_t)shift_score >= (uint64_t)shift_target * 2ull) ? 2 : 0;

    /* Tips were already collected into run->cash during shift; display only, NEVER double-count */
    ps->tips_earned  = (tips_earned > 0) ? tips_earned : 0;
    ps->total_cash   = (int16_t)(ps->base_pay + ps->lives_bonus + ps->interest + ps->overkill_bonus);
    ps->shift_score  = shift_score;
    ps->shift_target = shift_target;
    ps->timer        = 0u;
    ps->step         = 0u;

    /* Credit shift payout exactly once */
    run->cash = (int16_t)(run->cash + ps->total_cash);
    if (run->cash < 0) run->cash = 0;
}

int rl_payout_update(PayoutState *ps, RunState *run)
{
    if (!ps || !run) return 1;

    ps->timer++;
    if (ps->step < 5u && (ps->timer % 16u) == 0u) {
        ps->step++;
        if (ps->step == 5u) {
            audio_play_sfx(SFX_SHOP_BUY, 160, 0);
        } else {
            audio_play_sfx(SFX_TIP_PICKUP, 160, (int)ps->step * 80);
        }
    }

    if (ps->timer >= 10u && (input_pressed(BTN_CROSS) || input_pressed(BTN_START))) {
        if (ps->step < 5u) {
            ps->step = 5u;
            audio_play_sfx(SFX_SHOP_BUY, 160, 0);
        } else {
            audio_play_sfx(SFX_EMPTY_CATCH, 160, 0);
            return 1;
        }
    }
    return 0;
}

static void draw_payout_row(int y, const char *label, int16_t cash_val,
                            int in_parens, uint8_t r, uint8_t g, uint8_t b)
{
    char val[16];
    char *p = val;

    if (in_parens) {
        *p++ = '(';
    } else if (cash_val > 0) {
        *p++ = '+';
    }
    *p++ = '$';
    p = buf_append_int(p, val + sizeof(val) - 2, (int32_t)cash_val);
    if (in_parens) {
        *p++ = ')';
    }
    *p = '\0';

    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 42, y, label, r, g, b);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 278 - (int)strlen(val) * 8, y, val, r, g, b);
}

void rl_payout_draw(const PayoutState *ps, const RunState *run)
{
    uint32_t tick = gfx_frame_count();
    char buf[40];
    char *p;
    int is_boss;

    if (!ps || !run) return;
    is_boss = (run->shift_in_night == (uint8_t)SHIFT_BOSS);

    gfx_set_shake_apply(0);
    room3d_draw_barroom(run->night, tick, -1, 0, 0, 0);

    gfx_draw_rect(GFX_Z_HUD_BG, 24, 18, 272, 218, 12, 18, 34, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 24, 18, 272, 2, 255, 224, 90, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 24, 234, 272, 2, 80, 230, 150, GFX_TRANS_NONE);

    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 26,
                           is_boss ? "BOSS SHIFT DEFEATED!" : "SHIFT CLEARED!",
                           255, 236, 96);

    p = buf;
    p = buf_append_str(p, buf + sizeof(buf) - 1, "SCORE ");
    p = buf_append_uint(p, buf + sizeof(buf) - 1, ps->shift_score);
    p = buf_append_str(p, buf + sizeof(buf) - 1, " / TGT ");
    p = buf_append_uint(p, buf + sizeof(buf) - 1, ps->shift_target);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 42, buf, 160, 245, 255);

    gfx_draw_rect(GFX_Z_HUD_BG - 1, 38, 56, 244, 1, 70, 110, 150, GFX_TRANS_NONE);

    draw_payout_row(64, "BASE SHIFT PAY", ps->base_pay, 0, 220, 235, 250);
    if (ps->step >= 1u) {
        draw_payout_row(82, "FULL LIVES BONUS", ps->lives_bonus, 0,
                        ps->lives_bonus > 0 ? 150 : 140,
                        ps->lives_bonus > 0 ? 255 : 150,
                        ps->lives_bonus > 0 ? 175 : 165);
    }
    if (ps->step >= 2u) {
        draw_payout_row(100, "INTEREST ($1/$5)", ps->interest, 0,
                        ps->interest > 0 ? 255 : 140,
                        ps->interest > 0 ? 230 : 150,
                        ps->interest > 0 ? 110 : 165);
    }
    if (ps->step >= 3u) {
        draw_payout_row(118, "2* OVERKILL BONUS", ps->overkill_bonus, 0,
                        ps->overkill_bonus > 0 ? 255 : 140,
                        ps->overkill_bonus > 0 ? 190 : 150,
                        ps->overkill_bonus > 0 ? 90 : 165);
    }
    if (ps->step >= 4u) {
        draw_payout_row(136, "SHIFT TIPS BANKED", ps->tips_earned, 1, 165, 215, 175);
    }

    gfx_draw_rect(GFX_Z_HUD_BG - 1, 38, 152, 244, 1, 255, 224, 90, GFX_TRANS_NONE);

    if (ps->step >= 5u) {
        p = buf;
        p = buf_append_str(p, buf + sizeof(buf) - 1, "CASH OUT: +$");
        p = buf_append_int(p, buf + sizeof(buf) - 1, (int32_t)ps->total_cash);
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 162, buf, 140, 255, 160);

        p = buf;
        p = buf_append_str(p, buf + sizeof(buf) - 1, "TOTAL BANK: $");
        p = buf_append_int(p, buf + sizeof(buf) - 1, (int32_t)run->cash);
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 180, buf, 255, 240, 110);

        if ((tick & 16u) == 0u) {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 212,
                                   is_boss ? "PRESS X: BONUS ROUND!" : "PRESS X: ENTER SHOP",
                                   255, 255, 255);
        }
    } else {
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 212, "PRESS X TO SKIP", 180, 200, 220);
    }
}

/* =========================================================================
 * Night-Market Shop UI
 * ========================================================================= */

static void shop_set_toast(ShopState *ss, const char *msg)
{
    size_t i = 0;
    if (!ss || !msg) return;
    while (msg[i] && i + 1u < sizeof(ss->toast)) {
        ss->toast[i] = msg[i];
        i++;
    }
    ss->toast[i] = '\0';
    ss->toast_timer = 90;
}

static uint8_t shop_roll_coaster(RunState *run, int exclude_id)
{
    uint8_t pool[RL_NUM_COASTERS];
    int count = 0;
    int roll = rl_rand_range(run, 0, 99);
    uint8_t target_rarity;
    int i;

    /* Rarity odds rise with Night progression */
    if (run->night <= 2u) {
        target_rarity = (roll < 62) ? RARITY_COMMON : ((roll < 90) ? RARITY_UNCOMMON : RARITY_RARE);
    } else {
        target_rarity = (roll < 42) ? RARITY_COMMON : ((roll < 78) ? RARITY_UNCOMMON : RARITY_RARE);
    }

    for (i = 0; i < RL_NUM_COASTERS; i++) {
        if (i == exclude_id) continue;
        if (k_coasters[i].rarity == target_rarity && !rl_has_coaster(run, (CoasterId)i)) {
            pool[count++] = (uint8_t)i;
        }
    }
    if (count == 0) {
        for (i = 0; i < RL_NUM_COASTERS; i++) {
            if (i == exclude_id) continue;
            if (!rl_has_coaster(run, (CoasterId)i)) {
                pool[count++] = (uint8_t)i;
            }
        }
    }
    if (count == 0) {
        return 0u;
    }
    return pool[rl_rand_range(run, 0, count - 1)];
}

static void shop_roll_offers(ShopState *ss, RunState *run)
{
    uint8_t dpool[3];
    int dcnt = 0;
    int i;

    ss->coaster_offers[0] = shop_roll_coaster(run, -1);
    ss->coaster_offers[1] = shop_roll_coaster(run, (int)ss->coaster_offers[0]);
    ss->coaster_sold[0]   = 0u;
    ss->coaster_sold[1]   = 0u;

    for (i = 0; i < 3; i++) {
        if (run->drink_level[i] < 9u) {
            dpool[dcnt++] = (uint8_t)i;
        }
    }
    if (dcnt > 0) {
        ss->drink_offer = dpool[rl_rand_range(run, 0, dcnt - 1)];
        ss->drink_sold  = 0u;
    } else {
        ss->drink_offer = 0u;
        ss->drink_sold  = 1u;
    }

    ss->trick_offer = (int8_t)rl_rand_range(run, 0, RL_NUM_TRICKS - 1);
    ss->trick_sold  = 0u;

    for (i = 0; i < 4; i++) {
        ss->card_flip[i] = 0;
    }
}

static int16_t drink_upgrade_cost(uint8_t current_lv)
{
    (void)current_lv;
    return 3;
}

void rl_shop_init(ShopState *ss, RunState *run)
{
    if (!ss || !run) return;
    memset(ss, 0, sizeof(*ss));
    ss->reroll_cost = 3;
    ss->zone        = 1;
    ss->cursor      = 0;
    shop_roll_offers(ss, run);
}

static void shop_next_shift_info(const RunState *run,
                                 uint16_t *out_night,
                                 uint8_t *out_shift,
                                 uint32_t *out_target,
                                 BossType *out_boss)
{
    uint16_t nn = run->night;
    uint8_t ns = (uint8_t)(run->shift_in_night + 1u);
    if (ns > (uint8_t)SHIFT_BOSS) {
        ns = (uint8_t)SHIFT_REGULAR;
        nn++;
    }
    *out_night  = nn;
    *out_shift  = ns;
    *out_target = rl_shift_target(nn, ns);
    *out_boss   = (ns == (uint8_t)SHIFT_BOSS) ? rl_night_boss(run, nn) : BOSS_NONE;
}

int rl_shop_update(ShopState *ss, RunState *run)
{
    int i;
    if (!ss || !run) return 1;

    rl_tick_timers(run);
    for (i = 0; i < 4; i++) {
        if (ss->card_flip[i] < 32) {
            ss->card_flip[i]++;
        }
    }
    if (ss->toast_timer > 0) {
        ss->toast_timer--;
    }

    if (ss->zone == 0) {
        /* Zone 0: Equipped Coaster Tray (reorder L1/R1, sell Square) */
        if (run->coaster_count == 0u) {
            ss->zone   = 1;
            ss->cursor = 0;
            return 0;
        }
        if (ss->cursor >= (int8_t)run->coaster_count) {
            ss->cursor = (int8_t)(run->coaster_count - 1u);
        }
        if (input_pressed(BTN_LEFT) && ss->cursor > 0) {
            ss->cursor--;
            audio_play_sfx(SFX_EMPTY_CATCH, 140, 40);
        } else if (input_pressed(BTN_RIGHT) && ss->cursor + 1 < (int8_t)run->coaster_count) {
            ss->cursor++;
            audio_play_sfx(SFX_EMPTY_CATCH, 180, 40);
        } else if (input_pressed(BTN_DOWN)) {
            ss->zone = 1;
            if (ss->cursor > 3) ss->cursor = 3;
            audio_play_sfx(SFX_EMPTY_CATCH, 160, -20);
        } else if (input_pressed(BTN_L1) && ss->cursor > 0) {
            uint8_t idx = (uint8_t)ss->cursor;
            uint8_t tmp = run->coasters[idx - 1u];
            run->coasters[idx - 1u] = run->coasters[idx];
            run->coasters[idx]      = tmp;
            ss->cursor--;
            audio_play_sfx(SFX_CARD_FLIP, 150, 60);
            shop_set_toast(ss, "COASTER MOVED LEFT");
        } else if (input_pressed(BTN_R1) && ss->cursor + 1 < (int8_t)run->coaster_count) {
            uint8_t idx = (uint8_t)ss->cursor;
            uint8_t tmp = run->coasters[idx + 1u];
            run->coasters[idx + 1u] = run->coasters[idx];
            run->coasters[idx]      = tmp;
            ss->cursor++;
            audio_play_sfx(SFX_CARD_FLIP, 170, 60);
            shop_set_toast(ss, "COASTER MOVED RIGHT");
        } else if (input_pressed(BTN_SQUARE)) {
            uint8_t idx = (uint8_t)ss->cursor;
            const CoasterDef *def = rl_coaster_def(run->coasters[idx]);
            int16_t sell_val = (int16_t)(def->cost / 2u);
            uint8_t j;
            char msg[36];
            char *p;

            if (sell_val < 1) sell_val = 1;
            run->cash = (int16_t)(run->cash + sell_val);

            for (j = idx; j + 1u < run->coaster_count; j++) {
                run->coasters[j]     = run->coasters[j + 1u];
                run->coaster_ping[j] = run->coaster_ping[j + 1u];
            }
            run->coaster_count--;
            run->coasters[run->coaster_count]     = 0u;
            run->coaster_ping[run->coaster_count] = 0u;

            if (run->coaster_count == 0u) {
                ss->zone   = 1;
                ss->cursor = 0;
            } else if (ss->cursor >= (int8_t)run->coaster_count) {
                ss->cursor = (int8_t)(run->coaster_count - 1u);
            }
            audio_play_sfx(SFX_SHOP_BUY, 160, -80);

            p = msg;
            p = buf_append_str(p, msg + sizeof(msg) - 1, "SOLD FOR $");
            p = buf_append_int(p, msg + sizeof(msg) - 1, (int32_t)sell_val);
            shop_set_toast(ss, msg);
        }
    } else if (ss->zone == 1) {
        /* Zone 1: 4 Shop Offers (2 Coasters, 1 Drink Upgrade, 1 Pocket Trick) */
        if (input_pressed(BTN_LEFT)) {
            ss->cursor = (int8_t)((ss->cursor + 3) % 4);
            audio_play_sfx(SFX_EMPTY_CATCH, 140, 20);
        } else if (input_pressed(BTN_RIGHT)) {
            ss->cursor = (int8_t)((ss->cursor + 1) % 4);
            audio_play_sfx(SFX_EMPTY_CATCH, 180, 20);
        } else if (input_pressed(BTN_UP)) {
            if (run->coaster_count > 0u) {
                ss->zone = 0;
                if (ss->cursor >= (int8_t)run->coaster_count) {
                    ss->cursor = (int8_t)(run->coaster_count - 1u);
                }
                audio_play_sfx(SFX_EMPTY_CATCH, 160, 40);
            }
        } else if (input_pressed(BTN_DOWN)) {
            ss->zone   = 2;
            ss->cursor = (ss->cursor >= 2) ? 1 : 0;
            audio_play_sfx(SFX_EMPTY_CATCH, 160, -20);
        } else if (input_pressed(BTN_CROSS)) {
            if (ss->cursor == 0 || ss->cursor == 1) {
                int slot = (int)ss->cursor;
                const CoasterDef *def = rl_coaster_def(ss->coaster_offers[slot]);
                if (ss->coaster_sold[slot]) {
                    shop_set_toast(ss, "ALREADY SOLD!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else if (run->coaster_count >= RL_MAX_COASTERS) {
                    shop_set_toast(ss, "TRAY FULL! SELL ONE FIRST");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else if (rl_has_coaster(run, (CoasterId)def->id)) {
                    shop_set_toast(ss, "ALREADY EQUIPPED!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else if (run->cash < (int16_t)def->cost) {
                    shop_set_toast(ss, "NOT ENOUGH CASH!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else {
                    run->cash = (int16_t)(run->cash - (int16_t)def->cost);
                    rl_add_coaster(run, (CoasterId)def->id);
                    ss->coaster_sold[slot] = 1u;
                    audio_play_sfx(SFX_SHOP_BUY, 160, 40);
                    shop_set_toast(ss, "COASTER EQUIPPED!");
                }
            } else if (ss->cursor == 2) {
                uint8_t d = ss->drink_offer;
                uint8_t cur_lv = (d < 3u) ? run->drink_level[d] : 9u;
                int16_t cost = drink_upgrade_cost(cur_lv);
                if (ss->drink_sold || cur_lv >= 9u) {
                    shop_set_toast(ss, cur_lv >= 9u ? "DRINK AT MAX LV.9!" : "ALREADY SOLD!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else if (run->cash < cost) {
                    shop_set_toast(ss, "NOT ENOUGH CASH!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else {
                    run->cash = (int16_t)(run->cash - cost);
                    run->drink_level[d]++;
                    ss->drink_sold = 1u;
                    audio_play_sfx(SFX_SHOP_BUY, 160, 100);
                    shop_set_toast(ss, "RECIPE UPGRADED!");
                }
            } else {
                const TrickDef *tdef = rl_trick_def(ss->trick_offer);
                if (ss->trick_sold || !tdef) {
                    shop_set_toast(ss, "ALREADY SOLD!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else if (run->tricks[0] != (int8_t)TRICK_NONE &&
                           run->tricks[1] != (int8_t)TRICK_NONE) {
                    shop_set_toast(ss, "TRICK SLOTS FULL (MAX 2)!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else if (run->cash < (int16_t)tdef->cost) {
                    shop_set_toast(ss, "NOT ENOUGH CASH!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else {
                    run->cash = (int16_t)(run->cash - (int16_t)tdef->cost);
                    rl_add_trick(run, (TrickId)tdef->id);
                    ss->trick_sold = 1u;
                    audio_play_sfx(SFX_SHOP_BUY, 160, 140);
                    shop_set_toast(ss, "POCKET TRICK READY!");
                }
            }
        }
    } else {
        /* Zone 2: [0] Reroll / [1] Next Shift */
        if (input_pressed(BTN_LEFT) || input_pressed(BTN_RIGHT)) {
            ss->cursor = (int8_t)(ss->cursor ^ 1);
            audio_play_sfx(SFX_EMPTY_CATCH, 160, 20);
        } else if (input_pressed(BTN_UP)) {
            ss->zone   = 1;
            ss->cursor = (ss->cursor == 0) ? 0 : 3;
            audio_play_sfx(SFX_EMPTY_CATCH, 160, 40);
        } else if (input_pressed(BTN_CROSS)) {
            if (ss->cursor == 0) {
                if (run->cash < ss->reroll_cost) {
                    shop_set_toast(ss, "NOT ENOUGH CASH FOR REROLL!");
                    audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
                } else {
                    run->cash = (int16_t)(run->cash - ss->reroll_cost);
                    ss->reroll_cost++;
                    shop_roll_offers(ss, run);
                    audio_play_sfx(SFX_CARD_FLIP, 160, 0);
                    shop_set_toast(ss, "NEW STOCK DELIVERED!");
                }
            } else {
                audio_play_sfx(SFX_CAN_POP, 160, 60);
                return 1;
            }
        }
    }

    if (input_pressed(BTN_START)) {
        audio_play_sfx(SFX_CAN_POP, 160, 60);
        return 1;
    }

    return 0;
}

void rl_shop_draw(const ShopState *ss, const RunState *run)
{
    static const int16_t k_offer_cx[4] = { 44, 118, 198, 274 };
    uint32_t tick = gfx_frame_count();
    uint16_t next_night;
    uint8_t  next_shift;
    uint32_t next_target;
    BossType next_boss;
    char buf[44];
    char *p;
    int i;

    if (!ss || !run) return;

    shop_next_shift_info(run, &next_night, &next_shift, &next_target, &next_boss);

    gfx_set_shake_apply(0);
    room3d_draw_barroom(run->night, tick, -1, 0, 0, 0);

    /* 1. Top Header Bar (y = 8..20) */
    gfx_draw_rect(GFX_Z_HUD_BG, 8, 8, 304, 13, 10, 16, 30, GFX_TRANS_HALF);
    p = buf;
    p = buf_append_str(p, buf + sizeof(buf) - 1, "NIGHT ");
    p = buf_append_uint(p, buf + sizeof(buf) - 1, run->night);
    p = buf_append_str(p, buf + sizeof(buf) - 1, " SHOP");
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 14, 11, buf, 255, 236, 96);

    p = buf;
    p = buf_append_str(p, buf + sizeof(buf) - 1, "BANK: $");
    p = buf_append_int(p, buf + sizeof(buf) - 1, (int32_t)run->cash);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 128, 11, buf, 140, 255, 165);

    for (i = 0; i < (int)run->lives && i < 5; i++) {
        gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_ENV_UI,
                            SPR_HUD_LIFE_CAN_U, SPR_HUD_LIFE_CAN_V,
                            SPR_HUD_LIFE_CAN_W, SPR_HUD_LIFE_CAN_H,
                            246 + i * 12, 8,
                            GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                            0, GFX_TRANS_NONE);
    }

    /* 2. Next Shift Preview Strip (y = 23..44) */
    gfx_draw_rect(GFX_Z_HUD_BG, 8, 23, 304, 21, 14, 24, 44, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 8, 23, 304, 1, 70, 170, 210, GFX_TRANS_NONE);
    p = buf;
    p = buf_append_str(p, buf + sizeof(buf) - 1, "NEXT: N");
    p = buf_append_uint(p, buf + sizeof(buf) - 1, next_night);
    p = buf_append_str(p, buf + sizeof(buf) - 1, "-S");
    p = buf_append_uint(p, buf + sizeof(buf) - 1, (uint32_t)next_shift + 1u);
    p = buf_append_str(p, buf + sizeof(buf) - 1,
                       next_shift == (uint8_t)SHIFT_BOSS ? " BOSS" :
                       (next_shift == (uint8_t)SHIFT_HAPPY_HOUR ? " HAPPY HR" : " REG"));
    p = buf_append_str(p, buf + sizeof(buf) - 1, "  TGT:");
    p = buf_append_uint(p, buf + sizeof(buf) - 1, next_target);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 14, 25, buf, 160, 240, 255);

    if (next_boss != BOSS_NONE) {
        const BossDef *bdef = rl_boss_def((uint8_t)next_boss);
        p = buf;
        p = buf_append_str(p, buf + sizeof(buf) - 1, "BOSS TWIST: ");
        p = buf_append_str(p, buf + sizeof(buf) - 1, bdef->name);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 14, 35, buf, 255, 130, 110);
    } else {
        const BossDef *bdef = rl_boss_def((uint8_t)rl_night_boss(run, next_night));
        p = buf;
        p = buf_append_str(p, buf + sizeof(buf) - 1, "N");
        p = buf_append_uint(p, buf + sizeof(buf) - 1, next_night);
        p = buf_append_str(p, buf + sizeof(buf) - 1, " BOSS: ");
        p = buf_append_str(p, buf + sizeof(buf) - 1, bdef->name);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 14, 35, buf, 255, 210, 115);
    }

    /* 3. Equipped Coasters Tray (Zone 0) + Pocket Tricks (y = 46..88) */
    gfx_draw_rect(GFX_Z_HUD_BG, 8, 46, 214, 42, 12, 18, 32, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG, 226, 46, 86, 42, 12, 18, 32, GFX_TRANS_HALF);
    if (ss->zone == 0) {
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 8, 46, 214, 1, 255, 224, 90, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 8, 87, 214, 1, 255, 224, 90, GFX_TRANS_NONE);
    }

    for (i = 0; i < RL_MAX_COASTERS; i++) {
        int tx = 30 + i * 42;
        if (i < (int)run->coaster_count) {
            const CoasterDef *def = rl_coaster_def(run->coasters[i]);
            int sel = (ss->zone == 0 && ss->cursor == i);
            int yaw = sel ? integer_osc(tick, 70u, 12) : 0;
            room3d_draw_card(GFX_Z_POPUP, tx, sel ? 65 : 67, 28, 36,
                             yaw, 0, (int)def->rarity,
                             CARD_KIND_COASTER, (int)def->id,
                             sel, (def->rarity == RARITY_RARE) || sel, tick);
        } else {
            gfx_draw_rect(GFX_Z_HUD_BG - 1, tx - 13, 51, 26, 32, 22, 32, 52, GFX_TRANS_HALF);
        }
    }

    gfx_draw_btn(GFX_Z_HUD_TEXT, 240, 46, GLYPH_L1);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 280, 46, GLYPH_R1);
    for (i = 0; i < RL_MAX_TRICKS; i++) {
        int kx = 236 + i * 40;
        gfx_draw_rect(GFX_Z_HUD_BG - 1, kx, 59, 26, 24, 24, 34, 58, GFX_TRANS_NONE);
        if (run->tricks[i] != (int8_t)TRICK_NONE) {
            gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_CARDS,
                                SPR_TRICK_ICON_U(run->tricks[i]),
                                SPR_TRICK_ICON_V(run->tricks[i]),
                                SPR_TRICK_ICON_W, SPR_TRICK_ICON_H,
                                kx + 5, 63,
                                GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                0, GFX_TRANS_NONE);
        } else {
            gfx_draw_text(GFX_Z_HUD_TEXT, kx + 9, 67, "-", 100, 120, 140);
        }
    }

    /* 4. Shop Offers (Zone 1: 2 Coasters, 1 Drink Upgrade, 1 Pocket Trick) */
    for (i = 0; i < 4; i++) {
        int sel = (ss->zone == 1 && ss->cursor == i);
        int local_t = (int)ss->card_flip[i] - i * 4;
        int yaw = 0;
        int pitch = 0;
        int cy = sel ? 116 : 120;
        int sold = 0;
        int rarity = RARITY_COMMON;
        RoomCardKind kind = CARD_KIND_COASTER;
        int icon_id = 0;
        int holo = sel;
        int16_t cost = 0;

        if (i < 2) {
            const CoasterDef *def = rl_coaster_def(ss->coaster_offers[i]);
            sold    = (int)ss->coaster_sold[i];
            rarity  = (int)def->rarity;
            kind    = CARD_KIND_COASTER;
            icon_id = (int)def->id;
            cost    = (int16_t)def->cost;
            if (rarity == RARITY_RARE) holo = 1;
        } else if (i == 2) {
            sold    = (int)ss->drink_sold;
            rarity  = RARITY_UNCOMMON;
            kind    = CARD_KIND_DRINK;
            icon_id = (int)ss->drink_offer;
            cost    = drink_upgrade_cost(run->drink_level[ss->drink_offer]);
        } else {
            const TrickDef *tdef = rl_trick_def(ss->trick_offer);
            sold    = (int)ss->trick_sold || (tdef == NULL);
            rarity  = RARITY_COMMON;
            kind    = CARD_KIND_TRICK;
            icon_id = (int)ss->trick_offer;
            cost    = tdef ? (int16_t)tdef->cost : 4;
        }

        if (sold) {
            gfx_draw_rect(GFX_Z_HUD_BG, k_offer_cx[i] - 22, 95, 44, 58, 16, 22, 34, GFX_TRANS_HALF);
            if (sel) {
                gfx_draw_rect(GFX_Z_HUD_BG - 1, k_offer_cx[i] - 22, 95, 44, 1, 255, 224, 90, GFX_TRANS_NONE);
                gfx_draw_rect(GFX_Z_HUD_BG - 1, k_offer_cx[i] - 22, 152, 44, 1, 255, 224, 90, GFX_TRANS_NONE);
            }
            gfx_draw_text(GFX_Z_HUD_TEXT, k_offer_cx[i] - 16, 120, "SOLD", 140, 160, 180);
        } else {
            if (local_t < 0) {
                yaw = 180;
            } else if (local_t < 12) {
                yaw = 180 - (local_t * 180) / 12;
            } else if (sel) {
                yaw   = integer_osc(tick + (uint32_t)(i * 13u), 84u, 14);
                pitch = integer_osc(tick + 17u, 96u, 8);
            }
            room3d_draw_card(GFX_Z_POPUP, k_offer_cx[i], cy, 44, 56,
                             yaw, pitch, rarity, kind, icon_id, sel, holo, tick);
            if (i == 2 && local_t >= 12) {
                gfx_draw_drink_mini_btn(GFX_Z_HUD_TEXT, k_offer_cx[i] + 9, cy - 25,
                                        (int)ss->drink_offer);
            }

            p = buf;
            p = buf_append_str(p, buf + sizeof(buf) - 1, "$");
            p = buf_append_int(p, buf + sizeof(buf) - 1, (int32_t)cost);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT,
                                 k_offer_cx[i] - ((int)strlen(buf) * 4), 154, buf,
                                 (run->cash >= cost) ? 255 : 255,
                                 (run->cash >= cost) ? 236 : 105,
                                 (run->cash >= cost) ? 96 : 95);
        }
    }

    /* 5. Highlighted Item Detail Box (y = 168..203) */
    gfx_draw_rect(GFX_Z_HUD_BG, 8, 168, 304, 35, 10, 18, 34, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 8, 168, 304, 1, 90, 175, 210, GFX_TRANS_NONE);

    if (ss->zone == 0 && run->coaster_count > 0u) {
        const CoasterDef *def = rl_coaster_def(run->coasters[ss->cursor]);
        int16_t sell_val = (int16_t)(def->cost / 2u);
        if (sell_val < 1) sell_val = 1;
        p = buf;
        p = buf_append_str(p, buf + sizeof(buf) - 1, def->name);
        p = buf_append_str(p, buf + sizeof(buf) - 1, " [SELL $");
        p = buf_append_int(p, buf + sizeof(buf) - 1, (int32_t)sell_val);
        p = buf_append_str(p, buf + sizeof(buf) - 1, "]");
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 173, buf, 255, 236, 96);
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 188, def->desc, 220, 245, 255);
    } else if (ss->zone == 1) {
        if (ss->cursor < 2) {
            const CoasterDef *def = rl_coaster_def(ss->coaster_offers[ss->cursor]);
            p = buf;
            p = buf_append_str(p, buf + sizeof(buf) - 1, def->name);
            p = buf_append_str(p, buf + sizeof(buf) - 1, " [");
            p = buf_append_str(p, buf + sizeof(buf) - 1, k_rarity_names[def->rarity]);
            p = buf_append_str(p, buf + sizeof(buf) - 1, "]");
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 173, buf, 255, 236, 96);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 188, def->desc, 220, 245, 255);
        } else if (ss->cursor == 2) {
            static const char *const k_drink_up_desc[3] = {
                "+25 BASE PTS & +1 BASE MULT",
                "+35 BASE PTS & +1 BASE MULT",
                "+40 BASE PTS & +1 BASE MULT"
            };
            uint8_t d = ss->drink_offer;
            uint8_t lv = (d < 3u) ? run->drink_level[d] : 1u;
            p = buf;
            p = buf_append_str(p, buf + sizeof(buf) - 1, k_drink_names[d]);
            p = buf_append_str(p, buf + sizeof(buf) - 1, " LV.");
            p = buf_append_uint(p, buf + sizeof(buf) - 1, lv);
            if (lv < 9u) {
                p = buf_append_str(p, buf + sizeof(buf) - 1, " -> LV.");
                p = buf_append_uint(p, buf + sizeof(buf) - 1, (uint32_t)lv + 1u);
            } else {
                p = buf_append_str(p, buf + sizeof(buf) - 1, " (MAX)");
            }
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 173, buf, 140, 255, 190);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 188,
                                   k_drink_up_desc[(d < 3u) ? d : 0u], 220, 245, 255);
        } else {
            const TrickDef *tdef = rl_trick_def(ss->trick_offer);
            if (tdef) {
                p = buf;
                p = buf_append_str(p, buf + sizeof(buf) - 1, "TRICK: ");
                p = buf_append_str(p, buf + sizeof(buf) - 1, tdef->name);
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 173, buf, 180, 235, 255);
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 188, tdef->desc, 220, 245, 255);
            } else {
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 180, "OUT OF STOCK", 160, 180, 200);
            }
        }
    } else {
        if (ss->cursor == 0) {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 173, "RESTOCK SHOP OFFERS", 255, 236, 96);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 188,
                                   "REFRESHES ALL 4 CARDS (+$1/USE)", 210, 230, 245);
        } else {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 173, "START NEXT SHIFT", 140, 255, 170);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 188,
                                   "CLOCK STARTS WHEN YOU SERVE", 210, 230, 245);
        }
    }

    /* 6. Bottom Action Buttons (Zone 2: y = 206..225) */
    {
        int r_sel = (ss->zone == 2 && ss->cursor == 0);
        int n_sel = (ss->zone == 2 && ss->cursor == 1);

        gfx_draw_rect(GFX_Z_HUD_BG, 20, 206, 132, 18,
                      r_sel ? 40 : 16, r_sel ? 62 : 24, r_sel ? 92 : 42, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 20, 206, 132, 1,
                      r_sel ? 255 : 70, r_sel ? 230 : 120, r_sel ? 90 : 160, GFX_TRANS_NONE);
        p = buf;
        p = buf_append_str(p, buf + sizeof(buf) - 1, "REROLL $");
        p = buf_append_int(p, buf + sizeof(buf) - 1, (int32_t)ss->reroll_cost);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 86 - (int)strlen(buf) * 4, 211, buf,
                             r_sel ? 255 : 195, r_sel ? 245 : 210, r_sel ? 120 : 225);

        gfx_draw_rect(GFX_Z_HUD_BG, 168, 206, 132, 18,
                      n_sel ? 28 : 16, n_sel ? 78 : 34, n_sel ? 52 : 28, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 168, 206, 132, 1,
                      n_sel ? 140 : 70, n_sel ? 255 : 150, n_sel ? 160 : 110, GFX_TRANS_NONE);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 194, 211, "NEXT SHIFT",
                             n_sel ? 180 : 160, n_sel ? 255 : 220, n_sel ? 190 : 175);
    }

    /* 7. Bottom Controls / Toast Strip (y = 228..244, strictly inside safe margin!) */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 228, GFX_SCREEN_W, 18, 10, 14, 24, GFX_TRANS_HALF);
    if (ss->toast_timer > 0 && ss->toast[0] != '\0') {
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 233, ss->toast, 255, 236, 96);
    } else if (ss->zone == 0) {
        gfx_draw_btn(GFX_Z_HUD_TEXT, 12, 231, GLYPH_L1);
        gfx_draw_btn(GFX_Z_HUD_TEXT, 32, 231, GLYPH_R1);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 53, 233, "REORDER", 170, 235, 255);

        gfx_draw_btn(GFX_Z_HUD_TEXT, 124, 230, GLYPH_SQUARE);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 141, 233, "SELL", 255, 210, 120);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 206, 233, "START: GO", 160, 255, 175);
    } else {
        gfx_draw_btn(GFX_Z_HUD_TEXT, 18, 230, GLYPH_CROSS);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 35, 233, "SELECT/BUY", 190, 245, 255);
        gfx_draw_btn(GFX_Z_HUD_TEXT, 130, 230, GLYPH_DPAD);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 147, 233, "TRAY", 255, 224, 120);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 226, 233, "START: GO", 160, 255, 175);
    }
}
