#include "bonus.h"
#include "game.h"
#include "gfx.h"
#include "room3d.h"
#include "roguelike.h"
#include "input.h"
#include "audio.h"
#include "assets_layout.h"
#include "sfx_table.h"

#include <string.h>

/* 4 evenly spaced hero bar slots on 320x256 PAL screen */
static const int16_t k_slot_x[BONUS_NUM_CANS] = { 32, 92, 152, 212 };
#define BONUS_BAR_Y      178
#define BONUS_CAN_BASE_Y 132

static uint32_t bonus_rand(BonusState *bs)
{
    uint32_t x = bs->rng_state;
    if (x == 0) x = 0xC01DC01Du;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    bs->rng_state = x;
    return x;
}

static int find_can_in_slot(const BonusState *bs, int slot)
{
    int i;
    for (i = 0; i < BONUS_NUM_CANS; i++) {
        if ((int)bs->cans[i].slot == slot) {
            return i;
        }
    }
    return 0;
}

static void refresh_cold_slot(BonusState *bs)
{
    int i;
    for (i = 0; i < BONUS_NUM_CANS; i++) {
        if (bs->cans[i].is_cold) {
            bs->cold_slot = (int8_t)bs->cans[i].slot;
            return;
        }
    }
}

static void start_next_swap(BonusState *bs)
{
    int a = 0, b = 1;
    int slot_a, slot_b;
    int tries;

    for (tries = 0; tries < 24; tries++) {
        a = (int)(bonus_rand(bs) & 3u);
        b = (a + 1 + (int)(bonus_rand(bs) % 3u)) & 3;
        if ((a == bs->prev_swap_a && b == bs->prev_swap_b) ||
            (a == bs->prev_swap_b && b == bs->prev_swap_a)) {
            continue;
        }
        break;
    }

    slot_a = (int)bs->cans[a].slot;
    slot_b = (int)bs->cans[b].slot;

    bs->prev_swap_a = (int8_t)a;
    bs->prev_swap_b = (int8_t)b;
    bs->swap_a = (int8_t)a;
    bs->swap_b = (int8_t)b;
    bs->swap_start_ax = k_slot_x[slot_a];
    bs->swap_start_bx = k_slot_x[slot_b];
    bs->swap_tick = 0;

    /* Exchange logical slots immediately so end coordinates match */
    bs->cans[a].slot = (uint8_t)slot_b;
    bs->cans[b].slot = (uint8_t)slot_a;
    refresh_cold_slot(bs);

    audio_play_sfx(SFX_BOTTLE_SLIDE,
                   (bs->swap_start_ax + bs->swap_start_bx) / 2,
                   (int)(bonus_rand(bs) % 120u) - 60);
}

static void apply_reward_choice(BonusState *bs, RunState *run)
{
    if (!run) return;

    if (bs->reward_cursor == 0) {
        if (run->coaster_count < RL_MAX_COASTERS &&
            rl_add_coaster(run, (CoasterId)bs->reward_coaster)) {
            /* Free Rare Coaster granted */
        } else {
            run->cash = (int16_t)(run->cash + 8);
        }
    } else if (bs->reward_cursor == 1) {
        if (run->lives < MAX_LIVES) {
            run->lives++;
        } else {
            run->cash = (int16_t)(run->cash + 8);
        }
    } else {
        run->cash = (int16_t)(run->cash + 10);
    }
}

void bonus_init(BonusState *bs, RunState *run)
{
    int i;
    int cold_idx;
    int tier;

    memset(bs, 0, sizeof(*bs));
    bs->night = (run && run->night > 0) ? run->night : 1;
    bs->phase = BONUS_PHASE_INTRO;
    bs->phase_timer = 75; /* 1.5s intro banner */
    bs->rng_state = (run ? run->rng_state : 0x51A79600u) ^ (gfx_frame_count() * 2654435761u) ^ 0x51A79600u;
    if (bs->rng_state == 0) bs->rng_state = 0x51A79600u;

    cold_idx = (int)(bonus_rand(bs) & 3u);
    for (i = 0; i < BONUS_NUM_CANS; i++) {
        bs->cans[i].slot = (uint8_t)i;
        bs->cans[i].x_fp = TO_FP(k_slot_x[i]);
        bs->cans[i].y_fp = TO_FP(BONUS_CAN_BASE_Y);
        bs->cans[i].is_cold = (i == cold_idx) ? 1 : 0;
    }
    bs->cold_slot = (int8_t)cold_idx;

    tier = (int)bs->night;
    if (tier < 1) tier = 1;
    bs->swaps_left = (int16_t)(6 + tier);
    if (bs->swaps_left > 12) bs->swaps_left = 12;

    bs->swap_dur = (int16_t)(21 - tier);
    if (bs->swap_dur < 14) bs->swap_dur = 14;

    bs->swap_a = -1;
    bs->swap_b = -1;
    bs->prev_swap_a = -1;
    bs->prev_swap_b = -1;
    bs->cursor_slot = 0;
    bs->picked_can = -1;
    bs->won = 0;
    bs->pick_timeout = 500;
    bs->reward_cursor = -1;
    bs->reward_coaster = run ? (uint8_t)rl_roll_rare_coaster(run) : 0;
}

int bonus_update(BonusState *bs, RunState *run)
{
    int i;

    /* Pause is intentionally disabled during the Boss Bonus Round */
    switch (bs->phase) {
        case BONUS_PHASE_INTRO:
            if (bs->phase_timer > 0) {
                bs->phase_timer--;
            }
            if (bs->phase_timer == 0 || input_pressed(BTN_CROSS)) {
                bs->phase = BONUS_PHASE_SHAKE;
                bs->phase_timer = 115; /* ~2.3s clear shake phase */
                audio_play_sfx(SFX_CAN_POP, 160, -80);
            }
            break;

        case BONUS_PHASE_SHAKE:
            bs->phase_timer--;
            if ((bs->phase_timer % 22) == 0 && bs->phase_timer > 10) {
                audio_play_sfx(SFX_EMPTY_CATCH, 140, (bs->phase_timer & 4) ? 120 : -80);
            }
            if (bs->phase_timer == 0) {
                bs->phase = BONUS_PHASE_SHUFFLE;
                bs->swap_pause = 10;
                bs->swap_a = -1;
                bs->swap_b = -1;
            }
            break;

        case BONUS_PHASE_SHUFFLE:
            if (bs->swap_a < 0) {
                if (bs->swap_pause > 0) {
                    bs->swap_pause--;
                    break;
                }
                if (bs->swaps_left <= 0) {
                    for (i = 0; i < BONUS_NUM_CANS; i++) {
                        bs->cans[i].x_fp = TO_FP(k_slot_x[bs->cans[i].slot]);
                        bs->cans[i].y_fp = TO_FP(BONUS_CAN_BASE_Y);
                    }
                    refresh_cold_slot(bs);
                    bs->phase = BONUS_PHASE_PICK;
                    bs->cursor_slot = 1;
                    bs->pick_timeout = 500;
                    audio_play_sfx(SFX_RUSH_READY, 160, 0);
                    break;
                }
                start_next_swap(bs);
                bs->swaps_left--;
            } else {
                int t, dur, dest_ax, dest_bx;
                int32_t arc_h = 26;
                int32_t arc_y;

                bs->swap_tick++;
                t = bs->swap_tick;
                dur = bs->swap_dur;

                dest_ax = k_slot_x[bs->cans[bs->swap_a].slot];
                dest_bx = k_slot_x[bs->cans[bs->swap_b].slot];

                if (t >= dur) {
                    bs->cans[bs->swap_a].x_fp = TO_FP(dest_ax);
                    bs->cans[bs->swap_a].y_fp = TO_FP(BONUS_CAN_BASE_Y);
                    bs->cans[bs->swap_b].x_fp = TO_FP(dest_bx);
                    bs->cans[bs->swap_b].y_fp = TO_FP(BONUS_CAN_BASE_Y);
                    bs->swap_a = -1;
                    bs->swap_b = -1;
                    bs->swap_pause = 5;
                } else {
                    int32_t ax = bs->swap_start_ax + ((dest_ax - bs->swap_start_ax) * t) / dur;
                    int32_t bx = bs->swap_start_bx + ((dest_bx - bs->swap_start_bx) * t) / dur;
                    arc_y = (4 * arc_h * t * (dur - t)) / (dur * dur);

                    bs->cans[bs->swap_a].x_fp = TO_FP(ax);
                    bs->cans[bs->swap_a].y_fp = TO_FP(BONUS_CAN_BASE_Y - arc_y);
                    bs->cans[bs->swap_b].x_fp = TO_FP(bx);
                    bs->cans[bs->swap_b].y_fp = TO_FP(BONUS_CAN_BASE_Y + (arc_y >> 1));
                }
            }
            break;

        case BONUS_PHASE_PICK:
            if (input_pressed(BTN_LEFT)) {
                bs->cursor_slot = (int8_t)((bs->cursor_slot + BONUS_NUM_CANS - 1) % BONUS_NUM_CANS);
                audio_play_sfx(SFX_EMPTY_CATCH, k_slot_x[bs->cursor_slot] + 16, 120);
            } else if (input_pressed(BTN_RIGHT)) {
                bs->cursor_slot = (int8_t)((bs->cursor_slot + 1) % BONUS_NUM_CANS);
                audio_play_sfx(SFX_EMPTY_CATCH, k_slot_x[bs->cursor_slot] + 16, 120);
            }

            if (bs->pick_timeout > 0) {
                bs->pick_timeout--;
            }

            if (input_pressed(BTN_CROSS) || input_pressed(BTN_START) || bs->pick_timeout == 0) {
                int idx = find_can_in_slot(bs, bs->cursor_slot);
                bs->picked_can = (int8_t)idx;
                bs->won = bs->cans[idx].is_cold;
                bs->phase = BONUS_PHASE_REVEAL;
                bs->phase_timer = bs->won ? 105 : 140;

                if (bs->won) {
                    audio_play_sfx(SFX_BONUS_WIN, 160, 0);
                } else {
                    audio_play_sfx(SFX_FOAM_SPRAY, k_slot_x[bs->cursor_slot] + 16, 0);
                    gfx_set_shake(22, 4);
                }
            }
            break;

        case BONUS_PHASE_REVEAL:
            if (bs->phase_timer > 0) {
                bs->phase_timer--;
            }
            if (bs->phase_timer == 0 || (bs->phase_timer < 75 && input_pressed(BTN_CROSS | BTN_START))) {
                if (bs->won) {
                    bs->phase = BONUS_PHASE_REWARD;
                    bs->reward_cursor = 0;
                    bs->reward_flip = 24;
                    bs->reward_timeout = 600;
                    bs->reward_coaster = run ? (uint8_t)rl_roll_rare_coaster(run) : 0;
                    audio_play_sfx(SFX_CARD_FLIP, 160, 0);
                } else {
                    bs->phase = BONUS_PHASE_DONE;
                    return 1;
                }
            }
            break;

        case BONUS_PHASE_REWARD:
            if (bs->reward_flip > 0) {
                bs->reward_flip--;
            }
            if (bs->reward_timeout > 0) {
                bs->reward_timeout--;
            }
            if (input_pressed(BTN_LEFT)) {
                bs->reward_cursor = (int8_t)((bs->reward_cursor + 2) % 3);
                audio_play_sfx(SFX_CARD_FLIP, 100 + bs->reward_cursor * 60, 60);
            } else if (input_pressed(BTN_RIGHT)) {
                bs->reward_cursor = (int8_t)((bs->reward_cursor + 1) % 3);
                audio_play_sfx(SFX_CARD_FLIP, 100 + bs->reward_cursor * 60, 60);
            }
            if ((bs->reward_flip <= 6 && (input_pressed(BTN_CROSS) || input_pressed(BTN_START))) ||
                bs->reward_timeout == 0) {
                apply_reward_choice(bs, run);
                audio_play_sfx(SFX_SHOP_BUY, 160, 0);
                bs->phase = BONUS_PHASE_DONE;
                return 1;
            }
            break;

        case BONUS_PHASE_DONE:
        default:
            return 1;
    }

    return 0;
}

static void draw_reward_picker(const BonusState *bs, const RunState *run, uint32_t tick)
{
    static const int16_t k_card_x[3] = { 62, 160, 258 };
    const CoasterDef *cdef = rl_coaster_def(bs->reward_coaster);
    int i;
    int base_yaw = (bs->reward_flip > 0) ? ((int)bs->reward_flip * 7) : 0;
    int coasters_full = (run && run->coaster_count >= RL_MAX_COASTERS);
    int lives_full = (run && run->lives >= MAX_LIVES);

    gfx_set_shake_apply(0);
    gfx_draw_rect(GFX_Z_HUD_BG + 4, 0, 22, GFX_SCREEN_W, 210, 8, 12, 24, GFX_TRANS_HALF);

    /* Top header inside safe zone */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 2, GFX_SCREEN_W, 20, 12, 18, 34, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 0, 22, GFX_SCREEN_W, 1, 255, 224, 96, GFX_TRANS_NONE);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 8, "COLD ONE PRIZE! PICK 1 REWARD", 255, 236, 96);

    for (i = 0; i < 3; i++) {
        int sel = (i == (int)bs->reward_cursor);
        int cy = sel ? 102 : 108;
        int sway = sel ? (((int)((tick >> 2) & 7u) - 3) * 3) : 0;
        int pitch = sel ? -6 : 4;
        RoomCardKind kind = CARD_KIND_REWARD;
        int icon_id = i;
        int rarity = 2;

        if (i == 0) {
            if (!coasters_full) {
                kind = CARD_KIND_COASTER;
                icon_id = (int)bs->reward_coaster;
                rarity = cdef ? (int)cdef->rarity : 2;
            } else {
                icon_id = 2; /* Cash Tip Stack */
            }
        } else if (i == 1) {
            rarity = 1;
            if (lives_full) {
                icon_id = 2; /* Cash Tip Stack */
            }
        }

        room3d_draw_card(GFX_Z_POPUP, k_card_x[i], cy,
                         SPR_CARD_FRAME_W, SPR_CARD_FRAME_H,
                         base_yaw + sway, pitch,
                         rarity, kind, icon_id,
                         sel, 1, tick);

        if (i == 0) {
            if (!coasters_full) {
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, k_card_x[i] - 48, 146,
                                     "RARE COASTER", sel ? 255 : 190, sel ? 236 : 200, sel ? 96 : 220);
            } else {
                gfx_draw_text_shadow(GFX_Z_HUD_TEXT, k_card_x[i] - 36, 146,
                                     "+$8 BONUS", sel ? 140 : 180, 255, sel ? 140 : 190);
            }
        } else if (i == 1) {
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, k_card_x[i] - (lives_full ? 36 : 28), 146,
                                 lives_full ? "+$8 BONUS" : "+1 LIFE",
                                 sel ? 255 : 190, sel ? 240 : 200, sel ? 120 : 220);
        } else {
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, k_card_x[i] - 36, 146,
                                 "+$10 CASH", sel ? 140 : 180, 255, sel ? 140 : 190);
        }
    }

    /* Info panel for currently selected reward card */
    gfx_draw_rect(GFX_Z_HUD_BG, 18, 166, 284, 48, 12, 18, 34, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 18, 166, 284, 1, 255, 224, 96, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 18, 213, 284, 1, 255, 224, 96, GFX_TRANS_NONE);

    if (bs->reward_cursor == 0) {
        if (!coasters_full && cdef) {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 174, cdef->name, 255, 236, 96);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 190, cdef->desc, 190, 240, 255);
        } else {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 174, "COASTER TRAY FULL!", 255, 220, 110);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 190, "COLLECT +$8 CASH INSTEAD", 140, 255, 160);
        }
    } else if (bs->reward_cursor == 1) {
        if (!lives_full) {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 174, "EXTRA TSINGTAO CAN (+1 LIFE)", 255, 240, 120);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 190, "STAY SHARP FOR THE NEXT NIGHT!", 190, 240, 255);
        } else {
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 174, "LIVES FULL (5/5)!", 255, 220, 110);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 190, "COLLECT +$8 CASH INSTEAD", 140, 255, 160);
        }
    } else {
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 174, "BREWERY JACKPOT (+$10)", 140, 255, 160);
        gfx_draw_text_centered(GFX_Z_HUD_TEXT, 190, "SPEND BIG AT THE NIGHT SHOP!", 190, 240, 255);
    }

    /* Bottom safe-zone controls bar (y=232..247) */
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 231, GFX_SCREEN_W, 18, 10, 14, 26, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 0, 231, GFX_SCREEN_W, 1, 100, 220, 255, GFX_TRANS_NONE);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 78, 233, GLYPH_DPAD);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 95, 236, "SELECT", 130, 248, 255);
    gfx_draw_btn(GFX_Z_HUD_TEXT, 174, 233, GLYPH_CROSS);
    gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 191, 236, "CLAIM!", 160, 255, 175);
}

void bonus_draw(const BonusState *bs, const RunState *run)
{
    uint32_t tick = gfx_frame_count();
    int i;

    gfx_set_shake_apply(1);
    room3d_draw_hero_bar(bs->night, tick);

    if (bs->phase == BONUS_PHASE_REWARD || bs->phase == BONUS_PHASE_DONE) {
        draw_reward_picker(bs, run, tick);
        gfx_set_shake_apply(1);
        return;
    }

    /* Mascot Bartender standing at right end of the 2.5D hero bar */
    {
        int mu = ((tick >> 4) & 1u) ? SPR_MASCOT_IDLE2_U : SPR_MASCOT_IDLE_U;
        int mv = SPR_MASCOT_IDLE_V;
        if (bs->phase == BONUS_PHASE_REVEAL) {
            if (bs->won) {
                mu = SPR_MASCOT_CELEBRATE_U;
                mv = SPR_MASCOT_CELEBRATE_V;
            } else {
                mu = SPR_MASCOT_FOAM_U;
                mv = SPR_MASCOT_FOAM_V;
            }
        }
        gfx_draw_sheet_sprt(GFX_Z_BARTENDER, GFX_TEX_SPRITES,
                            mu, mv, SPR_MASCOT_W, SPR_MASCOT_H,
                            270, BONUS_BAR_Y - 26,
                            GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                            0, GFX_TRANS_NONE);
    }

    /* Draw the 4 Large Real-Digitized Tsingtao Hero Cans (32x48) */
    for (i = 0; i < BONUS_NUM_CANS; i++) {
        const BonusCan *c = &bs->cans[i];
        int cx = FROM_FP(c->x_fp);
        int cy = FROM_FP(c->y_fp);
        int z_can = GFX_Z_DRINK;

        if (bs->phase == BONUS_PHASE_SHAKE) {
            if (!c->is_cold) {
                int sx = (((int)(tick + (uint32_t)i * 3u) & 2) ? 4 : -4);
                int sy = (((int)(tick + (uint32_t)i * 5u) & 1) ? -3 : 3);
                cx += sx;
                cy += sy;

                gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                                    SPR_BONUS_HAND_U, SPR_BONUS_HAND_V,
                                    SPR_BONUS_HAND_W, SPR_BONUS_HAND_H,
                                    cx + 4, cy - 18,
                                    GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                                    0, GFX_TRANS_NONE);
                if ((tick & 4u) == 0) {
                    gfx_draw_sheet_sprt(GFX_Z_VFX - 1, GFX_TEX_SPRITES,
                                        SPR_FX_FOAM2_U, SPR_FX_FOAM2_V, 16, 16,
                                        cx + 8, cy - 10, 255, 210, 140, 0, GFX_TRANS_NONE);
                }
            } else {
                int sp_off = (int)((tick >> 2) & 7u) - 3;
                gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                                    ((tick & 4u) ? SPR_FX_ICE1_U : SPR_FX_ICE2_U),
                                    SPR_FX_ICE1_V, 16, 16,
                                    cx - 4, cy + 4 + sp_off, 140, 250, 255, 0, GFX_TRANS_ADD);
                gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                                    ((tick & 4u) ? SPR_FX_ICE2_U : SPR_FX_ICE1_U),
                                    SPR_FX_ICE1_V, 16, 16,
                                    cx + 22, cy + 20 - sp_off, 140, 250, 255, 0, GFX_TRANS_ADD);
                gfx_draw_text_shadow(GFX_Z_POPUP, cx - 2, cy - 16, "COLD!", 120, 250, 255);
            }
        } else if (bs->phase == BONUS_PHASE_SHUFFLE) {
            if (i == bs->swap_a) z_can = GFX_Z_DRINK - 4;
            if (i == bs->swap_b) z_can = GFX_Z_DRINK + 4;
        } else if (bs->phase == BONUS_PHASE_PICK) {
            if ((int)c->slot == bs->cursor_slot) {
                cy -= 6;
                z_can = GFX_Z_DRINK - 4;
            }
        } else if (bs->phase == BONUS_PHASE_REVEAL) {
            if (i == bs->picked_can) {
                cy -= 14;
                z_can = GFX_Z_DRINK - 6;
            }
        }

        room3d_draw_shadow(GFX_Z_COUNTER - 2, cx + 16, BONUS_BAR_Y + 3, 14, 4);

        gfx_draw_sheet_sprt(z_can, GFX_TEX_REAL,
                            SPR_REAL_HERO_CAN_U, SPR_REAL_HERO_CAN_V,
                            SPR_REAL_HERO_CAN_W, SPR_REAL_HERO_CAN_H,
                            cx, cy,
                            GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL, GFX_COLOR_NEUTRAL,
                            0, GFX_TRANS_NONE);

        if (bs->phase == BONUS_PHASE_REVEAL) {
            if (i == bs->picked_can && bs->won) {
                int k;
                for (k = 0; k < 4; k++) {
                    int ox = ((k & 1) ? 26 : -6) + (((int)(tick + (uint32_t)k * 5u) & 3) - 1) * 2;
                    int oy = (k < 2 ? -6 : 24) - (int)((tick >> 1) & 7u);
                    gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                                        ((tick + (uint32_t)k) & 4u) ? SPR_FX_ICE1_U : SPR_FX_ICE2_U,
                                        SPR_FX_ICE1_V, 16, 16,
                                        cx + ox, cy + oy, 150, 255, 255, 0, GFX_TRANS_ADD);
                }
            } else if (i == bs->picked_can && !bs->won) {
                int k;
                int mascot_x = 276;
                int mascot_y = BONUS_BAR_Y - 20;
                for (k = 0; k < 6; k++) {
                    int step = (int)(((tick >> 1) + (uint32_t)k * 3u) % 18u);
                    int fx = cx + 10 + ((mascot_x - (cx + 10)) * step) / 18;
                    int arc = (4 * 34 * step * (18 - step)) / (18 * 18);
                    int fy = (cy - 4) + ((mascot_y - (cy - 4)) * step) / 18 - arc;
                    gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                                        (k & 1) ? SPR_FX_FOAM1_U : SPR_FX_FOAM2_U,
                                        SPR_FX_FOAM1_V, 16, 16,
                                        fx, fy, 255, 255, 255, 0, GFX_TRANS_NONE);
                }
                gfx_draw_sheet_sprt(GFX_Z_VFX, GFX_TEX_SPRITES,
                                    (tick & 2u) ? SPR_FX_FOAM1_U : SPR_FX_FOAM2_U,
                                    SPR_FX_FOAM1_V, 16, 16,
                                    cx + 8, cy - 14 - (int)(tick & 3u),
                                    255, 255, 255, 0, GFX_TRANS_NONE);
            } else if (!bs->won && c->is_cold) {
                gfx_draw_text_shadow(GFX_Z_POPUP, cx - 2, cy - 14, "COLD!", 120, 250, 255);
            }
        }
    }

    /* Selection cursor during PICK phase */
    if (bs->phase == BONUS_PHASE_PICK) {
        int sx = k_slot_x[bs->cursor_slot];
        int bounce = ((tick >> 3) & 1u) ? -3 : 0;
        gfx_draw_text_shadow(GFX_Z_POPUP, sx + 12, BONUS_CAN_BASE_Y - 22 + bounce, "V", 255, 236, 80);
        gfx_draw_rect(GFX_Z_VFX, sx - 2, BONUS_CAN_BASE_Y - 10, 36, 2, 255, 236, 80, GFX_TRANS_NONE);
        gfx_draw_rect(GFX_Z_VFX, sx - 2, BONUS_BAR_Y + 2, 36, 2, 255, 236, 80, GFX_TRANS_NONE);
    }

    /* Top & Bottom Bonus Round UI Banners (strictly inside y=4..247 safe zone) */
    gfx_set_shake_apply(0);
    gfx_draw_rect(GFX_Z_HUD_BG, 0, 2, GFX_SCREEN_W, 20, 12, 16, 28, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 0, 22, GFX_SCREEN_W, 1, 255, 224, 96, GFX_TRANS_NONE);
    gfx_draw_text_centered(GFX_Z_HUD_TEXT, 8, "BOSS BONUS: SPOT THE COLD ONE!", 255, 236, 96);

    gfx_draw_rect(GFX_Z_HUD_BG, 16, 210, 288, 36, 12, 18, 32, GFX_TRANS_HALF);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 16, 210, 288, 1, 100, 220, 255, GFX_TRANS_NONE);
    gfx_draw_rect(GFX_Z_HUD_BG - 1, 16, 245, 288, 1, 100, 220, 255, GFX_TRANS_NONE);

    switch (bs->phase) {
        case BONUS_PHASE_INTRO:
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 217, "WATCH THE UNSHAKEN COLD CAN!", 130, 248, 255);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 230, "WIN: PICK A 3D BONUS CARD!", 255, 236, 96);
            break;

        case BONUS_PHASE_SHAKE:
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 217, "3 CANS ARE BEING SHAKEN!", 255, 160, 110);
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 230, "KEEP YOUR EYES ON THE COLD ONE!", 130, 248, 255);
            break;

        case BONUS_PHASE_SHUFFLE:
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 224, "SHUFFLING... STAY SHARP!", 255, 236, 96);
            break;

        case BONUS_PHASE_PICK:
            gfx_draw_text_centered(GFX_Z_HUD_TEXT, 217, "WHICH CAN IS ICE-COLD?", 255, 255, 255);
            gfx_draw_btn(GFX_Z_HUD_TEXT, 76, 227, GLYPH_DPAD);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 93, 230, "AIM", 130, 248, 255);
            gfx_draw_btn(GFX_Z_HUD_TEXT, 162, 227, GLYPH_CROSS);
            gfx_draw_text_shadow(GFX_Z_HUD_TEXT, 179, 230, "POP CAN!", 160, 255, 175);
            break;

        case BONUS_PHASE_REVEAL:
            if (bs->won) {
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 217, "ICE COLD! YOU NAILED IT!", 120, 255, 150);
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 230, "OPENING 3D PRIZE PICKER...", 255, 236, 96);
            } else {
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 217, "WHOA! SHAKEN CAN FOAM BLAST!", 255, 140, 110);
                gfx_draw_text_centered(GFX_Z_HUD_TEXT, 230, "0 PENALTY - BACK TO THE SHOP!", 255, 236, 140);
            }
            break;

        default:
            break;
    }

    gfx_set_shake_apply(1);
}
