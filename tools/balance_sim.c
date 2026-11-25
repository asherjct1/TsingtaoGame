/*
 * TSINGTAO EXPRESS '96 — High-Speed Multi-Threaded Headless Balance Simulator
 *
 * Compiles directly against the real PS1 game logic (src/game.c, src/roguelike.c,
 * src/bonus.c) with thread-local zero-overhead stubs for PS1 hardware APIs.
 * Runs hundreds of parallel full runs (Night 1..20+) across realistic human
 * reaction-time & skill profiles in a few seconds with 100% bit-exact math.
 */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "game.h"
#include "roguelike.h"
#include "bonus.h"
#include "input.h"
#include "gfx.h"
#include "room3d.h"
#include "audio.h"

/* =========================================================================
 * Thread-Local PS1 Hardware Stubs (Zero Global Contention Across Workers)
 * ========================================================================= */

typedef struct {
    uint32_t frame;
    uint16_t held;
    uint16_t prev_held;
    int      drink_glyphs;
} SimTls;

static _Thread_local SimTls g_tls;

static void sim_set_input(uint16_t next_held)
{
    g_tls.prev_held = g_tls.held;
    g_tls.held      = next_held;
}

static void sim_reset_tls(uint32_t start_frame)
{
    g_tls.frame        = start_frame;
    g_tls.held         = 0;
    g_tls.prev_held    = 0;
    g_tls.drink_glyphs = 1;
}

/* input.h stubs */
void input_init(void) {}
void input_update(void) {}
int  input_connected(void) { return 1; }
uint16_t input_raw_held(void) { return g_tls.held; }
int input_held(uint16_t btn) { return (g_tls.held & btn) != 0; }
int input_pressed(uint16_t btn) { return ((g_tls.held & ~g_tls.prev_held) & btn) != 0; }
int input_released(uint16_t btn) { return ((~g_tls.held & g_tls.prev_held) & btn) != 0; }

/* audio.h stubs */
void audio_init(void) {}
void audio_play_sfx(int sfx_id, int pan_x, int pitch_delta) { (void)sfx_id; (void)pan_x; (void)pitch_delta; }
void audio_stop_all_sfx(void) {}
void audio_bgm_start(void) {}
void audio_bgm_stop(void) {}
void audio_bgm_pause(int paused) { (void)paused; }
void audio_bgm_set_muted(int muted) { (void)muted; }
int  audio_bgm_is_muted(void) { return 0; }
int  audio_bgm_is_playing(void) { return 1; }
int  audio_bgm_is_available(void) { return 1; }
void audio_bgm_update(void) {}

/* gfx.h stubs */
void gfx_init(void) {}
void gfx_begin_frame(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void gfx_end_frame(void) {}
uint32_t gfx_frame_count(void) { return g_tls.frame; }
void gfx_set_shake(int frames, int magnitude) { (void)frames; (void)magnitude; }
void gfx_set_shake_apply(int apply) { (void)apply; }
void gfx_get_shake(int *dx, int *dy) { if (dx) *dx = 0; if (dy) *dy = 0; }
uint16_t gfx_get_tpage(GfxTexSheet s, int abr) { (void)s; (void)abr; return 0; }
uint16_t gfx_get_clut(GfxTexSheet s) { (void)s; return 0; }
void gfx_draw_sky_grad(int z, int y0, int y1, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1)
{ (void)z; (void)y0; (void)y1; (void)r0; (void)g0; (void)b0; (void)r1; (void)g1; (void)b1; }
void gfx_draw_grad_rect(int z, int x, int y, int w, int h, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, int st)
{ (void)z; (void)x; (void)y; (void)w; (void)h; (void)r0; (void)g0; (void)b0; (void)r1; (void)g1; (void)b1; (void)st; }
void gfx_draw_rect(int z, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, int st)
{ (void)z; (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b; (void)st; }
void gfx_draw_sprt(int z, uint16_t tp, uint16_t cl, int u, int v, int w, int h, int x, int y, uint8_t r, uint8_t g, uint8_t b, int fx, int st)
{ (void)z; (void)tp; (void)cl; (void)u; (void)v; (void)w; (void)h; (void)x; (void)y; (void)r; (void)g; (void)b; (void)fx; (void)st; }
void gfx_draw_sprt_scaled(int z, uint16_t tp, uint16_t cl, int u, int v, int w, int h, int x, int y, int dw, int dh, uint8_t r, uint8_t g, uint8_t b, int fx, int st)
{ (void)z; (void)tp; (void)cl; (void)u; (void)v; (void)w; (void)h; (void)x; (void)y; (void)dw; (void)dh; (void)r; (void)g; (void)b; (void)fx; (void)st; }
void gfx_draw_sheet_sprt(int z, GfxTexSheet s, int u, int v, int w, int h, int x, int y, uint8_t r, uint8_t g, uint8_t b, int fx, int st)
{ (void)z; (void)s; (void)u; (void)v; (void)w; (void)h; (void)x; (void)y; (void)r; (void)g; (void)b; (void)fx; (void)st; }
void gfx_draw_sheet_sprt_scaled(int z, GfxTexSheet s, int u, int v, int w, int h, int x, int y, int dw, int dh, uint8_t r, uint8_t g, uint8_t b, int fx, int st)
{ (void)z; (void)s; (void)u; (void)v; (void)w; (void)h; (void)x; (void)y; (void)dw; (void)dh; (void)r; (void)g; (void)b; (void)fx; (void)st; }
void gfx_draw_poly_f4(int z, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint8_t r, uint8_t g, uint8_t b, int st)
{ (void)z; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3; (void)r; (void)g; (void)b; (void)st; }
void gfx_draw_poly_g4(int z, int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0, int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1, int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2, int x3, int y3, uint8_t r3, uint8_t g3, uint8_t b3, int st)
{ (void)z; (void)x0; (void)y0; (void)r0; (void)g0; (void)b0; (void)x1; (void)y1; (void)r1; (void)g1; (void)b1; (void)x2; (void)y2; (void)r2; (void)g2; (void)b2; (void)x3; (void)y3; (void)r3; (void)g3; (void)b3; (void)st; }
void gfx_draw_poly_ft4(int z, GfxTexSheet s, int u0, int v0, int u1, int v1, int u2, int v2, int u3, int v3, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint8_t r, uint8_t g, uint8_t b, int st)
{ (void)z; (void)s; (void)u0; (void)v0; (void)u1; (void)v1; (void)u2; (void)v2; (void)u3; (void)v3; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)x3; (void)y3; (void)r; (void)g; (void)b; (void)st; }
void gfx_draw_poly_gt4(int z, GfxTexSheet s, int u0, int v0, int u1, int v1, int u2, int v2, int u3, int v3, int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0, int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1, int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2, int x3, int y3, uint8_t r3, uint8_t g3, uint8_t b3, int st)
{ (void)z; (void)s; (void)u0; (void)v0; (void)u1; (void)v1; (void)u2; (void)v2; (void)u3; (void)v3; (void)x0; (void)y0; (void)r0; (void)g0; (void)b0; (void)x1; (void)y1; (void)r1; (void)g1; (void)b1; (void)x2; (void)y2; (void)r2; (void)g2; (void)b2; (void)x3; (void)y3; (void)r3; (void)g3; (void)b3; (void)st; }
void gfx_draw_text(int z, int x, int y, const char *str, uint8_t r, uint8_t g, uint8_t b)
{ (void)z; (void)x; (void)y; (void)str; (void)r; (void)g; (void)b; }
void gfx_draw_text_shadow(int z, int x, int y, const char *str, uint8_t r, uint8_t g, uint8_t b)
{ (void)z; (void)x; (void)y; (void)str; (void)r; (void)g; (void)b; }
void gfx_draw_text_centered(int z, int y, const char *str, uint8_t r, uint8_t g, uint8_t b)
{ (void)z; (void)y; (void)str; (void)r; (void)g; (void)b; }
void gfx_draw_int(int z, int x, int y, int32_t v, int md, uint8_t r, uint8_t g, uint8_t b)
{ (void)z; (void)x; (void)y; (void)v; (void)md; (void)r; (void)g; (void)b; }
void gfx_set_btn_style(GfxBtnStyle style) { (void)style; }
GfxBtnStyle gfx_get_btn_style(void) { return BTN_STYLE_PS1; }
void gfx_set_drink_glyphs(int enabled) { g_tls.drink_glyphs = enabled; }
int  gfx_get_drink_glyphs(void) { return g_tls.drink_glyphs; }
void gfx_draw_btn(int z, int x, int y, GfxBtnGlyph glyph) { (void)z; (void)x; (void)y; (void)glyph; }
void gfx_draw_drink_mini_btn(int z, int x, int y, int dt) { (void)z; (void)x; (void)y; (void)dt; }
void gfx_draw_drink_mini_btn_ex(int z, int x, int y, int dt, int f) { (void)z; (void)x; (void)y; (void)dt; (void)f; }

/* room3d.h stubs */
int room3d_sin_deg(int deg) { (void)deg; return 0; }
int room3d_cos_deg(int deg) { (void)deg; return 256; }
void room3d_lane_project(int lane, int bar_x, int *sx, int *sy, int *sc)
{
    if (sx) *sx = 32 + (bar_x * 240) / 256;
    if (sy) *sy = 70 + lane * 42;
    if (sc) *sc = 220 + lane * 12;
}
int room3d_lane_z_base(int lane) { return 80 - lane * 16; }
void room3d_draw_shadow(int z, int sx, int sy, int rx, int ry) { (void)z; (void)sx; (void)sy; (void)rx; (void)ry; }
void room3d_draw_barroom(uint16_t n, uint32_t t, int al, int dc, int dn, int bm)
{ (void)n; (void)t; (void)al; (void)dc; (void)dn; (void)bm; }
void room3d_draw_barroom_ex(uint16_t n, uint32_t t, int al, int dc, int dn, int bm, const uint8_t ld[4], const uint8_t lb[4])
{ (void)n; (void)t; (void)al; (void)dc; (void)dn; (void)bm; (void)ld; (void)lb; }
void room3d_draw_hero_bar(uint16_t n, uint32_t t) { (void)n; (void)t; }
void room3d_draw_card(int z, int cx, int cy, int w, int h, int y, int p, int r, RoomCardKind k, int id, int s, int ho, uint32_t t)
{ (void)z; (void)cx; (void)cy; (void)w; (void)h; (void)y; (void)p; (void)r; (void)k; (void)id; (void)s; (void)ho; (void)t; }

/* =========================================================================
 * Realistic Human Reaction-Time & Skill Profiles (50 Hz PAL: 1f = 20ms)
 * ========================================================================= */

typedef enum {
    PROF_CASUAL  = 0,
    PROF_AVERAGE = 1,
    PROF_SKILLED = 2,
    PROF_COUNT   = 3
} ProfileId;

typedef struct {
    const char *name;
    int   reaction_frames;      /* Visual perception delay in frames (20ms each) */
    int   reaction_jitter;      /* +/- frame jitter */
    int   switch_delay_frames;  /* Cognitive delay when switching to a brand-new task/lane */
    int   hop_cadence_frames;   /* Min frames between D-Pad Up/Down lane taps */
    int   pour_cadence_frames;  /* Min frames between face-button pours (>= 8f game cadence) */
    int   wrong_drink_permille; /* Base probability (per 1000 pours) of wrong button mistap */
    int   lapse_permille_sec;   /* Probability per second of a brief 8-14f attention blink */
    int   tip_greed_margin;     /* Min threat TTI (frames) before dashing for a tip/buff */
    int   shop_iq;              /* 0=Casual, 1=Average, 2=Skilled (synergies + ordering) */
    int   bonus_eye_skill;      /* Visual tracking skill for Spot the Cold One (0..2) */
    int   rush_min_cust;        /* Min customers on screen before firing Ice-Cold Rush */
} HumanProfile;

static const HumanProfile k_profiles[PROF_COUNT] = {
    /* CASUAL: ~440ms reaction, slower thumb taps, occasional wrong button, naive shop */
    { "CASUAL (440ms)",  22, 4, 8, 9, 13, 45, 70, 55, 0, 0, 3 },
    /* AVERAGE: ~320ms reaction, normal human thumb speed, realistic ~2% mistap, sensible shop */
    { "AVERAGE (320ms)", 16, 3, 5, 6, 10, 18, 28, 70, 1, 1, 2 },
    /* SKILLED: ~200ms reaction, crisp arcade thumb, rare mistaps, optimal Balatro shop & tray order */
    { "SKILLED (200ms)", 10, 2, 2, 4,  8,  3,  4, 90, 2, 2, 2 }
};

/* =========================================================================
 * Agent PRNG & Delayed World Perception Buffer
 * ========================================================================= */

static uint32_t sim_rand(uint32_t *st)
{
    uint32_t x = *st;
    if (x == 0u) x = 0xCAFEBABEu;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *st = x;
    return x;
}

static int sim_rand_range(uint32_t *st, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(sim_rand(st) % (uint32_t)(hi - lo + 1));
}

#define PERCEPT_RING 64

typedef struct {
    uint8_t  cust_count[4];
    int16_t  front_x[4];
    int16_t  front_vx[4];     /* Estimated speed (fp) */
    uint8_t  front_drink[4];
    uint8_t  front_hp[4];
    int16_t  front_tti[4];    /* Frames until tap breach */

    uint8_t  empty_count[4];
    int16_t  empty_x[4];
    int16_t  empty_tti[4];    /* Frames until right-edge drop */

    uint8_t  tip_active[4];
    int16_t  tip_x[4];
    uint8_t  tip_kind[4];

    uint8_t  drink_count[4];
    int16_t  frost;
    int16_t  shift_frames_left;
    uint8_t  foam_catch_active;
    uint8_t  shield_charges;
    uint8_t  rush_active;
} PerceivedFrame;

typedef struct {
    PerceivedFrame ring[PERCEPT_RING];
    int            head;
    int            filled;
    uint32_t       rng;

    int            last_hop_f;
    int            last_pour_f;
    int            last_trick_f;
    int            last_rush_f;
    int            focus_lane;
    int            focus_action;    /* 0=IDLE, 1=EMPTY, 2=POUR, 3=TIP */
    int            switch_wait;     /* Cognitive switch latency countdown */
    int            blink_wait;      /* Brief human attention lapse countdown */

    /* Short-term working memory of player's own recent pour timestamps */
    int            pour_ts[4][8];
    int            pour_ts_idx[4];
    int            prev_sprinting;
    int            prev_sprint_lane;
} HumanBrain;

static void capture_perception(const GameState *gs, PerceivedFrame *pf)
{
    int l, i;
    memset(pf, 0, sizeof(*pf));
    pf->frost             = gs->frost;
    pf->shift_frames_left = gs->shift_frames_left;
    pf->foam_catch_active = (gs->foam_catch_timer > 0) ? 1 : 0;
    pf->shield_charges    = gs->foam_shield_charges;
    pf->rush_active       = gs->rush_active;

    for (l = 0; l < NUM_LANES; l++) {
        int32_t best_cx = -1;
        int16_t min_ctti = 9999;
        int32_t best_ex = -1;
        int16_t min_etti = 9999;
        int16_t best_tx = -1;
        uint8_t best_tk = 0;

        for (i = 0; i < MAX_CUSTOMERS; i++) {
            const Customer *c = &gs->customers[i];
            if (!c->active || c->lane != (uint8_t)l || c->state == CSTATE_SATISFIED) continue;
            pf->cust_count[l]++;
            {
                int cx = FROM_FP(c->x_fp);
                int spd = c->vx_fp;
                int dist = (BAR_RIGHT_X - 14) - cx;
                int tti;
                if (c->state == CSTATE_ANGRY) spd = (spd * 145) / 100;
                if (spd < 16) spd = 16;
                tti = (dist > 0) ? (dist * 256) / spd : 0;
                if (c->state == CSTATE_DRINKING) tti += c->state_timer;
                if (c->stun_timer > 0) tti += c->stun_timer;
                if (tti < min_ctti) min_ctti = (int16_t)tti;

                if (c->x_fp > best_cx) {
                    best_cx = c->x_fp;
                    pf->front_x[l]     = (int16_t)cx;
                    pf->front_vx[l]    = (int16_t)spd;
                    pf->front_drink[l] = c->wanted_drink;
                    pf->front_hp[l]    = c->hp;
                }
            }
        }
        pf->front_tti[l] = (pf->cust_count[l] > 0) ? min_ctti : 9999;

        for (i = 0; i < MAX_EMPTIES; i++) {
            const ReturningEmpty *e = &gs->empties[i];
            if (!e->active || e->lane != (uint8_t)l) continue;
            pf->empty_count[l]++;
            {
                int ex = FROM_FP(e->x_fp);
                int spd = (e->vx_fp > 16) ? e->vx_fp : 16;
                int dist = (BAR_RIGHT_X + 8) - ex;
                int tti = (dist > 0) ? (dist * 256) / spd : 0;
                if (tti < min_etti) min_etti = (int16_t)tti;
                if (e->x_fp > best_ex) {
                    best_ex = e->x_fp;
                    pf->empty_x[l] = (int16_t)ex;
                }
            }
        }
        pf->empty_tti[l] = (pf->empty_count[l] > 0) ? min_etti : 9999;

        for (i = 0; i < MAX_TIPS; i++) {
            const CashTip *t = &gs->tips[i];
            if (!t->active || t->lane != (uint8_t)l) continue;
            if (!pf->tip_active[l] || t->x > best_tx) {
                pf->tip_active[l] = 1;
                best_tx = t->x;
                best_tk = t->buff_kind;
            }
        }
        pf->tip_x[l]    = (best_tx >= 0) ? best_tx : 0;
        pf->tip_kind[l] = best_tk;

        for (i = 0; i < MAX_DRINKS; i++) {
            const SlidingDrink *d = &gs->drinks[i];
            if (!d->active || d->lane != (uint8_t)l) continue;
            if (d->is_rush) {
                /* A Rush wave sweeps all patrons on this lane; human sees lane is covered! */
                pf->drink_count[l] += 2;
            } else {
                pf->drink_count[l]++;
            }
        }
        /* Include drinks currently queued in tap-pull buffer or auto-sprint on this lane */
        if (gs->lane == (int8_t)l) {
            if (gs->pour_buf_timer > 0 && gs->pour_buf_drink >= 0) pf->drink_count[l]++;
            if (gs->is_auto_sprinting && gs->buffered_drink >= 0) pf->drink_count[l]++;
        }
    }
}

static uint16_t drink_to_btn(uint8_t dt)
{
    if (dt == DRINK_BOTTLE) return BTN_CIRCLE;
    if (dt == DRINK_BAG)    return BTN_SQUARE;
    return BTN_CROSS;
}

static uint16_t brain_step_playing(HumanBrain *br, const HumanProfile *prof,
                                   const GameState *gs, const RunState *run, int frame)
{
    PerceivedFrame pf;
    int lag, idx;
    int l;
    int cur_lane = (int)gs->lane;
    int bx = FROM_FP(gs->bartender_x_fp);
    int total_cust = 0;
    int min_any_tti = 9999;
    int active_threat_lanes = 0;
    uint16_t out = 0;
    BossType boss = rl_current_boss(run);

    /* Proprioception: if auto-sprint just finished at the tap, record the actual tap-pull frame */
    if (br->prev_sprinting && !gs->is_auto_sprinting && bx >= BARTENDER_POUR_X) {
        int sl = br->prev_sprint_lane;
        if (sl >= 0 && sl < 4) {
            br->pour_ts[sl][br->pour_ts_idx[sl] & 7] = frame;
            br->pour_ts_idx[sl]++;
        }
    }
    br->prev_sprinting   = gs->is_auto_sprinting ? 1 : 0;
    br->prev_sprint_lane = cur_lane;

    /* Push current truth into perception ring buffer */
    capture_perception(gs, &br->ring[br->head]);
    br->head = (br->head + 1) % PERCEPT_RING;
    if (br->filled < PERCEPT_RING) br->filled++;

    /* Sample delayed human view */
    lag = prof->reaction_frames + sim_rand_range(&br->rng, -prof->reaction_jitter, prof->reaction_jitter);
    if (boss == BOSS_BLACKOUT) lag += 3; /* Dim lighting slows visual read ~60ms */
    if (lag < 1) lag = 1;
    if (lag >= br->filled) lag = br->filled - 1;
    idx = (br->head - 1 - lag + PERCEPT_RING) % PERCEPT_RING;
    pf = br->ring[idx];

    /* Random brief human focus blink (~once every few seconds depending on profile) */
    if (br->blink_wait > 0) {
        br->blink_wait--;
        return 0;
    }
    if ((frame % 50) == 0 && sim_rand_range(&br->rng, 0, 999) < prof->lapse_permille_sec) {
        br->blink_wait = sim_rand_range(&br->rng, 7, 13);
        return 0;
    }

    for (l = 0; l < NUM_LANES; l++) {
        total_cust += pf.cust_count[l];
        if (pf.cust_count[l] > 0 && pf.front_tti[l] < min_any_tti) min_any_tti = pf.front_tti[l];
        if (pf.empty_count[l] > 0 && !pf.foam_catch_active && pf.empty_tti[l] < min_any_tti) {
            min_any_tti = pf.empty_tti[l];
        }
        if (pf.cust_count[l] > 0 || pf.empty_count[l] > 0) active_threat_lanes++;
    }

    /* 1. Ice-Cold Rush (Triangle) when Frost == 100 and worthwhile/emergency */
    if (pf.frost >= FROST_MAX && !pf.rush_active && (frame - br->last_rush_f) >= 30) {
        if (total_cust >= prof->rush_min_cust || min_any_tti < 55) {
            br->last_rush_f = frame;
            return BTN_TRIANGLE;
        }
    }

    /* 2. Pocket Tricks (L1 / R1) */
    if ((frame - br->last_trick_f) >= 50) {
        for (l = 0; l < RL_MAX_TRICKS; l++) {
            int8_t tid = run->tricks[l];
            int use_it = 0;
            if (tid == TRICK_NONE) continue;
            if (tid == TRICK_COLD_MUG && run->lives <= (prof->shop_iq >= 2 ? 4 : 3)) use_it = 1;
            else if (tid == TRICK_BOUNCER && min_any_tti < 65 && total_cust >= 2) use_it = 1;
            else if (tid == TRICK_ICE_BUCKET && pf.frost <= 35 && total_cust >= 2 && !pf.rush_active) use_it = 1;
            else if (tid == TRICK_HAPPY_BELL && total_cust >= 2 && pf.shift_frames_left > 250) use_it = 1;
            else if (tid == TRICK_FOAM_CATCH && (pf.empty_count[0] + pf.empty_count[1] + pf.empty_count[2] + pf.empty_count[3]) >= 1) use_it = 1;
            else if (tid == TRICK_VIP_COUPON && pf.shift_frames_left > 300) use_it = 1;
            if (use_it) {
                br->last_trick_f = frame;
                return (l == 0) ? BTN_L1 : BTN_R1;
            }
        }
    }

    /* 3. Prioritize lanes by human urgency (Time-To-Impact, adjusted by lag) */
    int best_lane = cur_lane;
    int best_act  = 0; /* 0=IDLE, 1=EMPTY, 2=POUR, 3=TIP */
    int best_prio = -1;

    for (l = 0; l < NUM_LANES; l++) {
        int hop_dist = abs(l - cur_lane);
        int k, pours_since = 0;
        int eff_drinks;
        if (hop_dist == 3) hop_dist = 1; /* Wrap-around lane hop! */
        for (k = 0; k < 8; k++) {
            if (br->pour_ts[l][k] >= (frame - lag) && br->pour_ts[l][k] <= frame) {
                pours_since++;
            }
        }
        eff_drinks = pf.drink_count[l] + pours_since;

        /* Returning empty urgency */
        if (pf.empty_count[l] > 0 && !pf.foam_catch_active) {
            int etti = pf.empty_tti[l];
            int prio;
            if (etti <= 55)      prio = 5000 - etti * 20;
            else if (etti <= 95) prio = 2600 - etti * 10;
            else                 prio = 800 - etti;
            if (l == cur_lane) prio += 120;
            if (prio > best_prio) { best_prio = prio; best_lane = l; best_act = 1; }
        }

        /* Customer serving urgency (account for drinks already in flight / just poured) */
        if (pf.cust_count[l] > 0 && eff_drinks < 2) {
            /* Allow pouring if total HP on lane exceeds drinks in flight, or front HP > eff_drinks */
            int need_more = (eff_drinks < pf.front_hp[l]) || (pf.cust_count[l] >= 2 && eff_drinks < 2);
            if (need_more) {
                int ctti = pf.front_tti[l];
                int prio;
                if (ctti <= 65)       prio = 4800 - ctti * 18;
                else if (ctti <= 130) prio = 2800 - ctti * 10;
                else                  prio = 1500 - ctti * 3;
                if (l == cur_lane) prio += 100;
                /* Slight preference for closer lanes */
                prio -= hop_dist * 35;
                if (prio > best_prio) { best_prio = prio; best_lane = l; best_act = 2; }
            }
        }

        /* Tip / Buff pickup if safe enough for this human's greed margin */
        if (pf.tip_active[l]) {
            int dash_cost = (256 - pf.tip_x[l]) / 4; /* rough round-trip frames */
            if (min_any_tti > prof->tip_greed_margin + dash_cost) {
                int prio = 1750 + pf.tip_x[l];
                if (pf.tip_kind[l] == BUFF_STAR_MULT || pf.tip_kind[l] == BUFF_FROST_ORB) prio += 200;
                if (prio > best_prio) { best_prio = prio; best_lane = l; best_act = 3; }
            }
        }
    }

    /* Cognitive task-switch delay when changing target lane/action */
    if (best_act != 0 && (best_lane != br->focus_lane || best_act != br->focus_action)) {
        br->focus_lane   = best_lane;
        br->focus_action = best_act;
        if (prof->switch_delay_frames > 0 && min_any_tti > 35) {
            br->switch_wait = sim_rand_range(&br->rng, 0, prof->switch_delay_frames);
        } else {
            br->switch_wait = 0;
        }
    }

    if (br->switch_wait > 0) {
        br->switch_wait--;
        /* Still keep walking right back to tap if idle/mid-counter */
        if (bx < 244 && best_act != 3) out |= BTN_RIGHT;
        return out;
    }

    /* 4. Execute lane hop (shortest direction including wrap-around!) */
    if (best_lane != cur_lane) {
        if ((frame - br->last_hop_f) >= prof->hop_cadence_frames) {
            int diff = (best_lane - cur_lane + NUM_LANES) % NUM_LANES;
            br->last_hop_f = frame;
            out |= (diff <= 2) ? BTN_DOWN : BTN_UP;
        }
        /* Return toward tap while hopping if not chasing a tip */
        if (bx < 244 && best_act != 3) out |= BTN_RIGHT;
        return out;
    }

    /* 5. Execute on-lane action */
    if (best_act == 1) { /* EMPTY */
        int ex = pf.empty_x[cur_lane];
        if (bx < ex - 6 || bx < 236) out |= BTN_RIGHT;
        else if (bx > ex + 24 && min_any_tti > 70) out |= BTN_LEFT;
        return out;
    }

    if (best_act == 3) { /* TIP / BUFF */
        int tx = pf.tip_x[cur_lane];
        if (bx > tx + 6) out |= BTN_LEFT;
        else if (bx < tx - 6) out |= BTN_RIGHT;
        return out;
    }

    if (best_act == 2) { /* POUR */
        /* Proprioception: if bartender is already auto-sprinting back to tap with a buffered pour, let it finish! */
        if (gs->is_auto_sprinting && gs->buffered_drink >= 0) {
            return 0;
        }
        if ((frame - br->last_pour_f) >= prof->pour_cadence_frames) {
            uint8_t dt = pf.front_drink[cur_lane];
            int err_rate = prof->wrong_drink_permille;
            if (active_threat_lanes >= 3) err_rate = (err_rate * 17) / 10;
            if (boss == BOSS_BLACKOUT)    err_rate = (err_rate * 15) / 10;
            /* Occasional human mistap on Bottle/Bag (or under pressure) */
            if (boss != BOSS_BOTTLENECK && sim_rand_range(&br->rng, 0, 999) < err_rate) {
                dt = (uint8_t)((dt + 1 + sim_rand_range(&br->rng, 0, 1)) % 3);
            }
            br->last_pour_f = frame;
            br->pour_ts[cur_lane][br->pour_ts_idx[cur_lane] & 7] = frame;
            br->pour_ts_idx[cur_lane]++;
            return drink_to_btn(dt);
        }
        if (bx < 248) out |= BTN_RIGHT;
        return out;
    }

    /* Idle: return to tap */
    if (bx < 248) out |= BTN_RIGHT;
    return out;
}

/* =========================================================================
 * Shop, Draft, Coaster Ordering & Bonus Round Human Logic
 * ========================================================================= */

static int coaster_score_value(CoasterId id, const RunState *run, int shop_iq)
{
    int val = 50;
    if (shop_iq == 0) {
        /* Casual likes shiny rares and simple flat bonuses */
        const CoasterDef *d = rl_coaster_def((uint8_t)id);
        return 40 + (int)d->rarity * 25;
    }
    switch (id) {
        case COASTER_GLASS_CANNON:
            /* Huge 3x mult, risky for Average (spills cost a life!), great for Skilled */
            val = (shop_iq >= 2) ? 145 : 40;
            break;
        case COASTER_VIP_VELVET:     val = 135; break;
        case COASTER_FROST_BITE:     val = 128; break;
        case COASTER_BIG_APPETITE:   val = 115; break;
        case COASTER_TOP_SHELF:      val = 118; break;
        case COASTER_QINGDAO_TRIO:   val = 122; break;
        case COASTER_ICE_KING:       val = 110; break;
        case COASTER_GOLDEN_TAP:     val = (run->cash >= 15 || shop_iq >= 2) ? 130 : 95; break;
        case COASTER_CLEAN_APRON:    val = (shop_iq >= 2) ? 132 : 80; break;
        case COASTER_TAP_RHYTHM:     val = 105; break;
        case COASTER_MIXTAPE_POUR:   val = 102; break;
        case COASTER_GLASS_JUGGLER:  val = 96;  break;
        case COASTER_GREEN_GLASS:    val = 88;  break;
        case COASTER_PLASTIC_HUSTLE: val = 90;  break;
        case COASTER_STREET_SIDE:    val = 86;  break;
        case COASTER_BAG_SPLASH:     val = 92;  break;
        case COASTER_TIP_JAR:        val = 84;  break;
        case COASTER_PIGGY_BANK:     val = (run->night <= 4) ? 112 : 65; break;
        case COASTER_COLD_FOAM:      val = (run->night <= 3) ? 88 : 55;  break;
        case COASTER_SPEED_SNEAKERS: val = 72;  break;
        default:                     val = 50;  break;
    }
    /* Late game: Skilled strongly prefers xMult coasters! */
    if (shop_iq >= 2 && run->night >= 4) {
        const CoasterDef *d = rl_coaster_def((uint8_t)id);
        if (d->flags & RL_FLAG_XMULT) val += 35;
    }
    return val;
}

static int coaster_order_rank(uint8_t cid)
{
    const CoasterDef *d = rl_coaster_def(cid);
    /* Optimal Balatro ordering: +Base first, then +Mult, then xMult last! */
    if (d->flags & RL_FLAG_XMULT) return 3;
    if (d->flags & RL_FLAG_MULT)  return 2;
    if (d->flags & RL_FLAG_BASE)  return 1;
    return 0;
}

static void run_draft_phase(RunState *run, const HumanProfile *prof, uint32_t *rng)
{
    DraftState ds;
    int best_i = 0, best_v = -9999;
    int i, f;

    rl_draft_init(&ds, run);
    for (i = 0; i < 3; i++) {
        int v = coaster_score_value((CoasterId)ds.choices[i], run, prof->shop_iq);
        if (prof->shop_iq == 0) v += sim_rand_range(rng, -40, 40);
        else if (prof->shop_iq == 1) v += sim_rand_range(rng, -15, 15);
        if (v > best_v) { best_v = v; best_i = i; }
    }

    for (f = 0; f < 120; f++) {
        uint16_t btn = 0;
        if (ds.cursor != best_i && (f % 4) == 0) btn = BTN_RIGHT;
        else if (ds.flip_timer >= 18 && ds.cursor == best_i && (f % 4) == 2) btn = BTN_CROSS;
        sim_set_input(btn);
        g_tls.frame++;
        if (rl_draft_update(&ds, run)) break;
    }
}

static void run_payout_phase(RunState *run, uint32_t shift_score, uint32_t shift_target, int16_t tips)
{
    PayoutState ps;
    int f;
    rl_payout_init(&ps, run, shift_score, shift_target, tips);
    for (f = 0; f < 120; f++) {
        uint16_t btn = (f == 12 || f == 16) ? BTN_CROSS : 0;
        sim_set_input(btn);
        g_tls.frame++;
        if (rl_payout_update(&ps, run)) break;
    }
}

static void run_bonus_phase(RunState *run, const HumanProfile *prof, uint32_t *rng,
                            int *bonus_attempts, int *bonus_wins)
{
    BonusState bs;
    int f;
    int decided_pick = -1;
    int decided_reward = -1;

    bonus_init(&bs, run);
    (*bonus_attempts)++;

    for (f = 0; f < 1600; f++) {
        uint16_t btn = 0;
        if (bs.phase == BONUS_PHASE_INTRO && (f % 6) == 2) {
            btn = BTN_CROSS;
        } else if (bs.phase == BONUS_PHASE_PICK) {
            if (decided_pick < 0) {
                /* Human visual tracking accuracy as a function of swap speed (frames/swap) */
                /* swap_dur=18f (360ms) is comfortable; swap_dur=10f (200ms) is fast blur */
                int dur = (int)bs.swap_dur;
                int track_pct;
                if (prof->bonus_eye_skill == 2) {
                    track_pct = 55 + (dur - 10) * 5; /* 55% @10f, 80% @15f, 95% @18f */
                } else if (prof->bonus_eye_skill == 1) {
                    track_pct = 35 + (dur - 10) * 6; /* 35% @10f, 65% @15f, 83% @18f */
                } else {
                    track_pct = 28 + (dur - 10) * 4; /* 28% @10f, 48% @15f, 60% @18f */
                }
                if (track_pct < 25) track_pct = 25;
                if (track_pct > 96) track_pct = 96;
                if (sim_rand_range(rng, 0, 99) < track_pct) {
                    decided_pick = bs.cold_slot;
                } else {
                    decided_pick = (bs.cold_slot + 1 + sim_rand_range(rng, 0, 2)) & 3;
                }
            }
            if (bs.cursor_slot != decided_pick && (f % 4) == 0) btn = BTN_RIGHT;
            else if (bs.cursor_slot == decided_pick && (f % 4) == 2) btn = BTN_CROSS;
        } else if (bs.phase == BONUS_PHASE_REVEAL) {
            if (bs.phase_timer < 75 && (f % 6) == 0) btn = BTN_CROSS;
        } else if (bs.phase == BONUS_PHASE_REWARD) {
            if (decided_reward < 0) {
                if (run->lives <= 2) decided_reward = 1; /* +1 Life */
                else if (run->coaster_count < RL_MAX_COASTERS) decided_reward = 0; /* Rare Coaster */
                else if (run->lives < MAX_LIVES && prof->shop_iq >= 1) decided_reward = 1;
                else decided_reward = 2; /* +$10 Cash */
            }
            if (bs.reward_cursor != decided_reward && (f % 4) == 0) btn = BTN_RIGHT;
            else if (bs.reward_flip <= 6 && bs.reward_cursor == decided_reward && (f % 4) == 2) btn = BTN_CROSS;
        }
        sim_set_input(btn);
        g_tls.frame++;
        if (bonus_update(&bs, run)) {
            if (bs.won) (*bonus_wins)++;
            break;
        }
    }
}

static void shop_move_and_press(ShopState *ss, RunState *run, int target_zone, int target_cur, uint16_t action_btn)
{
    int guard = 0;
    while (guard++ < 40) {
        uint16_t btn = 0;
        if (ss->zone < target_zone) btn = BTN_DOWN;
        else if (ss->zone > target_zone) btn = BTN_UP;
        else if (ss->cursor < target_cur) btn = BTN_RIGHT;
        else if (ss->cursor > target_cur) btn = BTN_LEFT;
        else {
            sim_set_input(0); g_tls.frame++; rl_shop_update(ss, run);
            sim_set_input(action_btn); g_tls.frame++; rl_shop_update(ss, run);
            sim_set_input(0); g_tls.frame++; rl_shop_update(ss, run);
            return;
        }
        sim_set_input(0); g_tls.frame++; rl_shop_update(ss, run);
        sim_set_input(btn); g_tls.frame++; rl_shop_update(ss, run);
    }
}

static void run_shop_phase(RunState *run, const HumanProfile *prof, uint32_t *rng)
{
    ShopState ss;
    int rerolls = 0;
    int max_rerolls = (prof->shop_iq == 2) ? 2 : (prof->shop_iq == 1 ? 1 : 0);
    int cash_reserve = (prof->shop_iq == 2 && run->night <= 5) ? 10 : (prof->shop_iq == 1 ? 5 : 0);

    rl_shop_init(&ss, run);

    for (;;) {
        int bought_something = 0;

        /* 1. Always consider Drink Upgrade (high ROI: +Base & +1 Base Mult for $3!) */
        if (!ss.drink_sold && run->drink_level[ss.drink_offer] < 9 && run->cash >= 3) {
            int want_drink = 1;
            if (prof->shop_iq == 0 && sim_rand_range(rng, 0, 99) < 35) want_drink = 0;
            if (want_drink) {
                shop_move_and_press(&ss, run, 1, 2, BTN_CROSS);
                bought_something = 1;
            }
        }

        /* 2. Evaluate the 2 Coaster offers */
        {
            int cslot;
            for (cslot = 0; cslot < 2; cslot++) {
                const CoasterDef *def;
                int val;
                if (ss.coaster_sold[cslot]) continue;
                def = rl_coaster_def(ss.coaster_offers[cslot]);
                if (rl_has_coaster(run, (CoasterId)def->id)) continue;
                val = coaster_score_value((CoasterId)def->id, run, prof->shop_iq);
                if (prof->shop_iq == 0) val += sim_rand_range(rng, -35, 35);
                else if (prof->shop_iq == 1) val += sim_rand_range(rng, -15, 15);

                if (run->coaster_count < RL_MAX_COASTERS) {
                    int min_val = (run->coaster_count < 3) ? 60 : 85;
                    if (val >= min_val && run->cash >= (int16_t)def->cost + (run->coaster_count >= 3 ? cash_reserve / 2 : 0)) {
                        shop_move_and_press(&ss, run, 1, cslot, BTN_CROSS);
                        bought_something = 1;
                    }
                } else if (prof->shop_iq >= 1) {
                    /* Consider selling weakest equipped coaster for a clear upgrade */
                    int worst_i = -1, worst_v = 9999, j;
                    for (j = 0; j < (int)run->coaster_count; j++) {
                        int ev = coaster_score_value((CoasterId)run->coasters[j], run, prof->shop_iq);
                        if (ev < worst_v) { worst_v = ev; worst_i = j; }
                    }
                    int margin = (prof->shop_iq == 2) ? 20 : 35;
                    if (worst_i >= 0 && val >= worst_v + margin) {
                        int sell_gain = (int)(rl_coaster_def(run->coasters[worst_i])->cost / 2u);
                        if (sell_gain < 1) sell_gain = 1;
                        if (run->cash + sell_gain >= (int16_t)def->cost) {
                            shop_move_and_press(&ss, run, 0, worst_i, BTN_SQUARE);
                            shop_move_and_press(&ss, run, 1, cslot, BTN_CROSS);
                            bought_something = 1;
                        }
                    }
                }
            }
        }

        /* 3. Consider Pocket Trick if a slot is empty */
        if (!ss.trick_sold && (run->tricks[0] == TRICK_NONE || run->tricks[1] == TRICK_NONE)) {
            const TrickDef *tdef = rl_trick_def(ss.trick_offer);
            if (tdef && run->cash >= (int16_t)tdef->cost + cash_reserve) {
                int want_trick = 0;
                if (tdef->id == TRICK_COLD_MUG && run->lives < 4) want_trick = 1;
                else if (tdef->id == TRICK_HAPPY_BELL || tdef->id == TRICK_BOUNCER ||
                         tdef->id == TRICK_FOAM_CATCH || tdef->id == TRICK_ICE_BUCKET) want_trick = 1;
                else if (prof->shop_iq == 0) want_trick = 1;
                if (want_trick) {
                    shop_move_and_press(&ss, run, 1, 3, BTN_CROSS);
                    bought_something = 1;
                }
            }
        }

        /* 4. Reroll if rich and still building */
        if (rerolls < max_rerolls && run->cash >= ss.reroll_cost + cash_reserve + 6) {
            shop_move_and_press(&ss, run, 2, 0, BTN_CROSS);
            rerolls++;
            continue;
        }
        if (!bought_something) break;
        break;
    }

    /* 5. Skilled players sort equipped coasters left-to-right (+Base -> +Mult -> xMult) */
    if (prof->shop_iq == 2 && run->coaster_count >= 2) {
        int pass, j;
        for (pass = 0; pass < (int)run->coaster_count; pass++) {
            for (j = 0; j + 1 < (int)run->coaster_count; j++) {
                if (coaster_order_rank(run->coasters[j]) > coaster_order_rank(run->coasters[j + 1])) {
                    shop_move_and_press(&ss, run, 0, j, BTN_R1);
                }
            }
        }
    }

    /* Exit shop -> Next Shift */
    sim_set_input(0); g_tls.frame++; rl_shop_update(&ss, run);
    sim_set_input(BTN_START); g_tls.frame++; rl_shop_update(&ss, run);
    sim_set_input(0);
}

/* =========================================================================
 * Physical Fairness Audit & Per-Run Telemetry
 * ========================================================================= */

typedef enum {
    DEATH_NONE          = 0,
    DEATH_TAP_BREACH    = 1,
    DEATH_DROPPED_EMPTY = 2,
    DEATH_TARGET_MISSED = 3,
    DEATH_GLASS_CANNON  = 4
} DeathCause;

typedef struct {
    int        won_night8;
    int        reached_night;
    int        reached_shift;
    int        survived_horizon;
    DeathCause death_cause;
    BossType   death_boss;
    uint32_t   final_score;
    uint32_t   coasters_mask;

    /* Per-boss tracking (1..8) */
    int        boss_played[RL_NUM_BOSSES + 1];
    int        boss_failed[RL_NUM_BOSSES + 1];

    /* Physical fairness audit */
    int        min_empty_tti_frames;
    int        impossible_empty_overlaps;
    int        peak_concurrent_threats;
    int        total_lives_lost_tap;
    int        total_lives_lost_empty;
    int        total_lives_lost_cannon;
    int        bonus_attempts;
    int        bonus_wins;

    /* Score / Target ratios at Boss shifts for key nights */
    double     boss_score_ratio[32];
    uint8_t    boss_ratio_valid[32];
} RunReport;

static DeathCause detect_life_loss_cause(const GameState *gs)
{
    int i;
    int best_life = -1;
    DeathCause cause = DEATH_TAP_BREACH;
    for (i = 0; i < MAX_POPUPS; i++) {
        const ScorePopup *sp = &gs->popups[i];
        if (!sp->active) continue;
        if (sp->life > best_life) {
            if (strcmp(sp->text, "DROPPED!") == 0) {
                best_life = sp->life; cause = DEATH_DROPPED_EMPTY;
            } else if (strcmp(sp->text, "TOO SLOW!") == 0) {
                best_life = sp->life; cause = DEATH_TAP_BREACH;
            } else if (strcmp(sp->text, "CANNON!") == 0) {
                best_life = sp->life; cause = DEATH_GLASS_CANNON;
            }
        }
    }
    return cause;
}

static void audit_frame_fairness(const GameState *gs, RunReport *rep, uint8_t prev_empty_active[MAX_EMPTIES])
{
    int i, j;
    int active_threats = 0;
    int lane_best_etti[4] = { 9999, 9999, 9999, 9999 };

    for (i = 0; i < MAX_CUSTOMERS; i++) {
        if (gs->customers[i].active && gs->customers[i].state != CSTATE_SATISFIED) {
            active_threats++;
        }
    }
    for (i = 0; i < MAX_EMPTIES; i++) {
        const ReturningEmpty *e = &gs->empties[i];
        if (e->active) {
            int ex = FROM_FP(e->x_fp);
            int spd = (e->vx_fp > 1) ? e->vx_fp : 1;
            int dist = (BAR_RIGHT_X + 8) - ex;
            int tti = (dist > 0) ? (dist * 256) / spd : 0;
            active_threats++;
            if (!prev_empty_active[i]) {
                /* Brand-new empty spawned this frame! Record its total catch window */
                if (tti < rep->min_empty_tti_frames) {
                    rep->min_empty_tti_frames = tti;
                }
            }
            if (e->lane < 4 && tti < lane_best_etti[e->lane]) {
                lane_best_etti[e->lane] = tti;
            }
            prev_empty_active[i] = 1;
        } else {
            prev_empty_active[i] = 0;
        }
    }
    if (active_threats > rep->peak_concurrent_threats) {
        rep->peak_concurrent_threats = active_threats;
    }

    /* Check for physically uncatchable multi-lane simultaneous empty drops (both arriving in <= 12f on different lanes) */
    for (i = 0; i < 4; i++) {
        if (lane_best_etti[i] > 10) continue;
        for (j = i + 1; j < 4; j++) {
            if (lane_best_etti[j] <= 10 && abs(lane_best_etti[i] - lane_best_etti[j]) <= 6) {
                /* Only count right at the danger threshold (once per collision) */
                if (lane_best_etti[i] == 10 || lane_best_etti[j] == 10) {
                    rep->impossible_empty_overlaps++;
                }
            }
        }
    }
}

static void simulate_one_run(ProfileId prof_id, uint32_t seed, int max_night, RunReport *rep)
{
    const HumanProfile *prof = &k_profiles[prof_id];
    RunState run;
    GameState gs;
    HumanBrain brain;
    uint32_t rng = seed ^ 0xA5A51996u;

    memset(rep, 0, sizeof(*rep));
    rep->min_empty_tti_frames = 9999;

    sim_reset_tls(seed & 0xFFFFu);
    rl_run_init(&run, seed, 0);

    /* Starter Coaster Draft */
    run_draft_phase(&run, prof, &rng);

    while ((int)run.night <= max_night) {
        BossType boss = rl_current_boss(&run);
        uint8_t prev_empty_active[MAX_EMPTIES];
        int shift_frame = 0;
        int8_t prev_lives = run.lives;
        DeathCause last_life_cause = DEATH_NONE;
        GameStepResult res = GAME_STEP_PLAYING;

        if (boss != BOSS_NONE) {
            rep->boss_played[boss]++;
        }

        game_init_shift(&gs, &run);
        memset(&brain, 0, sizeof(brain));
        memset(prev_empty_active, 0, sizeof(prev_empty_active));
        {
            int pl, pk;
            for (pl = 0; pl < 4; pl++)
                for (pk = 0; pk < 8; pk++) brain.pour_ts[pl][pk] = -1000;
        }
        brain.rng          = sim_rand(&rng);
        brain.last_hop_f   = -100;
        brain.last_pour_f  = -100;
        brain.last_trick_f = -100;
        brain.last_rush_f  = -100;
        brain.focus_lane   = gs.lane;

        for (shift_frame = 0; shift_frame < 6000; shift_frame++) {
            uint16_t btn = 0;
            if (gs.shift_banner_timer == 0 && gs.life_lost_timer == 0 && gs.shift_end_timer == 0) {
                btn = brain_step_playing(&brain, prof, &gs, &run, shift_frame);
            }
            sim_set_input(btn);
            g_tls.frame++;
            res = game_update(&gs, &run);

            if (gs.shift_banner_timer == 0 && gs.life_lost_timer == 0 && gs.shift_end_timer == 0) {
                audit_frame_fairness(&gs, rep, prev_empty_active);
            }

            if (run.lives < prev_lives) {
                last_life_cause = detect_life_loss_cause(&gs);
                if (last_life_cause == DEATH_TAP_BREACH) rep->total_lives_lost_tap++;
                else if (last_life_cause == DEATH_DROPPED_EMPTY) rep->total_lives_lost_empty++;
                else if (last_life_cause == DEATH_GLASS_CANNON) rep->total_lives_lost_cannon++;
                prev_lives = run.lives;
            } else if (run.lives > prev_lives) {
                prev_lives = run.lives;
            }

            if (res == GAME_STEP_SHIFT_WIN || res == GAME_STEP_GAME_OVER) {
                break;
            }
        }

        /* Track coasters seen on this run */
        {
            uint8_t ci;
            for (ci = 0; ci < run.coaster_count && ci < RL_MAX_COASTERS; ci++) {
                rep->coasters_mask |= (1u << run.coasters[ci]);
            }
        }
        if (run.shift_in_night == (uint8_t)SHIFT_BOSS && run.night < 32 && gs.shift_target > 0) {
            rep->boss_score_ratio[run.night] = (double)gs.shift_score / (double)gs.shift_target;
            rep->boss_ratio_valid[run.night] = 1;
        }

        if (res != GAME_STEP_SHIFT_WIN) {
            rep->reached_night = (int)run.night;
            rep->reached_shift = (int)run.shift_in_night + 1;
            rep->final_score   = run.run_score;
            rep->death_boss    = boss;
            if (boss != BOSS_NONE) rep->boss_failed[boss]++;

            if (run.lives <= 0) {
                rep->death_cause = (last_life_cause != DEATH_NONE) ? last_life_cause : DEATH_TAP_BREACH;
            } else {
                rep->death_cause = DEATH_TARGET_MISSED;
            }
            return;
        }

        /* Shift Won -> Payout -> (Night 8 Win / Bonus) -> Shop -> Advance */
        run_payout_phase(&run, gs.shift_score, gs.shift_target, gs.shift_tips_cash);

        if (run.shift_in_night == (uint8_t)SHIFT_BOSS) {
            if (run.night == RL_WIN_NIGHT && !run.won_night8) {
                run.won_night8   = 1;
                run.endless_mode = 1;
                rep->won_night8  = 1;
            }
            run_bonus_phase(&run, prof, &rng, &rep->bonus_attempts, &rep->bonus_wins);
        }

        if ((int)run.night == max_night && run.shift_in_night == (uint8_t)SHIFT_BOSS) {
            rep->reached_night    = max_night;
            rep->reached_shift    = 3;
            rep->survived_horizon = 1;
            rep->final_score      = run.run_score;
            return;
        }

        run_shop_phase(&run, prof, &rng);
        rl_advance_shift(&run);
    }
}

/* =========================================================================
 * Multi-Threaded Sweep Driver & Statistical Aggregator
 * ========================================================================= */

typedef struct {
    ProfileId  prof_id;
    int        start_idx;
    int        end_idx;
    int        max_night;
    uint32_t   base_seed;
    RunReport *reports;
} WorkerArgs;

static void *worker_main(void *arg)
{
    WorkerArgs *wa = (WorkerArgs *)arg;
    int i;
    for (i = wa->start_idx; i < wa->end_idx; i++) {
        uint32_t seed = wa->base_seed + (uint32_t)(i * 2654435761u) + (uint32_t)(wa->prof_id * 0x9E3779B9u);
        if (seed == 0u) seed = 1u;
        simulate_one_run(wa->prof_id, seed, wa->max_night, &wa->reports[i]);
    }
    return NULL;
}

static int cmp_int(const void *a, const void *b)
{
    int va = *(const int *)a;
    int vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

static void print_profile_report(ProfileId pid, const RunReport *reps, int n_runs, int max_night, int verbose_tables)
{
    int i, b, c;
    int won_n8 = 0, survived_max = 0;
    int d_tap = 0, d_empty = 0, d_tgt = 0, d_cannon = 0;
    int n8_d_tap = 0, n8_d_empty = 0, n8_d_tgt = 0, n8_d_cannon = 0;
    int end_d_tap = 0, end_d_empty = 0, end_d_tgt = 0, end_d_cannon = 0;
    int min_empty_tti = 9999;
    long long total_overlaps = 0;
    long long total_lives_tap = 0, total_lives_empty = 0, total_lives_cannon = 0;
    int peak_threats = 0;
    long long bonus_att = 0, bonus_win = 0;
    int boss_p[RL_NUM_BOSSES + 1] = {0};
    int boss_f[RL_NUM_BOSSES + 1] = {0};
    int coast_seen[RL_NUM_COASTERS] = {0};
    int coast_won8[RL_NUM_COASTERS] = {0};
    int night_reached_cnt[32] = {0};
    int *nights = (int *)malloc((size_t)n_runs * sizeof(int));
    int *endless_nights = (int *)malloc((size_t)n_runs * sizeof(int));
    int endless_cnt = 0;

    for (i = 0; i < n_runs; i++) {
        const RunReport *r = &reps[i];
        nights[i] = r->reached_night;
        if (r->won_night8) {
            won_n8++;
            endless_nights[endless_cnt++] = r->reached_night;
        }
        if (r->survived_horizon) survived_max++;
        if (r->reached_night < 32) {
            int k;
            for (k = 1; k <= r->reached_night && k < 32; k++) night_reached_cnt[k]++;
        }

        if (r->death_cause == DEATH_TAP_BREACH) {
            d_tap++;
            if (r->won_night8) end_d_tap++; else n8_d_tap++;
        } else if (r->death_cause == DEATH_DROPPED_EMPTY) {
            d_empty++;
            if (r->won_night8) end_d_empty++; else n8_d_empty++;
        } else if (r->death_cause == DEATH_TARGET_MISSED) {
            d_tgt++;
            if (r->won_night8) end_d_tgt++; else n8_d_tgt++;
        } else if (r->death_cause == DEATH_GLASS_CANNON) {
            d_cannon++;
            if (r->won_night8) end_d_cannon++; else n8_d_cannon++;
        }

        if (r->min_empty_tti_frames < min_empty_tti) min_empty_tti = r->min_empty_tti_frames;
        total_overlaps     += r->impossible_empty_overlaps;
        total_lives_tap    += r->total_lives_lost_tap;
        total_lives_empty  += r->total_lives_lost_empty;
        total_lives_cannon += r->total_lives_lost_cannon;
        if (r->peak_concurrent_threats > peak_threats) peak_threats = r->peak_concurrent_threats;
        bonus_att += r->bonus_attempts;
        bonus_win += r->bonus_wins;

        for (b = 1; b <= RL_NUM_BOSSES; b++) {
            boss_p[b] += r->boss_played[b];
            boss_f[b] += r->boss_failed[b];
        }
        for (c = 0; c < RL_NUM_COASTERS; c++) {
            if (r->coasters_mask & (1u << c)) {
                coast_seen[c]++;
                if (r->won_night8) coast_won8[c]++;
            }
        }
    }

    qsort(nights, (size_t)n_runs, sizeof(int), cmp_int);
    if (endless_cnt > 0) qsort(endless_nights, (size_t)endless_cnt, sizeof(int), cmp_int);

    printf("\n====================================================================\n");
    printf(" PROFILE: %-16s  |  Runs: %d  |  Horizon: Night %d\n",
           k_profiles[pid].name, n_runs, max_night);
    printf("====================================================================\n");
    printf("  Night 8 Win Rate        : %5.1f%% (%d / %d)\n",
           100.0 * won_n8 / n_runs, won_n8, n_runs);
    printf("  Survived Endless N%-2d    : %5.1f%% (%d / %d)\n",
           max_night, 100.0 * survived_max / n_runs, survived_max, n_runs);
    printf("  Night Reached (Med/P90) : N%d / N%d",
           nights[n_runs / 2], nights[(n_runs * 9) / 10]);
    if (endless_cnt > 0) {
        printf("  (Among N8 winners: Med N%d, P90 N%d)\n",
               endless_nights[endless_cnt / 2], endless_nights[(endless_cnt * 9) / 10]);
    } else {
        printf("\n");
    }
    printf("  Survival Curve          : N2=%4.0f%%  N4=%4.0f%%  N6=%4.0f%%  N8=%4.0f%%  N12=%4.0f%%  N16=%4.0f%%  N%d=%4.0f%%\n",
           100.0 * night_reached_cnt[2] / n_runs,
           100.0 * night_reached_cnt[4] / n_runs,
           100.0 * night_reached_cnt[6] / n_runs,
           100.0 * night_reached_cnt[8] / n_runs,
           100.0 * (max_night >= 12 ? night_reached_cnt[12] : 0) / n_runs,
           100.0 * (max_night >= 16 ? night_reached_cnt[16] : 0) / n_runs,
           max_night, 100.0 * survived_max / n_runs);

    printf("  Deaths (N1-N8)          : Tap=%d  Empty=%d  TargetMiss=%d  Cannon=%d\n",
           n8_d_tap, n8_d_empty, n8_d_tgt, n8_d_cannon);
    printf("  Deaths (Endless N9+)    : Tap=%d  Empty=%d  TargetMiss=%d  Cannon=%d\n",
           end_d_tap, end_d_empty, end_d_tgt, end_d_cannon);
    printf("  Total Lives Lost (all)  : Tap=%lld  Empty=%lld  Cannon=%lld\n",
           total_lives_tap, total_lives_empty, total_lives_cannon);
    printf("  Bonus Round Win Rate    : %5.1f%% (%lld / %lld)\n",
           bonus_att > 0 ? (100.0 * bonus_win / bonus_att) : 0.0, bonus_win, bonus_att);
    printf("  Physical Fairness Audit : MinEmptyTTI=%dms (%df) | ImpossibleEmptyPairs=%.2f/run | PeakThreats=%d\n",
           min_empty_tti * 20, min_empty_tti, (double)total_overlaps / n_runs, peak_threats);

    /* Mean Boss Shift Score/Target ratio across key milestone nights */
    {
        static const int k_mn[7] = { 1, 4, 8, 10, 12, 16, 20 };
        int m;
        printf("  Boss Score/Target Ratio :");
        for (m = 0; m < 7; m++) {
            int n = k_mn[m];
            double sum = 0.0;
            int cnt = 0;
            if (n > max_night) continue;
            for (i = 0; i < n_runs; i++) {
                if (reps[i].boss_ratio_valid[n]) {
                    sum += reps[i].boss_score_ratio[n];
                    cnt++;
                }
            }
            if (cnt > 0) printf(" N%d=%.2fx", n, sum / cnt);
            else         printf(" N%d=---", n);
        }
        printf("\n");
    }

    if (verbose_tables) {
        printf("\n  --- Boss Twist Fail Rates (Shift Loss %%) ---\n");
        for (b = 1; b <= RL_NUM_BOSSES; b++) {
            const BossDef *bd = rl_boss_def((uint8_t)b);
            double pct = boss_p[b] > 0 ? (100.0 * boss_f[b] / boss_p[b]) : 0.0;
            printf("    %-18s : %5.1f%% fail (%4d / %4d shifts)\n",
                   bd->name, pct, boss_f[b], boss_p[b]);
        }

        printf("\n  --- Coaster N8 Win-Rate Correlation ---\n");
        for (c = 0; c < RL_NUM_COASTERS; c++) {
            const CoasterDef *cd = rl_coaster_def((uint8_t)c);
            double pct = coast_seen[c] > 0 ? (100.0 * coast_won8[c] / coast_seen[c]) : 0.0;
            printf("    %-16s ($%u) : %5.1f%% N8 win (seen in %3d runs)\n",
                   cd->name, cd->cost, pct, coast_seen[c]);
        }
    }

    (void)d_tap; (void)d_empty; (void)d_tgt; (void)d_cannon;
    free(nights);
    free(endless_nights);
}

int main(int argc, char **argv)
{
    int n_runs = 240;
    int max_night = 20;
    int n_threads = 8;
    int only_prof = -1;
    int verbose = 1;
    uint32_t base_seed = 0x19960815u;
    int i, p;
    struct timespec t0, t1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) n_runs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--max-night") == 0 && i + 1 < argc) max_night = atoi(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) n_threads = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) base_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--brief") == 0) verbose = 0;
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            const char *pn = argv[++i];
            if (strcmp(pn, "casual") == 0) only_prof = PROF_CASUAL;
            else if (strcmp(pn, "average") == 0) only_prof = PROF_AVERAGE;
            else if (strcmp(pn, "skilled") == 0) only_prof = PROF_SKILLED;
            else only_prof = -1;
        }
    }

    if (n_runs < 1) n_runs = 1;
    if (n_threads < 1) n_threads = 1;
    if (n_threads > 32) n_threads = 32;
    if (n_threads > n_runs) n_threads = n_runs;
    if (max_night < 8) max_night = 8;
    if (max_night > 30) max_night = 30;

    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (p = 0; p < PROF_COUNT; p++) {
        RunReport *reps;
        pthread_t threads[32];
        WorkerArgs args[32];
        int t;

        if (only_prof >= 0 && p != only_prof) continue;

        reps = (RunReport *)calloc((size_t)n_runs, sizeof(RunReport));
        for (t = 0; t < n_threads; t++) {
            int chunk = n_runs / n_threads;
            int rem   = n_runs % n_threads;
            args[t].prof_id   = (ProfileId)p;
            args[t].start_idx = t * chunk + (t < rem ? t : rem);
            args[t].end_idx   = args[t].start_idx + chunk + (t < rem ? 1 : 0);
            args[t].max_night = max_night;
            args[t].base_seed = base_seed;
            args[t].reports   = reps;
            pthread_create(&threads[t], NULL, worker_main, &args[t]);
        }
        for (t = 0; t < n_threads; t++) {
            pthread_join(threads[t], NULL);
        }

        print_profile_report((ProfileId)p, reps, n_runs, max_night, verbose && (p == PROF_AVERAGE));
        free(reps);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    {
        double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
        int total_runs = (only_prof >= 0) ? n_runs : (n_runs * PROF_COUNT);
        printf("\nCompleted %d full simulations (up to Night %d) on %d threads in %.2f seconds.\n",
               total_runs, max_night, n_threads, dt);
    }
    return 0;
}
