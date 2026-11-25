#include "game.h"
#include "gfx.h"
#include "room3d.h"
#include "roguelike.h"
#include "input.h"
#include "audio.h"
#include "assets_layout.h"
#include "sfx_table.h"

#include <string.h>

/* =========================================================================
 * Deterministic PRNG & Pure-Integer Formatting Helpers (Zero Floats)
 * ========================================================================= */

static uint32_t game_rand(GameState *gs)
{
    uint32_t x = gs->rng_state;
    if (x == 0) {
        x = 0x96A51996u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    gs->rng_state = x;
    return x;
}

static int game_rand_range(GameState *gs, int min_v, int max_v)
{
    if (max_v <= min_v) {
        return min_v;
    }
    return min_v + (int)(game_rand(gs) % (uint32_t)(max_v - min_v + 1));
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int append_uint(char *dst, int pos, int max_len, uint32_t val)
{
    char tmp[12];
    int n = 0;
    if (val == 0) {
        if (pos < max_len) dst[pos++] = '0';
        return pos;
    }
    while (val > 0 && n < 11) {
        tmp[n++] = (char)('0' + (val % 10u));
        val /= 10u;
    }
    while (n > 0 && pos < max_len) {
        dst[pos++] = tmp[--n];
    }
    return pos;
}

static void format_plus_score(char out[16], int32_t pts)
{
    int pos = 0;
    out[pos++] = '+';
    if (pts < 0) pts = 0;
    pos = append_uint(out, pos, 15, (uint32_t)pts);
    out[pos] = '\0';
}

static void format_compact_val(char *dst, int *pos, int max_len, uint32_t v)
{
    if (v >= 1000000u) {
        *pos = append_uint(dst, *pos, max_len, v / 1000000u);
        if (*pos < max_len) dst[(*pos)++] = 'M';
    } else if (v >= 100000u) {
        *pos = append_uint(dst, *pos, max_len, v / 1000u);
        if (*pos < max_len) dst[(*pos)++] = 'K';
    } else {
        *pos = append_uint(dst, *pos, max_len, v);
    }
}

static void format_score_target(char out[16], uint32_t score, uint32_t target)
{
    int pos = 0;
    format_compact_val(out, &pos, 15, score);
    if (pos < 15) out[pos++] = '/';
    format_compact_val(out, &pos, 15, target);
    out[pos] = '\0';
}

static void format_clock(char out[8], int frames_left)
{
    int secs;
    int mins;
    int rem;
    if (frames_left <= 0) {
        out[0] = '0'; out[1] = ':'; out[2] = '0'; out[3] = '0'; out[4] = '\0';
        return;
    }
    secs = (frames_left + 49) / 50;
    mins = secs / 60;
    rem = secs % 60;
    if (mins > 9) mins = 9;
    out[0] = (char)('0' + mins);
    out[1] = ':';
    out[2] = (char)('0' + (rem / 10));
    out[3] = (char)('0' + (rem % 10));
    out[4] = '\0';
}

static void format_cash(char out[12], int16_t cash)
{
    int pos = 0;
    out[pos++] = '$'; /* Font maps '$' -> Yuan glyph */
    if (cash < 0) cash = 0;
    pos = append_uint(out, pos, 11, (uint32_t)cash);
    out[pos] = '\0';
}

/* =========================================================================
 * VFX & Non-Overlapping Popup Spawners
 * ========================================================================= */

static void project_lane_pos(int lane, int bar_x, int *out_sx, int *out_sy, int *out_scale)
{
    int sx = 160, sy = 160, sc = 256;
    room3d_lane_project(lane, clamp_int(bar_x, 0, 256), &sx, &sy, &sc);
    if (out_sx) *out_sx = sx;
    if (out_sy) *out_sy = sy;
    if (out_scale) *out_scale = sc;
}

static void spawn_particle(GameState *gs, uint8_t kind, int sx, int sy,
                           int vx_fp, int vy_fp, int life,
                           uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) {
        if (!gs->particles[i].active) {
            Particle *p = &gs->particles[i];
            p->active = 1;
            p->kind = kind;
            p->x_fp = TO_FP(sx);
            p->y_fp = TO_FP(sy);
            p->vx_fp = vx_fp;
            p->vy_fp = vy_fp;
            p->life = (int16_t)life;
            p->r = r;
            p->g = g;
            p->b = b;
            return;
        }
    }
}

static void spawn_burst_bar(GameState *gs, uint8_t kind, int lane, int bar_x,
                            int y_off, int count, uint8_t r, uint8_t g, uint8_t b)
{
    int sx, sy, sc;
    int i;
    project_lane_pos(lane, bar_x, &sx, &sy, &sc);
    (void)sc;
    for (i = 0; i < count; i++) {
        int vx = game_rand_range(gs, -440, 440);
        int vy = game_rand_range(gs, -500, -120);
        int life = game_rand_range(gs, 16, 28);
        spawn_particle(gs, kind, sx, sy + y_off, vx, vy, life, r, g, b);
    }
}

static void spawn_popup_screen(GameState *gs, int sx, int sy, const char *text,
                               uint8_t r, uint8_t g, uint8_t b)
{
    int i, pass;
    int slot = -1;
    int x = clamp_int(sx, 8, 252);
    int y = clamp_int(sy, 40, 222);

    /* Stagger Y cleanly so active popups never overlap each other */
    for (pass = 0; pass < 4; pass++) {
        int overlap = 0;
        for (i = 0; i < MAX_POPUPS; i++) {
            const ScorePopup *other = &gs->popups[i];
            if (!other->active) continue;
            if (other->x - x > -46 && other->x - x < 46 &&
                other->y - y > -11 && other->y - y < 11) {
                overlap = 1;
                break;
            }
        }
        if (!overlap) break;
        y -= 11;
        if (y < 40) y = sy + (pass + 1) * 11;
    }
    y = clamp_int(y, 40, 224);

    for (i = 0; i < MAX_POPUPS; i++) {
        if (!gs->popups[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Recycle oldest popup (lowest remaining life) */
        int min_life = 9999;
        slot = 0;
        for (i = 0; i < MAX_POPUPS; i++) {
            if (gs->popups[i].life < min_life) {
                min_life = gs->popups[i].life;
                slot = i;
            }
        }
    }

    {
        ScorePopup *sp = &gs->popups[slot];
        int len = (int)strlen(text);
        if (len > 15) len = 15;
        sp->active = 1;
        sp->x = (int16_t)x;
        sp->y = (int16_t)y;
        sp->life = 44;
        sp->r = r;
        sp->g = g;
        sp->b = b;
        memcpy(sp->text, text, (size_t)len);
        sp->text[len] = '\0';
    }
}

static void spawn_popup_bar(GameState *gs, int lane, int bar_x, int y_off,
                            const char *text, uint8_t r, uint8_t g, uint8_t b)
{
    int sx, sy, sc;
    project_lane_pos(lane, bar_x, &sx, &sy, &sc);
    (void)sc;
    spawn_popup_screen(gs, sx - 12, sy + y_off, text, r, g, b);
}

/* =========================================================================
 * Score, Frost, Combo, Mistakes & Life Loss Helpers
 * ========================================================================= */

static void add_shift_score(GameState *gs, RunState *run, uint32_t pts)
{
    uint64_t next_shift = (uint64_t)gs->shift_score + (uint64_t)pts;
    uint64_t next_run = (uint64_t)run->run_score + (uint64_t)pts;
    if (next_shift > RL_SCORE_CAP) next_shift = RL_SCORE_CAP;
    if (next_run > RL_SCORE_CAP) next_run = RL_SCORE_CAP;

    gs->shift_score = (uint32_t)next_shift;
    run->run_score = (uint32_t)next_run;
    if (run->run_score > run->high_score) {
        run->high_score = run->run_score;
    }
}

static void add_frost(GameState *gs, const RunState *run, int amount)
{
    int bonus = 0;
    if (gs->rush_active) {
        return;
    }
    if (rl_has_coaster(run, COASTER_FROST_BITE)) {
        bonus += 4;
    }
    gs->frost = (int16_t)(gs->frost + amount + bonus);
    if (gs->frost >= FROST_MAX) {
        gs->frost = FROST_MAX;
        if (!gs->frost_ready_sfx) {
            gs->frost_ready_sfx = 1;
            audio_play_sfx(SFX_RUSH_READY, 160, 0);
            spawn_popup_screen(gs, 114, 44, "RUSH READY!", 100, 240, 255);
        }
    }
}

static void bump_combo(GameState *gs)
{
    if (gs->combo < 99) {
        gs->combo++;
    }
    gs->combo_timer = 240; /* 4.8s combo window */
}

static void trigger_flash(GameState *gs, int frames, uint8_t r, uint8_t g, uint8_t b)
{
    gs->flash_timer = (int16_t)frames;
    gs->flash_r = r;
    gs->flash_g = g;
    gs->flash_b = b;
}

static void lose_life(GameState *gs, RunState *run, int lane, int bar_x, const char *reason)
{
    int i;
    int sx, sy, sc;
    if (gs->life_lost_timer > 0) {
        return;
    }

    project_lane_pos(lane, bar_x, &sx, &sy, &sc);
    (void)sc;

    if (gs->tutorial_mode) {
        gs->life_lost_timer = 20;
        gs->buffered_drink = -1;
        gs->is_auto_sprinting = 0;
        gs->pour_buf_drink = -1;
        gs->pour_buf_timer = 0;
        audio_play_sfx(SFX_WRONG_DRINK, sx, 0);
        trigger_flash(gs, 6, 160, 48, 32);
        spawn_popup_bar(gs, lane, bar_x, -26, "STAY SHARP!", 255, 190, 96);
    } else {
        if (run->lives > 0) {
            run->lives--;
        }
        gs->combo = 0;
        gs->combo_timer = 0;
        gs->life_lost_timer = 65; /* ~1.3s recovery beat */
        gs->buffered_drink = -1;
        gs->is_auto_sprinting = 0;
        gs->pour_buf_drink = -1;
        gs->pour_buf_timer = 0;

        audio_play_sfx(SFX_GLASS_SMASH, sx, 0);
        gfx_set_shake(14, 4);
        trigger_flash(gs, 12, 180, 32, 32);
        spawn_burst_bar(gs, 2, lane, bar_x, -10, 10, 255, 220, 120);
        spawn_popup_bar(gs, lane, bar_x, -26, reason, 255, 96, 84);
    }

    /* Non-exploitable recovery: push lane patrons back only 48 units (NO deletion!) */
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        Customer *c = &gs->customers[i];
        if (c->active && c->lane == (uint8_t)lane && c->state != CSTATE_SATISFIED) {
            c->x_fp -= TO_FP(48);
            if (c->x_fp < TO_FP(4)) {
                c->x_fp = TO_FP(4);
            }
            if (c->state == CSTATE_ANGRY) {
                c->state = CSTATE_WALKING;
                c->state_timer = 0;
            }
        }
    }

    /* Only clear empties within 40 units of the tap to prevent double-death traps */
    for (i = 0; i < MAX_EMPTIES; i++) {
        ReturningEmpty *e = &gs->empties[i];
        if (e->active && e->lane == (uint8_t)lane &&
            FROM_FP(e->x_fp) >= (BAR_RIGHT_X - 40)) {
            e->active = 0;
        }
    }
}

static void apply_mistake_penalty(GameState *gs, RunState *run, int lane, int bar_x,
                                  const char *label, int is_left_spill)
{
    BossType boss = rl_current_boss(run);
    int fee = (boss == BOSS_HEALTH_INSPECTOR) ? 2 : 1;
    int sx, sy, sc;

    project_lane_pos(lane, bar_x, &sx, &sy, &sc);
    (void)sc;

    gs->combo = 0;
    gs->combo_timer = 0;
    rl_on_spill(run);

    if (boss == BOSS_HEALTH_INSPECTOR) {
        gs->frost = 0;
    } else {
        gs->frost = (int16_t)(gs->frost - 15);
        if (gs->frost < 0) gs->frost = 0;
    }
    if (gs->frost < FROST_MAX) {
        gs->frost_ready_sfx = 0;
    }

    /* Bounded Spill Fee: Yuan cash (clamped >= 0) or 100 pts if cash == 0 */
    if (run->cash > 0) {
        int paid = (run->cash >= fee) ? fee : (int)run->cash;
        run->cash = (int16_t)(run->cash - paid);
        if (run->cash < 0) run->cash = 0;
        spawn_popup_bar(gs, lane, bar_x, -26, label, 255, 110, 90);
        spawn_popup_bar(gs, lane, bar_x, -38, (paid >= 2) ? "-$2" : "-$1", 255, 160, 110);
    } else {
        uint32_t loss = (gs->shift_score >= 100u) ? 100u : gs->shift_score;
        gs->shift_score -= loss;
        if (run->run_score >= loss) run->run_score -= loss;
        else run->run_score = 0;
        spawn_popup_bar(gs, lane, bar_x, -26, label, 255, 110, 90);
        spawn_popup_bar(gs, lane, bar_x, -38, "-100", 255, 160, 110);
    }

    audio_play_sfx(SFX_WRONG_DRINK, sx, 0);
    spawn_burst_bar(gs, 2, lane, bar_x, -10, 5, 255, 110, 90);

    if (is_left_spill && rl_has_coaster(run, COASTER_GLASS_CANNON)) {
        lose_life(gs, run, lane, bar_x, "CANNON!");
    }
}

/* =========================================================================
 * Entity Helpers & Spawners
 * ========================================================================= */

static int lane_has_active_patron(const GameState *gs, int lane)
{
    int i;
    if (lane < 0 || lane >= NUM_LANES) return 0;
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        const Customer *c = &gs->customers[i];
        if (c->active && c->lane == (uint8_t)lane && c->state != CSTATE_SATISFIED) {
            return 1;
        }
    }
    return 0;
}

static int count_lane_nonrush_drinks(const GameState *gs, int lane)
{
    int i, count = 0;
    for (i = 0; i < MAX_DRINKS; i++) {
        if (gs->drinks[i].active && gs->drinks[i].lane == (uint8_t)lane && !gs->drinks[i].is_rush) {
            count++;
        }
    }
    return count;
}

static int spawn_buff(GameState *gs, const RunState *run, int lane, int bar_x,
                      uint8_t buff_kind, int cash_val, int score_val)
{
    int i;
    BossType boss = rl_current_boss(run);
    if (gs->tutorial_mode && gs->tutorial_step != 3) {
        return 0;
    }
    if (buff_kind == BUFF_GOLD_TIP) {
        if (boss == BOSS_NO_TIPS) return 0;
        if (gs->shift_tips_spawned >= SHIFT_MAX_TIPS_SPAWNED) return 0;
        if (gs->shift_tips_cash >= SHIFT_MAX_TIPS_CASH) return 0;
    } else {
        if (gs->shift_buffs_spawned >= SHIFT_MAX_BUFFS_SPAWNED) return 0;
    }

    bar_x = clamp_int(bar_x, 28, 212);

    for (i = 0; i < MAX_TIPS; i++) {
        if (gs->tips[i].active && gs->tips[i].lane == (uint8_t)lane) {
            int dx = gs->tips[i].x - bar_x;
            if (dx > -20 && dx < 20) {
                return 0; /* Do not overlap pickups in the same spot */
            }
        }
    }

    for (i = 0; i < MAX_TIPS; i++) {
        if (!gs->tips[i].active) {
            gs->tips[i].active = 1;
            gs->tips[i].lane = (uint8_t)lane;
            gs->tips[i].buff_kind = buff_kind;
            gs->tips[i].x = (int16_t)bar_x;
            gs->tips[i].cash_val = (int16_t)cash_val;
            gs->tips[i].score_val = (int16_t)score_val;
            gs->tips[i].life_timer = 420; /* 8.4s at 50 Hz */
            if (buff_kind == BUFF_GOLD_TIP) {
                gs->shift_tips_spawned++;
            } else {
                gs->shift_buffs_spawned++;
            }
            return 1;
        }
    }
    return 0;
}

static void spawn_empty(GameState *gs, const RunState *run, int lane, int bar_x,
                        uint8_t drink_type, int fast)
{
    int i, j;
    BossType boss = rl_current_boss(run);
    if (gs->tutorial_mode && gs->tutorial_step != 1) {
        return;
    }
    for (i = 0; i < MAX_EMPTIES; i++) {
        if (!gs->empties[i].active) {
            ReturningEmpty *e = &gs->empties[i];
            int spawn_x = clamp_int(bar_x, 12, 220);
            int dist = (BAR_RIGHT_X + 8) - spawn_x;
            int speed;
            if (gs->tutorial_mode) {
                speed = 135;
            } else if (run->night == 1) {
                static const int k_n1_empty_spd[3] = { 195, 225, 255 };
                speed = k_n1_empty_spd[clamp_int((int)run->shift_in_night, 0, 2)];
            } else {
                speed = 275 + (int)((run->night - 2u) * 19u) + (int)(run->shift_in_night * 13u);
            }
            if (fast && !gs->tutorial_mode) speed += 60;
            if (speed > 495) speed = 495;
            if (boss == BOSS_SLIPPERY_BAR && !gs->tutorial_mode) {
                speed = (speed * 132) / 100;
                if (speed > 590) speed = 590;
            }

            /* Human reaction-window safeguard: guarantee a fair catch window even near tap */
            if (!gs->tutorial_mode && dist > 0) {
                int min_tti = (boss == BOSS_SLIPPERY_BAR) ? 26 : 30; /* 0.52s / 0.60s @ 50Hz */
                int max_fair_spd = (dist * 256) / min_tti;
                if (max_fair_spd < 120) max_fair_spd = 120;
                if (speed > max_fair_spd) speed = max_fair_spd;

                /* Anti-collision stagger: prevent physically impossible simultaneous multi-lane drops */
                for (j = 0; j < MAX_EMPTIES; j++) {
                    const ReturningEmpty *other = &gs->empties[j];
                    int odist, otti, my_tti;
                    if (!other->active || other->lane == (uint8_t)lane || other->vx_fp <= 0) continue;
                    odist = (BAR_RIGHT_X + 8) - FROM_FP(other->x_fp);
                    otti = (odist > 0) ? (odist * 256) / other->vx_fp : 0;
                    my_tti = (dist * 256) / speed;
                    if (my_tti >= otti - 17 && my_tti <= otti + 17) {
                        int desired_tti = otti + 19;
                        int adj_spd = (dist * 256) / desired_tti;
                        if (adj_spd < 120) adj_spd = 120;
                        if (adj_spd < speed) speed = adj_spd;
                    }
                }
            }

            e->active = 1;
            e->lane = (uint8_t)lane;
            e->type = drink_type;
            e->x_fp = TO_FP(spawn_x);
            e->vx_fp = speed;
            e->spin_tick = 0;
            return;
        }
    }
}

static void spawn_drink(GameState *gs, int lane, uint8_t type, int is_rush)
{
    int i;
    for (i = 0; i < MAX_DRINKS; i++) {
        if (!gs->drinks[i].active) {
            SlidingDrink *d = &gs->drinks[i];
            d->active = 1;
            d->lane = (uint8_t)lane;
            d->type = type;
            d->is_rush = (uint8_t)is_rush;
            d->x_fp = TO_FP(BAR_RIGHT_X - 8);
            if (is_rush) {
                d->vx_fp = -1536; /* ~6.0 units/frame blistering Rush wave */
            } else if (type == DRINK_BOTTLE) {
                d->vx_fp = -1152; /* ~4.5 units/frame slick glass bottle */
            } else if (type == DRINK_BAG) {
                d->vx_fp = -920;  /* ~3.6 units/frame sloshing plastic bag */
            } else {
                d->vx_fp = -1040; /* ~4.1 units/frame crisp Tsingtao can */
            }
            return;
        }
    }
}

static void spawn_customer(GameState *gs, const RunState *run)
{
    int i;
    int slot = -1;
    int lane;
    int roll;
    uint8_t ctype = CUST_REGULAR;
    int base_speed;
    BossType boss = rl_current_boss(run);

    for (i = 0; i < MAX_CUSTOMERS; i++) {
        if (!gs->customers[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;

    lane = game_rand_range(gs, 0, NUM_LANES - 1);
    if (lane == (int)gs->last_spawn_lane) {
        lane = (lane + game_rand_range(gs, 1, NUM_LANES - 1)) & 3;
    }
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        if (gs->customers[i].active &&
            gs->customers[i].lane == (uint8_t)lane &&
            (FROM_FP(gs->customers[i].x_fp) < 28 || FROM_FP(gs->customers[i].x_fp) >= 165)) {
            lane = (lane + 1) & 3;
            break;
        }
    }
    gs->last_spawn_lane = (uint8_t)lane;

    roll = game_rand_range(gs, 0, 99);
    if (gs->vip_coupon_spawns > 0) {
        gs->vip_coupon_spawns--;
        if (boss == BOSS_BOTTLENECK) {
            ctype = CUST_BOTTLE_VIP;
        } else if (boss == BOSS_BAG_FESTIVAL) {
            ctype = CUST_BAG_VIP;
        } else {
            ctype = (roll < 50) ? CUST_BOTTLE_VIP : CUST_BAG_VIP;
        }
    } else if (boss == BOSS_BAG_FESTIVAL) {
        if (roll < 70)      ctype = CUST_BAG_VIP;
        else if (roll < 82) ctype = CUST_REGULAR;
        else if (roll < 92) ctype = CUST_FOODIE;
        else                ctype = CUST_BOTTLE_VIP;
    } else if (run->night == 1 && run->shift_in_night == 0) {
        if (roll < 52)      ctype = CUST_REGULAR;
        else if (roll < 78) ctype = CUST_BOTTLE_VIP;
        else                ctype = CUST_BAG_VIP;
    } else if (run->night == 1 && run->shift_in_night == 1) {
        if (roll < 42)      ctype = CUST_REGULAR;
        else if (roll < 60) ctype = CUST_FOODIE;
        else if (roll < 82) ctype = CUST_BOTTLE_VIP;
        else                ctype = CUST_BAG_VIP;
    } else {
        if (roll < 26)      ctype = CUST_REGULAR;
        else if (roll < 48) ctype = CUST_FOODIE;
        else if (roll < 68) ctype = CUST_BOTTLE_VIP;
        else if (roll < 86) ctype = CUST_BAG_VIP;
        else                ctype = CUST_RUSHER;
    }

    if (run->night == 1) {
        static const int k_n1_cust_spd[3] = { 68, 84, 98 };
        base_speed = k_n1_cust_spd[clamp_int((int)run->shift_in_night, 0, 2)];
    } else {
        base_speed = 116 + (int)((run->night - 2u) * 15u) + (int)(run->shift_in_night * 10u);
    }
    if (base_speed > 228) base_speed = 228;
    if (boss == BOSS_DOUBLE_SHIFT) {
        base_speed = (base_speed * 122) / 100;
    } else if (boss == BOSS_BOTTLENECK || boss == BOSS_BAG_FESTIVAL) {
        base_speed = (base_speed * 120) / 100;
    } else if (boss == BOSS_NO_TIPS) {
        base_speed = (base_speed * 108) / 100;
    }

    {
        Customer *c = &gs->customers[slot];
        c->active = 1;
        c->lane = (uint8_t)lane;
        c->type = ctype;
        c->state = CSTATE_WALKING;
        c->x_fp = TO_FP(BAR_LEFT_X);
        c->state_timer = 0;
        c->stun_timer = 0;
        c->hit_flash = 0;
        c->anim_tick = (uint16_t)game_rand_range(gs, 0, 31);

        switch (ctype) {
            case CUST_FOODIE:
                c->wanted_drink = DRINK_CAN;
                c->hp = 2;
                c->vx_fp = (base_speed * 80) / 100;
                break;
            case CUST_BOTTLE_VIP:
                c->wanted_drink = DRINK_BOTTLE;
                c->hp = 1;
                c->vx_fp = (base_speed * 118) / 100;
                break;
            case CUST_BAG_VIP:
                c->wanted_drink = DRINK_BAG;
                c->hp = 1;
                c->vx_fp = (base_speed * 105) / 100;
                break;
            case CUST_RUSHER:
                c->wanted_drink = DRINK_CAN;
                c->hp = 1;
                c->vx_fp = (base_speed * 135) / 100;
                break;
            case CUST_REGULAR:
            default:
                c->wanted_drink = DRINK_CAN;
                c->hp = 1;
                c->vx_fp = base_speed;
                break;
        }

        if (boss == BOSS_BOTTLENECK) {
            c->wanted_drink = DRINK_BOTTLE;
        }
        if (boss == BOSS_TOUGH_CROWD) {
            c->hp = (uint8_t)(c->hp + 1);
        }
    }
}

/* =========================================================================
 * Pocket Tricks & Ice-Cold Rush
 * ========================================================================= */

static void trigger_ice_cold_rush(GameState *gs)
{
    int lane, i;
    gs->frost = 0;
    gs->frost_ready_sfx = 0;
    gs->rush_active = 1;
    gs->celebrate_timer = 32;
    gs->buffered_drink = -1;
    gs->is_auto_sprinting = 0;
    gs->pour_buf_drink = -1;
    gs->pour_buf_timer = 0;

    /* Supercharge any regular drinks already in flight so Rush never causes phantom spills */
    for (i = 0; i < MAX_DRINKS; i++) {
        if (gs->drinks[i].active && !gs->drinks[i].is_rush) {
            gs->drinks[i].is_rush = 1;
            if (gs->drinks[i].vx_fp > -1536) {
                gs->drinks[i].vx_fp = -1536;
            }
        }
    }

    audio_play_sfx(SFX_RUSH_FIRE, 160, 0);
    gfx_set_shake(16, 4);
    trigger_flash(gs, 14, 110, 225, 255);
    spawn_popup_screen(gs, 96, 62, "ICE-COLD RUSH!", 120, 248, 255);

    for (lane = 0; lane < NUM_LANES; lane++) {
        spawn_drink(gs, lane, DRINK_CAN, 1);
        spawn_burst_bar(gs, 1, lane, BAR_RIGHT_X - 6, -12, 4, 140, 245, 255);
    }
}

static void use_pocket_trick(GameState *gs, RunState *run, int slot)
{
    int i;
    TrickId tid;
    if (slot < 0 || slot >= RL_MAX_TRICKS) return;
    if (run->tricks[slot] == TRICK_NONE) return;

    tid = (TrickId)run->tricks[slot];
    run->tricks[slot] = TRICK_NONE;
    audio_play_sfx(SFX_TRICK_USE, 160, 0);
    trigger_flash(gs, 8, 255, 236, 120);

    switch (tid) {
        case TRICK_COLD_MUG:
            if (run->lives < MAX_LIVES) {
                run->lives++;
                spawn_popup_screen(gs, 112, 48, "COLD MUG! +1", 140, 255, 170);
            } else {
                add_shift_score(gs, run, 500);
                spawn_popup_screen(gs, 112, 48, "MUG +500!", 255, 236, 96);
            }
            break;

        case TRICK_HAPPY_BELL:
            gs->happy_bell_timer = 400;
            spawn_popup_screen(gs, 108, 48, "HAPPY BELL 2X!", 255, 236, 96);
            break;

        case TRICK_BOUNCER:
            gfx_set_shake(10, 3);
            for (i = 0; i < MAX_CUSTOMERS; i++) {
                Customer *c = &gs->customers[i];
                if (!c->active || c->state == CSTATE_SATISFIED) continue;
                c->x_fp -= TO_FP(60);
                if (c->x_fp < TO_FP(4)) c->x_fp = TO_FP(4);
                c->stun_timer = 75;
                spawn_burst_bar(gs, 0, c->lane, FROM_FP(c->x_fp), -14, 3, 255, 236, 140);
            }
            spawn_popup_screen(gs, 116, 48, "BOUNCER!", 255, 200, 110);
            break;

        case TRICK_ICE_BUCKET:
            gs->frost = FROST_MAX;
            gs->frost_ready_sfx = 1;
            spawn_popup_screen(gs, 110, 48, "ICE BUCKET 100%", 120, 248, 255);
            break;

        case TRICK_FOAM_CATCH:
            gs->foam_catch_timer = 400;
            spawn_popup_screen(gs, 106, 48, "FOAM CATCH ON!", 150, 255, 220);
            break;

        case TRICK_VIP_COUPON:
            gs->vip_coupon_spawns = 3;
            spawn_popup_screen(gs, 108, 48, "3 VIP GUESTS!", 255, 224, 100);
            break;

        default:
            break;
    }
}

/* =========================================================================
 * Public Lifecycle
 * ========================================================================= */

void game_init_shift(GameState *gs, RunState *run)
{
    BossType boss = rl_current_boss(run);
    memset(gs, 0, sizeof(*gs));

    gs->shift_target = rl_shift_target(run->night, run->shift_in_night);
    gs->shift_frames_left = (int16_t)rl_shift_duration_frames(run->shift_in_night);
    gs->lane = 1;
    gs->bartender_x_fp = TO_FP(BARTENDER_TAP_X);
    gs->buffered_drink = -1;
    gs->pour_buf_drink = -1;
    gs->pour_buf_timer = 0;
    gs->shift_banner_timer = 75; /* 1.5s intro banner */
    gs->spawn_timer = 15;
    gs->rng_state = run->rng_state ^ (gfx_frame_count() * 2654435761u) ^ 0x19960715u;
    if (gs->rng_state == 0) gs->rng_state = 0x19960715u;
    gs->last_style_label[0] = '\0';

    run->shift_spills = 0;

    if (boss != BOSS_NONE) {
        audio_play_sfx(SFX_BOSS_SIREN, 160, 0);
    }
}

/* =========================================================================
 * Pouring & Input Handling (No Teleport-Back Exploit + Anti-Spam)
 * ========================================================================= */

static void hop_lane(GameState *gs, int delta)
{
    int sx;
    gs->lane = (int8_t)((gs->lane + NUM_LANES + delta) % NUM_LANES);
    /* Cancel queued pour/auto-sprint from previous lane so hopping never misfires a spill */
    gs->is_auto_sprinting = 0;
    gs->buffered_drink = -1;
    gs->pour_buf_drink = -1;
    gs->pour_buf_timer = 0;
    project_lane_pos(gs->lane, FROM_FP(gs->bartender_x_fp), &sx, NULL, NULL);
    audio_play_sfx(SFX_CARD_FLIP, sx, -80 + (int)gs->lane * 60);
}

static void trigger_max2_feedback(GameState *gs)
{
    int i, sx, sy, sc;
    int recent = 0;
    project_lane_pos(gs->lane, BAR_RIGHT_X - 10, &sx, &sy, &sc);
    (void)sc;
    audio_play_sfx(SFX_EMPTY_CATCH, sx, -220);
    for (i = 0; i < MAX_POPUPS; i++) {
        const ScorePopup *sp = &gs->popups[i];
        if (sp->active && sp->life > 32 &&
            sp->y - (sy - 30) > -14 && sp->y - (sy - 30) < 14 &&
            strcmp(sp->text, "MAX 2") == 0) {
            recent = 1;
            break;
        }
    }
    if (!recent) {
        spawn_popup_bar(gs, gs->lane, BAR_RIGHT_X - 24, -30, "MAX 2", 255, 196, 100);
    }
}

static void execute_tap_pour(GameState *gs, uint8_t drink_type)
{
    int pitch_jit = game_rand_range(gs, -64, 64);

    gs->bartender_x_fp = TO_FP(BARTENDER_TAP_X);
    gs->is_dashing = 0;
    gs->dash_dir = 0;
    gs->is_auto_sprinting = 0;
    gs->buffered_drink = -1;
    gs->pour_buf_drink = -1;
    gs->pour_buf_timer = 0;
    gs->pour_timer = 8; /* 8-frame tap-pull cadence */
    gs->last_drink_served = drink_type;

    spawn_drink(gs, gs->lane, drink_type, 0);

    if (drink_type == DRINK_CAN) {
        audio_play_sfx(SFX_CAN_POP, 260, pitch_jit);
        spawn_burst_bar(gs, 0, gs->lane, BAR_RIGHT_X - 4, -14, 3, 240, 250, 255);
    } else if (drink_type == DRINK_BOTTLE) {
        audio_play_sfx(SFX_BOTTLE_SLIDE, 260, pitch_jit);
        spawn_burst_bar(gs, 0, gs->lane, BAR_RIGHT_X - 4, -14, 3, 160, 255, 180);
    } else {
        audio_play_sfx(SFX_BAG_POUR, 260, pitch_jit);
        spawn_burst_bar(gs, 0, gs->lane, BAR_RIGHT_X - 4, -14, 4, 255, 224, 120);
    }
}

static void handle_player_input(GameState *gs, RunState *run)
{
    int want_drink = -1;
    int32_t dash_speed = rl_has_coaster(run, COASTER_SPEED_SNEAKERS) ? 1536 : 1152;
    int bx;

    /* Up/Down preserves bartender's horizontal bar_x (NO teleport-back exploit!) */
    if (input_pressed(BTN_UP)) {
        hop_lane(gs, -1);
    } else if (input_pressed(BTN_DOWN)) {
        hop_lane(gs, 1);
    }

    /* Mid-shift toggle for drink/bubble button badges */
    if (input_pressed(BTN_SELECT)) {
        gfx_set_drink_glyphs(!gfx_get_drink_glyphs());
        audio_play_sfx(SFX_CARD_FLIP, 160, gfx_get_drink_glyphs() ? 80 : -80);
        spawn_popup_screen(gs, 112, 42,
                           gfx_get_drink_glyphs() ? "GLYPHS: ON" : "GLYPHS: OFF",
                           130, 248, 255);
    }

    /* Pocket Tricks on L1 / R1 */
    if (input_pressed(BTN_L1)) {
        use_pocket_trick(gs, run, 0);
    }
    if (input_pressed(BTN_R1)) {
        use_pocket_trick(gs, run, 1);
    }

    /* Ice-Cold Rush on Triangle when Frost == 100% and no active rush wave */
    if (input_pressed(BTN_TRIANGLE) && gs->frost >= FROST_MAX && !gs->rush_active) {
        trigger_ice_cold_rush(gs);
        return;
    }

    /* Face button drink selection (suppressed while Ice-Cold Rush wave is sweeping all lanes) */
    if (!gs->rush_active) {
        if (input_pressed(BTN_CROSS)) {
            want_drink = DRINK_CAN;
        } else if (input_pressed(BTN_CIRCLE)) {
            want_drink = DRINK_BOTTLE;
        } else if (input_pressed(BTN_SQUARE)) {
            want_drink = DRINK_BAG;
        }
    }

    bx = FROM_FP(gs->bartender_x_fp);

    /* Explicit Left tap cancels buffered sprint so player is never locked */
    if (want_drink < 0 && gs->is_auto_sprinting && input_pressed(BTN_LEFT)) {
        gs->is_auto_sprinting = 0;
        gs->buffered_drink = -1;
    }

    if (want_drink >= 0) {
        if (bx < BARTENDER_POUR_X) {
            /* Buffer drink and trigger 8 px/frame auto-sprint back to the tap */
            gs->buffered_drink = (int8_t)want_drink;
            gs->is_auto_sprinting = 1;
            gs->pour_buf_drink = -1;
            gs->pour_buf_timer = 0;
        } else {
            gs->is_auto_sprinting = 0;
            gs->buffered_drink = -1;
            if (count_lane_nonrush_drinks(gs, gs->lane) >= 2) {
                gs->pour_buf_drink = -1;
                gs->pour_buf_timer = 0;
                trigger_max2_feedback(gs);
            } else if (gs->pour_timer == 0) {
                execute_tap_pour(gs, (uint8_t)want_drink);
                return;
            } else {
                /* 8-frame pour buffer so zero button presses are dropped */
                gs->pour_buf_drink = (int8_t)want_drink;
                gs->pour_buf_timer = 8;
            }
        }
    } else if (bx >= BARTENDER_POUR_X && gs->pour_timer == 0 &&
               gs->pour_buf_timer > 0 && gs->pour_buf_drink >= 0) {
        int8_t qdrink = gs->pour_buf_drink;
        gs->pour_buf_drink = -1;
        gs->pour_buf_timer = 0;
        if (count_lane_nonrush_drinks(gs, gs->lane) < 2) {
            execute_tap_pour(gs, (uint8_t)qdrink);
            return;
        }
        trigger_max2_feedback(gs);
    }

    /* Horizontal movement: auto-sprint (8 px/frame) OR manual D-Pad dash */
    gs->is_dashing = 0;
    gs->dash_dir = 0;
    if (gs->is_auto_sprinting && gs->buffered_drink >= 0) {
        gs->bartender_x_fp += TO_FP(8);
        gs->is_dashing = 1;
        gs->dash_dir = 1;
        if (FROM_FP(gs->bartender_x_fp) >= BARTENDER_POUR_X) {
            gs->bartender_x_fp = TO_FP(BARTENDER_TAP_X);
            if (gs->pour_timer == 0) {
                if (count_lane_nonrush_drinks(gs, gs->lane) < 2) {
                    execute_tap_pour(gs, (uint8_t)gs->buffered_drink);
                } else {
                    gs->buffered_drink = -1;
                    gs->is_auto_sprinting = 0;
                    trigger_max2_feedback(gs);
                }
            }
        }
    } else if (input_held(BTN_LEFT)) {
        gs->bartender_x_fp -= dash_speed;
        if (gs->bartender_x_fp < TO_FP(BARTENDER_MIN_X)) {
            gs->bartender_x_fp = TO_FP(BARTENDER_MIN_X);
        }
        gs->is_dashing = 1;
        gs->dash_dir = -1;
    } else if (input_held(BTN_RIGHT)) {
        gs->bartender_x_fp += dash_speed;
        if (gs->bartender_x_fp > TO_FP(BARTENDER_TAP_X)) {
            gs->bartender_x_fp = TO_FP(BARTENDER_TAP_X);
        }
        gs->is_dashing = 1;
        gs->dash_dir = 1;
    }
}

/* =========================================================================
 * Serve Scoring, Bag Splash & Collision Resolution
 * ========================================================================= */

static void apply_bag_splash(GameState *gs, RunState *run, int src_lane, int src_x, uint32_t total_pts)
{
    int i;
    int best_j = -1;
    int best_dist = 9999;
    Customer *adj;
    uint32_t splash_pts = total_pts / 2u;
    char buf[16];

    for (i = 0; i < MAX_CUSTOMERS; i++) {
        Customer *c = &gs->customers[i];
        int dl, dist, cx;
        if (!c->active || c->state == CSTATE_SATISFIED) continue;
        dl = (int)c->lane - src_lane;
        if (dl != -1 && dl != 1) continue;
        cx = FROM_FP(c->x_fp);
        dist = (cx > src_x) ? (cx - src_x) : (src_x - cx);
        if (dist < best_dist) {
            best_dist = dist;
            best_j = i;
        }
    }

    if (best_j < 0 || splash_pts == 0) return;
    adj = &gs->customers[best_j];
    if (adj->hp > 0) adj->hp--;
    adj->hit_flash = 4;

    add_shift_score(gs, run, splash_pts);
    format_plus_score(buf, (int32_t)splash_pts);
    spawn_burst_bar(gs, 0, adj->lane, FROM_FP(adj->x_fp), -14, 5, 255, 224, 120);
    spawn_popup_bar(gs, adj->lane, FROM_FP(adj->x_fp), -28, buf, 255, 224, 120);

    if (adj->hp == 0) {
        adj->state = CSTATE_SATISFIED;
        adj->vx_fp = -920;
        adj->state_timer = 0;
        gs->served_total++;
        if (run->served_total < RL_SERVE_CAP) run->served_total++;

        /* If splash cleared the last patron on adj->lane, dissipate in-flight drinks harmlessly */
        if (!lane_has_active_patron(gs, adj->lane)) {
            for (i = 0; i < MAX_DRINKS; i++) {
                if (gs->drinks[i].active && !gs->drinks[i].is_rush && gs->drinks[i].lane == adj->lane) {
                    spawn_burst_bar(gs, 0, adj->lane, FROM_FP(gs->drinks[i].x_fp), -10, 3, 255, 224, 120);
                    gs->drinks[i].active = 0;
                }
            }
        }
    } else {
        adj->state = CSTATE_DRINKING;
        adj->state_timer = 24;
        adj->x_fp -= TO_FP(32);
        if (adj->x_fp < TO_FP(4)) adj->x_fp = TO_FP(4);
    }
}

static void score_and_satisfy_hit(GameState *gs, RunState *run, Customer *c,
                                  uint8_t drink_type, int is_rush)
{
    int cx = FROM_FP(c->x_fp);
    uint8_t hp_before = c->hp;
    int adj_occ = lane_has_active_patron(gs, (int)c->lane - 1) ||
                  lane_has_active_patron(gs, (int)c->lane + 1);
    ServeScoreBreakdown bd;
    uint32_t pts;
    char buf[16];
    int sx, sy, sc;

    project_lane_pos(c->lane, cx, &sx, &sy, &sc);
    (void)sc;

    if (!is_rush) {
        c->hp--;
    } else {
        c->hp = 0;
    }
    c->hit_flash = 4;

    bump_combo(gs);
    rl_score_serve(run, drink_type, c->lane, c->type, hp_before, gs->combo,
                   gs->frost, (uint8_t)is_rush, (uint8_t)adj_occ,
                   (uint8_t)((gs->happy_bell_timer > 0 || gs->star_mult_timer > 0) ? 1 : 0), &bd);

    pts = bd.total_pts;

    add_shift_score(gs, run, pts);
    gs->last_serve_base = bd.base;
    gs->last_serve_mult_x10 = bd.mult_x10;
    gs->last_serve_pts = pts;
    if (bd.style_label) {
        int len = (int)strlen(bd.style_label);
        if (len > 15) len = 15;
        memcpy(gs->last_style_label, bd.style_label, (size_t)len);
        gs->last_style_label[len] = '\0';
    }

    audio_play_sfx(SFX_DRINK_CATCH, sx, is_rush ? 120 : game_rand_range(gs, -40, 60));
    spawn_burst_bar(gs, is_rush ? 1 : 0, c->lane, cx, -14, 6,
                    is_rush ? 130 : 255, is_rush ? 248 : 250, is_rush ? 255 : 220);

    format_plus_score(buf, (int32_t)pts);

    if (c->hp > 0) {
        /* Multi-HP patron (Foodie / Tough Crowd): immediate score + knockback! */
        c->state = CSTATE_DRINKING;
        c->state_timer = 26;
        c->x_fp -= TO_FP(44);
        if (c->x_fp < TO_FP(4)) c->x_fp = TO_FP(4);
        add_frost(gs, run, 5);
        if (drink_type != DRINK_BAG) {
            spawn_empty(gs, run, c->lane, cx + 8, drink_type, 0);
        }
        spawn_popup_bar(gs, c->lane, cx, -28, buf, 255, 224, 96);
    } else {
        BossType boss = rl_current_boss(run);
        int frost_gain = (c->type == CUST_BAG_VIP) ? ((boss == BOSS_BAG_FESTIVAL) ? 10 : 18) :
                         (c->type == CUST_BOTTLE_VIP ? 10 : 7);
        int tip_chance = (c->type == CUST_BOTTLE_VIP) ? 45 :
                         (c->type == CUST_BAG_VIP ? 42 :
                          (c->type == CUST_FOODIE ? 35 : 18));

        if (rl_has_coaster(run, COASTER_TIP_JAR)) {
            tip_chance += 20;
        }

        c->state = CSTATE_SATISFIED;
        c->vx_fp = is_rush ? -1350 : -920;
        c->state_timer = 0;
        gs->served_total++;
        if (run->served_total < RL_SERVE_CAP) run->served_total++;

        if (!is_rush) {
            int spawned_item = 0;
            int is_vip = (c->type == CUST_BOTTLE_VIP || c->type == CUST_BAG_VIP);
            int is_combo5 = (gs->combo > 0u && (gs->combo % 5u) == 0u);

            add_frost(gs, run, frost_gain);
            if (drink_type != DRINK_BAG) {
                spawn_empty(gs, run, c->lane, cx + 8, drink_type, (c->type == CUST_RUSHER));
            }
            if (gs->shift_buffs_spawned < SHIFT_MAX_BUFFS_SPAWNED &&
                (is_combo5 || (is_vip && game_rand_range(gs, 0, 99) < 42))) {
                uint8_t bkind;
                int broll = game_rand_range(gs, 0, 99);
                if (gs->foam_shield_charges == 0 && broll < 30) {
                    bkind = BUFF_FOAM_SHIELD;
                } else if (broll < 65) {
                    bkind = BUFF_FROST_ORB;
                } else {
                    bkind = BUFF_STAR_MULT;
                }
                spawned_item = spawn_buff(gs, run, c->lane, cx + 4, bkind, 0,
                                          (bkind == BUFF_FROST_ORB) ? 150 : 0);
            }
            if (!spawned_item && game_rand_range(gs, 0, 99) < tip_chance) {
                int cash_val = rl_has_coaster(run, COASTER_TIP_JAR) ? 2 : 1;
                spawn_buff(gs, run, c->lane, cx + 4, BUFF_GOLD_TIP, cash_val, 200);
            }
        }

        spawn_popup_bar(gs, c->lane, cx, -26, buf,
                        is_rush ? 120 : ((c->type == CUST_BOTTLE_VIP || c->type == CUST_BAG_VIP) ? 140 : 255),
                        255,
                        is_rush ? 255 : 150);

        if (!is_rush && bd.splash_triggered) {
            apply_bag_splash(gs, run, c->lane, cx, pts);
        }
    }
}

static void update_drinks_and_collisions(GameState *gs, RunState *run)
{
    int i, j;

    for (i = 0; i < MAX_DRINKS; i++) {
        SlidingDrink *d = &gs->drinks[i];
        int dx;
        if (!d->active) continue;

        d->x_fp += d->vx_fp;
        dx = FROM_FP(d->x_fp);

        if (d->is_rush && ((gfx_frame_count() & 1u) == 0)) {
            int sx, sy, sc;
            project_lane_pos(d->lane, dx + 6, &sx, &sy, &sc);
            (void)sc;
            spawn_particle(gs, 1, sx, sy - 10,
                           game_rand_range(gs, 40, 180),
                           game_rand_range(gs, -160, 40),
                           14, 130, 245, 255);
        } else if (!d->is_rush && (((gfx_frame_count() + (uint32_t)i) & 3u) == 0)) {
            int sx, sy, sc;
            project_lane_pos(d->lane, dx + 8, &sx, &sy, &sc);
            (void)sc;
            spawn_particle(gs, 3, sx, sy - 6,
                           game_rand_range(gs, 30, 110),
                           game_rand_range(gs, -90, -20),
                           10, 180, 244, 255);
        }

        if (d->is_rush) {
            /* Rush wave catches returning empties safely without spawning tips */
            for (j = 0; j < MAX_EMPTIES; j++) {
                ReturningEmpty *e = &gs->empties[j];
                if (!e->active || e->lane != d->lane) continue;
                if (FROM_FP(e->x_fp) >= dx - 16) {
                    int ex = FROM_FP(e->x_fp);
                    e->active = 0;
                    add_shift_score(gs, run, 100);
                    spawn_burst_bar(gs, 1, d->lane, ex, -8, 4, 150, 255, 255);
                    spawn_popup_bar(gs, d->lane, ex, -20, "+100", 140, 255, 255);
                }
            }

            /* Rush sweeps all non-satisfied customers on its lane */
            for (j = 0; j < MAX_CUSTOMERS; j++) {
                Customer *c = &gs->customers[j];
                int cx;
                if (!c->active || c->lane != d->lane || c->state == CSTATE_SATISFIED) continue;
                cx = FROM_FP(c->x_fp);
                if (dx <= cx + 16 && dx + 14 >= cx) {
                    score_and_satisfy_hit(gs, run, c, d->type, 1);
                }
            }
        } else {
            /* Regular drink hits rightmost non-satisfied patron (INCLUDES CSTATE_DRINKING!) */
            int best_j = -1;
            int32_t best_x = -0x7FFFFFFF;
            for (j = 0; j < MAX_CUSTOMERS; j++) {
                Customer *c = &gs->customers[j];
                int cx;
                if (!c->active || c->lane != d->lane) continue;
                if (c->state == CSTATE_SATISFIED) continue;

                cx = FROM_FP(c->x_fp);
                if (dx <= cx + 16 && dx + 14 >= cx) {
                    if (c->x_fp > best_x) {
                        best_x = c->x_fp;
                        best_j = j;
                    }
                }
            }

            if (best_j >= 0) {
                Customer *c = &gs->customers[best_j];
                int cx = FROM_FP(c->x_fp);
                d->active = 0;

                if (d->type == c->wanted_drink) {
                    score_and_satisfy_hit(gs, run, c, d->type, 0);
                } else {
                    /* WRONG DRINK! Resets combo, drains Frost, Spill Fee, angers customer */
                    c->state = CSTATE_ANGRY;
                    c->state_timer = 65;
                    apply_mistake_penalty(gs, run, d->lane, cx, "WRONG!", 0);
                    if (run->lives <= 0) return;
                }
            }
        }

        /* Left-edge spill check */
        if (d->active && dx < BAR_LEFT_X - 8) {
            d->active = 0;
            if (!d->is_rush && !gs->rush_active) {
                apply_mistake_penalty(gs, run, d->lane, BAR_LEFT_X + 8, "SPILL!", 1);
                if (run->lives <= 0) return;
            }
        }
    }
}

static void update_customers(GameState *gs, RunState *run)
{
    int i;
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        Customer *c = &gs->customers[i];
        int cx;
        if (!c->active) continue;

        c->anim_tick++;
        if (c->hit_flash > 0) c->hit_flash--;

        if (c->state == CSTATE_SATISFIED) {
            c->x_fp += c->vx_fp;
            if (FROM_FP(c->x_fp) < -24) {
                c->active = 0;
            }
            continue;
        }

        if (c->stun_timer > 0) {
            c->stun_timer--;
            continue;
        }

        if (c->state == CSTATE_DRINKING) {
            c->state_timer--;
            if (c->state_timer <= 0) {
                c->state = CSTATE_WALKING;
            }
            continue;
        }

        if (c->state == CSTATE_ANGRY) {
            c->x_fp += (c->vx_fp * 145) / 100;
            c->state_timer--;
            if (c->state_timer <= 0) {
                c->state = CSTATE_WALKING;
            }
        } else {
            c->x_fp += c->vx_fp;
        }

        cx = FROM_FP(c->x_fp);
        if ((c->state == CSTATE_ANGRY || cx >= 180) &&
            (((gfx_frame_count() + (uint32_t)i) & 3u) == 0)) {
            int sx, sy, sc;
            project_lane_pos(c->lane, cx - 6, &sx, &sy, &sc);
            (void)sc;
            spawn_particle(gs, 4, sx, sy - 16,
                           game_rand_range(gs, -120, -30),
                           game_rand_range(gs, -140, -50),
                           11, 255, 96, 32);
        }

        if (cx >= BAR_RIGHT_X - 14) {
            /* Customer reached the tap end -> tap breach costs a life */
            lose_life(gs, run, c->lane, BAR_RIGHT_X - 12, "TOO SLOW!");
            if (run->lives <= 0) return;
        }
    }
}

static void update_empties_and_tips(GameState *gs, RunState *run)
{
    int i;
    int bx = FROM_FP(gs->bartender_x_fp);

    for (i = 0; i < MAX_EMPTIES; i++) {
        ReturningEmpty *e = &gs->empties[i];
        int ex;
        int caught = 0;
        if (!e->active) continue;

        e->x_fp += e->vx_fp;
        e->spin_tick++;
        ex = FROM_FP(e->x_fp);

        /* Magnetic Foam Catch buff catches empties cleanly near tap */
        {
            int shield_caught = 0;
            if (gs->foam_catch_timer > 0 && ex >= BAR_RIGHT_X - 56) {
                caught = 1;
            } else if (e->lane == (uint8_t)gs->lane && ex >= bx - 22 && ex <= bx + 26) {
                caught = 1;
            } else if (gs->foam_shield_charges > 0 && ex >= BAR_RIGHT_X - 6) {
                caught = 1;
                shield_caught = 1;
            }

            if (caught) {
                int sx, sy, sc;
                project_lane_pos(e->lane, ex, &sx, &sy, &sc);
                (void)sc;
                e->active = 0;
                rl_on_empty_caught(run);
                add_shift_score(gs, run, 50);
                add_frost(gs, run, 4);
                audio_play_sfx(SFX_EMPTY_CATCH, sx, game_rand_range(gs, -40, 40));
                if (shield_caught) {
                    gs->foam_shield_charges = 0;
                    trigger_flash(gs, 8, 70, 235, 145);
                    spawn_burst_bar(gs, 0, e->lane, ex, -8, 6, 100, 255, 165);
                    spawn_popup_bar(gs, e->lane, ex, -30, "SHIELD!", 100, 255, 165);
                } else {
                    spawn_burst_bar(gs, 0, e->lane, ex, -8, 4, 220, 255, 220);
                }
                spawn_popup_bar(gs, e->lane, ex, -20, "+50", 180, 255, 190);
                continue;
            }
        }

        if (ex > BAR_RIGHT_X + 8) {
            e->active = 0;
            lose_life(gs, run, e->lane, BAR_RIGHT_X, "DROPPED!");
            if (run->lives <= 0) return;
        } else if (((gfx_frame_count() + (uint32_t)i) & 3u) == 0) {
            int sx, sy, sc;
            project_lane_pos(e->lane, ex - 6, &sx, &sy, &sc);
            (void)sc;
            spawn_particle(gs, 4, sx, sy - 6,
                           game_rand_range(gs, -130, -40),
                           game_rand_range(gs, -100, -25),
                           10, 255, 104, 32);
        }
    }

    for (i = 0; i < MAX_TIPS; i++) {
        CashTip *t = &gs->tips[i];
        int sx, sy, sc;
        if (!t->active) continue;

        t->life_timer--;
        if (t->life_timer <= 0) {
            t->active = 0;
            continue;
        }

        if (t->lane == (uint8_t)gs->lane && (t->x - bx) >= -22 && (t->x - bx) <= 24) {
            uint8_t bk = t->buff_kind;
            t->active = 0;
            project_lane_pos(t->lane, t->x, &sx, &sy, &sc);
            (void)sc;
            gs->celebrate_timer = 14;

            if (bk == BUFF_FROST_ORB) {
                add_frost(gs, run, 25);
                add_shift_score(gs, run, 150);
                audio_play_sfx(SFX_TIP_PICKUP, sx, 120);
                spawn_burst_bar(gs, 1, t->lane, t->x, -8, 6, 120, 248, 255);
                spawn_popup_bar(gs, t->lane, t->x, -26, "ICE +25%!", 120, 248, 255);
            } else if (bk == BUFF_STAR_MULT) {
                gs->star_mult_timer = 250;
                gs->combo_timer = 240;
                audio_play_sfx(SFX_MULT_PING, sx, 160);
                spawn_burst_bar(gs, 1, t->lane, t->x, -8, 6, 255, 240, 110);
                spawn_popup_bar(gs, t->lane, t->x, -26, "2* MULT!", 255, 240, 110);
            } else if (bk == BUFF_FOAM_SHIELD) {
                audio_play_sfx(SFX_FOAM_SPRAY, sx, 80);
                spawn_burst_bar(gs, 0, t->lane, t->x, -8, 6, 100, 255, 165);
                if (gs->foam_shield_charges == 0) {
                    gs->foam_shield_charges = 1;
                    spawn_popup_bar(gs, t->lane, t->x, -26, "SHIELD!", 100, 255, 165);
                } else {
                    add_shift_score(gs, run, 250);
                    spawn_popup_bar(gs, t->lane, t->x, -26, "SHD +250!", 100, 255, 165);
                }
            } else {
                int cash_gain = (int)t->cash_val;
                if (gs->shift_tips_cash + cash_gain > SHIFT_MAX_TIPS_CASH) {
                    cash_gain = SHIFT_MAX_TIPS_CASH - gs->shift_tips_cash;
                }
                bump_combo(gs);
                rl_on_tip_collected(run);
                add_shift_score(gs, run, (uint32_t)t->score_val);
                add_frost(gs, run, 12);

                if (cash_gain > 0) {
                    gs->shift_tips_cash = (int16_t)(gs->shift_tips_cash + cash_gain);
                    run->cash = (int16_t)(run->cash + cash_gain);
                }

                audio_play_sfx(SFX_TIP_PICKUP, sx, 0);
                spawn_burst_bar(gs, 1, t->lane, t->x, -8, 6, 255, 236, 96);
                spawn_popup_bar(gs, t->lane, t->x, -26,
                                (cash_gain >= 2) ? "TIP +$2!" : (cash_gain == 1 ? "TIP +$1!" : "TIP!"),
                                255, 236, 80);
            }
        } else if (((gfx_frame_count() + (uint32_t)i) % 5u) == 0) {
            uint8_t pr = 255, pg = 225, pb = 80;
            project_lane_pos(t->lane, t->x, &sx, &sy, &sc);
            (void)sc;
            if (t->buff_kind == BUFF_FROST_ORB) {
                pr = 100; pg = 245; pb = 255;
            } else if (t->buff_kind == BUFF_STAR_MULT) {
                pr = 255; pg = 246; pb = 150;
            } else if (t->buff_kind == BUFF_FOAM_SHIELD) {
                pr = 90; pg = 255; pb = 165;
            }
            spawn_particle(gs, 3, sx + game_rand_range(gs, -5, 5), sy - 8,
                           game_rand_range(gs, -40, 40),
                           game_rand_range(gs, -160, -90),
                           12, pr, pg, pb);
        }
    }
}

static void update_particles_and_popups(GameState *gs)
{
    int i;
    for (i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &gs->particles[i];
        if (!p->active) continue;
        p->x_fp += p->vx_fp;
        p->y_fp += p->vy_fp;
        p->vy_fp += (p->kind >= 3) ? 6 : 22;
        p->life--;
        if (p->life <= 0) p->active = 0;
    }

    for (i = 0; i < MAX_POPUPS; i++) {
        ScorePopup *sp = &gs->popups[i];
        if (!sp->active) continue;
        if ((sp->life & 1) == 0) sp->y--;
        sp->life--;
        if (sp->life <= 0) sp->active = 0;
    }
}

GameStepResult game_update(GameState *gs, RunState *run)
{
    int active_customers = 0;
    int active_rush_drinks = 0;
    int i;
    BossType boss = rl_current_boss(run);
    ShiftKind skind = (ShiftKind)run->shift_in_night;

    if (input_pressed(BTN_START)) {
        return GAME_STEP_PAUSE;
    }

    if (gs->pour_timer > 0)       gs->pour_timer--;
    if (gs->pour_buf_timer > 0) {
        gs->pour_buf_timer--;
        if (gs->pour_buf_timer == 0) {
            gs->pour_buf_drink = -1;
        }
    }
    if (gs->celebrate_timer > 0)  gs->celebrate_timer--;
    if (gs->flash_timer > 0)      gs->flash_timer--;
    if (gs->happy_bell_timer > 0) gs->happy_bell_timer--;
    if (gs->star_mult_timer > 0)  gs->star_mult_timer--;
    if (gs->foam_catch_timer > 0) gs->foam_catch_timer--;
    rl_tick_timers(run);
    if (gs->combo_timer > 0) {
        gs->combo_timer--;
        if (gs->combo_timer == 0) {
            gs->combo = 0;
        }
    }

    update_particles_and_popups(gs);

    if (gs->life_lost_timer > 0) {
        gs->life_lost_timer--;
        if (run->lives <= 0 && gs->life_lost_timer <= 15) {
            return GAME_STEP_GAME_OVER;
        }
        return GAME_STEP_PLAYING;
    }

    /* Closing bell end beat */
    if (gs->shift_end_timer > 0) {
        gs->shift_end_timer--;
        if (gs->shift_end_timer == 0) {
            return (gs->shift_end_result == 1) ? GAME_STEP_SHIFT_WIN : GAME_STEP_GAME_OVER;
        }
        return GAME_STEP_PLAYING;
    }

    /* Intro banner */
    if (gs->shift_banner_timer > 0) {
        gs->shift_banner_timer--;
        if (input_pressed(BTN_UP)) {
            hop_lane(gs, -1);
        } else if (input_pressed(BTN_DOWN)) {
            hop_lane(gs, 1);
        }
        return GAME_STEP_PLAYING;
    }

    /* Countdown clock */
    if (gs->shift_frames_left > 0) {
        gs->shift_frames_left--;
    }

    handle_player_input(gs, run);
    update_drinks_and_collisions(gs, run);
    if (run->lives <= 0) return GAME_STEP_PLAYING; /* let life_lost_timer run */

    update_customers(gs, run);
    if (run->lives <= 0) return GAME_STEP_PLAYING;

    update_empties_and_tips(gs, run);
    if (run->lives <= 0) return GAME_STEP_PLAYING;

    /* End Ice-Cold Rush cleanly as soon as all Rush drinks exit the bar */
    for (i = 0; i < MAX_DRINKS; i++) {
        if (gs->drinks[i].active && gs->drinks[i].is_rush) {
            active_rush_drinks++;
        }
    }
    if (gs->rush_active && active_rush_drinks == 0) {
        gs->rush_active = 0;
    }

    /* Closing Bell check */
    if (gs->shift_frames_left <= 0) {
        if (!gs->closing_bell_sfx) {
            gs->closing_bell_sfx = 1;
            audio_play_sfx(SFX_BELL_CLOSE, 160, 0);
        }
        if (gs->shift_score >= gs->shift_target) {
            gs->shift_end_result = 1;
            gs->shift_end_timer = 45;
            gs->celebrate_timer = 45;
            audio_play_sfx(SFX_BONUS_WIN, 160, 0);
            spawn_popup_screen(gs, 108, 96, "SHIFT CLEAR!", 130, 255, 160);
        } else {
            gs->shift_end_result = 2;
            gs->shift_end_timer = 45;
            audio_play_sfx(SFX_WRONG_DRINK, 160, -80);
            spawn_popup_screen(gs, 94, 96, "TARGET MISSED!", 255, 100, 90);
        }
        return GAME_STEP_PLAYING;
    }

    /* Clock-paced spawner */
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        if (gs->customers[i].active && gs->customers[i].state != CSTATE_SATISFIED) {
            active_customers++;
        }
    }

    if (gs->shift_frames_left > 50) {
        if (active_customers == 0 && gs->spawn_timer > 12) {
            gs->spawn_timer = 12;
        }
        gs->spawn_timer--;
        if (gs->spawn_timer <= 0) {
            int max_conc;
            if (run->night == 1 && run->shift_in_night == 0) {
                max_conc = 3;
            } else if (run->night == 1 && run->shift_in_night == 1) {
                max_conc = 4;
            } else {
                max_conc = 4 + (int)(run->night / 2) +
                           ((skind == SHIFT_HAPPY_HOUR) ? 1 : 0) +
                           ((boss == BOSS_DOUBLE_SHIFT) ? 2 : 0);
            }
            if (max_conc > 10) max_conc = 10;

            if (active_customers < max_conc) {
                int next_delay;
                if (run->night == 1 && run->shift_in_night == 0) {
                    next_delay = 110;
                } else if (run->night == 1 && run->shift_in_night == 1) {
                    next_delay = 86;
                } else {
                    next_delay = 74 - (int)(run->night * 4) - (int)(run->shift_in_night * 6);
                    if (skind == SHIFT_HAPPY_HOUR) next_delay = (next_delay * 86) / 100;
                    if (boss == BOSS_DOUBLE_SHIFT) next_delay = (next_delay * 78) / 100;
                    else if (boss == BOSS_BOTTLENECK || boss == BOSS_BAG_FESTIVAL) {
                        next_delay = (next_delay * 82) / 100;
                    }
                    next_delay = clamp_int(next_delay, 26, 82);
                }
                spawn_customer(gs, run);
                gs->spawn_timer = (int16_t)game_rand_range(gs, next_delay - 4, next_delay + 6);
            } else {
                gs->spawn_timer = 12;
            }
        }
    }

    return GAME_STEP_PLAYING;
}

/* =========================================================================
 * 2.5D Depth-Sorted Rendering & Safe-Zone HUD (y = 4..248)
 * ========================================================================= */

static void compute_lane_status(const GameState *gs,
                                uint8_t lane_danger[NUM_LANES],
                                uint8_t lane_buff[NUM_LANES])
{
    int i;
    for (i = 0; i < NUM_LANES; i++) {
        lane_danger[i] = 0;
        lane_buff[i] = 0;
    }

    for (i = 0; i < MAX_CUSTOMERS; i++) {
        const Customer *c = &gs->customers[i];
        int cx;
        if (!c->active || c->lane >= NUM_LANES || c->state == CSTATE_SATISFIED) continue;
        cx = FROM_FP(c->x_fp);
        if (cx >= 180 || c->state == CSTATE_ANGRY) {
            lane_danger[c->lane] = 2;
        } else if (cx >= 140 && lane_danger[c->lane] < 1) {
            lane_danger[c->lane] = 1;
        }
    }

    for (i = 0; i < MAX_EMPTIES; i++) {
        const ReturningEmpty *e = &gs->empties[i];
        int ex;
        if (!e->active || e->lane >= NUM_LANES) continue;
        ex = FROM_FP(e->x_fp);
        if (ex >= 175) {
            lane_danger[e->lane] = 2;
        } else if (ex >= 115 && lane_danger[e->lane] < 1) {
            lane_danger[e->lane] = 1;
        }
    }

    for (i = 0; i < MAX_TIPS; i++) {
        const CashTip *t = &gs->tips[i];
        if (!t->active || t->lane >= NUM_LANES) continue;
        if (lane_buff[t->lane] == 0) {
            lane_buff[t->lane] = (uint8_t)(t->buff_kind + 1u);
        }
    }
}

static void draw_scaled_centered(int z, GfxTexSheet sheet, int u, int v, int w, int h,
                                 int anchor_sx, int bottom_sy, int scale_fp,
                                 uint8_t r, uint8_t g, uint8_t b,
                                 int flip_x, int semi_trans)
{
    int dw = (w * scale_fp) >> 8;
    int dh = (h * scale_fp) >> 8;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    gfx_draw_sheet_sprt_scaled(z, sheet, u, v, w, h,
                               anchor_sx - (dw >> 1), bottom_sy - dh,
                               dw, dh, r, g, b, flip_x, semi_trans);
}

static void draw_lane_entities(const GameState *gs, const RunState *run,
                               const uint8_t lane_danger[NUM_LANES], uint32_t tick)
{
    int lane, i;
    int pulse = ((tick & 7u) < 4u) ? (int)(tick & 3u) : (int)(3u - (tick & 3u));
    (void)run;

    for (lane = 0; lane < NUM_LANES; lane++) {
        int z_base = room3d_lane_z_base(lane);
        int z_cust   = z_base + 2;
        int z_shadow = z_base - 1;
        int z_item   = z_base - 2;
        int z_bub    = z_base - 4;
        int z_bart   = z_base - 5;

        /* 1. Customers standing behind the 3D wooden counter */
        for (i = 0; i < MAX_CUSTOMERS; i++) {
            const Customer *c = &gs->customers[i];
            int cx, sx, sy, sc, frame;
            int dw, dh, top_h, draw_x, draw_y;
            uint8_t cr = GFX_COLOR_NEUTRAL, cg = GFX_COLOR_NEUTRAL, cb = GFX_COLOR_NEUTRAL;

            if (!c->active || c->lane != (uint8_t)lane) continue;
            cx = FROM_FP(c->x_fp);
            project_lane_pos(lane, cx, &sx, &sy, &sc);

            if (c->state == CSTATE_ANGRY) {
                frame = SPR_CUST_FRAME_ANGRY;
            } else if (c->state == CSTATE_DRINKING || c->state == CSTATE_SATISFIED) {
                frame = SPR_CUST_FRAME_DRINK;
            } else {
                frame = ((c->anim_tick >> 3) & 1u) ? SPR_CUST_FRAME_WALK2 : SPR_CUST_FRAME_WALK1;
            }

            if (c->hit_flash > 0) {
                cr = 255; cg = 244; cb = 168;
            } else if (c->stun_timer > 0) {
                cr = 180; cg = 210; cb = 255;
            }

            dw = (SPR_CUST_W * sc) >> 8;
            dh = (SPR_CUST_H * sc) >> 8;
            top_h = (8 * sc) >> 8;
            draw_x = sx - (dw >> 1);
            draw_y = sy - top_h - dh + 10;

            /* Pulsing crimson-orange danger halo behind Angry or Critical (bar_x >= 180) enemies */
            if (c->state != CSTATE_SATISFIED && (c->state == CSTATE_ANGRY || cx >= 180)) {
                int halo_sc = (sc * (320 + pulse * 18)) >> 8;
                uint8_t hr = (uint8_t)(116 + pulse * 18);
                uint8_t hg = (uint8_t)(98 + pulse * 14);
                uint8_t hb = (uint8_t)(84 + pulse * 8);
                draw_scaled_centered(z_cust + 1, GFX_TEX_SPRITES,
                                     SPR_DANGER_HALO_U, SPR_DANGER_HALO_V,
                                     SPR_DANGER_HALO_W, SPR_DANGER_HALO_H,
                                     sx, sy - top_h + 2, halo_sc,
                                     hr, hg, hb, 0, GFX_TRANS_ADD);
            }

            gfx_draw_sheet_sprt_scaled(z_cust, GFX_TEX_SPRITES,
                                       SPR_CUST_U(frame), SPR_CUST_V(c->type),
                                       SPR_CUST_W, SPR_CUST_H,
                                       draw_x, draw_y, dw, dh,
                                       cr, cg, cb, 0, GFX_TRANS_NONE);

            /* Overhead order/mood bubble (ANGRY STILL SHOWS wanted drink!) */
            {
                int bu = SPR_BUBBLE_CAN_U;
                int bv = SPR_BUBBLE_CAN_V;
                int bw = (SPR_BUBBLE_W * sc) >> 8;
                int bh = (SPR_BUBBLE_H * sc) >> 8;
                int bx = sx - (bw >> 1);
                int by = draw_y - bh + 2;

                if (c->state == CSTATE_SATISFIED) {
                    bu = SPR_BUBBLE_HAPPY_U;
                    bv = SPR_BUBBLE_HAPPY_V;
                } else if (c->state == CSTATE_ANGRY) {
                    if (c->wanted_drink == DRINK_BOTTLE) {
                        bu = SPR_BUBBLE_ANGRY_BOTTLE_U;
                        bv = SPR_BUBBLE_ANGRY_BOTTLE_V;
                    } else if (c->wanted_drink == DRINK_BAG) {
                        bu = SPR_BUBBLE_ANGRY_BAG_U;
                        bv = SPR_BUBBLE_ANGRY_BAG_V;
                    } else {
                        bu = SPR_BUBBLE_ANGRY_CAN_U;
                        bv = SPR_BUBBLE_ANGRY_CAN_V;
                    }
                } else {
                    int bob = ((tick >> 3) & 1u) ? -1 : 0;
                    by += bob;
                    if (c->wanted_drink == DRINK_BOTTLE) {
                        bu = SPR_BUBBLE_BOTTLE_U;
                        bv = SPR_BUBBLE_BOTTLE_V;
                    } else if (c->wanted_drink == DRINK_BAG) {
                        bu = SPR_BUBBLE_BAG_U;
                        bv = SPR_BUBBLE_BAG_V;
                    } else {
                        bu = SPR_BUBBLE_CAN_U;
                        bv = SPR_BUBBLE_CAN_V;
                    }
                }

                gfx_draw_sheet_sprt_scaled(z_bub, GFX_TEX_SPRITES,
                                           bu, bv, SPR_BUBBLE_W, SPR_BUBBLE_H,
                                           bx, by, bw, bh,
                                           GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                           0, GFX_TRANS_NONE);

                /* Toggleable 10x10 button badge on customer order bubble */
                if (c->state != CSTATE_SATISFIED) {
                    gfx_draw_drink_mini_btn_ex(z_bub - 1, bx + bw - 5, by + bh - 8,
                                               (int)c->wanted_drink, gs->tutorial_mode);
                }

                /* Show multi-HP badge (*2 / *3) for Foodies / Tough Crowd */
                if (c->state != CSTATE_SATISFIED && c->hp > 1) {
                    char hp_str[4];
                    hp_str[0] = '*'; /* Font renders '*' -> multiply sign */
                    hp_str[1] = (char)('0' + (c->hp <= 9 ? c->hp : 9));
                    hp_str[2] = '\0';
                    gfx_draw_text_shadow(z_bub - 1, bx + bw - 2, by + 2, hp_str, 255, 236, 96);
                }
            }
        }

        /* 2. Beneficial Bar Pickups (Gold Tips + Ice Frost Orbs + 2x Stars + Foam Shields) */
        for (i = 0; i < MAX_TIPS; i++) {
            const CashTip *t = &gs->tips[i];
            int sx, sy, sc, surf_y, bob;
            int u = SPR_CASH_TIP_U, v = SPR_CASH_TIP_V, w = SPR_CASH_TIP_W, h = SPR_CASH_TIP_H;
            uint8_t hr = 148, hg = 118, hb = 26;
            uint8_t tr = 255, tg = 236, tb = 96;
            const char *beacon = "$";
            int halo_sc;
            int blen;

            if (!t->active || t->lane != (uint8_t)lane) continue;
            if (t->life_timer < 100 && ((tick >> 2) & 1u)) continue;

            project_lane_pos(lane, t->x, &sx, &sy, &sc);
            surf_y = sy - ((4 * sc) >> 8);
            bob = ((tick >> 3) & 1u) ? -1 : 0;
            room3d_draw_shadow(z_shadow, sx, surf_y, 7, 2);

            if (t->buff_kind == BUFF_FROST_ORB) {
                u = SPR_BUFF_FROST_U; v = SPR_BUFF_FROST_V; w = SPR_BUFF_W; h = SPR_BUFF_H;
                hr = (uint8_t)(34 + pulse * 6); hg = (uint8_t)(132 + pulse * 12); hb = (uint8_t)(166 + pulse * 10);
                tr = 120; tg = 248; tb = 255;
                beacon = "ICE";
            } else if (t->buff_kind == BUFF_STAR_MULT) {
                u = SPR_BUFF_STAR_U; v = SPR_BUFF_STAR_V; w = SPR_BUFF_W; h = SPR_BUFF_H;
                hr = (uint8_t)(158 + pulse * 10); hg = (uint8_t)(138 + pulse * 12); hb = (uint8_t)(54 + pulse * 8);
                tr = 255; tg = 242; tb = 110;
                beacon = "2*";
            } else if (t->buff_kind == BUFF_FOAM_SHIELD) {
                u = SPR_BUFF_SHIELD_U; v = SPR_BUFF_SHIELD_V; w = SPR_BUFF_W; h = SPR_BUFF_H;
                hr = (uint8_t)(34 + pulse * 6); hg = (uint8_t)(148 + pulse * 12); hb = (uint8_t)(88 + pulse * 8);
                tr = 100; tg = 255; tb = 165;
                beacon = "SHD";
            } else {
                hr = (uint8_t)(146 + pulse * 10); hg = (uint8_t)(116 + pulse * 10); hb = (uint8_t)(28 + pulse * 4);
                tr = 255; tg = 236; tb = 96;
                beacon = "$";
            }

            halo_sc = (sc * (260 + pulse * 14)) >> 8;
            draw_scaled_centered(z_shadow, GFX_TEX_SPRITES,
                                 SPR_BUFF_HALO_U, SPR_BUFF_HALO_V,
                                 SPR_BUFF_HALO_W, SPR_BUFF_HALO_H,
                                 sx, surf_y + ((4 * sc) >> 8) + bob, halo_sc,
                                 hr, hg, hb, 0, GFX_TRANS_ADD);

            draw_scaled_centered(z_item, GFX_TEX_SPRITES,
                                 u, v, w, h,
                                 sx, surf_y + bob, sc,
                                 GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                 0, GFX_TRANS_NONE);

            blen = (int)strlen(beacon);
            gfx_draw_text_shadow(z_bub,
                                 sx - blen * 4,
                                 surf_y + bob - ((h * sc) >> 8) - 9,
                                 beacon, tr, tg, tb);
        }

        /* 3. Returning Empties in this lane (Danger Category: Crimson-Orange Hazard Halo) */
        for (i = 0; i < MAX_EMPTIES; i++) {
            const ReturningEmpty *e = &gs->empties[i];
            int sx, sy, sc, surf_y, halo_sc;
            int flip = (int)((e->spin_tick >> 3) & 1u);
            uint8_t hr, hg, hb;
            if (!e->active || e->lane != (uint8_t)lane) continue;

            project_lane_pos(lane, FROM_FP(e->x_fp), &sx, &sy, &sc);
            surf_y = sy - ((4 * sc) >> 8);
            room3d_draw_shadow(z_shadow, sx, surf_y, 7, 2);

            halo_sc = (sc * (256 + pulse * 18)) >> 8;
            hr = (uint8_t)(114 + pulse * 18);
            hg = (uint8_t)(96 + pulse * 14);
            hb = (uint8_t)(84 + pulse * 8);
            draw_scaled_centered(z_shadow, GFX_TEX_SPRITES,
                                 SPR_DANGER_HALO_U, SPR_DANGER_HALO_V,
                                 SPR_DANGER_HALO_W, SPR_DANGER_HALO_H,
                                 sx, surf_y + ((4 * sc) >> 8), halo_sc,
                                 hr, hg, hb, 0, GFX_TRANS_ADD);

            if (e->type == DRINK_BOTTLE) {
                draw_scaled_centered(z_item, GFX_TEX_SPRITES,
                                     SPR_EMPTY_BOTTLE_U, SPR_EMPTY_BOTTLE_V,
                                     SPR_EMPTY_BOTTLE_W, SPR_EMPTY_BOTTLE_H,
                                     sx, surf_y, sc,
                                     GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                     flip, GFX_TRANS_NONE);
            } else {
                draw_scaled_centered(z_item, GFX_TEX_SPRITES,
                                     SPR_EMPTY_CAN_U, SPR_EMPTY_CAN_V,
                                     SPR_EMPTY_CAN_W, SPR_EMPTY_CAN_H,
                                     sx, surf_y, sc,
                                     GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                     flip, GFX_TRANS_NONE);
            }
        }

        /* 4. Sliding Drinks in this lane (Real rescaled Tsingtao Can/Bottle + Bag) */
        for (i = 0; i < MAX_DRINKS; i++) {
            const SlidingDrink *d = &gs->drinks[i];
            int sx, sy, sc, surf_y;
            if (!d->active || d->lane != (uint8_t)lane) continue;

            project_lane_pos(lane, FROM_FP(d->x_fp), &sx, &sy, &sc);
            surf_y = sy - ((4 * sc) >> 8);
            room3d_draw_shadow(z_shadow, sx, surf_y, 6, 2);

            if (d->is_rush) {
                draw_scaled_centered(z_item, GFX_TEX_REAL,
                                     SPR_REAL_CAN_U, SPR_REAL_CAN_V,
                                     SPR_REAL_CAN_W, SPR_REAL_CAN_H,
                                     sx, surf_y, sc,
                                     160, 235, 255, 0, GFX_TRANS_NONE);
                draw_scaled_centered(z_item - 1, GFX_TEX_SPRITES,
                                     ((tick & 2u) ? SPR_FX_ICE1_U : SPR_FX_ICE2_U),
                                     SPR_FX_ICE1_V, SPR_FX_ICE1_W, SPR_FX_ICE1_H,
                                     sx, surf_y - 6, sc,
                                     160, 250, 255, 0, GFX_TRANS_ADD);
            } else if (d->type == DRINK_CAN) {
                draw_scaled_centered(z_item, GFX_TEX_REAL,
                                     SPR_REAL_CAN_U, SPR_REAL_CAN_V,
                                     SPR_REAL_CAN_W, SPR_REAL_CAN_H,
                                     sx, surf_y, sc,
                                     GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                     0, GFX_TRANS_NONE);
            } else if (d->type == DRINK_BOTTLE) {
                draw_scaled_centered(z_item, GFX_TEX_REAL,
                                     SPR_REAL_BOTTLE_U, SPR_REAL_BOTTLE_V,
                                     SPR_REAL_BOTTLE_W, SPR_REAL_BOTTLE_H,
                                     sx, surf_y, sc,
                                     GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                     0, GFX_TRANS_NONE);
            } else {
                draw_scaled_centered(z_item, GFX_TEX_SPRITES,
                                     SPR_BEER_BAG_U, SPR_BEER_BAG_V,
                                     SPR_BEER_BAG_W, SPR_BEER_BAG_H,
                                     sx, surf_y, sc,
                                     GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                     0, GFX_TRANS_NONE);
            }
            if (!d->is_rush) {
                gfx_draw_drink_mini_btn_ex(z_item - 1, sx - 13, surf_y - 12,
                                           (int)d->type, gs->tutorial_mode);
            }
        }

        /* 5. Bartender Mascot (+ protective emerald shield aura when shielded) */
        if (gs->lane == (int8_t)lane) {
            int bx = FROM_FP(gs->bartender_x_fp);
            int sx, sy, sc;
            int u = SPR_MASCOT_IDLE_U, v = SPR_MASCOT_IDLE_V;
            int flip = 0;

            project_lane_pos(lane, bx, &sx, &sy, &sc);

            if (gs->life_lost_timer > 0 && (gs->life_lost_timer & 4)) {
                u = SPR_MASCOT_FOAM_U;
                v = SPR_MASCOT_FOAM_V;
            } else if (gs->celebrate_timer > 0) {
                u = SPR_MASCOT_CELEBRATE_U;
                v = SPR_MASCOT_CELEBRATE_V;
            } else if (gs->pour_timer > 0) {
                u = SPR_MASCOT_POUR_U;
                v = SPR_MASCOT_POUR_V;
            } else if (gs->is_dashing) {
                u = ((tick >> 2) & 1u) ? SPR_MASCOT_DASH2_U : SPR_MASCOT_DASH_U;
                v = SPR_MASCOT_DASH_V;
                flip = (gs->dash_dir > 0) ? 1 : 0;
            } else if ((tick >> 4) & 1u) {
                u = SPR_MASCOT_IDLE2_U;
                v = SPR_MASCOT_IDLE2_V;
            }

            room3d_draw_shadow(z_shadow, sx, sy + 3, 11, 4);
            if (gs->foam_shield_charges > 0) {
                int shd_sc = (sc * (450 + pulse * 20)) >> 8;
                draw_scaled_centered(z_bub, GFX_TEX_SPRITES,
                                     SPR_BUFF_HALO_U, SPR_BUFF_HALO_V,
                                     SPR_BUFF_HALO_W, SPR_BUFF_HALO_H,
                                     sx, sy + 9, shd_sc,
                                     (uint8_t)(38 + pulse * 6),
                                     (uint8_t)(165 + pulse * 14),
                                     (uint8_t)(98 + pulse * 10),
                                     0, GFX_TRANS_ADD);
                draw_scaled_centered(z_bart - 1, GFX_TEX_SPRITES,
                                     SPR_BUFF_SHIELD_U, SPR_BUFF_SHIELD_V,
                                     SPR_BUFF_W, SPR_BUFF_H,
                                     sx - 12, sy - 16, (sc * 150) >> 8,
                                     GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                     0, GFX_TRANS_NONE);
            }
            draw_scaled_centered(z_bart, GFX_TEX_SPRITES,
                                 u, v, SPR_MASCOT_W, SPR_MASCOT_H,
                                 sx, sy + 8, sc,
                                 GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                 flip, GFX_TRANS_NONE);
        }

        /* 6. Flashing Danger Alert (!) badge right above the tap when lane_danger[lane] >= 1 */
        if (lane_danger[lane] >= 1) {
            int level2 = (lane_danger[lane] >= 2);
            int blink_on = level2 ? (((tick >> 2) & 1u) == 0) : (((tick >> 3) & 1u) == 0);
            if (blink_on) {
                int rx, ly, sc, top_h, tap_w, tap_h, badge_x, badge_y;
                room3d_lane_project(lane, BAR_RIGHT_X, &rx, &ly, &sc);
                top_h = (8 * sc) >> 8;
                tap_w = (SPR_BEER_TAP_W * sc) >> 8;
                tap_h = (SPR_BEER_TAP_H * sc) >> 8;
                badge_x = rx + 2 + (tap_w >> 1);
                badge_y = ly - top_h - tap_h + 7 - ((level2 && (tick & 4u)) ? 1 : 0);
                draw_scaled_centered(z_bart - 1, GFX_TEX_SPRITES,
                                     SPR_DANGER_ALERT_U, SPR_DANGER_ALERT_V,
                                     SPR_DANGER_ALERT_W, SPR_DANGER_ALERT_H,
                                     badge_x, badge_y, sc,
                                     level2 ? 185 : 140, level2 ? 148 : 132, 128,
                                     0, GFX_TRANS_NONE);
            }
        }
    }
}

static void draw_safe_zone_hud(const GameState *gs, const RunState *run, uint32_t tick)
{
    int i;
    char buf[20];
    BossType boss = rl_current_boss(run);
    ShiftKind skind = (ShiftKind)run->shift_in_night;

    gfx_set_shake_apply(0);

    /* ---- TOP HUD (y = 4..22) ---- */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 2, GFX_SCREEN_W, 20, 10, 14, 26, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 0, 22, GFX_SCREEN_W, 1, 56, 175, 110, GFX_TRANS_NONE);

    /* Score / Target + Progress Bar (x = 4..102) */
    format_score_target(buf, gs->shift_score, gs->shift_target);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 4, 4, buf,
                         (gs->shift_score >= gs->shift_target) ? 140 : 255,
                         255,
                         (gs->shift_score >= gs->shift_target) ? 160 : 236);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 4, 14, 96, 5, 18, 26, 44, GFX_TRANS_NONE);
    if (gs->shift_target > 0) {
        uint64_t fill_w = ((uint64_t)gs->shift_score * 94u) / gs->shift_target;
        if (fill_w > 94u) fill_w = 94u;
        if (fill_w > 0) {
            gfx_draw_rect(GFX_Z_HUD_TEXT, 5, 15, (int)fill_w, 3,
                          (gs->shift_score >= gs->shift_target) ? 110 : 80,
                          240,
                          (gs->shift_score >= gs->shift_target) ? 140 : 255,
                          GFX_TRANS_NONE);
        }
    }

    /* Countdown Clock & Night/Shift (x = 106..144) */
    format_clock(buf, gs->shift_frames_left);
    {
        int urgent = (gs->shift_frames_left <= 500 && (tick & 8u) != 0);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 108, 4, buf,
                             255, urgent ? 96 : 240, urgent ? 88 : 130);
    }
    buf[0] = 'N';
    buf[1] = (char)('0' + ((run->night / 10) % 10));
    if (run->night < 10) {
        buf[1] = (char)('0' + run->night);
        buf[2] = '-';
        buf[3] = (char)('1' + run->shift_in_night);
        buf[4] = '\0';
    } else {
        buf[2] = (char)('0' + (run->night % 10));
        buf[3] = '-';
        buf[4] = (char)('1' + run->shift_in_night);
        buf[5] = '\0';
    }
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 108, 13, buf, 160, 220, 255);

    /* Lives Cans (x = 152..200) */
    for (i = 0; i < run->lives && i < MAX_LIVES; i++) {
        gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_ENV_UI,
                            SPR_HUD_LIFE_CAN_U, SPR_HUD_LIFE_CAN_V,
                            SPR_HUD_LIFE_CAN_W, SPR_HUD_LIFE_CAN_H,
                            150 + i * 10, 5,
                            GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                            0, GFX_TRANS_NONE);
    }

    /* Yuan Cash & Combo + Combo Timer Bar (x = 204..240) */
    format_cash(buf, run->cash);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 204, 3, buf, 140, 255, 160);
    if (gs->combo >= 2) {
        int pos = 0;
        int cbar_w = ((int)gs->combo_timer * 32) / 240;
        if (cbar_w > 32) cbar_w = 32;
        buf[pos++] = '*';
        pos = append_uint(buf, pos, 8, gs->combo);
        buf[pos] = '\0';
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 204, 11, buf, 255, 236, 96);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 204, 19, 34, 3, 18, 26, 44, GFX_TRANS_NONE);
        if (cbar_w > 0) {
            uint8_t cr = 255;
            uint8_t cg = (gs->combo_timer < 55) ? ((tick & 4u) ? 110 : 190) : 224;
            uint8_t cb = (gs->combo_timer < 55) ? 56 : 72;
            gfx_draw_rect(GFX_Z_HUD_TEXT, 205, 19, cbar_w, 2, cr, cg, cb, GFX_TRANS_NONE);
        }
    }

    /* Frost Gauge (x = 244..316) */
    gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_ENV_UI,
                        SPR_HUD_FROST_ICON_U, SPR_HUD_FROST_ICON_V,
                        SPR_HUD_FROST_ICON_W, SPR_HUD_FROST_ICON_H,
                        242, 5,
                        (gs->frost >= FROST_MAX && (tick & 4u)) ? 220 : GFX_COLOR_NEUTRAL,
                        (gs->frost >= FROST_MAX && (tick & 4u)) ? 248 : GFX_COLOR_NEUTRAL,
                        255, 0, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 256, 5, 58, 12, 16, 28, 48, GFX_TRANS_NONE);
    {
        int fill_px = ((int)gs->frost * 54) / FROST_MAX;
        if (fill_px > 0) {
            uint8_t fr = (gs->frost >= FROST_MAX && (tick & 4u)) ? 220 : 80;
            uint8_t fg = (gs->frost >= FROST_MAX && (tick & 4u)) ? 255 : 210;
            gfx_draw_rect(GFX_Z_HUD_TEXT, 258, 7, fill_px, 8, fr, fg, 255, GFX_TRANS_NONE);
        }
        if (gs->frost >= FROST_MAX && (tick & 8u) == 0) {
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT + 1, 260, 7, "^RUSH", 255, 255, 110);
        }
    }

    /* ---- COASTER STRIP (y = 24..36) ---- */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 23, GFX_SCREEN_W, 13, 12, 16, 30, GFX_TRANS_HALF);
    for (i = 0; i < (int)run->coaster_count && i < RL_MAX_COASTERS; i++) {
        uint8_t cid = run->coasters[i];
        int bounce = (run->coaster_ping[i] > 0) ? -2 : 0;
        gfx_draw_sheet_sprt_scaled(GFX_Z_HUD_TEXT, GFX_TEX_CARDS,
                                   SPR_COASTER_ICON_U(cid), SPR_COASTER_ICON_V(cid),
                                   SPR_COASTER_ICON_W, SPR_COASTER_ICON_H,
                                   6 + i * 15, 24 + bounce, 12, 12,
                                   GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                   0, GFX_TRANS_NONE);
    }

    /* Active Buff Badges + Live Base * Mult badge */
    {
        int pos = 0;
        int mult_on = (gs->happy_bell_timer > 0 || gs->star_mult_timer > 0);
        int shd_on = (gs->foam_shield_charges > 0);
        int next_x = 82;

        if (mult_on) {
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, next_x, 26, "2*BUFF",
                                 255, (tick & 4u) ? 248 : 220, 96);
            next_x += 50;
        }
        if (shd_on) {
            const char *shd_lbl = mult_on ? "SHD" : "SHIELD";
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, next_x, 26, shd_lbl, 100, 255, 165);
            next_x += mult_on ? 28 : 52;
        }
        if (next_x <= 148) {
            if (next_x < 96) next_x = 96;
            if (gs->last_serve_pts > 0) {
                pos = append_uint(buf, pos, 16, gs->last_serve_base);
                buf[pos++] = '*';
                pos = append_uint(buf, pos, 16, gs->last_serve_mult_x10 / 10u);
                if ((gs->last_serve_mult_x10 % 10u) != 0) {
                    buf[pos++] = '.';
                    buf[pos++] = (char)('0' + (gs->last_serve_mult_x10 % 10u));
                }
                buf[pos] = '\0';
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, next_x, 26, buf, 140, 245, 255);
            } else {
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, next_x, 26, "READY", 170, 210, 230);
            }
        }
    }

    /* Boss Twist / Shift Mode Pill (x = 196..316) */
    if (gs->tutorial_mode) {
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 216, 24, 100, 11, 28, 66, 94, GFX_TRANS_NONE);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 234, 26, "TRAINING", 130, 248, 255);
    } else if (boss != BOSS_NONE) {
        const BossDef *bdef = rl_boss_def(boss);
        int bx_pill = (gs->happy_bell_timer > 0 || gs->star_mult_timer > 0) &&
                      (gs->foam_shield_charges > 0) ? 204 : 196;
        gfx_draw_rect(GFX_Z_HUD_BG - 1, bx_pill, 24, 316 - bx_pill, 11, 96, 24, 28, GFX_TRANS_NONE);
        if (bdef) {
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, bx_pill + 3, 26, bdef->name, 255, 220, 120);
        }
    } else if (skind == SHIFT_HAPPY_HOUR) {
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 216, 24, 100, 11, 28, 66, 94, GFX_TRANS_NONE);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 222, 26, "HAPPY HOUR!", 130, 248, 255);
    } else {
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 216, 24, 100, 11, 24, 52, 42, GFX_TRANS_NONE);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 224, 26, "NIGHT SHIFT", 170, 245, 190);
    }

    /* ---- BOTTOM CONTROLS BAR (y = 232..248, ZERO CLIPPING!) ---- */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 232, GFX_SCREEN_W, 17, 10, 14, 24, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 0, 232, GFX_SCREEN_W, 1, 64, 180, 110, GFX_TRANS_NONE);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 3, 233, GLYPH_CROSS);
    gfx_draw_text(GFX_Z_HUD_TEXT, 18, 236, "CAN", 180, 255, 190);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 44, 233, GLYPH_CIRCLE);
    gfx_draw_text(GFX_Z_HUD_TEXT, 59, 236, "BTL", 140, 255, 140);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 85, 233, GLYPH_SQUARE);
    gfx_draw_text(GFX_Z_HUD_TEXT, 100, 236, "BAG", 255, 224, 120);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 126, 233, GLYPH_TRIANGLE);
    gfx_draw_text(GFX_Z_HUD_TEXT, 141, 236, "RUSH",
                  (gs->frost >= FROST_MAX && (tick & 8u) == 0) ? 255 : 130,
                  245,
                  (gs->frost >= FROST_MAX && (tick & 8u) == 0) ? 120 : 255);

    /* L1 & R1 Live Pocket Trick slots */
    for (i = 0; i < RL_MAX_TRICKS; i++) {
        int bx = (i == 0) ? 176 : 246;
        GfxBtnGlyph bglyph = (i == 0) ? GLYPH_L1 : GLYPH_R1;
        int8_t tid = run->tricks[i];

        gfx_draw_btn(GFX_Z_HUD_TEXT, bx, 234, bglyph);

        if (tid != TRICK_NONE) {
            const TrickDef *tdef = rl_trick_def(tid);
            char short_name[6];
            int k;
            gfx_draw_sheet_sprt_scaled(GFX_Z_HUD_TEXT, GFX_TEX_CARDS,
                                       SPR_TRICK_ICON_U(tid), SPR_TRICK_ICON_V(tid),
                                       SPR_TRICK_ICON_W, SPR_TRICK_ICON_H,
                                       bx + 19, 234, 12, 12,
                                       GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                       0, GFX_TRANS_NONE);
            if (tdef && tdef->name) {
                for (k = 0; k < 4 && tdef->name[k]; k++) {
                    short_name[k] = tdef->name[k];
                }
                short_name[k] = '\0';
                gfx_draw_text(GFX_Z_HUD_TEXT, bx + 33, 236, short_name, 255, 236, 120);
            }
        } else {
            gfx_draw_text(GFX_Z_HUD_TEXT, bx + 20, 236, "---", 100, 120, 140);
        }
    }

    gfx_set_shake_apply(1);
}

void game_draw(const GameState *gs, const RunState *run)
{
    uint32_t tick = gfx_frame_count();
    BossType boss = rl_current_boss(run);
    uint8_t lane_danger[NUM_LANES];
    uint8_t lane_buff[NUM_LANES];
    int i;

    compute_lane_status(gs, lane_danger, lane_buff);

    gfx_set_shake_apply(1);
    room3d_draw_barroom_ex(run->night, tick, gs->lane, 1, 1,
                           (boss == BOSS_BLACKOUT) ? 1 : 0,
                           lane_danger, lane_buff);

    draw_lane_entities(gs, run, lane_danger, tick);

    /* Particles */
    for (i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &gs->particles[i];
        int px, py, u, v, trans;
        if (!p->active) continue;
        if (p->kind == 3 || p->kind == 4) {
            px = FROM_FP(p->x_fp) - 4;
            py = FROM_FP(p->y_fp) - 4;
            if (p->kind == 3) {
                u = (p->life & 4) ? SPR_FX_ICE1_U : SPR_FX_ICE2_U;
                v = SPR_FX_ICE1_V;
            } else {
                u = (p->life & 4) ? SPR_FX_FOAM1_U : SPR_FX_FOAM2_U;
                v = SPR_FX_FOAM1_V;
            }
            gfx_draw_sheet_sprt_scaled(GFX_Z_VFX, GFX_TEX_SPRITES,
                                       u, v, 16, 16, px, py, 8, 8,
                                       p->r, p->g, p->b, 0, GFX_TRANS_ADD);
            continue;
        }
        px = FROM_FP(p->x_fp) - 8;
        py = FROM_FP(p->y_fp) - 8;
        if (p->kind == 1) {
            u = (p->life & 4) ? SPR_FX_ICE1_U : SPR_FX_ICE2_U;
            v = SPR_FX_ICE1_V;
            trans = GFX_TRANS_ADD;
        } else if (p->kind == 2) {
            u = SPR_FX_SMASH_U;
            v = SPR_FX_SMASH_V;
            trans = GFX_TRANS_NONE;
        } else {
            u = (p->life & 4) ? SPR_FX_FOAM1_U : SPR_FX_FOAM2_U;
            v = SPR_FX_FOAM1_V;
            trans = GFX_TRANS_NONE;
        }
        gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                            u, v, 16, 16, px, py, p->r, p->g, p->b, 0, trans);
    }

    /* Non-overlapping Floating Score Popups */
    for (i = 0; i < MAX_POPUPS; i++) {
        const ScorePopup *sp = &gs->popups[i];
        if (!sp->active) continue;
        gfx_draw_text_shadow(GFX_Z_POPUP, sp->x, sp->y, sp->text, sp->r, sp->g, sp->b);
    }

    /* Shift Start Intro Banner */
    if (gs->shift_banner_timer > 0) {
        char title_buf[24];
        int pos = 0;
        gfx_set_shake_apply(0);
        gfx_draw_rect(GFX_Z_HUD_BG, 16, 88, 288, 64, 12, 22, 38, GFX_TRANS_HALF);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 16, 88, 288, 2, 255, 220, 80, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 16, 150, 288, 2, 255, 220, 80, GFX_TRANS_NONE);

        memcpy(title_buf, "NIGHT ", 6);
        pos = 6;
        pos = append_uint(title_buf, pos, 23, run->night);
        title_buf[pos++] = '-';
        title_buf[pos++] = ' ';
        if (boss != BOSS_NONE) {
            memcpy(title_buf + pos, "BOSS!", 5);
            pos += 5;
        } else if (run->shift_in_night == 1) {
            memcpy(title_buf + pos, "HAPPY HR", 8);
            pos += 8;
        } else {
            memcpy(title_buf + pos, "SHIFT 1", 7);
            pos += 7;
        }
        title_buf[pos] = '\0';

        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 96, title_buf, 255, 236, 96);
        if (boss != BOSS_NONE) {
            const BossDef *bdef = rl_boss_def(boss);
            if (bdef) {
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 113, bdef->name, 255, 130, 110);
            }
        } else {
            char tgt[20];
            memcpy(tgt, "TARGET: ", 8);
            pos = append_uint(tgt, 8, 19, gs->shift_target);
            tgt[pos] = '\0';
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 113, tgt, 140, 248, 255);
        }
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 24, 132, "RED !=DANGER/CATCH", 255, 130, 100);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 184, 132, "GOLD/CYAN=BUFF", 255, 236, 110);
        gfx_set_shake_apply(1);
    }

    /* Full-screen flash overlay */
    if (gs->flash_timer > 0) {
        int alpha = gs->flash_timer * 14;
        if (alpha > 192) alpha = 192;
        gfx_set_shake_apply(0);
        gfx_draw_rect(GFX_Z_OVERLAY, 0, 0, GFX_SCREEN_W, GFX_SCREEN_H,
                      (uint8_t)((gs->flash_r * alpha) >> 8),
                      (uint8_t)((gs->flash_g * alpha) >> 8),
                      (uint8_t)((gs->flash_b * alpha) >> 8),
                      GFX_TRANS_ADD);
        gfx_set_shake_apply(1);
    }

    draw_safe_zone_hud(gs, run, tick);
}

/* =========================================================================
 * Interactive 5-Step Guided Tutorial
 * ========================================================================= */

static void tutorial_clear_entities(GameState *gs)
{
    memset(gs->customers, 0, sizeof(gs->customers));
    memset(gs->drinks, 0, sizeof(gs->drinks));
    memset(gs->empties, 0, sizeof(gs->empties));
    memset(gs->tips, 0, sizeof(gs->tips));
    gs->rush_active = 0;
    gs->buffered_drink = -1;
    gs->is_auto_sprinting = 0;
    gs->pour_buf_drink = -1;
    gs->pour_buf_timer = 0;
}

static void tutorial_spawn_patron(GameState *gs, int lane, uint8_t ctype,
                                  uint8_t drink, int vx_fp)
{
    int i;
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        if (!gs->customers[i].active) {
            Customer *c = &gs->customers[i];
            memset(c, 0, sizeof(*c));
            c->active = 1;
            c->lane = (uint8_t)lane;
            c->type = ctype;
            c->wanted_drink = drink;
            c->hp = 1;
            c->state = CSTATE_WALKING;
            c->x_fp = TO_FP(BAR_LEFT_X + 18);
            c->vx_fp = vx_fp;
            return;
        }
    }
}

static int tutorial_count_unsat_patrons(const GameState *gs)
{
    int i, count = 0;
    for (i = 0; i < MAX_CUSTOMERS; i++) {
        if (gs->customers[i].active && gs->customers[i].state != CSTATE_SATISFIED) {
            count++;
        }
    }
    return count;
}

static int tutorial_count_active_empties(const GameState *gs)
{
    int i, count = 0;
    for (i = 0; i < MAX_EMPTIES; i++) {
        if (gs->empties[i].active) count++;
    }
    return count;
}

static int tutorial_count_active_tips(const GameState *gs)
{
    int i, count = 0;
    for (i = 0; i < MAX_TIPS; i++) {
        if (gs->tips[i].active) count++;
    }
    return count;
}

static void tutorial_setup_step(GameState *gs, RunState *run, uint8_t step)
{
    int i;
    tutorial_clear_entities(gs);
    gs->tutorial_step = step;
    gs->tutorial_step_timer = 0;
    gs->bartender_x_fp = TO_FP(BARTENDER_TAP_X);

    switch (step) {
        case 0:
            gs->lane = 0;
            tutorial_spawn_patron(gs, 2, CUST_REGULAR, DRINK_CAN, 42);
            break;

        case 1:
            gs->lane = 1;
            gs->shift_score = 0u;
            spawn_empty(gs, run, 1, 52, DRINK_CAN, 0);
            break;

        case 2:
            gs->lane = 0;
            tutorial_spawn_patron(gs, 1, CUST_BOTTLE_VIP, DRINK_BOTTLE, 44);
            tutorial_spawn_patron(gs, 2, CUST_BAG_VIP, DRINK_BAG, 44);
            break;

        case 3:
            gs->lane = 1;
            gs->shift_tips_spawned = 0;
            gs->shift_tips_cash = 0;
            gs->shift_buffs_spawned = 0;
            spawn_buff(gs, run, 1, 132, BUFF_GOLD_TIP, 1, 150);
            spawn_buff(gs, run, 2, 102, BUFF_FROST_ORB, 0, 150);
            for (i = 0; i < MAX_TIPS; i++) {
                if (gs->tips[i].active) {
                    gs->tips[i].life_timer = 1800;
                }
            }
            break;

        case 4:
            gs->lane = 1;
            gs->frost = FROST_MAX;
            gs->frost_ready_sfx = 1;
            tutorial_spawn_patron(gs, 0, CUST_REGULAR, DRINK_CAN, 34);
            tutorial_spawn_patron(gs, 1, CUST_REGULAR, DRINK_CAN, 34);
            tutorial_spawn_patron(gs, 2, CUST_REGULAR, DRINK_CAN, 34);
            break;

        case 5:
        default:
            gs->tutorial_step = 5;
            gs->celebrate_timer = 60;
            audio_play_sfx(SFX_BONUS_WIN, 160, 0);
            break;
    }
}

void game_init_tutorial(GameState *gs, RunState *run)
{
    uint32_t hi = run->high_score;
    rl_run_init(run, 0x19960715u, hi);
    run->lives = 4;

    memset(gs, 0, sizeof(*gs));
    gs->tutorial_mode = 1;
    gs->shift_target = 9999u;
    gs->shift_frames_left = 999;
    gs->buffered_drink = -1;
    gs->pour_buf_drink = -1;
    gs->rng_state = 0x19960715u;

    tutorial_setup_step(gs, run, 0);
}

GameStepResult game_update_tutorial(GameState *gs, RunState *run)
{
    int i, active_rush_drinks = 0;

    run->lives = 4;
    gs->shift_frames_left = 999;

    if (gs->tutorial_step < 5 && input_pressed(BTN_START)) {
        return GAME_STEP_PAUSE;
    }

    if (gs->pour_timer > 0)       gs->pour_timer--;
    if (gs->pour_buf_timer > 0) {
        gs->pour_buf_timer--;
        if (gs->pour_buf_timer == 0) {
            gs->pour_buf_drink = -1;
        }
    }
    if (gs->celebrate_timer > 0)  gs->celebrate_timer--;
    if (gs->flash_timer > 0)      gs->flash_timer--;
    if (gs->happy_bell_timer > 0) gs->happy_bell_timer--;
    if (gs->star_mult_timer > 0)  gs->star_mult_timer--;
    if (gs->foam_catch_timer > 0) gs->foam_catch_timer--;
    rl_tick_timers(run);
    if (gs->combo_timer > 0) {
        gs->combo_timer--;
        if (gs->combo_timer == 0) gs->combo = 0;
    }

    update_particles_and_popups(gs);

    if (gs->life_lost_timer > 0) {
        gs->life_lost_timer--;
        return GAME_STEP_PLAYING;
    }

    if (gs->tutorial_step >= 5) {
        gs->tutorial_step_timer++;
        if (gs->tutorial_step_timer >= 18) {
            if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) {
                return GAME_STEP_SHIFT_WIN;
            }
            if (input_pressed(BTN_CIRCLE)) {
                return GAME_STEP_GAME_OVER;
            }
        }
        return GAME_STEP_PLAYING;
    }

    handle_player_input(gs, run);
    update_drinks_and_collisions(gs, run);
    update_customers(gs, run);
    update_empties_and_tips(gs, run);
    run->lives = 4;

    if (gs->tutorial_step == 3) {
        for (i = 0; i < MAX_TIPS; i++) {
            if (gs->tips[i].active && gs->tips[i].life_timer < 300) {
                gs->tips[i].life_timer = 900;
            }
        }
    }

    for (i = 0; i < MAX_DRINKS; i++) {
        if (gs->drinks[i].active && gs->drinks[i].is_rush) {
            active_rush_drinks++;
        }
    }
    if (gs->rush_active && active_rush_drinks == 0) {
        gs->rush_active = 0;
    }

    switch (gs->tutorial_step) {
        case 0:
            if (tutorial_count_unsat_patrons(gs) == 0) {
                gs->tutorial_step_timer++;
                if (gs->tutorial_step_timer >= 24) {
                    audio_play_sfx(SFX_BONUS_WIN, 160, 0);
                    spawn_popup_screen(gs, 114, 104, "NICE POUR!", 130, 255, 160);
                    tutorial_setup_step(gs, run, 1);
                }
            }
            break;

        case 1:
            if (gs->shift_score > 0u) {
                gs->tutorial_step_timer++;
                if (gs->tutorial_step_timer >= 24) {
                    audio_play_sfx(SFX_BONUS_WIN, 160, 0);
                    spawn_popup_screen(gs, 108, 104, "GREAT CATCH!", 130, 255, 160);
                    tutorial_setup_step(gs, run, 2);
                }
            } else if (tutorial_count_active_empties(gs) == 0) {
                spawn_popup_screen(gs, 112, 104, "CATCH ON BAR!", 255, 190, 96);
                spawn_empty(gs, run, (int)gs->lane, 56, DRINK_CAN, 0);
            }
            break;

        case 2:
            if (tutorial_count_unsat_patrons(gs) == 0) {
                gs->tutorial_step_timer++;
                if (gs->tutorial_step_timer >= 24) {
                    audio_play_sfx(SFX_BONUS_WIN, 160, 0);
                    spawn_popup_screen(gs, 110, 104, "VIP SERVED!", 255, 224, 120);
                    tutorial_setup_step(gs, run, 3);
                }
            }
            break;

        case 3:
            if (tutorial_count_active_tips(gs) == 0) {
                gs->tutorial_step_timer++;
                if (gs->tutorial_step_timer >= 24) {
                    audio_play_sfx(SFX_BONUS_WIN, 160, 0);
                    spawn_popup_screen(gs, 108, 104, "BUFFS READY!", 120, 248, 255);
                    tutorial_setup_step(gs, run, 4);
                }
            }
            break;

        case 4:
            if (gs->rush_active && gs->tutorial_step_timer == 0) {
                gs->tutorial_step_timer = 1;
            }
            if (gs->tutorial_step_timer == 0) {
                gs->frost = FROST_MAX;
                gs->frost_ready_sfx = 1;
                if (tutorial_count_unsat_patrons(gs) == 0) {
                    spawn_popup_screen(gs, 114, 104, "PRESS RUSH!", 120, 248, 255);
                    tutorial_spawn_patron(gs, 0, CUST_REGULAR, DRINK_CAN, 34);
                    tutorial_spawn_patron(gs, 1, CUST_REGULAR, DRINK_CAN, 34);
                    tutorial_spawn_patron(gs, 2, CUST_REGULAR, DRINK_CAN, 34);
                }
            } else {
                gs->tutorial_step_timer++;
                if (!gs->rush_active && tutorial_count_unsat_patrons(gs) == 0 &&
                    gs->tutorial_step_timer >= 28) {
                    tutorial_setup_step(gs, run, 5);
                }
            }
            break;

        default:
            break;
    }

    return GAME_STEP_PLAYING;
}

void game_draw_tutorial(const GameState *gs, const RunState *run)
{
    uint32_t tick = gfx_frame_count();
    game_draw(gs, run);

    gfx_set_shake_apply(0);
    if (gs->tutorial_step < 5) {
        static const char *const k_step_titles[5] = {
            "STEP 1/5: HOP LANES & POUR CAN!",
            "STEP 2/5: CATCH RED EMPTY!",
            "STEP 3/5: MATCH DRINK ORDER!",
            "STEP 4/5: GRAB GLOWING BUFFS!",
            "STEP 5/5: 100% ICE-COLD RUSH!"
        };
        gfx_draw_rect(GFX_Z_HUD_BG, 16, 38, 288, 30, 10, 18, 36, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 16, 38, 288, 1, 100, 230, 255, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 16, 67, 288, 1, 255, 224, 96, GFX_TRANS_NONE);

        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 41, k_step_titles[gs->tutorial_step], 255, 236, 96);

        switch (gs->tutorial_step) {
            case 0:
                gfx_draw_btn(GFX_Z_HUD_TEXT, 44, 51, GLYPH_DPAD);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 61, 54, "CHANGE LANE", 190, 235, 255);
                gfx_draw_btn(GFX_Z_HUD_TEXT, 168, 51, GLYPH_CROSS);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 185, 54, "SERVE CAN", 160, 255, 175);
                break;

            case 1:
                gfx_draw_btn(GFX_Z_HUD_TEXT, 28, 51, GLYPH_DPAD);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 46, 54, "MEET FLASHING EMPTY ON BAR!", 255, 165, 140);
                break;

            case 2:
                gfx_draw_btn(GFX_Z_HUD_TEXT, 36, 51, GLYPH_CIRCLE);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 53, 54, "BOTTLE", 140, 255, 145);
                gfx_draw_btn(GFX_Z_HUD_TEXT, 126, 51, GLYPH_SQUARE);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 143, 54, "BEER BAG", 255, 224, 120);
                gfx_draw_btn(GFX_Z_HUD_TEXT, 226, 51, GLYPH_DPAD);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 243, 54, "LANE", 180, 235, 255);
                break;

            case 3:
                gfx_draw_btn(GFX_Z_HUD_TEXT, 34, 51, GLYPH_DPAD);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 52, 54, "HOLD LEFT FOR GOLD & ICE!", 255, 236, 120);
                break;

            case 4:
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 50, 54, "FROST FULL -> PRESS",
                                     130, 248, 255);
                gfx_draw_btn(GFX_Z_HUD_TEXT, 208, 51, GLYPH_TRIANGLE);
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 225, 54, "RUSH!",
                                     (tick & 8u) ? 255 : 150, 255, 130);
                break;

            default:
                break;
        }
    } else {
        gfx_draw_rect(GFX_Z_OVERLAY + 2, 24, 72, 272, 96, 10, 18, 34, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_OVERLAY + 1, 24, 72, 272, 2, 255, 224, 96, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_OVERLAY + 1, 24, 166, 272, 2, 255, 224, 96, GFX_TRANS_NONE);

        gfx_draw_text_centered(GFX_Z_OVERLAY, 84, "TRAINING COMPLETE!", 255, 236, 96);
        gfx_draw_text_centered(GFX_Z_OVERLAY, 100, "READY FOR QINGDAO NIGHTS!", 130, 248, 255);
        gfx_draw_text_centered(GFX_Z_OVERLAY, 116, "RED !=DANGER   GOLD/CYAN=BUFF", 210, 225, 240);

        gfx_draw_btn(GFX_Z_OVERLAY, 52, 136, GLYPH_CROSS);
        gfx_draw_text_shadow(GFX_Z_OVERLAY, 69, 139, "START RUN", 160, 255, 175);
        gfx_draw_btn(GFX_Z_OVERLAY, 174, 136, GLYPH_CIRCLE);
        gfx_draw_text_shadow(GFX_Z_OVERLAY, 191, 139, "TITLE", 210, 225, 245);
    }
    gfx_set_shake_apply(1);
}
