#include "gfx.h"
#include "room3d.h"
#include "roguelike.h"
#include "input.h"
#include "audio.h"
#include "memcard.h"
#include "game.h"
#include "bonus.h"
#include "assets_layout.h"
#include "sfx_table.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    STATE_TITLE         = 0,
    STATE_STARTER_DRAFT = 1,
    STATE_PLAYING       = 2,
    STATE_PAUSED        = 3,
    STATE_PAYOUT        = 4,
    STATE_BONUS         = 5,
    STATE_VICTORY       = 6,
    STATE_SHOP          = 7,
    STATE_GAME_OVER     = 8,
    STATE_NAME_ENTRY    = 9,
    STATE_HIGH_SCORES   = 10,
    STATE_TUTORIAL      = 11
} AppState;

#define TELEMETRY_MAGIC 0x54534732u /* "TSG2" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frame;
    uint8_t  app_state;
    uint8_t  night;
    uint8_t  shift_in_night;
    uint8_t  boss_id;
    int8_t   lives;
    int8_t   bartender_lane;
    uint8_t  bartender_x;
    uint8_t  frost;
    int16_t  cash;
    int16_t  shift_frames_left;
    uint32_t shift_score;
    uint32_t shift_target;
    uint32_t run_score;
    uint16_t combo;
    uint8_t  coaster_count;
    uint8_t  tricks[2];
    uint8_t  shift_tips_spawned;
    int16_t  shift_tips_cash;
    uint8_t  shift_spills;
    uint8_t  bonus_phase;
    int8_t   bonus_cursor;
    int8_t   bonus_cold_slot;
    int8_t   bonus_reward_cursor;
    uint8_t  draft_cursor;
    uint8_t  shop_zone;
    uint8_t  shop_cursor;
    int16_t  shop_reroll_cost;
    uint8_t  shop_sold_mask;
    uint8_t  lane_cust_count[4];
    uint8_t  lane_front_x[4];
    uint8_t  lane_front_drink[4];
    uint8_t  lane_front_hp[4];
    uint8_t  lane_empty_count[4];
    uint8_t  lane_empty_x[4];
    uint8_t  lane_tip_active[4];
    uint8_t  lane_tip_x[4];
    uint8_t  lane_drink_count[4];
} TelemetrySnapshot;

_Static_assert(sizeof(TelemetrySnapshot) == 87, "TelemetrySnapshot must be 87 bytes");

/* Non-static global telemetry snapshot polled from PS1 RAM by tools/record_gameplay.py */
volatile TelemetrySnapshot g_telemetry;

static AppState       g_state = STATE_TITLE;
static RunState       g_run;
static GameState      g_game;
static BonusState     g_bonus;
static DraftState     g_draft;
static PayoutState    g_payout;
static ShopState      g_shop;
static HighScoreEntry g_scores[MC_MAX_SCORES];

static int            g_title_cursor = 0;
static int            g_pause_cursor = 0;
static AppState       g_pause_return_state = STATE_PLAYING;
static int            g_victory_cursor = 0;
static int            g_state_timer = 0;

/* Arcade 3-letter initials entry state */
static const char k_charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
#define CHARSET_LEN 37
static int            g_entry_chars[3] = { 19, 18, 6 }; /* Default: T S G */
static int            g_entry_pos = 0;
static int            g_last_rank = -1;
static char           g_mc_toast[40] = "";
static int            g_mc_toast_ok = 0;

static uint8_t clamp_u8_coord(int v)
{
    if (v < 0) return 0u;
    if (v > 255) return 255u;
    return (uint8_t)v;
}

static void update_telemetry(void)
{
    int l, i;
    int bx = FROM_FP(g_game.bartender_x_fp);

    g_telemetry.magic          = TELEMETRY_MAGIC;
    g_telemetry.frame          = gfx_frame_count();
    g_telemetry.app_state      = (uint8_t)g_state;
    g_telemetry.night          = (uint8_t)(g_run.night > 255u ? 255u : g_run.night);
    g_telemetry.shift_in_night = g_run.shift_in_night;
    g_telemetry.boss_id        = (uint8_t)rl_current_boss(&g_run);
    g_telemetry.lives          = g_run.lives;
    g_telemetry.bartender_lane = g_game.lane;
    g_telemetry.bartender_x    = clamp_u8_coord(bx);
    g_telemetry.frost          = (uint8_t)(g_game.frost < 0 ? 0 : (g_game.frost > 100 ? 100 : g_game.frost));
    g_telemetry.cash           = g_run.cash;
    g_telemetry.shift_frames_left = g_game.shift_frames_left;
    g_telemetry.shift_score    = g_game.shift_score;
    g_telemetry.shift_target   = g_game.shift_target;
    g_telemetry.run_score      = g_run.run_score;
    g_telemetry.combo          = g_game.combo;
    g_telemetry.coaster_count  = g_run.coaster_count;
    g_telemetry.tricks[0]      = (uint8_t)g_run.tricks[0];
    g_telemetry.tricks[1]      = (uint8_t)g_run.tricks[1];
    g_telemetry.shift_tips_spawned = g_game.shift_tips_spawned;
    g_telemetry.shift_tips_cash    = g_game.shift_tips_cash;
    g_telemetry.shift_spills   = g_run.shift_spills;

    g_telemetry.bonus_phase         = (uint8_t)g_bonus.phase;
    g_telemetry.bonus_cursor        = g_bonus.cursor_slot;
    g_telemetry.bonus_cold_slot     = g_bonus.cold_slot;
    g_telemetry.bonus_reward_cursor = g_bonus.reward_cursor;

    g_telemetry.draft_cursor     = (uint8_t)(g_draft.cursor < 0 ? 0 : g_draft.cursor);
    g_telemetry.shop_zone        = (uint8_t)(g_shop.zone < 0 ? 0 : g_shop.zone);
    g_telemetry.shop_cursor      = (uint8_t)(g_shop.cursor < 0 ? 0 : g_shop.cursor);
    g_telemetry.shop_reroll_cost = g_shop.reroll_cost;
    g_telemetry.shop_sold_mask   = (uint8_t)(
        (g_shop.coaster_sold[0] ? 1u : 0u) |
        (g_shop.coaster_sold[1] ? 2u : 0u) |
        (g_shop.drink_sold      ? 4u : 0u) |
        (g_shop.trick_sold      ? 8u : 0u));

    for (l = 0; l < NUM_LANES; l++) {
        uint8_t c_cnt = 0;
        int32_t front_x_fp = -1;
        uint8_t f_drink = 0, f_hp = 0;

        uint8_t e_cnt = 0;
        int32_t empty_x_fp = -1;

        uint8_t tip_act = 0;
        int16_t tip_x = 0;

        uint8_t d_cnt = 0;

        for (i = 0; i < MAX_CUSTOMERS; i++) {
            const Customer *c = &g_game.customers[i];
            if (!c->active || c->lane != (uint8_t)l || c->state == CSTATE_SATISFIED) continue;
            c_cnt++;
            if (c->x_fp > front_x_fp) {
                front_x_fp = c->x_fp;
                f_drink = c->wanted_drink;
                f_hp = c->hp;
            }
        }

        for (i = 0; i < MAX_EMPTIES; i++) {
            const ReturningEmpty *e = &g_game.empties[i];
            if (!e->active || e->lane != (uint8_t)l) continue;
            e_cnt++;
            if (e->x_fp > empty_x_fp) {
                empty_x_fp = e->x_fp;
            }
        }

        for (i = 0; i < MAX_TIPS; i++) {
            const CashTip *t = &g_game.tips[i];
            if (!t->active || t->lane != (uint8_t)l) continue;
            if (!tip_act || t->x > tip_x) {
                tip_act = 1u;
                tip_x = t->x;
            }
        }

        for (i = 0; i < MAX_DRINKS; i++) {
            const SlidingDrink *d = &g_game.drinks[i];
            if (!d->active || d->lane != (uint8_t)l || d->is_rush) continue;
            d_cnt++;
        }

        g_telemetry.lane_cust_count[l]  = c_cnt;
        g_telemetry.lane_front_x[l]     = (c_cnt > 0) ? clamp_u8_coord(FROM_FP(front_x_fp)) : 0u;
        g_telemetry.lane_front_drink[l] = f_drink;
        g_telemetry.lane_front_hp[l]    = f_hp;

        g_telemetry.lane_empty_count[l] = e_cnt;
        g_telemetry.lane_empty_x[l]     = (e_cnt > 0) ? clamp_u8_coord(FROM_FP(empty_x_fp)) : 0u;

        g_telemetry.lane_tip_active[l]  = tip_act;
        g_telemetry.lane_tip_x[l]       = tip_act ? clamp_u8_coord((int)tip_x) : 0u;

        g_telemetry.lane_drink_count[l] = d_cnt;
    }
}

static void set_toast_from_status(MemCardStatus st, int is_save)
{
    if (st == MC_OK) {
        g_mc_toast_ok = 1;
        strcpy(g_mc_toast, is_save ? "SAVED TO MEMORY CARD!" : "MEMORY CARD LOADED!");
    } else if (st == MC_NO_CARD) {
        g_mc_toast_ok = 0;
        strcpy(g_mc_toast, is_save ? "NO MEMORY CARD - SESSION SAVED" : "SLOT 1: NO MEMORY CARD");
    } else if (st == MC_NO_FILE) {
        g_mc_toast_ok = 1;
        strcpy(g_mc_toast, "SLOT 1 MEMORY CARD READY");
    } else {
        const char *msg = memcard_status_str(st);
        size_t len = strlen(msg);
        g_mc_toast_ok = 0;
        if (len >= sizeof(g_mc_toast)) len = sizeof(g_mc_toast) - 1;
        memcpy(g_mc_toast, msg, len);
        g_mc_toast[len] = '\0';
    }
}

static void format_night_shift(char out[12], uint16_t night, uint8_t shift)
{
    int pos = 0;
    out[pos++] = 'N';
    if (night >= 10u) {
        out[pos++] = (char)('0' + ((night / 10u) % 10u));
    }
    out[pos++] = (char)('0' + (night % 10u));
    out[pos++] = '-';
    out[pos++] = 'S';
    out[pos++] = (char)('1' + (shift % 3u));
    out[pos] = '\0';
}

/* =========================================================================
 * TITLE SCREEN (2.5D Hero Bar + Floating 3D Showcase Cards + Safe Bottom Bar)
 * ========================================================================= */

static void start_new_run(void)
{
    audio_play_sfx(SFX_CAN_POP, 160, 0);
    rl_run_init(&g_run, gfx_frame_count() ^ 0x19960815u, g_scores[0].score);
    rl_draft_init(&g_draft, &g_run);
    g_last_rank = -1;
    g_state = STATE_STARTER_DRAFT;
    if (!audio_bgm_is_playing() && !audio_bgm_is_muted()) {
        audio_bgm_start();
    }
}

static void update_title(void)
{
    if (input_pressed(BTN_UP)) {
        g_title_cursor = (g_title_cursor + 2) % 3;
        audio_play_sfx(SFX_EMPTY_CATCH, 160, 80);
    } else if (input_pressed(BTN_DOWN)) {
        g_title_cursor = (g_title_cursor + 1) % 3;
        audio_play_sfx(SFX_EMPTY_CATCH, 160, -40);
    }

    if (input_pressed(BTN_L1) || input_pressed(BTN_R1)) {
        GfxBtnStyle next = (gfx_get_btn_style() == BTN_STYLE_PS1) ? BTN_STYLE_XBOX : BTN_STYLE_PS1;
        gfx_set_btn_style(next);
        audio_play_sfx(SFX_CARD_FLIP, 160, (next == BTN_STYLE_XBOX) ? 80 : -40);
    }

    if (input_pressed(BTN_SELECT)) {
        int next = !gfx_get_drink_glyphs();
        gfx_set_drink_glyphs(next);
        audio_play_sfx(SFX_CARD_FLIP, 160, next ? 100 : -80);
    }

    if (input_pressed(BTN_START) || input_pressed(BTN_CROSS)) {
        if (g_title_cursor == 0) {
            start_new_run();
        } else if (g_title_cursor == 1) {
            audio_play_sfx(SFX_CAN_POP, 160, 40);
            g_run.high_score = g_scores[0].score;
            game_init_tutorial(&g_game, &g_run);
            g_state = STATE_TUTORIAL;
            if (!audio_bgm_is_playing() && !audio_bgm_is_muted()) {
                audio_bgm_start();
            }
        } else {
            audio_play_sfx(SFX_TIP_PICKUP, 160, 0);
            g_last_rank = -1;
            g_state = STATE_HIGH_SCORES;
        }
    }
}

static void draw_title(void)
{
    uint32_t tick = gfx_frame_count();
    int bob_can = (((tick >> 3) & 3u) == 1) ? -2 : ((((tick >> 3) & 3u) == 3) ? 2 : 0);
    int bob_btl = -bob_can;
    int yaw_l = ((int)((tick >> 1) & 31u) - 16) * 2;
    int yaw_r = -yaw_l;

    gfx_set_shake_apply(0);
    room3d_draw_hero_bar(5, tick);

    /* Dark vignette behind logo */
    gfx_draw_rect(GFX_Z_DECOR - 2, 82, 16, 156, 68, 10, 16, 30, GFX_TRANS_HALF);

    /* REAL Digitized Tsingtao Brewery Logo (128x64) */
    gfx_draw_sheet_sprt(GFX_Z_POPUP + 2, GFX_TEX_REAL,
                        SPR_REAL_LOGO_U, SPR_REAL_LOGO_V,
                        SPR_REAL_LOGO_W, SPR_REAL_LOGO_H,
                        96, 18,
                        GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                        0, GFX_TRANS_NONE);

    /* Neon Title Banner */
    gfx_draw_rect(GFX_Z_POPUP + 4, 42, 84, 236, 18, 12, 22, 42, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_POPUP + 3, 42, 84, 236, 1, 80, 235, 160, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_POPUP + 3, 42, 101, 236, 1, 255, 220, 80, GFX_TRANS_NONE);
    {
        uint8_t br = (uint8_t)(220 + (((tick >> 2) & 3u) * 10));
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 89, "TSINGTAO EXPRESS '96", br, 240, 105);
    }

    /* Floating 3D Holographic Coaster Showcase Cards framing the Hero Bar */
    room3d_draw_card(GFX_Z_VFX, 40, 146 + bob_can, 44, 58,
                     yaw_l, 6, RARITY_RARE, CARD_KIND_COASTER,
                     COASTER_QINGDAO_TRIO, 1, 1, tick);
    room3d_draw_card(GFX_Z_VFX, 280, 146 + bob_btl, 44, 58,
                     yaw_r, -6, RARITY_RARE, CARD_KIND_COASTER,
                     COASTER_GOLDEN_TAP, 1, 1, tick);

    /* Real Digitized Showcase Can & Bottle on Hero Counter (+ toggleable mini badges) */
    gfx_draw_sheet_sprt(GFX_Z_DRINK, GFX_TEX_REAL,
                        SPR_REAL_SHOWCASE_CAN_U, SPR_REAL_SHOWCASE_CAN_V,
                        SPR_REAL_SHOWCASE_CAN_W, SPR_REAL_SHOWCASE_CAN_H,
                        72, 118 + bob_can,
                        GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                        0, GFX_TRANS_NONE);
    gfx_draw_drink_mini_btn(GFX_Z_DRINK - 1, 92, 120 + bob_can, 0);

    gfx_draw_sheet_sprt(GFX_Z_DRINK, GFX_TEX_REAL,
                        SPR_REAL_HERO_BOTTLE_U, SPR_REAL_HERO_BOTTLE_V,
                        SPR_REAL_HERO_BOTTLE_W, SPR_REAL_HERO_BOTTLE_H,
                        224, 118 + bob_btl,
                        GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                        0, GFX_TRANS_NONE);
    gfx_draw_drink_mini_btn(GFX_Z_DRINK - 1, 240, 120 + bob_btl, 1);

    /* Mascot Bartender behind Hero Counter */
    {
        int beat = (int)((tick >> 4) & 3u);
        int mu = SPR_MASCOT_IDLE_U;
        if (beat == 1) mu = SPR_MASCOT_IDLE2_U;
        else if (beat == 3) mu = SPR_MASCOT_CELEBRATE_U;
        gfx_draw_sheet_sprt(GFX_Z_BARTENDER, GFX_TEX_SPRITES,
                            mu, SPR_MASCOT_IDLE_V,
                            SPR_MASCOT_W, SPR_MASCOT_H,
                            146, 142,
                            GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                            0, GFX_TRANS_NONE);
    }
    gfx_draw_sheet_sprt(GFX_Z_BARTENDER, GFX_TEX_SPRITES,
                        SPR_BEER_BAG_U, SPR_BEER_BAG_V,
                        SPR_BEER_BAG_W, SPR_BEER_BAG_H,
                        118, 152, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                        0, GFX_TRANS_NONE);
    gfx_draw_drink_mini_btn(GFX_Z_BARTENDER - 1, 128, 150, 2);
    gfx_draw_sheet_sprt(GFX_Z_BARTENDER, GFX_TEX_ENV_UI,
                        SPR_BEER_TAP_U, SPR_BEER_TAP_V,
                        SPR_BEER_TAP_W, SPR_BEER_TAP_H,
                        188, 148, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                        0, GFX_TRANS_NONE);

    /* 3-Item Title Menu (NEW RUN / TUTORIAL / HIGH SCORES) + Quick Toggles */
    {
        static const char *const k_menu[3] = {
            "NEW RUN",
            "TUTORIAL",
            "HIGH SCORES"
        };
        int mi;
        gfx_draw_rect(GFX_Z_HUD_BG, 66, 179, 188, 39, 10, 16, 30, GFX_TRANS_HALF);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 66, 179, 188, 1, 80, 210, 150, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_HUD_BG - 1, 66, 217, 188, 1, 255, 224, 96, GFX_TRANS_NONE);
        for (mi = 0; mi < 3; mi++) {
            int my = 183 + mi * 11;
            if (mi == g_title_cursor) {
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 94, my, ">", 120, 250, 255);
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, my, k_menu[mi],
                                       (tick & 16u) ? 255 : 230, 240, 96);
            } else {
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, my, k_menu[mi], 185, 215, 235);
            }
        }
    }

    gfx_draw_btn(GFX_Z_HUD_TEXT, 14, 219, GLYPH_L1);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 35, 221,
                         (gfx_get_btn_style() == BTN_STYLE_XBOX) ? "BTN:XBOX" : "BTN:PS1",
                         140, 248, 255);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 184, 221,
                         gfx_get_drink_glyphs() ? "SEL:BADGES ON" : "SEL:BADGES OFF",
                         gfx_get_drink_glyphs() ? 160 : 255,
                         gfx_get_drink_glyphs() ? 255 : 190,
                         gfx_get_drink_glyphs() ? 175 : 120);

    /* Unclipped Bottom Controls Bar (y = 232..249) */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 232, GFX_SCREEN_W, 17, 10, 14, 24, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 0, 232, GFX_SCREEN_W, 1, 64, 180, 110, GFX_TRANS_NONE);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 10, 233, GLYPH_CROSS);
    gfx_draw_text(GFX_Z_HUD_TEXT, 26, 236, "CAN", 180, 255, 190);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 72, 233, GLYPH_CIRCLE);
    gfx_draw_text(GFX_Z_HUD_TEXT, 88, 236, "BOTTLE", 140, 255, 140);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 156, 233, GLYPH_SQUARE);
    gfx_draw_text(GFX_Z_HUD_TEXT, 172, 236, "BAG", 255, 224, 120);

    gfx_draw_btn(GFX_Z_HUD_TEXT, 220, 233, GLYPH_TRIANGLE);
    gfx_draw_text(GFX_Z_HUD_TEXT, 236, 236, "RUSH", 120, 245, 255);
}

/* =========================================================================
 * OPAQUE PAUSE MENU (Shows Coasters + Boss Twist; Prevents Frame-Scumming)
 * ========================================================================= */

static void update_paused(void)
{
    int lr_toggle = input_pressed(BTN_LEFT) || input_pressed(BTN_RIGHT);

    if (input_pressed(BTN_UP)) {
        g_pause_cursor = (g_pause_cursor + 4) % 5;
        audio_play_sfx(SFX_EMPTY_CATCH, 160, 80);
    } else if (input_pressed(BTN_DOWN)) {
        g_pause_cursor = (g_pause_cursor + 1) % 5;
        audio_play_sfx(SFX_EMPTY_CATCH, 160, -40);
    }

    if (input_pressed(BTN_START) || input_pressed(BTN_CIRCLE)) {
        audio_bgm_pause(0);
        g_state = g_pause_return_state;
        return;
    }

    if (input_pressed(BTN_CROSS) || (lr_toggle && g_pause_cursor >= 1 && g_pause_cursor <= 3)) {
        if (g_pause_cursor == 0) {
            audio_bgm_pause(0);
            g_state = g_pause_return_state;
        } else if (g_pause_cursor == 1) {
            int next = !gfx_get_drink_glyphs();
            gfx_set_drink_glyphs(next);
            audio_play_sfx(SFX_CARD_FLIP, 160, next ? 90 : -60);
        } else if (g_pause_cursor == 2) {
            GfxBtnStyle next = (gfx_get_btn_style() == BTN_STYLE_PS1) ? BTN_STYLE_XBOX : BTN_STYLE_PS1;
            gfx_set_btn_style(next);
            audio_play_sfx(SFX_CARD_FLIP, 160, (next == BTN_STYLE_XBOX) ? 90 : -60);
        } else if (g_pause_cursor == 3) {
            int muted = !audio_bgm_is_muted();
            audio_bgm_set_muted(muted);
            audio_play_sfx(SFX_TIP_PICKUP, 160, muted ? -120 : 120);
        } else {
            audio_bgm_pause(0);
            audio_play_sfx(SFX_WRONG_DRINK, 160, 0);
            g_title_cursor = 0;
            g_state = STATE_TITLE;
        }
    }
}

static void draw_paused(void)
{
    int i;
    BossType boss = rl_current_boss(&g_run);
    const BossDef *bdef = rl_boss_def((uint8_t)boss);
    const char *items[5];

    items[0] = (g_pause_return_state == STATE_TUTORIAL) ? "RESUME TUTORIAL" : "RESUME SHIFT";
    items[1] = gfx_get_drink_glyphs() ? "DRINK GLYPHS: ON" : "DRINK GLYPHS: OFF";
    items[2] = (gfx_get_btn_style() == BTN_STYLE_XBOX) ? "BTN STYLE: XBOX" : "BTN STYLE: PS1";
    items[3] = audio_bgm_is_muted() ? "MUSIC: OFF" : "MUSIC: ON";
    items[4] = "QUIT TO TITLE";

    if (g_pause_return_state == STATE_TUTORIAL) {
        game_draw_tutorial(&g_game, &g_run);
    } else {
        game_draw(&g_game, &g_run);
    }

    /* Fully opaque anti-frame-scum panel covering the bar lanes */
    gfx_set_shake_apply(0);
    gfx_draw_rect(GFX_Z_OVERLAY + 2, 16, 18, 288, 210, 12, 18, 34, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_OVERLAY + 1, 16, 18, 288, 2, 255, 224, 90, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_OVERLAY + 1, 16, 226, 288, 2, 255, 224, 90, GFX_TRANS_NONE);

    gfx_draw_text_centered(GFX_Z_OVERLAY, 24,
                           (g_pause_return_state == STATE_TUTORIAL) ? "- TUTORIAL PAUSED -" : "- SHIFT PAUSED -",
                           255, 236, 96);

    /* Boss Twist / Shift Rule */
    gfx_draw_text_centered(GFX_Z_OVERLAY, 37,
                           (g_pause_return_state == STATE_TUTORIAL) ? "GUIDED TRAINING MODE" :
                           (boss != BOSS_NONE ? bdef->name :
                            (g_run.shift_in_night == 1 ? "HAPPY HOUR SHIFT" : "REGULAR SHIFT")),
                           (g_pause_return_state != STATE_TUTORIAL && boss != BOSS_NONE) ? 255 : 140,
                           (g_pause_return_state != STATE_TUTORIAL && boss != BOSS_NONE) ? 120 : 245,
                           (g_pause_return_state != STATE_TUTORIAL && boss != BOSS_NONE) ? 100 : 255);
    if (g_pause_return_state != STATE_TUTORIAL && bdef && boss != BOSS_NONE) {
        gfx_draw_text_centered(GFX_Z_OVERLAY, 48, bdef->desc, 220, 230, 245);
    }

    /* Equipped Coasters List (compact 2-column grid) */
    gfx_draw_rect(GFX_Z_OVERLAY + 1, 28, 60, 264, 1, 64, 100, 140, GFX_TRANS_NONE);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 28, 65, "EQUIPPED COASTERS:", 160, 235, 255);
    if (g_run.coaster_count == 0) {
        gfx_draw_text_shadow(GFX_Z_OVERLAY, 42, 79, "(NONE)", 140, 160, 180);
    } else {
        for (i = 0; i < (int)g_run.coaster_count && i < RL_MAX_COASTERS; i++) {
            const CoasterDef *cdef = rl_coaster_def(g_run.coasters[i]);
            int col = i / 3;
            int row = i % 3;
            int rx = 28 + col * 136;
            int ry = 77 + row * 12;
            char cname[15];
            int k;
            for (k = 0; k < 14 && cdef->name[k]; k++) {
                cname[k] = cdef->name[k];
            }
            cname[k] = '\0';
            gfx_draw_sheet_sprt_scaled(GFX_Z_OVERLAY, GFX_TEX_CARDS,
                                       SPR_COASTER_ICON_U(cdef->id), SPR_COASTER_ICON_V(cdef->id),
                                       SPR_COASTER_ICON_W, SPR_COASTER_ICON_H,
                                       rx, ry - 1, 10, 10,
                                       GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                       0, GFX_TRANS_NONE);
            gfx_draw_text_shadow(GFX_Z_OVERLAY, rx + 13, ry, cname, 255, 236, 130);
        }
    }

    /* 5-Item Menu choices */
    gfx_draw_rect(GFX_Z_OVERLAY + 1, 28, 117, 264, 1, 64, 100, 140, GFX_TRANS_NONE);
    for (i = 0; i < 5; i++) {
        int iy = 123 + i * 12;
        if (i == g_pause_cursor) {
            gfx_draw_text_shadow(GFX_Z_OVERLAY, 68, iy, ">", 120, 250, 255);
            gfx_draw_text_shadow(GFX_Z_OVERLAY, 84, iy, items[i], 255, 255, 130);
        } else {
            gfx_draw_text_shadow(GFX_Z_OVERLAY, 84, iy, items[i], 185, 200, 215);
        }
    }

    /* Glossy Button Guide */
    gfx_draw_btn(GFX_Z_OVERLAY, 36, 187, GLYPH_DPAD);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 53, 190, "NAV", 170, 225, 255);
    gfx_draw_btn(GFX_Z_OVERLAY, 110, 187, GLYPH_CROSS);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 127, 190, "TOGGLE/OK", 160, 255, 175);
    gfx_draw_btn(GFX_Z_OVERLAY, 224, 187, GLYPH_CIRCLE);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 241, 190, "BACK", 220, 235, 245);

    /* 1-line 2-category visual legend */
    gfx_draw_rect(GFX_Z_OVERLAY + 1, 28, 207, 264, 1, 64, 100, 140, GFX_TRANS_NONE);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 24, 213, "RED !=DANGER/CATCH", 255, 130, 100);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 184, 213, "GOLD/CYAN=BUFF", 255, 236, 110);
}

/* =========================================================================
 * NIGHT 8 VICTORY CHOICE (Endless Mode vs Cash Out)
 * ========================================================================= */

static void update_victory(void)
{
    if (input_pressed(BTN_UP) || input_pressed(BTN_DOWN)) {
        g_victory_cursor ^= 1;
        audio_play_sfx(SFX_CARD_FLIP, 160, 40);
    }
    if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) {
        if (g_victory_cursor == 0) {
            /* Endless Mode (Night 9+): play Boss Bonus Round then continue! */
            g_run.endless_mode = 1;
            audio_play_sfx(SFX_BONUS_WIN, 160, 0);
            bonus_init(&g_bonus, &g_run);
            g_state = STATE_BONUS;
        } else {
            /* Cash Out & save run record */
            audio_play_sfx(SFX_SHOP_BUY, 160, 0);
            if (memcard_qualifies(g_scores, g_run.run_score)) {
                g_entry_pos = 0;
                g_state = STATE_NAME_ENTRY;
            } else {
                g_last_rank = -1;
                g_state = STATE_HIGH_SCORES;
            }
        }
    }
}

static void draw_victory(void)
{
    uint32_t tick = gfx_frame_count();
    int i;
    const char *opts[2] = {
        "ENDLESS MODE (NIGHT 9+!)",
        "CASH OUT & SAVE RECORD"
    };

    gfx_set_shake_apply(0);
    room3d_draw_hero_bar(8, tick);

    gfx_draw_rect(GFX_Z_HUD_BG, 24, 34, 272, 178, 12, 20, 38, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 24, 34, 272, 2, 255, 224, 90, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 24, 210, 272, 2, 255, 224, 90, GFX_TRANS_NONE);

    gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_ENV_UI,
                        SPR_HUD_STAR_U, SPR_HUD_STAR_V, SPR_HUD_STAR_W, SPR_HUD_STAR_H,
                        42, 46, 255, 236, 90, 0, GFX_TRANS_NONE);
    gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_ENV_UI,
                        SPR_HUD_STAR_U, SPR_HUD_STAR_V, SPR_HUD_STAR_W, SPR_HUD_STAR_H,
                        270, 46, 255, 236, 90, 0, GFX_TRANS_NONE);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 46, "NIGHT 8 CHAMPION!", 255, 236, 96);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 64, "QINGDAO NIGHT MARKET CONQUERED!", 140, 255, 170);

    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 56, 90, "RUN SCORE:", 190, 225, 245);
    gfx_draw_int(GFX_Z_HUD_TEXT, 160, 90, (int32_t)g_run.run_score, 7, 255, 236, 96);

    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 56, 108, "PATRONS SERVED:", 190, 225, 245);
    gfx_draw_int(GFX_Z_HUD_TEXT, 188, 108, (int32_t)g_run.served_total, 4, 150, 255, 175);

    gfx_draw_rect(GFX_Z_HUD_BG - 1, 44, 128, 232, 1, 90, 150, 190, GFX_TRANS_NONE);

    for (i = 0; i < 2; i++) {
        int oy = 144 + i * 24;
        if (i == g_victory_cursor) {
            gfx_draw_rect(GFX_Z_HUD_BG - 1, 42, oy - 4, 236, 16, 36, 76, 56, GFX_TRANS_HALF);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 50, oy, ">", 120, 250, 255);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 64, oy, opts[i], 255, 255, 130);
        } else {
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 64, oy, opts[i], 185, 200, 220);
        }
    }

    gfx_draw_btn(GFX_Z_HUD_TEXT, 74, 231, GLYPH_DPAD);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 91, 234, "CHOOSE", 160, 235, 255);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 166, 231, GLYPH_CROSS);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 183, 234, "CONFIRM", 160, 255, 175);
}

/* =========================================================================
 * GAME OVER SCREEN
 * ========================================================================= */

static void update_game_over(void)
{
    g_state_timer++;
    if (g_state_timer > 35 &&
        (input_pressed(BTN_CROSS) || input_pressed(BTN_START) || g_state_timer > 240)) {
        if (memcard_qualifies(g_scores, g_run.run_score)) {
            g_entry_pos = 0;
            g_state = STATE_NAME_ENTRY;
            audio_play_sfx(SFX_RUSH_READY, 160, 0);
        } else {
            g_last_rank = -1;
            g_state = STATE_HIGH_SCORES;
        }
    }
}

static void draw_game_over(void)
{
    char ns[12];
    game_draw(&g_game, &g_run);

    gfx_set_shake_apply(0);
    gfx_draw_rect(GFX_Z_OVERLAY + 2, 0, 0, GFX_SCREEN_W, GFX_SCREEN_H, 16, 8, 12, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_OVERLAY + 1, 42, 62, 236, 126, 18, 14, 28, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_OVERLAY, 42, 62, 236, 2, 255, 100, 80, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_OVERLAY, 42, 186, 236, 2, 255, 100, 80, GFX_TRANS_NONE);

    gfx_draw_text_centered(GFX_Z_OVERLAY, 74, "SHIFT OVER!", 255, 110, 90);

    gfx_draw_text_shadow(GFX_Z_OVERLAY, 62, 100, "RUN SCORE:", 200, 220, 240);
    gfx_draw_int(GFX_Z_OVERLAY, 166, 100, (int32_t)g_run.run_score, 7, 255, 236, 96);

    format_night_shift(ns, g_run.night, g_run.shift_in_night);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 62, 118, "REACHED:", 200, 220, 240);
    gfx_draw_text_shadow(GFX_Z_OVERLAY, 174, 118, ns, 140, 250, 255);

    gfx_draw_text_shadow(GFX_Z_OVERLAY, 62, 136, "SERVED:", 200, 220, 240);
    gfx_draw_int(GFX_Z_OVERLAY, 174, 136, (int32_t)g_run.served_total, 4, 160, 255, 170);

    if (g_state_timer > 35 && ((gfx_frame_count() & 16u) == 0)) {
        gfx_draw_text_centered(GFX_Z_OVERLAY, 164, "PRESS X OR START", 255, 255, 255);
    }
}

/* =========================================================================
 * 3-LETTER ARCADE INITIALS ENTRY + MEMORY CARD v2 SAVE
 * ========================================================================= */

static void finalize_name_entry(void)
{
    char initials[4];
    MemCardStatus st;

    initials[0] = k_charset[g_entry_chars[0]];
    initials[1] = k_charset[g_entry_chars[1]];
    initials[2] = k_charset[g_entry_chars[2]];
    initials[3] = '\0';

    g_last_rank = memcard_insert_score_ex(g_scores, initials, g_run.run_score,
                                          g_run.night, g_run.shift_in_night, g_run.won_night8);
    st = memcard_save_scores(g_scores);
    set_toast_from_status(st, 1);

    audio_play_sfx(SFX_BONUS_WIN, 160, 0);
    g_state = STATE_HIGH_SCORES;
}

static void update_name_entry(void)
{
    if (input_pressed(BTN_UP)) {
        g_entry_chars[g_entry_pos] = (g_entry_chars[g_entry_pos] + 1) % CHARSET_LEN;
        audio_play_sfx(SFX_EMPTY_CATCH, 160, 120);
    } else if (input_pressed(BTN_DOWN)) {
        g_entry_chars[g_entry_pos] = (g_entry_chars[g_entry_pos] + CHARSET_LEN - 1) % CHARSET_LEN;
        audio_play_sfx(SFX_EMPTY_CATCH, 160, -80);
    } else if (input_pressed(BTN_LEFT) || input_pressed(BTN_CIRCLE)) {
        if (g_entry_pos > 0) {
            g_entry_pos--;
            audio_play_sfx(SFX_EMPTY_CATCH, 140, 0);
        }
    } else if (input_pressed(BTN_RIGHT)) {
        if (g_entry_pos < 2) {
            g_entry_pos++;
            audio_play_sfx(SFX_EMPTY_CATCH, 180, 0);
        }
    } else if (input_pressed(BTN_CROSS)) {
        if (g_entry_pos < 2) {
            g_entry_pos++;
            audio_play_sfx(SFX_TIP_PICKUP, 160, 60);
        } else {
            finalize_name_entry();
        }
    } else if (input_pressed(BTN_START)) {
        finalize_name_entry();
    }
}

static void draw_name_entry(void)
{
    uint32_t tick = gfx_frame_count();
    int i;

    gfx_set_shake_apply(0);
    room3d_draw_barroom(7, tick, -1, 0, 0, 0);

    gfx_draw_rect(GFX_Z_HUD_BG, 34, 46, 252, 168, 12, 18, 34, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 34, 46, 252, 2, 255, 224, 90, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 34, 212, 252, 2, 255, 224, 90, GFX_TRANS_NONE);

    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 58, "NEW TOP 5 HIGH SCORE!", 255, 236, 96);

    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 88, 78, "SCORE:", 160, 230, 255);
    gfx_draw_int(GFX_Z_HUD_TEXT, 144, 78, (int32_t)g_run.run_score, 7, 255, 255, 255);

    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 100, "ENTER YOUR INITIALS", 180, 255, 190);

    for (i = 0; i < 3; i++) {
        int bx = 114 + i * 36;
        int by = 122;
        char ch[2];
        ch[0] = k_charset[g_entry_chars[i]];
        ch[1] = '\0';

        if (i == g_entry_pos) {
            gfx_draw_rect(GFX_Z_HUD_BG - 2, bx - 2, by - 2, 24, 28, 255, 230, 90, GFX_TRANS_NONE);
            gfx_draw_rect(GFX_Z_HUD_BG - 3, bx, by, 20, 24, 24, 42, 74, GFX_TRANS_NONE);
            if ((tick & 8u) == 0) {
                gfx_draw_text(GFX_Z_HUD_TEXT, bx + 6, by - 12, "^", 255, 236, 96);
                gfx_draw_text(GFX_Z_HUD_TEXT, bx + 6, by + 28, "V", 255, 236, 96);
            }
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, bx + 6, by + 8, ch, 255, 255, 120);
        } else {
            gfx_draw_rect(GFX_Z_HUD_BG - 3, bx, by, 20, 24, 18, 26, 44, GFX_TRANS_NONE);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, bx + 6, by + 8, ch, 210, 220, 235);
        }
    }

    gfx_draw_btn(GFX_Z_HUD_TEXT, 58, 171, GLYPH_DPAD);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 75, 174, "LETTER", 190, 225, 255);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 154, 171, GLYPH_CROSS);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 171, 174, "CONFIRM", 160, 255, 175);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 192, "START: SAVE TO MEMORY CARD", 150, 255, 165);
}

/* =========================================================================
 * TOP 5 HIGH SCORES SCREEN (v2 with Night/Shift & Crown!)
 * ========================================================================= */

static void update_high_scores(void)
{
    if (input_pressed(BTN_START) || input_pressed(BTN_CROSS) ||
        input_pressed(BTN_SELECT) || input_pressed(BTN_CIRCLE)) {
        audio_play_sfx(SFX_EMPTY_CATCH, 160, 0);
        g_state = STATE_TITLE;
    }
}

static void draw_high_scores(void)
{
    static const char *const k_ranks[MC_MAX_SCORES] = { "1ST", "2ND", "3RD", "4TH", "5TH" };
    uint32_t tick = gfx_frame_count();
    int i;

    gfx_set_shake_apply(0);
    room3d_draw_barroom(5, tick, -1, 0, 0, 0);

    gfx_draw_rect(GFX_Z_HUD_BG, 22, 32, 276, 188, 12, 18, 32, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 22, 32, 276, 2, 80, 230, 150, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 22, 218, 276, 2, 80, 230, 150, GFX_TRANS_NONE);

    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 42, "TSINGTAO EXPRESS '96", 255, 236, 96);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 56, "TOP 5 QINGDAO BARTENDERS", 130, 248, 255);

    gfx_draw_text(GFX_Z_HUD_TEXT, 34, 76, "RNK NAME   SCORE    SHIFT", 160, 190, 215);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 32, 87, 256, 1, 90, 150, 180, GFX_TRANS_NONE);

    for (i = 0; i < MC_MAX_SCORES; i++) {
        int ry = 95 + i * 18;
        int hi = (i == g_last_rank);
        char ns[12];
        uint8_t r = hi ? 255 : ((i == 0) ? 255 : 225);
        uint8_t g = hi ? ((tick & 8u) ? 255 : 210) : ((i == 0) ? 230 : 235);
        uint8_t b = hi ? 96 : ((i == 0) ? 105 : 245);

        if (hi) {
            gfx_draw_rect(GFX_Z_HUD_BG - 1, 30, ry - 3, 260, 14, 36, 78, 56, GFX_TRANS_HALF);
        }

        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 34, ry, k_ranks[i], r, g, b);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 68, ry, g_scores[i].initials, r, g, b);
        gfx_draw_int(GFX_Z_HUD_TEXT, 108, ry, (int32_t)g_scores[i].score, 7, r, g, b);

        format_night_shift(ns, g_scores[i].wave, g_scores[i].shift);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 196, ry, ns, 140, 230, 180);

        if (g_scores[i].won) {
            gfx_draw_sheet_sprt(GFX_Z_HUD_TEXT, GFX_TEX_ENV_UI,
                                SPR_HUD_STAR_U, SPR_HUD_STAR_V, SPR_HUD_STAR_W, SPR_HUD_STAR_H,
                                248, ry, 255, 236, 80, 0, GFX_TRANS_NONE);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 258, ry, "WIN", 255, 236, 80);
        }
    }

    if (g_mc_toast[0] != '\0') {
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 194, g_mc_toast,
                               g_mc_toast_ok ? 130 : 255,
                               g_mc_toast_ok ? 255 : 200,
                               g_mc_toast_ok ? 160 : 110);
    }

    if ((tick & 24u) < 16u) {
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 66, 232, "START OR", 255, 255, 255);
        gfx_draw_btn(GFX_Z_HUD_TEXT, 134, 229, GLYPH_CROSS);
        gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 152, 232, "FOR TITLE", 160, 240, 255);
    }
}

/* =========================================================================
 * PLAYSTATION 1 MAIN ENTRYPOINT (PAL 50 Hz)
 * ========================================================================= */

int main(void)
{
    MemCardStatus mc_st;

    gfx_init();
    input_init();
    memcard_init();
    audio_init();

    mc_st = memcard_load_scores(g_scores);
    set_toast_from_status(mc_st, 0);

    rl_run_init(&g_run, 0x19960815u, g_scores[0].score);
    memset(&g_game, 0, sizeof(g_game));
    memset(&g_bonus, 0, sizeof(g_bonus));
    memset(&g_draft, 0, sizeof(g_draft));
    memset(&g_payout, 0, sizeof(g_payout));
    memset(&g_shop, 0, sizeof(g_shop));

    audio_bgm_start();
    g_state = STATE_TITLE;
    update_telemetry();

    for (;;) {
        input_update();
        audio_bgm_update();

        switch (g_state) {
            case STATE_TITLE:
                update_title();
                break;

            case STATE_STARTER_DRAFT:
                if (rl_draft_update(&g_draft, &g_run)) {
                    game_init_shift(&g_game, &g_run);
                    g_state = STATE_PLAYING;
                }
                break;

            case STATE_PLAYING: {
                GameStepResult res = game_update(&g_game, &g_run);
                if (res == GAME_STEP_PAUSE) {
                    g_pause_cursor = 0;
                    g_pause_return_state = STATE_PLAYING;
                    audio_bgm_pause(1);
                    g_state = STATE_PAUSED;
                } else if (res == GAME_STEP_SHIFT_WIN) {
                    rl_payout_init(&g_payout, &g_run, g_game.shift_score,
                                   g_game.shift_target, g_game.shift_tips_cash);
                    g_state = STATE_PAYOUT;
                } else if (res == GAME_STEP_BONUS) {
                    bonus_init(&g_bonus, &g_run);
                    g_state = STATE_BONUS;
                } else if (res == GAME_STEP_GAME_OVER) {
                    g_state_timer = 0;
                    g_state = STATE_GAME_OVER;
                }
                break;
            }

            case STATE_TUTORIAL: {
                GameStepResult res = game_update_tutorial(&g_game, &g_run);
                if (res == GAME_STEP_PAUSE) {
                    g_pause_cursor = 0;
                    g_pause_return_state = STATE_TUTORIAL;
                    audio_bgm_pause(1);
                    g_state = STATE_PAUSED;
                } else if (res == GAME_STEP_SHIFT_WIN) {
                    start_new_run();
                } else if (res == GAME_STEP_GAME_OVER) {
                    g_title_cursor = 0;
                    g_state = STATE_TITLE;
                }
                break;
            }

            case STATE_PAUSED:
                update_paused();
                break;

            case STATE_PAYOUT:
                if (rl_payout_update(&g_payout, &g_run)) {
                    if (g_run.shift_in_night == (uint8_t)SHIFT_BOSS) {
                        if (g_run.night == RL_WIN_NIGHT && !g_run.endless_mode && !g_run.won_night8) {
                            g_run.won_night8 = 1u;
                            g_victory_cursor = 0;
                            audio_play_sfx(SFX_BONUS_WIN, 160, 0);
                            g_state = STATE_VICTORY;
                        } else {
                            bonus_init(&g_bonus, &g_run);
                            g_state = STATE_BONUS;
                        }
                    } else {
                        rl_shop_init(&g_shop, &g_run);
                        g_state = STATE_SHOP;
                    }
                }
                break;

            case STATE_BONUS:
                if (bonus_update(&g_bonus, &g_run)) {
                    rl_shop_init(&g_shop, &g_run);
                    g_state = STATE_SHOP;
                }
                break;

            case STATE_VICTORY:
                update_victory();
                break;

            case STATE_SHOP:
                if (rl_shop_update(&g_shop, &g_run)) {
                    rl_advance_shift(&g_run);
                    game_init_shift(&g_game, &g_run);
                    g_state = STATE_PLAYING;
                }
                break;

            case STATE_GAME_OVER:
                update_game_over();
                break;

            case STATE_NAME_ENTRY:
                update_name_entry();
                break;

            case STATE_HIGH_SCORES:
                update_high_scores();
                break;
        }

        update_telemetry();
        gfx_begin_frame(8, 12, 24);

        switch (g_state) {
            case STATE_TITLE:         draw_title();                   break;
            case STATE_STARTER_DRAFT: rl_draft_draw(&g_draft, &g_run); break;
            case STATE_PLAYING:       game_draw(&g_game, &g_run);     break;
            case STATE_PAUSED:        draw_paused();                  break;
            case STATE_PAYOUT:        rl_payout_draw(&g_payout, &g_run); break;
            case STATE_BONUS:         bonus_draw(&g_bonus, &g_run);   break;
            case STATE_VICTORY:       draw_victory();                 break;
            case STATE_SHOP:          rl_shop_draw(&g_shop, &g_run);  break;
            case STATE_GAME_OVER:     draw_game_over();                  break;
            case STATE_NAME_ENTRY:    draw_name_entry();                 break;
            case STATE_HIGH_SCORES:   draw_high_scores();                break;
            case STATE_TUTORIAL:      game_draw_tutorial(&g_game, &g_run); break;
        }

        gfx_end_frame();
    }

    return 0;
}
