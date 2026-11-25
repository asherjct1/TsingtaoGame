#ifndef TSINGTAO_ROOM3D_H
#define TSINGTAO_ROOM3D_H

#include <stdint.h>

typedef enum {
    CARD_KIND_COASTER = 0,
    CARD_KIND_DRINK   = 1,
    CARD_KIND_TRICK   = 2,
    CARD_KIND_REWARD  = 3
} RoomCardKind;

/* Pure integer trig helpers (returns 24.8 fixed-point value in [-256 .. 256]) */
int room3d_sin_deg(int deg);
int room3d_cos_deg(int deg);

/*
 * Map lane 0..3 (back to front) & bar_x_0_256 (0=left doorway, 256=right tap)
 * to 2.5D perspective screen coordinates (sx, sy) and scale_fp (216..262 where 256=1.0x).
 */
void room3d_lane_project(int lane, int bar_x_0_256, int *out_sx, int *out_sy, int *out_scale_fp);

/*
 * Base Ordering Table Z slot for lane 0..3 (80, 64, 48, 32 so front lanes sort over back lanes).
 * Recommended per-lane sub-layering (stays strictly inside each 16-slot lane band):
 *   z_base + 4 : doorway arch / noren (drawn by room3d_draw_barroom)
 *   z_base + 2 : customers standing behind counter
 *   z_base     : wooden bar counter & brass tap pedestal (drawn by room3d_draw_barroom)
 *   z_base - 2 : drinks, returning empties, cash tips on counter
 *   z_base - 4 : customer mood/order bubbles
 *   z_base - 5 : bartender
 */
int room3d_lane_z_base(int lane);

/* Soft elliptical 2.5D drop shadow at (sx, sy) with semi-axes (rx, ry) */
void room3d_draw_shadow(int z, int sx, int sy, int rx, int ry);

/*
 * Render 1996 PS1 2.5D Low-Poly Qingdao Night-Market Barroom:
 * - 4 dynamic Qingdao atmospheres (Sunset 1-2, Dusk 3-4, Neon 5-6, Festival 7+)
 * - Twinkling stars, festival fireworks, shimmering harbor water + pier/skyline reflection
 * - Striped awning, optional neon sign, swinging 3D low-poly red lanterns + additive glow cones
 * - Perspective tiled night-market floor with pulsing Gouraud lantern pools & blackout flicker
 * - Optional 4 perspective 3D wooden counters + doorway arches + red noren + 3D brass taps
 */
void room3d_draw_barroom(uint16_t night, uint32_t tick,
                         int active_lane, int draw_counters,
                         int draw_neon_sign, int blackout_mode);

/*
 * Extended barroom renderer with per-lane threat & buff visual feedback:
 * - lane_danger[lane] (optional NULL):
 *     0 = normal
 *     1 = warning (warm orange-red right counter glow + amber-red tap pedestal tint)
 *     2 = critical (strong pulsing crimson right counter/lip + pulsing crimson pedestal aura)
 * - lane_buff[lane] (optional NULL):
 *     > 0 = gentle shimmering gold/cyan positive highlight along front brass lip
 * - active_lane: glowing cyan-gold front lip rail trim + animated selector chevron
 */
void room3d_draw_barroom_ex(uint16_t night, uint32_t tick,
                            int active_lane, int draw_counters,
                            int draw_neon_sign, int blackout_mode,
                            const uint8_t lane_danger[4],
                            const uint8_t lane_buff[4]);

/* 2.5D foreground hero bar counter for Title Screen & Boss Bonus Round */
void room3d_draw_hero_bar(uint16_t night, uint32_t tick);

/*
 * 3D integer-rotated Balatro-style card quad (POLY_GT4) with perspective foreshortening:
 * - Shows card back (SPR_CARD_BACK) when cos(yaw) < 0
 * - Front frame by rarity (0=Common, 1=Uncommon, 2=Rare, or TRICK frame for CARD_KIND_TRICK)
 * - Centered scaled icon (Coaster / Drink / Trick / Reward seal)
 * - Optional selection gold rim & subtle rainbow diagonal holographic sheen (POLY_G4, GFX_TRANS_ADD)
 */
void room3d_draw_card(int z, int cx, int cy, int w, int h,
                      int yaw_deg, int pitch_deg,
                      int rarity, RoomCardKind kind, int icon_id,
                      int selected, int holo, uint32_t tick);

#endif /* TSINGTAO_ROOM3D_H */
