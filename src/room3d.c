#include "room3d.h"
#include "gfx.h"
#include "../assets/gen/assets_layout.h"

#include <stddef.h>

/* =========================================================================
 * Pure Integer 256-Entry Sine Lookup Table (4096 = 1.0)
 * ========================================================================= */

static const int16_t k_sin256[256] = {
        0,   101,   201,   301,   401,   501,   601,   700,   799,   897,   995,  1092,  1189,  1285,  1380,  1474,
     1567,  1660,  1751,  1842,  1931,  2019,  2106,  2191,  2276,  2359,  2440,  2520,  2598,  2675,  2751,  2824,
     2896,  2967,  3035,  3102,  3166,  3229,  3290,  3349,  3406,  3461,  3513,  3564,  3612,  3659,  3703,  3745,
     3784,  3822,  3857,  3889,  3920,  3948,  3973,  3996,  4017,  4036,  4052,  4065,  4076,  4085,  4091,  4095,
     4096,  4095,  4091,  4085,  4076,  4065,  4052,  4036,  4017,  3996,  3973,  3948,  3920,  3889,  3857,  3822,
     3784,  3745,  3703,  3659,  3612,  3564,  3513,  3461,  3406,  3349,  3290,  3229,  3166,  3102,  3035,  2967,
     2896,  2824,  2751,  2675,  2598,  2520,  2440,  2359,  2276,  2191,  2106,  2019,  1931,  1842,  1751,  1660,
     1567,  1474,  1380,  1285,  1189,  1092,   995,   897,   799,   700,   601,   501,   401,   301,   201,   101,
        0,  -101,  -201,  -301,  -401,  -501,  -601,  -700,  -799,  -897,  -995, -1092, -1189, -1285, -1380, -1474,
    -1567, -1660, -1751, -1842, -1931, -2019, -2106, -2191, -2276, -2359, -2440, -2520, -2598, -2675, -2751, -2824,
    -2896, -2967, -3035, -3102, -3166, -3229, -3290, -3349, -3406, -3461, -3513, -3564, -3612, -3659, -3703, -3745,
    -3784, -3822, -3857, -3889, -3920, -3948, -3973, -3996, -4017, -4036, -4052, -4065, -4076, -4085, -4091, -4095,
    -4096, -4095, -4091, -4085, -4076, -4065, -4052, -4036, -4017, -3996, -3973, -3948, -3920, -3889, -3857, -3822,
    -3784, -3745, -3703, -3659, -3612, -3564, -3513, -3461, -3406, -3349, -3290, -3229, -3166, -3102, -3035, -2967,
    -2896, -2824, -2751, -2675, -2598, -2520, -2440, -2359, -2276, -2191, -2106, -2019, -1931, -1842, -1751, -1660,
    -1567, -1474, -1380, -1285, -1189, -1092,  -995,  -897,  -799,  -700,  -601,  -501,  -401,  -301,  -201,  -101
};

static int deg_to_idx(int deg) {
    int d = deg % 360;
    if (d < 0) d += 360;
    return ((d * 256 + 180) / 360) & 255;
}

static int sin_deg_4096(int deg) {
    return (int)k_sin256[deg_to_idx(deg)];
}

static int cos_deg_4096(int deg) {
    return (int)k_sin256[(deg_to_idx(deg) + 64) & 255];
}

int room3d_sin_deg(int deg) {
    return sin_deg_4096(deg) >> 4;
}

int room3d_cos_deg(int deg) {
    return cos_deg_4096(deg) >> 4;
}

static inline int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline uint8_t scale_chan(int c, int mul_256) {
    return clamp_u8((c * mul_256) >> 8);
}

/* =========================================================================
 * Perspective Lane Projection Constants
 * ========================================================================= */

static const int16_t k_lane_y[4]        = { 114, 146, 180, 214 };
static const int16_t k_lane_left_x[4]   = {  36,  30,  24,  18 };
static const int16_t k_lane_right_x[4]  = { 262, 267, 272, 276 };
static const int16_t k_lane_scale_fp[4] = { 216, 231, 246, 262 };
static const int16_t k_lane_z_base[4]   = {  80,  64,  48,  32 };

void room3d_lane_project(int lane, int bar_x_0_256, int *out_sx, int *out_sy, int *out_scale_fp) {
    int l = clamp_int(lane, 0, 3);
    int sx = (int)k_lane_left_x[l] + (((int)k_lane_right_x[l] - (int)k_lane_left_x[l]) * bar_x_0_256) / 256;
    if (out_sx) *out_sx = sx;
    if (out_sy) *out_sy = (int)k_lane_y[l];
    if (out_scale_fp) *out_scale_fp = (int)k_lane_scale_fp[l];
}

int room3d_lane_z_base(int lane) {
    return (int)k_lane_z_base[clamp_int(lane, 0, 3)];
}

/* =========================================================================
 * Soft 2.5D Drop Shadow
 * ========================================================================= */

void room3d_draw_shadow(int z, int sx, int sy, int rx, int ry) {
    int hx, hy, irx, iry;

    if (rx <= 0 || ry <= 0) return;

    hx  = (rx * 6) / 10;
    hy  = ry / 2;
    irx = (rx * 6) / 10;
    iry = (ry * 6) / 10;
    if (hx < 1) hx = 1;
    if (hy < 1) hy = 1;
    if (irx < 1) irx = 1;
    if (iry < 1) iry = 1;

    /* LIFO OT: emit inner umbra first so it draws ON TOP of outer 3-slice octagon */
    gfx_draw_poly_f4(z,
                     sx - irx, sy - iry,
                     sx + irx, sy - iry,
                     sx - irx, sy + iry,
                     sx + irx, sy + iry,
                     8, 8, 12, GFX_TRANS_HALF);

    gfx_draw_poly_f4(z,
                     sx - hx, sy - ry,
                     sx + hx, sy - ry,
                     sx - rx, sy - hy,
                     sx + rx, sy - hy,
                     14, 12, 18, GFX_TRANS_HALF);
    gfx_draw_poly_f4(z,
                     sx - rx, sy - hy,
                     sx + rx, sy - hy,
                     sx - rx, sy + hy,
                     sx + rx, sy + hy,
                     14, 12, 18, GFX_TRANS_HALF);
    gfx_draw_poly_f4(z,
                     sx - rx, sy + hy,
                     sx + rx, sy + hy,
                     sx - hx, sy + ry,
                     sx + hx, sy + ry,
                     14, 12, 18, GFX_TRANS_HALF);
}

/* =========================================================================
 * Qingdao Atmospheres & Barroom Scene Geometry
 * ========================================================================= */

typedef struct {
    uint8_t top_r, top_g, top_b;
    uint8_t mid_r, mid_g, mid_b;
    uint8_t bot_r, bot_g, bot_b;
    uint8_t sea_top_r, sea_top_g, sea_top_b;
    uint8_t sea_bot_r, sea_bot_g, sea_bot_b;
    uint8_t floor_r, floor_g, floor_b;
    uint8_t lanterns_lit;
    uint8_t stars_on;
    uint8_t neon_pulse;
    uint8_t fireworks;
} RoomAtmos;

static const RoomAtmos k_room_atmos[4] = {
    /* Night 1-2: Golden Qingdao Sunset */
    { 72, 32, 96,  218, 96, 56,  248, 180, 76,  128, 72, 74,  48, 34, 64,  88, 74, 68,  0, 0, 0, 0 },
    /* Night 3-4: Purple & Amber Dusk (Lanterns Ignite!) */
    { 26, 18, 72,   94, 40,106,  216, 102, 54,   78, 44, 72,  28, 24, 58,  72, 62, 74,  1, 1, 1, 0 },
    /* Night 5-6: Electric Neon Night Market */
    { 10, 12, 44,   22, 24, 82,   88,  26,104,   34, 28, 76,  14, 22, 54,  58, 62, 84,  1, 1, 1, 0 },
    /* Night 7+: Midnight Qingdao Beer Festival */
    {  8, 10, 34,   36, 14, 68,  136,  30, 72,   56, 26, 62,  18, 18, 46,  66, 58, 76,  1, 1, 1, 1 }
};

static const int16_t k_stars[16][3] = {
    {  14, 20,  0 }, {  38, 32,  7 }, {  58, 18, 13 }, {  84, 30,  4 },
    { 118, 16, 19 }, { 148, 28,  9 }, { 174, 18, 23 }, { 206, 32,  2 },
    { 230, 18, 15 }, { 262, 30, 11 }, { 294, 20,  5 }, { 308, 36, 17 },
    {  46, 44, 21 }, { 134, 40,  3 }, { 218, 42, 27 }, { 276, 44,  8 }
};

static int atmos_index_for_night(uint16_t night) {
    if (night <= 2) return 0;
    if (night <= 4) return 1;
    if (night <= 6) return 2;
    return 3;
}

static int compute_blackout_dim(uint32_t tick, int blackout_mode) {
    if (!blackout_mode) return 256;
    /* Mostly dim moody blackout with occasional brief fluorescent/lantern sputter */
    if ((tick % 113u) < 4u || ((tick % 173u) >= 60u && (tick % 173u) < 63u)) {
        return 178;
    }
    if ((tick % 71u) == 19u) {
        return 145;
    }
    return 94;
}

static void draw_harbor_reflection(int u, int v, int w, int h, int x) {
    gfx_draw_poly_gt4(GFX_Z_SKYLINE + 2, GFX_TEX_ENV_UI,
                      u, v + h - 1,
                      u + w - 1, v + h - 1,
                      u, v + 18,
                      u + w - 1, v + 18,
                      x, 82, 48, 52, 68,
                      x + w, 82, 48, 52, 68,
                      x - 4, 93, 24, 28, 42,
                      x + w + 4, 93, 24, 28, 42,
                      GFX_TRANS_HALF);
}

static void draw_sky_and_harbor(const RoomAtmos *at, uint32_t tick, int dim_fp, int blackout_mode) {
    int i;
    uint8_t tr = scale_chan(at->top_r, dim_fp);
    uint8_t tg = scale_chan(at->top_g, dim_fp);
    uint8_t tb = scale_chan(at->top_b, dim_fp);
    uint8_t mr = scale_chan(at->mid_r, dim_fp);
    uint8_t mg = scale_chan(at->mid_g, dim_fp);
    uint8_t mb = scale_chan(at->mid_b, dim_fp);
    uint8_t br = scale_chan(at->bot_r, dim_fp);
    uint8_t bg = scale_chan(at->bot_g, dim_fp);
    uint8_t bb = scale_chan(at->bot_b, dim_fp);

    /* Sky gradient bands (y = 0..76) */
    gfx_draw_sky_grad(GFX_Z_SKY, 0, 38, tr, tg, tb, mr, mg, mb);
    gfx_draw_sky_grad(GFX_Z_SKY, 38, 76, mr, mg, mb, br, bg, bb);

    /* Twinkling night stars */
    if (at->stars_on || blackout_mode) {
        for (i = 0; i < 16; i++) {
            int phase = (int)((tick >> 2) + (uint32_t)k_stars[i][2]) & 15;
            if (phase < 11) {
                uint8_t sb = (phase < 5) ? 205 : 120;
                gfx_draw_sheet_sprt(GFX_Z_SKY - 3, GFX_TEX_ENV_UI,
                                    SPR_HUD_STAR_U, SPR_HUD_STAR_V,
                                    SPR_HUD_STAR_W, SPR_HUD_STAR_H,
                                    k_stars[i][0], k_stars[i][1],
                                    sb, sb, 240, 0, GFX_TRANS_ADD);
            }
        }
    }

    /* Beer Festival fireworks (Night 7+) */
    if (at->fireworks && !blackout_mode) {
        int fw_cycle = (int)(tick % 60u);
        if (fw_cycle < 24) {
            int fw_id = (int)((tick / 60u) & 3u);
            int fx = 48 + fw_id * 72;
            int fy = 30 + (fw_id & 1) * 10;
            int spread = fw_cycle;
            uint8_t cr = (fw_id & 1) ? 255 : 110;
            uint8_t cg = (fw_id & 2) ? 230 : 150;
            uint8_t cb = (fw_id & 1) ? 90  : 255;

            gfx_draw_sheet_sprt(GFX_Z_SKY - 4, GFX_TEX_ENV_UI,
                                SPR_HUD_STAR_U, SPR_HUD_STAR_V, 8, 8,
                                fx - spread, fy - (spread >> 1), cr, cg, cb, 0, GFX_TRANS_ADD);
            gfx_draw_sheet_sprt(GFX_Z_SKY - 4, GFX_TEX_ENV_UI,
                                SPR_HUD_STAR_U, SPR_HUD_STAR_V, 8, 8,
                                fx + spread, fy - (spread >> 1), cr, cg, cb, 0, GFX_TRANS_ADD);
            gfx_draw_sheet_sprt(GFX_Z_SKY - 4, GFX_TEX_ENV_UI,
                                SPR_HUD_STAR_U, SPR_HUD_STAR_V, 8, 8,
                                fx, fy + (spread >> 1), cr, cg, cb, 0, GFX_TRANS_ADD);
        }
    }

    /* Harbor water strip (y = 76..94) */
    gfx_draw_poly_g4(GFX_Z_SKYLINE + 6,
                     0, 76, scale_chan(at->sea_top_r, dim_fp), scale_chan(at->sea_top_g, dim_fp), scale_chan(at->sea_top_b, dim_fp),
                     GFX_SCREEN_W, 76, scale_chan(at->sea_top_r, dim_fp), scale_chan(at->sea_top_g, dim_fp), scale_chan(at->sea_top_b, dim_fp),
                     0, 94, scale_chan(at->sea_bot_r, dim_fp), scale_chan(at->sea_bot_g, dim_fp), scale_chan(at->sea_bot_b, dim_fp),
                     GFX_SCREEN_W, 94, scale_chan(at->sea_bot_r, dim_fp), scale_chan(at->sea_bot_g, dim_fp), scale_chan(at->sea_bot_b, dim_fp),
                     GFX_TRANS_NONE);

    /* Shimmering harbor water ripples */
    for (i = 0; i < 8; i++) {
        int wave_s = room3d_sin_deg((int)tick * 3 + i * 47);
        int rx = 18 + i * 38 + ((wave_s * 6) >> 8);
        int ry = 79 + (i & 1) * 5 + ((i >> 1) & 1) * 3;
        int rw = 18 + (i & 2) * 5;
        int shimmer = 52 + ((wave_s + 256) >> 4);
        uint8_t wr = scale_chan(shimmer + 28, dim_fp);
        uint8_t wg = scale_chan(shimmer + 20, dim_fp);
        uint8_t wb = scale_chan(shimmer + 44, dim_fp);
        gfx_draw_poly_g4(GFX_Z_SKYLINE + 4,
                         rx, ry, 0, 0, 0,
                         rx + rw, ry, wr, wg, wb,
                         rx - 2, ry + 2, 0, 0, 0,
                         rx + rw + 2, ry + 2, 0, 0, 0,
                         GFX_TRANS_ADD);
    }

    /* Zhanqiao Pier Pavilion & Qingdao City Skyline + soft harbor reflections */
    {
        uint8_t sr = scale_chan(GFX_COLOR_NEUTRAL, dim_fp);
        uint8_t sb = scale_chan(GFX_COLOR_NEUTRAL + 8, dim_fp);
        if (sr < 38) sr = 38;
        if (sb < 44) sb = 44;

        gfx_draw_sheet_sprt(GFX_Z_SKYLINE, GFX_TEX_ENV_UI,
                            SPR_SKYLINE_ZHANQIAO_U, SPR_SKYLINE_ZHANQIAO_V,
                            SPR_SKYLINE_ZHANQIAO_W, SPR_SKYLINE_ZHANQIAO_H,
                            14, 34, sr, sr, sb, 0, GFX_TRANS_NONE);
        gfx_draw_sheet_sprt(GFX_Z_SKYLINE, GFX_TEX_ENV_UI,
                            SPR_SKYLINE_CITY_U, SPR_SKYLINE_CITY_V,
                            SPR_SKYLINE_CITY_W, SPR_SKYLINE_CITY_H,
                            198, 34, sr, sr, sb, 0, GFX_TRANS_NONE);

        /* Subtle inverted silhouette reflections across harbor water (y = 82..93) */
        draw_harbor_reflection(SPR_SKYLINE_ZHANQIAO_U, SPR_SKYLINE_ZHANQIAO_V,
                               SPR_SKYLINE_ZHANQIAO_W, SPR_SKYLINE_ZHANQIAO_H, 14);
        draw_harbor_reflection(SPR_SKYLINE_CITY_U, SPR_SKYLINE_CITY_V,
                               SPR_SKYLINE_CITY_W, SPR_SKYLINE_CITY_H, 198);
    }

    /* Harbor stone seawall parapet separating Jiaozhou Bay from the barroom floor */
    {
        uint8_t p_hi = scale_chan(110, dim_fp);
        uint8_t p_lo = scale_chan(54, dim_fp);
        gfx_draw_poly_g4(GFX_Z_SKYLINE - 4,
                         0, 91, p_hi, p_hi - (p_hi >> 4), p_hi - (p_hi >> 3),
                         GFX_SCREEN_W, 91, p_hi, p_hi - (p_hi >> 4), p_hi - (p_hi >> 3),
                         0, 95, p_lo, p_lo, p_lo + 6,
                         GFX_SCREEN_W, 95, p_lo, p_lo, p_lo + 6,
                         GFX_TRANS_NONE);
    }
}

static void eval_floor_vertex_light(int vx, int vy, const RoomAtmos *at, uint32_t tick,
                                    int active_lane, int dim_fp, int blackout_mode,
                                    uint8_t *out_r, uint8_t *out_g, uint8_t *out_b) {
    int r = ((int)at->floor_r * dim_fp) >> 8;
    int g = ((int)at->floor_g * dim_fp) >> 8;
    int b = ((int)at->floor_b * dim_fp) >> 8;
    int lit = at->lanterns_lit || (dim_fp > 140);
    int p1 = 216 + (room3d_sin_deg((int)tick * 4) >> 3);
    int p2 = 216 + (room3d_cos_deg((int)tick * 3 + 60) >> 3);

    /* Distance attenuation for left & right lantern warm pools */
    if (lit && (!blackout_mode || dim_fp > 120)) {
        int dxL = vx - 56;
        int dyL = vy - 134;
        int dL = (dxL * dxL) + (dyL * dyL) * 2;
        if (dL < 9000) {
            int fall = ((9000 - dL) * p1) / 9000;
            r += (fall * 62) >> 8;
            g += (fall * 30) >> 8;
            b += (fall * 8) >> 8;
        }

        int dxR = vx - 264;
        int dyR = vy - 134;
        int dR = (dxR * dxR) + (dyR * dyR) * 2;
        if (dR < 9000) {
            int fall = ((9000 - dR) * p2) / 9000;
            r += (fall * 62) >> 8;
            g += (fall * 30) >> 8;
            b += (fall * 8) >> 8;
        }
    }

    /* Active lane warm floor bounce */
    if (active_lane >= 0 && active_lane <= 3) {
        int ly = (int)k_lane_y[active_lane] + 6;
        int dyA = vy - ly;
        if (dyA < 0) dyA = -dyA;
        if (dyA < 30) {
            int boost = ((30 - dyA) * 34) / 30;
            r += boost;
            g += (boost * 3) / 4;
            b += boost / 3;
        }
    }

    /* Subtle foreground vignette darkening near bottom edge */
    if (vy > 210) {
        int edge = (vy - 210) / 2;
        r -= edge;
        g -= edge;
        b -= edge / 2;
    }

    *out_r = clamp_u8(clamp_int(r, 14, 196));
    *out_g = clamp_u8(clamp_int(g, 12, 184));
    *out_b = clamp_u8(clamp_int(b, 18, 196));
}

static void draw_perspective_floor(const RoomAtmos *at, uint32_t tick,
                                   int active_lane, int dim_fp, int blackout_mode) {
    static const int16_t k_row_y[6] = { 95, 115, 140, 171, 210, 256 };
    int row, col;
    int gx[6][9];
    uint8_t gr[6][9], gg[6][9], gb[6][9];
    int u0 = SPR_TEX_FLOOR_U;
    int v0 = SPR_TEX_FLOOR_V;
    int u1 = SPR_TEX_FLOOR_U + SPR_TEX_FLOOR_W - 1;
    int v1 = SPR_TEX_FLOOR_V + SPR_TEX_FLOOR_H - 1;

    for (row = 0; row < 6; row++) {
        int y = (int)k_row_y[row];
        int xl = 22 - ((y - 95) * 28) / 161;
        int xr = 298 + ((y - 95) * 28) / 161;
        for (col = 0; col < 9; col++) {
            int vx = xl + ((xr - xl) * col) / 8;
            gx[row][col] = vx;
            eval_floor_vertex_light(vx, y, at, tick, active_lane, dim_fp, blackout_mode,
                                    &gr[row][col], &gg[row][col], &gb[row][col]);
        }
    }

    for (row = 0; row < 5; row++) {
        int y0 = (int)k_row_y[row];
        int y1 = (int)k_row_y[row + 1];
        for (col = 0; col < 8; col++) {
            gfx_draw_poly_gt4(88, GFX_TEX_ENV_UI,
                              u0, v0, u1, v0, u0, v1, u1, v1,
                              gx[row][col],     y0, gr[row][col],     gg[row][col],     gb[row][col],
                              gx[row][col + 1], y0, gr[row][col + 1], gg[row][col + 1], gb[row][col + 1],
                              gx[row + 1][col],     y1, gr[row + 1][col],     gg[row + 1][col],     gb[row + 1][col],
                              gx[row + 1][col + 1], y1, gr[row + 1][col + 1], gg[row + 1][col + 1], gb[row + 1][col + 1],
                              GFX_TRANS_NONE);
        }
    }
}

static void draw_awning_and_lanterns(const RoomAtmos *at, uint32_t tick,
                                     int draw_neon_sign, int dim_fp, int blackout_mode) {
    static const int16_t k_lx[4] = { 26, 68, 252, 294 };
    int i, x;
    int au0 = SPR_AWNING_TILE_U;
    int av0 = SPR_AWNING_TILE_V;
    int au1 = SPR_AWNING_TILE_U + SPR_AWNING_TILE_W - 1;
    int av1 = SPR_AWNING_TILE_V + SPR_AWNING_TILE_H - 1;
    uint8_t top_c = scale_chan(136, dim_fp);
    uint8_t bot_c = scale_chan(102, dim_fp);
    if (top_c < 44) top_c = 44;
    if (bot_c < 34) bot_c = 34;

    /* 3D angled striped Qingdao awning overhang */
    for (x = 0; x < GFX_SCREEN_W; x += SPR_AWNING_TILE_W) {
        int skew0 = (x - 160) / 24;
        int skew1 = (x + SPR_AWNING_TILE_W - 160) / 24;
        gfx_draw_poly_gt4(GFX_Z_DECOR, GFX_TEX_ENV_UI,
                          au0, av0, au1, av0, au0, av1, au1, av1,
                          x, 16, top_c, top_c, top_c,
                          x + SPR_AWNING_TILE_W, 16, top_c, top_c, top_c,
                          x + skew0, 32, bot_c, bot_c, bot_c,
                          x + SPR_AWNING_TILE_W + skew1, 32, bot_c, bot_c, bot_c,
                          GFX_TRANS_NONE);
    }

    /* Shadow band under awning */
    gfx_draw_poly_g4(GFX_Z_DECOR + 1,
                     0, 32, 0, 0, 0,
                     GFX_SCREEN_W, 32, 0, 0, 0,
                     0, 38, 0, 0, 0,
                     GFX_SCREEN_W, 38, 0, 0, 0,
                     GFX_TRANS_HALF);

    /* Optional Glowing Neon "TSINGTAO '96" Bar Sign below top HUD/coaster strip */
    if (draw_neon_sign) {
        int pulse = (int)((tick >> 2) & 15u);
        int tri = (pulse < 8) ? pulse : (15 - pulse);
        int nr = at->neon_pulse ? (118 + tri * 4) : 128;
        int ng = at->neon_pulse ? (118 + tri * 4) : 128;
        int nb = at->neon_pulse ? (126 + tri * 5) : 128;

        if (blackout_mode) {
            nr = scale_chan(nr, dim_fp);
            ng = scale_chan(ng, dim_fp);
            nb = scale_chan(nb, dim_fp + 24);
        }

        gfx_draw_poly_f4(GFX_Z_DECOR,
                         94, 37, 226, 37, 94, 65, 226, 65,
                         10, 14, 24, GFX_TRANS_HALF);
        gfx_draw_sheet_sprt(GFX_Z_DECOR - 1, GFX_TEX_ENV_UI,
                            SPR_NEON_SIGN_U, SPR_NEON_SIGN_V,
                            SPR_NEON_SIGN_W, SPR_NEON_SIGN_H,
                            96, 37,
                            clamp_u8(nr), clamp_u8(ng), clamp_u8(nb),
                            0, GFX_TRANS_NONE);
        if (!blackout_mode || dim_fp > 130) {
            gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                             92, 36, 16, 42, 32,
                             228, 36, 16, 42, 32,
                             92, 67, 0, 0, 0,
                             228, 67, 0, 0, 0,
                             GFX_TRANS_ADD);
        }
    }

    /* Swinging 3D low-poly red lanterns (shaded polygonal bodies + additive glow cones) */
    for (i = 0; i < 4; i++) {
        int ax = (int)k_lx[i];
        int ay = 31;
        int sway = (room3d_sin_deg((int)tick * 3 + i * 75) * 4) >> 8;
        int cx = ax + sway;
        int cy = 46;
        int lit = at->lanterns_lit && (!blackout_mode || dim_fp > 125);
        uint8_t c_hi_r = lit ? 255 : scale_chan(148, dim_fp);
        uint8_t c_hi_g = lit ? 116 : scale_chan(48, dim_fp);
        uint8_t c_hi_b = lit ?  54 : scale_chan(36, dim_fp);
        uint8_t c_md_r = lit ? 218 : scale_chan(112, dim_fp);
        uint8_t c_md_g = lit ?  34 : scale_chan(26, dim_fp);
        uint8_t c_md_b = lit ?  24 : scale_chan(24, dim_fp);
        uint8_t c_dk_r = lit ? 118 : scale_chan(62, dim_fp);
        uint8_t c_dk_g = lit ?  16 : scale_chan(14, dim_fp);
        uint8_t c_dk_b = lit ?  18 : scale_chan(16, dim_fp);

        /* Hanging cord */
        gfx_draw_poly_f4(GFX_Z_DECOR - 1,
                         ax, ay, ax + 1, ay,
                         cx, cy - 8, cx + 1, cy - 8,
                         42, 30, 22, GFX_TRANS_NONE);

        /* Top & bottom lacquered gold caps */
        gfx_draw_poly_f4(GFX_Z_DECOR - 2,
                         cx - 4, cy - 9, cx + 4, cy - 9,
                         cx - 6, cy - 7, cx + 6, cy - 7,
                         188, 144, 48, GFX_TRANS_NONE);
        gfx_draw_poly_f4(GFX_Z_DECOR - 2,
                         cx - 6, cy + 7, cx + 6, cy + 7,
                         cx - 4, cy + 9, cx + 4, cy + 9,
                         188, 144, 48, GFX_TRANS_NONE);

        /* 3D low-poly lantern body: 3 shaded polygonal ribs (left bright, center glowing, right shaded) */
        /* Upper hemisphere */
        gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                         cx - 5, cy - 7, c_hi_r, c_hi_g, c_hi_b,
                         cx - 1, cy - 7, c_md_r, c_md_g, c_md_b,
                         cx - 8, cy,     c_hi_r, c_hi_g, c_hi_b,
                         cx - 2, cy,     c_md_r, c_md_g, c_md_b,
                         GFX_TRANS_NONE);
        gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                         cx - 1, cy - 7, c_md_r, c_md_g, c_md_b,
                         cx + 3, cy - 7, c_md_r, c_md_g, c_md_b,
                         cx - 2, cy,     c_md_r, c_md_g, c_md_b,
                         cx + 3, cy,     c_dk_r, c_dk_g, c_dk_b,
                         GFX_TRANS_NONE);
        gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                         cx + 3, cy - 7, c_md_r, c_md_g, c_md_b,
                         cx + 5, cy - 7, c_dk_r, c_dk_g, c_dk_b,
                         cx + 3, cy,     c_dk_r, c_dk_g, c_dk_b,
                         cx + 8, cy,     c_dk_r, c_dk_g, c_dk_b,
                         GFX_TRANS_NONE);

        /* Lower hemisphere */
        gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                         cx - 8, cy,     c_hi_r, c_hi_g, c_hi_b,
                         cx - 2, cy,     c_md_r, c_md_g, c_md_b,
                         cx - 5, cy + 7, c_md_r, c_md_g, c_md_b,
                         cx - 1, cy + 7, c_dk_r, c_dk_g, c_dk_b,
                         GFX_TRANS_NONE);
        gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                         cx - 2, cy,     c_md_r, c_md_g, c_md_b,
                         cx + 3, cy,     c_dk_r, c_dk_g, c_dk_b,
                         cx - 1, cy + 7, c_dk_r, c_dk_g, c_dk_b,
                         cx + 3, cy + 7, c_dk_r, c_dk_g, c_dk_b,
                         GFX_TRANS_NONE);
        gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                         cx + 3, cy,     c_dk_r, c_dk_g, c_dk_b,
                         cx + 8, cy,     c_dk_r, c_dk_g, c_dk_b,
                         cx + 3, cy + 7, c_dk_r, c_dk_g, c_dk_b,
                         cx + 5, cy + 7, c_dk_r, c_dk_g, c_dk_b,
                         GFX_TRANS_NONE);

        /* Swinging golden-red tassel */
        {
            int tx = cx + ((sway * 3) / 2);
            gfx_draw_poly_g4(GFX_Z_DECOR - 2,
                             cx - 1, cy + 9, 235, 190, 70,
                             cx + 2, cy + 9, 210, 160, 50,
                             tx - 2, cy + 16, 195, 36, 28,
                             tx + 2, cy + 16, 145, 22, 20,
                             GFX_TRANS_NONE);
        }

        /* Additive glow halo + downward light cone */
        if (lit) {
            int pulse_g = 90 + (room3d_sin_deg((int)tick * 5 + i * 90) >> 4);
            uint8_t gr = clamp_u8(pulse_g);
            uint8_t gg = clamp_u8(pulse_g / 2);
            uint8_t gb = 16;

            gfx_draw_sheet_sprt(GFX_Z_DECOR - 3, GFX_TEX_ENV_UI,
                                SPR_LANTERN_GLOW_U, SPR_LANTERN_GLOW_V,
                                SPR_LANTERN_GLOW_W, SPR_LANTERN_GLOW_H,
                                cx - 12, cy - 12, gr, gg, gb, 0, GFX_TRANS_ADD);

            gfx_draw_poly_g4(GFX_Z_DECOR - 3,
                             cx - 6, cy + 4, (uint8_t)(gr >> 1), (uint8_t)(gg >> 1), 10,
                             cx + 6, cy + 4, (uint8_t)(gr >> 1), (uint8_t)(gg >> 1), 10,
                             cx - 26, cy + 48, 0, 0, 0,
                             cx + 26, cy + 48, 0, 0, 0,
                             GFX_TRANS_ADD);
        }
    }
}

/* Draw a single Gouraud triangle via degenerate PS1 POLY_G4 (v2 == v3) */
static void draw_tri_g4(int z,
                        int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0,
                        int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1,
                        int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2,
                        int semi_trans) {
    gfx_draw_poly_g4(z,
                     x0, y0, r0, g0, b0,
                     x1, y1, r1, g1, b1,
                     x2, y2, r2, g2, b2,
                     x2, y2, r2, g2, b2,
                     semi_trans);
}

/* Compute wood vertex RGB with optional right-half danger tinting (0=normal, 1=warn, 2=crit) */
static void eval_wood_danger_rgb(int base_c, uint8_t danger, int danger_pulse,
                                 int tinted, uint8_t *out_r, uint8_t *out_g, uint8_t *out_b) {
    if (!tinted || danger == 0) {
        *out_r = (uint8_t)base_c;
        *out_g = (uint8_t)base_c;
        *out_b = (uint8_t)((base_c * 15) >> 4);
    } else if (danger == 1) {
        *out_r = clamp_u8(base_c + 34);
        *out_g = clamp_u8((base_c * 72) / 100);
        *out_b = clamp_u8((base_c * 34) / 100);
    } else {
        int boost = 42 + (danger_pulse >> 4);
        *out_r = clamp_u8(base_c + boost);
        *out_g = clamp_u8((base_c * 34) / 100);
        *out_b = clamp_u8((base_c * 28) / 100);
    }
}

/* Shared helper to draw a textured wooden bar top + bevelled brass lip + panelled apron */
static void draw_wooden_bar_structure(int z, int x_top_l, int x_top_r, int y_top,
                                      int x_front_l, int x_front_r, int y_front,
                                      int lip_h, int apron_h, int segs,
                                      int highlight_boost, int dim_fp,
                                      uint8_t danger, int danger_pulse) {
    int s;
    int half_seg = segs / 2;
    int wu0 = SPR_TEX_WOOD_U;
    int wv0 = SPR_TEX_WOOD_V;
    int wu1 = SPR_TEX_WOOD_U + SPR_TEX_WOOD_W - 1;
    int wv1 = SPR_TEX_WOOD_V + SPR_TEX_WOOD_H - 1;

    int fu0 = SPR_TEX_WOOD_FRONT_U;
    int fv0 = SPR_TEX_WOOD_FRONT_V;
    int fu1 = SPR_TEX_WOOD_FRONT_U + SPR_TEX_WOOD_FRONT_W - 1;
    int fv1 = SPR_TEX_WOOD_FRONT_V + SPR_TEX_WOOD_FRONT_H - 1;

    int c_top_back  = ((102 + highlight_boost) * dim_fp) >> 8;
    int c_top_front = ((132 + highlight_boost) * dim_fp) >> 8;
    int c_apron_hi  = ((116 + (highlight_boost >> 1)) * dim_fp) >> 8;
    int c_apron_lo  = ((54 + (highlight_boost >> 2)) * dim_fp) >> 8;
    uint8_t tb = clamp_u8(clamp_int(c_top_back, 32, 165));
    uint8_t tf = clamp_u8(clamp_int(c_top_front, 42, 185));
    uint8_t ah = clamp_u8(clamp_int(c_apron_hi, 34, 150));
    uint8_t al = clamp_u8(clamp_int(c_apron_lo, 20, 96));

    /* 1. Perspective wooden counter top (POLY_GT4 tiled segments) */
    for (s = 0; s < segs; s++) {
        int xt0 = x_top_l + ((x_top_r - x_top_l) * s) / segs;
        int xt1 = x_top_l + ((x_top_r - x_top_l) * (s + 1)) / segs;
        int xf0 = x_front_l + ((x_front_r - x_front_l) * s) / segs;
        int xf1 = x_front_l + ((x_front_r - x_front_l) * (s + 1)) / segs;
        int tint_l = (s > half_seg);
        int tint_r = (s >= half_seg);
        uint8_t tb_l_r, tb_l_g, tb_l_b, tb_r_r, tb_r_g, tb_r_b;
        uint8_t tf_l_r, tf_l_g, tf_l_b, tf_r_r, tf_r_g, tf_r_b;

        eval_wood_danger_rgb(tb, danger, danger_pulse, tint_l, &tb_l_r, &tb_l_g, &tb_l_b);
        eval_wood_danger_rgb(tb, danger, danger_pulse, tint_r, &tb_r_r, &tb_r_g, &tb_r_b);
        eval_wood_danger_rgb(tf, danger, danger_pulse, tint_l, &tf_l_r, &tf_l_g, &tf_l_b);
        eval_wood_danger_rgb(tf, danger, danger_pulse, tint_r, &tf_r_r, &tf_r_g, &tf_r_b);

        gfx_draw_poly_gt4(z, GFX_TEX_ENV_UI,
                          wu0, wv0, wu1, wv0, wu0, wv1, wu1, wv1,
                          xt0, y_top,   tb_l_r, tb_l_g, tb_l_b,
                          xt1, y_top,   tb_r_r, tb_r_g, tb_r_b,
                          xf0, y_front, tf_l_r, tf_l_g, tf_l_b,
                          xf1, y_front, tf_r_r, tf_r_g, tf_r_b,
                          GFX_TRANS_NONE);
    }

    /* 2. Glossy lacquer specular highlight band across counter top */
    {
        int gy0 = y_top + ((y_front - y_top) * 2) / 5;
        int gy1 = y_front;
        int gx0 = x_top_l + ((x_front_l - x_top_l) * 2) / 5 + 6;
        int gx1 = x_top_r + ((x_front_r - x_top_r) * 2) / 5 - 6;
        uint8_t s_hi = clamp_u8((24 + highlight_boost) * dim_fp >> 8);
        gfx_draw_poly_g4(z,
                         gx0, gy0, 0, 0, 0,
                         gx1, gy0, 0, 0, 0,
                         x_front_l + 6, gy1, s_hi, (uint8_t)(s_hi - (s_hi >> 3)), (uint8_t)(s_hi >> 1),
                         x_front_r - 6, gy1, s_hi, (uint8_t)(s_hi - (s_hi >> 3)), (uint8_t)(s_hi >> 1),
                         GFX_TRANS_ADD);
    }

    /* 3. Bevelled brass counter lip (right half warms/reddens smoothly on danger) */
    {
        uint8_t br0 = scale_chan(224 + highlight_boost, dim_fp);
        uint8_t bg0 = scale_chan(184 + highlight_boost, dim_fp);
        uint8_t bb0 = scale_chan(82, dim_fp);
        uint8_t br1 = scale_chan(112, dim_fp);
        uint8_t bg1 = scale_chan(76, dim_fp);
        uint8_t bb1 = scale_chan(28, dim_fp);

        if (danger == 0) {
            gfx_draw_poly_g4(z,
                             x_front_l, y_front,         br0, bg0, bb0,
                             x_front_r, y_front,         br0, bg0, bb0,
                             x_front_l, y_front + lip_h, br1, bg1, bb1,
                             x_front_r, y_front + lip_h, br1, bg1, bb1,
                             GFX_TRANS_NONE);
        } else {
            int x_mid = (x_front_l + x_front_r) >> 1;
            uint8_t dr0 = (danger == 1) ? 245 : clamp_u8(215 + (danger_pulse >> 3));
            uint8_t dg0 = (danger == 1) ? 102 : clamp_u8(28 + (danger_pulse >> 4));
            uint8_t db0 = (danger == 1) ?  22 : 20;
            uint8_t dr1 = (danger == 1) ? 156 : clamp_u8(144 + (danger_pulse >> 4));
            uint8_t dg1 = (danger == 1) ?  44 : 16;
            uint8_t db1 = (danger == 1) ?  12 : 14;

            gfx_draw_poly_g4(z,
                             x_front_l, y_front,         br0, bg0, bb0,
                             x_mid,     y_front,         br0, bg0, bb0,
                             x_front_l, y_front + lip_h, br1, bg1, bb1,
                             x_mid,     y_front + lip_h, br1, bg1, bb1,
                             GFX_TRANS_NONE);
            gfx_draw_poly_g4(z,
                             x_mid,     y_front,         br0, bg0, bb0,
                             x_front_r, y_front,         dr0, dg0, db0,
                             x_mid,     y_front + lip_h, br1, bg1, bb1,
                             x_front_r, y_front + lip_h, dr1, dg1, db1,
                             GFX_TRANS_NONE);
        }
    }

    /* 4. Front vertical panelled wood apron */
    {
        int ay0 = y_front + lip_h;
        int ay1 = ay0 + apron_h;
        for (s = 0; s < segs; s++) {
            int ax0 = x_front_l + ((x_front_r - x_front_l) * s) / segs;
            int ax1 = x_front_l + ((x_front_r - x_front_l) * (s + 1)) / segs;
            gfx_draw_poly_gt4(z, GFX_TEX_ENV_UI,
                              fu0, fv0, fu1, fv0, fu0, fv1, fu1, fv1,
                              ax0, ay0, ah, ah, ah,
                              ax1, ay0, ah, ah, ah,
                              ax0, ay1, al, al, al,
                              ax1, ay1, al, al, al,
                              GFX_TRANS_NONE);
        }
    }
}

/* Crisp animated triangular selector chevron pointing left at the active tap pedestal */
static void draw_active_selector_chevron(int z, int tip_x, int cy, int w, int h, uint32_t tick) {
    int bounce = ((room3d_sin_deg((int)tick * 12) + 256) * 2) >> 9; /* 0..2 px */
    int pulse  = (room3d_sin_deg((int)tick * 10) + 256) >> 1;      /* 0..256 */
    int tx = tip_x + bounce;
    int bx = tx + w;
    int nx = tx + (w * 2) / 3;

    uint8_t wr = clamp_u8(68 + ((pulse * 184) >> 8));
    uint8_t wg = 238;
    uint8_t wb = clamp_u8(255 - ((pulse * 155) >> 8));

    /* LIFO OT: emit front-most inner cyan-gold chevron FIRST, outline & glow LAST */
    draw_tri_g4(z,
                tx, cy,     225, 255, 255,
                bx, cy - h, wr, wg, wb,
                nx, cy,     255, 236, 118,
                GFX_TRANS_NONE);
    draw_tri_g4(z,
                tx, cy,     225, 255, 255,
                nx, cy,     255, 236, 118,
                bx, cy + h, wr, wg, wb,
                GFX_TRANS_NONE);

    /* Crisp dark navy 1px outline behind chevron */
    draw_tri_g4(z,
                tx - 1, cy,         8, 16, 36,
                bx + 1, cy - h - 1, 8, 16, 36,
                nx + 1, cy,         8, 16, 36,
                GFX_TRANS_NONE);
    draw_tri_g4(z,
                tx - 1, cy,         8, 16, 36,
                nx + 1, cy,         8, 16, 36,
                bx + 1, cy + h + 1, 8, 16, 36,
                GFX_TRANS_NONE);

    /* Soft additive cyan-gold halo behind selector chevron */
    gfx_draw_poly_g4(z,
                     tx - 2, cy - h - 2, 0, 0, 0,
                     bx + 3, cy - h - 2, 0, 0, 0,
                     tx - 2, cy,         42, 132, 148,
                     bx + 3, cy,         110, 96, 26,
                     GFX_TRANS_ADD);
    gfx_draw_poly_g4(z,
                     tx - 2, cy,         42, 132, 148,
                     bx + 3, cy,         110, 96, 26,
                     tx - 2, cy + h + 2, 0, 0, 0,
                     bx + 3, cy + h + 2, 0, 0, 0,
                     GFX_TRANS_ADD);
}

/* Bright glowing cyan-gold active rail trim along the selected counter's front brass lip */
static void draw_active_rail_trim(int z, int x_l, int x_r, int y_front, int lip_h, uint32_t tick) {
    int x_mid = (x_l + x_r) >> 1;
    int p = (room3d_sin_deg((int)tick * 7) + 256) >> 1; /* 0..256 */
    uint8_t mr = clamp_u8(126 + ((p * 122) >> 8));
    uint8_t mg = 246;
    uint8_t mb = clamp_u8(248 - ((p * 130) >> 8));

    /* LIFO OT: emit soft additive bloom FIRST so it layers on top of the solid rail below */
    gfx_draw_poly_g4(z,
                     x_l,   y_front - 2, 0, 0, 0,
                     x_mid, y_front - 2, 0, 0, 0,
                     x_l,   y_front + 1, 32, 118, 136,
                     x_mid, y_front + 1, 84, 128, 96,
                     GFX_TRANS_ADD);
    gfx_draw_poly_g4(z,
                     x_mid, y_front - 2, 0, 0, 0,
                     x_r,   y_front - 2, 0, 0, 0,
                     x_mid, y_front + 1, 84, 128, 96,
                     x_r,   y_front + 1, 128, 106, 28,
                     GFX_TRANS_ADD);
    gfx_draw_poly_g4(z,
                     x_l,   y_front + 1,         32, 118, 136,
                     x_mid, y_front + 1,         84, 128, 96,
                     x_l,   y_front + lip_h + 3, 0, 0, 0,
                     x_mid, y_front + lip_h + 3, 0, 0, 0,
                     GFX_TRANS_ADD);
    gfx_draw_poly_g4(z,
                     x_mid, y_front + 1,         84, 128, 96,
                     x_r,   y_front + 1,         128, 106, 28,
                     x_mid, y_front + lip_h + 3, 0, 0, 0,
                     x_r,   y_front + lip_h + 3, 0, 0, 0,
                     GFX_TRANS_ADD);

    /* Crisp cyan -> cyan-gold -> gold active rail core along front lip */
    gfx_draw_poly_g4(z,
                     x_l,   y_front,         58, 238, 255,
                     x_mid, y_front,         mr, mg, mb,
                     x_l,   y_front + lip_h, 24, 168, 198,
                     x_mid, y_front + lip_h, (uint8_t)(mr >> 1), 192, (uint8_t)(mb >> 1),
                     GFX_TRANS_NONE);
    gfx_draw_poly_g4(z,
                     x_mid, y_front,         mr, mg, mb,
                     x_r,   y_front,         255, 224, 92,
                     x_mid, y_front + lip_h, (uint8_t)(mr >> 1), 192, (uint8_t)(mb >> 1),
                     x_r,   y_front + lip_h, 214, 158, 38,
                     GFX_TRANS_NONE);
}

/* Gentle shimmering gold/cyan positive highlight along front brass lip when a buff sits on lane */
static void draw_buff_lip_highlight(int z, int x_l, int x_r, int y_front, int lip_h,
                                    int lane, int is_active, uint32_t tick) {
    int x_mid = (x_l + x_r) >> 1;
    int phase = (int)tick * 5 + lane * 45;
    int w0 = (room3d_sin_deg(phase) + 256) >> 1;
    int w1 = (room3d_sin_deg(phase + 90) + 256) >> 1;
    int w2 = (room3d_sin_deg(phase + 180) + 256) >> 1;
    int y0 = is_active ? (y_front + lip_h) : (y_front - 1);
    int y1 = is_active ? (y_front + lip_h + 3) : (y_front + lip_h + 1);

    uint8_t r0 = clamp_u8(38 + ((132 * (256 - w0)) >> 8));
    uint8_t g0 = clamp_u8(136 + ((46 * w0) >> 8));
    uint8_t b0 = clamp_u8(24 + ((178 * w0) >> 8));

    uint8_t r1 = clamp_u8(38 + ((132 * (256 - w1)) >> 8));
    uint8_t g1 = clamp_u8(136 + ((46 * w1) >> 8));
    uint8_t b1 = clamp_u8(24 + ((178 * w1) >> 8));

    uint8_t r2 = clamp_u8(38 + ((132 * (256 - w2)) >> 8));
    uint8_t g2 = clamp_u8(136 + ((46 * w2) >> 8));
    uint8_t b2 = clamp_u8(24 + ((178 * w2) >> 8));

    gfx_draw_poly_g4(z,
                     x_l,   y0, (uint8_t)(r0 >> 1), (uint8_t)(g0 >> 1), (uint8_t)(b0 >> 1),
                     x_mid, y0, (uint8_t)(r1 >> 1), (uint8_t)(g1 >> 1), (uint8_t)(b1 >> 1),
                     x_l,   y1, r0, g0, b0,
                     x_mid, y1, r1, g1, b1,
                     GFX_TRANS_ADD);
    gfx_draw_poly_g4(z,
                     x_mid, y0, (uint8_t)(r1 >> 1), (uint8_t)(g1 >> 1), (uint8_t)(b1 >> 1),
                     x_r,   y0, (uint8_t)(r2 >> 1), (uint8_t)(g2 >> 1), (uint8_t)(b2 >> 1),
                     x_mid, y1, r1, g1, b1,
                     x_r,   y1, r2, g2, b2,
                     GFX_TRANS_ADD);
}

/* Additive right-half counter danger glow & tap pedestal alert aura */
static void draw_lane_danger_overlays(int z,
                                      int x_top_l, int x_top_r, int y_top,
                                      int x_front_l, int x_front_r, int y_front, int lip_h,
                                      int px0, int px1, int py0, int py1,
                                      uint8_t danger, int danger_pulse,
                                      int lane, uint32_t tick) {
    if (danger == 0) return;

    if (danger == 1) {
        int breathe = (room3d_sin_deg((int)tick * 5 + lane * 30) + 256) >> 1;
        int xt_mid = (x_top_l + x_top_r) >> 1;
        int xf_mid = (x_front_l + x_front_r) >> 1;
        uint8_t wr = clamp_u8(178 + ((breathe * 40) >> 8));
        uint8_t wg = clamp_u8(58 + ((breathe * 24) >> 8));
        uint8_t wb = 8;

        /* Amber-red alert glow over tap pedestal */
        gfx_draw_poly_g4(z,
                         px0 - 1, py0 - 1, 168, 56, 8,
                         px1 + 3, py0 - 1, 148, 48, 6,
                         px0 - 1, py1 + 1, 118, 34, 4,
                         px1 + 3, py1 + 1,  92, 26, 4,
                         GFX_TRANS_ADD);

        /* Warm orange-red Gouraud danger glow strip on right half of counter top & lip */
        gfx_draw_poly_g4(z,
                         xt_mid,    y_top,   0, 0, 0,
                         x_top_r,   y_top,   (uint8_t)((wr * 3) >> 2), (uint8_t)((wg * 3) >> 2), wb,
                         xf_mid,    y_front, 0, 0, 0,
                         x_front_r, y_front, wr, wg, wb,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         xf_mid,    y_front,         0, 0, 0,
                         x_front_r, y_front,         wr, wg, wb,
                         xf_mid,    y_front + lip_h, 0, 0, 0,
                         x_front_r, y_front + lip_h, (uint8_t)((wr * 3) >> 2), (uint8_t)((wg * 2) / 3), 6,
                         GFX_TRANS_ADD);
    } else {
        int xt_start = x_top_l + ((x_top_r - x_top_l) * 45) / 100;
        int xf_start = x_front_l + ((x_front_r - x_front_l) * 45) / 100;
        int xt_hot   = x_top_l + ((x_top_r - x_top_l) * 76) / 100;
        int xf_hot   = x_front_l + ((x_front_r - x_front_l) * 76) / 100;
        uint8_t cr = clamp_u8(185 + ((danger_pulse * 70) >> 8));
        uint8_t cg = clamp_u8(14 + ((danger_pulse * 20) >> 8));
        uint8_t cb = clamp_u8(14 + ((danger_pulse * 16) >> 8));
        uint8_t cr_lo = (uint8_t)((cr * 3) >> 2);
        int pad_x = 5 + (danger_pulse >> 6);
        int pad_y = 3 + (danger_pulse >> 8);
        int ax0 = px0;
        int ax1 = px1 + 3;

        /* Pulsing crimson alert aura around brass tap pedestal (center + 4 soft fade skirts) */
        gfx_draw_poly_g4(z,
                         ax0, py0, cr, cg, cb,
                         ax1, py0, cr, cg, cb,
                         ax0, py1, cr_lo, cg, cb,
                         ax1, py1, cr_lo, cg, cb,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         ax0 - pad_x, py0 - pad_y, 0, 0, 0,
                         ax1 + pad_x, py0 - pad_y, 0, 0, 0,
                         ax0,         py0,         cr, cg, cb,
                         ax1,         py0,         cr, cg, cb,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         ax0,         py1,         cr_lo, cg, cb,
                         ax1,         py1,         cr_lo, cg, cb,
                         ax0 - pad_x, py1 + pad_y, 0, 0, 0,
                         ax1 + pad_x, py1 + pad_y, 0, 0, 0,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         ax0 - pad_x, py0 - pad_y, 0, 0, 0,
                         ax0,         py0,         cr, cg, cb,
                         ax0 - pad_x, py1 + pad_y, 0, 0, 0,
                         ax0,         py1,         cr_lo, cg, cb,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         ax1,         py0,         cr, cg, cb,
                         ax1 + pad_x, py0 - pad_y, 0, 0, 0,
                         ax1,         py1,         cr_lo, cg, cb,
                         ax1 + pad_x, py1 + pad_y, 0, 0, 0,
                         GFX_TRANS_ADD);

        /* Strong pulsing crimson-red Gouraud danger strip across right counter top & lip */
        gfx_draw_poly_g4(z,
                         xt_start,  y_top,   0, 0, 0,
                         x_top_r,   y_top,   (uint8_t)((cr * 4) / 5), cg, cb,
                         xf_start,  y_front, 0, 0, 0,
                         x_front_r, y_front, cr, cg, cb,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         xt_hot,        y_top,   0, 0, 0,
                         x_top_r,       y_top,   (uint8_t)(cr >> 1), 12, 10,
                         xf_hot,        y_front, 0, 0, 0,
                         x_front_r + 2, y_front, (uint8_t)((cr * 3) >> 2), 16, 12,
                         GFX_TRANS_ADD);
        gfx_draw_poly_g4(z,
                         xf_start,      y_front,             0, 0, 0,
                         x_front_r + 2, y_front,             cr, (uint8_t)(cg + 4), cb,
                         xf_start,      y_front + lip_h + 1, 0, 0, 0,
                         x_front_r + 2, y_front + lip_h + 1, (uint8_t)((cr * 4) / 5), cg, cb,
                         GFX_TRANS_ADD);
    }
}

static void draw_lane_counters(uint32_t tick, int active_lane, int dim_fp,
                               const uint8_t lane_danger[4], const uint8_t lane_buff[4]) {
    int lane;
    int cu0 = SPR_TEX_CURTAIN_U;
    int cv0 = SPR_TEX_CURTAIN_V;
    int cu1 = SPR_TEX_CURTAIN_U + SPR_TEX_CURTAIN_W - 1;
    int cv1 = SPR_TEX_CURTAIN_V + SPR_TEX_CURTAIN_H - 1;

    for (lane = 0; lane < 4; lane++) {
        int lx, rx, ly, scale_fp;
        int z_base = room3d_lane_z_base(lane);
        int top_h, apron_h, is_active, boost;
        int door_w, door_h, sway;
        int x_top_l, x_top_r, y_top, x_front_l, x_front_r, y_front;
        int px0, px1, py0, py1, tap_w, tap_h;
        uint8_t danger = lane_danger ? lane_danger[lane] : 0;
        uint8_t buff = lane_buff ? lane_buff[lane] : 0;
        int danger_pulse = (danger >= 2) ? ((room3d_sin_deg((int)tick * 16 + lane * 24) + 256) >> 1) : 0;

        room3d_lane_project(lane, 0, &lx, &ly, &scale_fp);
        room3d_lane_project(lane, 256, &rx, &ly, &scale_fp);

        top_h = (8 * scale_fp) >> 8;
        apron_h = (10 * scale_fp) >> 8;
        is_active = (lane == active_lane);
        boost = is_active ? 22 : 0;

        x_top_l   = lx + 4;
        x_top_r   = rx - 2;
        y_top     = ly - top_h;
        x_front_l = lx - 2;
        x_front_r = rx + 4;
        y_front   = ly + 1;

        px0 = rx + 2;
        px1 = rx + 16;
        py0 = y_top;
        py1 = ly + 4 + apron_h;
        tap_w = (SPR_BEER_TAP_W * scale_fp) >> 8;
        tap_h = (SPR_BEER_TAP_H * scale_fp) >> 8;

        /* Left wooden doorway arch & dark threshold (behind lane entities at z_base + 4) */
        door_w = (16 * scale_fp) >> 8;
        door_h = (32 * scale_fp) >> 8;
        gfx_draw_poly_f4(z_base + 4,
                         lx - door_w, ly - door_h,
                         lx + 2,      ly - door_h,
                         lx - door_w, ly + 10,
                         lx + 2,      ly + 10,
                         12, 10, 18, GFX_TRANS_NONE);

        /* Left & right wooden doorposts + lintel */
        gfx_draw_poly_g4(z_base + 4,
                         lx - door_w - 3, ly - door_h - 3, scale_chan(124, dim_fp), scale_chan(74, dim_fp), scale_chan(38, dim_fp),
                         lx - door_w,     ly - door_h - 3, scale_chan(88, dim_fp),  scale_chan(48, dim_fp), scale_chan(24, dim_fp),
                         lx - door_w - 3, ly + 10,         scale_chan(78, dim_fp),  scale_chan(44, dim_fp), scale_chan(22, dim_fp),
                         lx - door_w,     ly + 10,         scale_chan(56, dim_fp),  scale_chan(32, dim_fp), scale_chan(16, dim_fp),
                         GFX_TRANS_NONE);
        gfx_draw_poly_g4(z_base + 4,
                         lx - door_w - 3, ly - door_h - 4, scale_chan(148, dim_fp), scale_chan(92, dim_fp), scale_chan(46, dim_fp),
                         lx + 4,          ly - door_h - 4, scale_chan(132, dim_fp), scale_chan(80, dim_fp), scale_chan(40, dim_fp),
                         lx - door_w - 3, ly - door_h,     scale_chan(84, dim_fp),  scale_chan(48, dim_fp), scale_chan(24, dim_fp),
                         lx + 4,          ly - door_h,     scale_chan(74, dim_fp),  scale_chan(42, dim_fp), scale_chan(20, dim_fp),
                         GFX_TRANS_NONE);

        /* Swaying red Qingdao noren curtain hanging in the doorway arch */
        sway = (room3d_sin_deg((int)tick * 3 + lane * 48) * 2) >> 8;
        {
            int p;
            int cur_h = (12 * scale_fp) >> 8;
            int mid_x = lx - (door_w / 2);
            int span_x[2][2];
            uint8_t cr = clamp_u8(scale_chan(140, dim_fp));
            uint8_t cr_bot = (uint8_t)(cr - 14);
            span_x[0][0] = lx - door_w + 1; span_x[0][1] = mid_x;
            span_x[1][0] = mid_x + 1;       span_x[1][1] = lx + 2;
            for (p = 0; p < 2; p++) {
                gfx_draw_poly_gt4(z_base + 3, GFX_TEX_ENV_UI,
                                  cu0, cv0, cu1, cv0, cu0, cv1, cu1, cv1,
                                  span_x[p][0],        ly - door_h,         cr, cr, cr,
                                  span_x[p][1],        ly - door_h,         cr, cr, cr,
                                  span_x[p][0] + sway, ly - door_h + cur_h, cr_bot, cr_bot, cr_bot,
                                  span_x[p][1] + sway, ly - door_h + cur_h, cr_bot, cr_bot, cr_bot,
                                  GFX_TRANS_NONE);
            }
        }

        /* 3D Wooden Counter Top + Brass Lip + Wooden Apron (at z_base + 1) */
        draw_wooden_bar_structure(z_base + 1,
                                  x_top_l, x_top_r, y_top,
                                  x_front_l, x_front_r, y_front,
                                  3, apron_h, 4, boost, dim_fp,
                                  danger, danger_pulse);

        /* Right 3D brass tap pedestal (at z_base + 1, alert-tinted on lane_danger >= 1) */
        {
            uint8_t ph_r, ph_g, ph_b, pl_r, pl_g, pl_b;
            uint8_t ps_r, ps_g, ps_b, ps2_r, ps2_g, ps2_b;
            if (danger >= 2) {
                ph_r = clamp_u8(204 + ((danger_pulse * 50) >> 8));
                ph_g = clamp_u8(46 + (danger_pulse >> 4));
                ph_b = clamp_u8(38 + (danger_pulse >> 4));
                pl_r = clamp_u8(122 + (danger_pulse >> 3));
                pl_g = 22;
                pl_b = 20;
                ps_r = (uint8_t)(ph_r >> 1); ps_g = 22; ps_b = 18;
                ps2_r = (uint8_t)(pl_r >> 1); ps2_g = 14; ps2_b = 12;
            } else if (danger == 1) {
                ph_r = 226; ph_g = 104; ph_b = 28;
                pl_r = 132; pl_g =  48; pl_b = 14;
                ps_r = 114; ps_g =  46; ps_b = 12;
                ps2_r = 66; ps2_g =  26; ps2_b = 10;
            } else {
                uint8_t p_hi = scale_chan(148 + boost, dim_fp);
                uint8_t p_lo = scale_chan(64, dim_fp);
                ph_r = p_hi; ph_g = (uint8_t)((p_hi * 4) / 5); ph_b = (uint8_t)(p_hi / 3);
                pl_r = p_lo; pl_g = (uint8_t)((p_lo * 3) / 4); pl_b = (uint8_t)(p_lo / 3);
                ps_r = (uint8_t)(p_hi >> 1); ps_g = (uint8_t)(p_hi / 3); ps_b = 12;
                ps2_r = (uint8_t)(p_lo >> 1); ps2_g = (uint8_t)(p_lo / 3); ps2_b = 8;
            }

            gfx_draw_poly_g4(z_base + 1,
                             px0, py0, ph_r, ph_g, ph_b,
                             px1, py0, ph_r, ph_g, ph_b,
                             px0, py1, pl_r, pl_g, pl_b,
                             px1, py1, pl_r, pl_g, pl_b,
                             GFX_TRANS_NONE);
            gfx_draw_poly_g4(z_base + 1,
                             px1,     py0,     ps_r,  ps_g,  ps_b,
                             px1 + 4, py0 + 2, ps2_r, ps2_g, ps2_b,
                             px1,     py1,     ps2_r, ps2_g, ps2_b,
                             px1 + 4, py1 - 2, 16, 12, 8,
                             GFX_TRANS_NONE);
        }

        /*
         * LIFO OT order inside `z_base` (everything here draws over counter/pedestal at z_base+1,
         * and behind shadows/entities at z_base-1..-5):
         *   Emit front-most FIRST, back-most LAST:
         *   1. Active selector chevron beside tap pedestal
         *   2. Brass beer tap sprite
         *   3. Active glowing cyan-gold front rail trim
         *   4. Buff shimmering gold/cyan front lip highlight
         *   5. Danger right-counter glow strip & pedestal alert aura
         *   6. Active lane overhead spotlight beam
         */

        /* 1. Crisp animated triangular selector chevron right beside active tap pedestal */
        if (is_active) {
            int chev_h = (7 * scale_fp) >> 8;
            if (chev_h < 5) chev_h = 5;
            draw_active_selector_chevron(z_base, px1 + 5, ly - 2, 9, chev_h, tick);
        }

        /* 2. Brass beer tap mounted atop pedestal */
        {
            uint8_t tr = clamp_u8(scale_chan(GFX_COLOR_NEUTRAL + (is_active ? 22 : 0), dim_fp));
            uint8_t tg = tr;
            uint8_t tb = scale_chan(GFX_COLOR_NEUTRAL, dim_fp);
            if (danger >= 2) {
                tr = clamp_u8(GFX_COLOR_NEUTRAL + 22);
                tg = 104;
                tb = 96;
            } else if (danger == 1) {
                tr = clamp_u8(GFX_COLOR_NEUTRAL + 16);
                tg = 114;
                tb = 96;
            }
            gfx_draw_sheet_sprt_scaled(z_base, GFX_TEX_ENV_UI,
                                       SPR_BEER_TAP_U, SPR_BEER_TAP_V,
                                       SPR_BEER_TAP_W, SPR_BEER_TAP_H,
                                       rx + 1, py0 - tap_h + 7,
                                       tap_w, tap_h,
                                       tr, tg, tb,
                                       0, GFX_TRANS_NONE);
        }

        /* 3. Bright glowing cyan-gold active rail trim along selected counter's front brass lip */
        if (is_active) {
            draw_active_rail_trim(z_base, x_front_l, x_front_r, y_front, 3, tick);
        }

        /* 4. Gentle shimmering gold/cyan highlight along front brass lip when a buff sits on lane */
        if (buff > 0) {
            draw_buff_lip_highlight(z_base, x_front_l, x_front_r, y_front, 3, lane, is_active, tick);
        }

        /* 5. Danger right-counter glow strip & tap pedestal alert aura */
        if (danger > 0) {
            draw_lane_danger_overlays(z_base,
                                      x_top_l, x_top_r, y_top,
                                      x_front_l, x_front_r, y_front, 3,
                                      px0, px1, py0, py1,
                                      danger, danger_pulse, lane, tick);
        }

        /* 6. Active lane spotlight beam (back-most layer inside z_base) */
        if (is_active) {
            int pulse = 24 + (room3d_sin_deg((int)tick * 6) >> 5);
            uint8_t sp_hi = clamp_u8(pulse + 18);
            uint8_t sp_lo = clamp_u8(pulse >> 1);
            gfx_draw_poly_g4(z_base,
                             lx + 26, y_top - 14, sp_lo, sp_lo, 4,
                             rx + 12, y_top - 14, sp_hi, (uint8_t)(sp_hi - 4), 12,
                             lx + 2,  ly + 2,     0, 0, 0,
                             rx + 6,  ly + 2,     sp_lo, (uint8_t)(sp_lo >> 1), 4,
                             GFX_TRANS_ADD);
        }
    }
}

void room3d_draw_barroom_ex(uint16_t night, uint32_t tick,
                            int active_lane, int draw_counters,
                            int draw_neon_sign, int blackout_mode,
                            const uint8_t lane_danger[4],
                            const uint8_t lane_buff[4]) {
    const RoomAtmos *at = &k_room_atmos[atmos_index_for_night(night)];
    int dim_fp = compute_blackout_dim(tick, blackout_mode);
    int y;

    draw_sky_and_harbor(at, tick, dim_fp, blackout_mode);
    draw_perspective_floor(at, tick, active_lane, dim_fp, blackout_mode);
    draw_awning_and_lanterns(at, tick, draw_neon_sign, dim_fp, blackout_mode);

    /* Slim outer wooden framing pillars behind lanes */
    for (y = 92; y < 248; y += 32) {
        uint8_t pc = clamp_u8(scale_chan(112, dim_fp));
        gfx_draw_sheet_sprt(87, GFX_TEX_ENV_UI,
                            SPR_WOOD_POST_U, SPR_WOOD_POST_V,
                            SPR_WOOD_POST_W, SPR_WOOD_POST_H,
                            0, y, pc, pc, pc, 0, GFX_TRANS_NONE);
        gfx_draw_sheet_sprt(87, GFX_TEX_ENV_UI,
                            SPR_WOOD_POST_U, SPR_WOOD_POST_V,
                            SPR_WOOD_POST_W, SPR_WOOD_POST_H,
                            308, y, pc, pc, pc, 0, GFX_TRANS_NONE);
    }

    if (draw_counters) {
        draw_lane_counters(tick, active_lane, dim_fp, lane_danger, lane_buff);
    }
}

void room3d_draw_barroom(uint16_t night, uint32_t tick,
                         int active_lane, int draw_counters,
                         int draw_neon_sign, int blackout_mode) {
    room3d_draw_barroom_ex(night, tick, active_lane, draw_counters,
                           draw_neon_sign, blackout_mode, NULL, NULL);
}

void room3d_draw_hero_bar(uint16_t night, uint32_t tick) {
    int p;
    room3d_draw_barroom(night, tick, -1, 0, 0, 0);

    /* Grand 2.5D foreground mahogany counter (z = GFX_Z_COUNTER = 56, leaves y=232..256 clear) */
    draw_wooden_bar_structure(GFX_Z_COUNTER,
                              24, 296, 174,
                              12, 308, 186,
                              4, 34, 6, 16, 256,
                              0, 0);

    /* Carved timber pilasters & brass footrail across hero bar apron */
    for (p = 0; p <= 6; p++) {
        int px = 12 + (296 * p) / 6 - 3;
        gfx_draw_poly_g4(GFX_Z_COUNTER - 1,
                         px,     190, 96, 54, 26,
                         px + 6, 190, 74, 40, 18,
                         px,     224, 52, 28, 14,
                         px + 6, 224, 38, 20, 10,
                         GFX_TRANS_NONE);
    }
    gfx_draw_poly_g4(GFX_Z_COUNTER - 1,
                     14, 220, 228, 192, 92,
                     306, 220, 228, 192, 92,
                     14, 223, 128, 92, 34,
                     306, 223, 128, 92, 34,
                     GFX_TRANS_NONE);
}

/* =========================================================================
 * 3D Integer-Rotated Balatro Card Renderer
 * ========================================================================= */

static void project_card_pt(int lx, int ly, int cx, int cy,
                            int cyaw, int syaw, int cpitch, int spitch,
                            int *out_x, int *out_y) {
    int x1 = (lx * cyaw) >> 12;
    int z1 = (-lx * syaw) >> 12;
    int y2 = (ly * cpitch - z1 * spitch) >> 12;
    int z2 = (ly * spitch + z1 * cpitch) >> 12;
    int denom = 240 + z2;
    if (denom < 64) denom = 64;
    *out_x = cx + (x1 * 240) / denom;
    *out_y = cy + (y2 * 240) / denom;
}

static void swap_horiz_corners(int x[4], int y[4]) {
    int tx = x[0], ty = y[0];
    x[0] = x[1]; x[1] = tx;
    y[0] = y[1]; y[1] = ty;
    tx = x[2]; ty = y[2];
    x[2] = x[3]; x[3] = tx;
    y[2] = y[3]; y[3] = ty;
}

static void rainbow_corner(int phase, uint8_t *or_, uint8_t *og_, uint8_t *ob_) {
    int p = phase & 255;
    int r = (k_sin256[p] + 4096) >> 8;
    int g = (k_sin256[(p + 85) & 255] + 4096) >> 8;
    int b = (k_sin256[(p + 170) & 255] + 4096) >> 8;
    *or_ = (uint8_t)(8 + (r * 28) / 32);
    *og_ = (uint8_t)(8 + (g * 28) / 32);
    *ob_ = (uint8_t)(10 + (b * 30) / 32);
}

void room3d_draw_card(int z, int cx, int cy, int w, int h,
                      int yaw_deg, int pitch_deg,
                      int rarity, RoomCardKind kind, int icon_id,
                      int selected, int holo, uint32_t tick) {
    int cyaw = cos_deg_4096(yaw_deg);
    int syaw = sin_deg_4096(yaw_deg);
    int cpitch = cos_deg_4096(pitch_deg);
    int spitch = sin_deg_4096(pitch_deg);
    int hw = w / 2;
    int hh = h / 2;
    int front_facing = (cyaw >= 0);

    int px[4], py[4];
    int sx[4], sy[4];
    uint8_t cr[4], cg[4], cb[4];
    int fu, fv, i;

    if (w <= 0 || h <= 0) return;

    /* Near 90-degree edge-on slice */
    if (cyaw > -120 && cyaw < 120) {
        int ey0, ey1, dummy;
        project_card_pt(0, -hh, cx, cy, cyaw, syaw, cpitch, spitch, &dummy, &ey0);
        project_card_pt(0,  hh, cx, cy, cyaw, syaw, cpitch, spitch, &dummy, &ey1);
        gfx_draw_poly_f4(z,
                         cx - 1, ey0, cx + 1, ey0,
                         cx - 1, ey1, cx + 1, ey1,
                         220, 206, 172, GFX_TRANS_NONE);
        return;
    }

    /* Project card frame corners in local Z-order (TL, TR, BL, BR) */
    project_card_pt(-hw, -hh, cx, cy, cyaw, syaw, cpitch, spitch, &px[0], &py[0]);
    project_card_pt( hw, -hh, cx, cy, cyaw, syaw, cpitch, spitch, &px[1], &py[1]);
    project_card_pt(-hw,  hh, cx, cy, cyaw, syaw, cpitch, spitch, &px[2], &py[2]);
    project_card_pt( hw,  hh, cx, cy, cyaw, syaw, cpitch, spitch, &px[3], &py[3]);

    /* Swap horizontal corners on back-face so screen Z-order remains TL,TR,BL,BR */
    if (!front_facing) {
        swap_horiz_corners(px, py);
    }

    /* Directional Gouraud lighting across card tilt */
    {
        int tilt_x = syaw >> 7;   /* -32 .. 32 */
        int tilt_y = spitch >> 7; /* -32 .. 32 */
        int l0 = clamp_int(128 - (tilt_x >> 1) - (tilt_y >> 1), 98, 152);
        int l1 = clamp_int(128 + (tilt_x >> 1) - (tilt_y >> 1), 98, 152);
        int l2 = clamp_int(128 - (tilt_x >> 1) + (tilt_y >> 1), 98, 152);
        int l3 = clamp_int(128 + (tilt_x >> 1) + (tilt_y >> 1), 98, 152);
        cr[0] = cg[0] = cb[0] = (uint8_t)l0;
        cr[1] = cg[1] = cb[1] = (uint8_t)l1;
        cr[2] = cg[2] = cb[2] = (uint8_t)l2;
        cr[3] = cg[3] = cb[3] = (uint8_t)l3;
    }

    /*
     * LIFO OT insertion in same slot `z`:
     *   Emit front-most layers FIRST, back-most layers LAST,
     *   so GPU draws: Shadow -> Selection Rim -> Card Frame -> Icon -> Holo Sheen!
     */

    /* 5. Subtle diagonal rainbow holographic sheen (front face only) */
    if (front_facing && holo) {
        int ihw = hw - 3;
        int ihh = hh - 3;
        int hx[4], hy[4];
        uint8_t hr[4], hg[4], hb[4];
        int base_p = (int)((tick * 3u) & 255u);
        if (ihw < 2) ihw = hw;
        if (ihh < 2) ihh = hh;

        project_card_pt(-ihw, -ihh, cx, cy, cyaw, syaw, cpitch, spitch, &hx[0], &hy[0]);
        project_card_pt( ihw, -ihh, cx, cy, cyaw, syaw, cpitch, spitch, &hx[1], &hy[1]);
        project_card_pt(-ihw,  ihh, cx, cy, cyaw, syaw, cpitch, spitch, &hx[2], &hy[2]);
        project_card_pt( ihw,  ihh, cx, cy, cyaw, syaw, cpitch, spitch, &hx[3], &hy[3]);

        rainbow_corner(base_p,       &hr[0], &hg[0], &hb[0]);
        rainbow_corner(base_p + 56,  &hr[1], &hg[1], &hb[1]);
        rainbow_corner(base_p + 84,  &hr[2], &hg[2], &hb[2]);
        rainbow_corner(base_p + 140, &hr[3], &hg[3], &hb[3]);

        gfx_draw_poly_g4(z,
                         hx[0], hy[0], hr[0], hg[0], hb[0],
                         hx[1], hy[1], hr[1], hg[1], hb[1],
                         hx[2], hy[2], hr[2], hg[2], hb[2],
                         hx[3], hy[3], hr[3], hg[3], hb[3],
                         GFX_TRANS_ADD);
    }

    /* 4. Centered scaled icon (front face only) */
    if (front_facing) {
        GfxTexSheet icon_sheet = GFX_TEX_CARDS;
        int iu = 0, iv = 0, iw_tex = 24, ih_tex = 24;
        int icon_hw = w / 4;
        int icon_hh = (h * 3) / 16;
        int icon_cy_off = -((h + 15) / 16);
        int ix[4], iy[4];

        if (kind == CARD_KIND_COASTER) {
            int id = clamp_int(icon_id, 0, 19);
            iu = SPR_COASTER_ICON_U(id);
            iv = SPR_COASTER_ICON_V(id);
            iw_tex = SPR_COASTER_ICON_W;
            ih_tex = SPR_COASTER_ICON_H;
        } else if (kind == CARD_KIND_DRINK) {
            int id = clamp_int(icon_id, 0, 2);
            iu = SPR_DRINK_CARD_U(id);
            iv = SPR_DRINK_CARD_V(id);
            iw_tex = SPR_DRINK_CARD_W;
            ih_tex = SPR_DRINK_CARD_H;
        } else if (kind == CARD_KIND_TRICK) {
            int id = clamp_int(icon_id, 0, 5);
            iu = SPR_TRICK_ICON_U(id);
            iv = SPR_TRICK_ICON_V(id);
            iw_tex = SPR_TRICK_ICON_W;
            ih_tex = SPR_TRICK_ICON_H;
            icon_hw = (w * 5) / 24;
            icon_hh = (h * 5) / 32;
        } else {
            /* CARD_KIND_REWARD: 0=Boss Seal, 1=Life Mini Can, 2=Cash Tip Stack */
            if (icon_id == 1) {
                icon_sheet = GFX_TEX_REAL;
                iu = SPR_REAL_MINI_CAN_U;
                iv = SPR_REAL_MINI_CAN_V;
                iw_tex = SPR_REAL_MINI_CAN_W;
                ih_tex = SPR_REAL_MINI_CAN_H;
                icon_hw = w / 6;
                icon_hh = (h * 3) / 16;
            } else if (icon_id == 2) {
                icon_sheet = GFX_TEX_SPRITES;
                iu = SPR_CASH_TIP_U;
                iv = SPR_CASH_TIP_V;
                iw_tex = SPR_CASH_TIP_W;
                ih_tex = SPR_CASH_TIP_H;
                icon_hw = (w * 5) / 20;
                icon_hh = (h * 3) / 20;
            } else {
                iu = SPR_BOSS_SEAL_U;
                iv = SPR_BOSS_SEAL_V;
                iw_tex = SPR_BOSS_SEAL_W;
                ih_tex = SPR_BOSS_SEAL_H;
            }
        }

        if (icon_hw < 4) icon_hw = 4;
        if (icon_hh < 4) icon_hh = 4;

        project_card_pt(-icon_hw, -icon_hh + icon_cy_off, cx, cy, cyaw, syaw, cpitch, spitch, &ix[0], &iy[0]);
        project_card_pt( icon_hw, -icon_hh + icon_cy_off, cx, cy, cyaw, syaw, cpitch, spitch, &ix[1], &iy[1]);
        project_card_pt(-icon_hw,  icon_hh + icon_cy_off, cx, cy, cyaw, syaw, cpitch, spitch, &ix[2], &iy[2]);
        project_card_pt( icon_hw,  icon_hh + icon_cy_off, cx, cy, cyaw, syaw, cpitch, spitch, &ix[3], &iy[3]);

        gfx_draw_poly_gt4(z, icon_sheet,
                          iu, iv,
                          iu + iw_tex - 1, iv,
                          iu, iv + ih_tex - 1,
                          iu + iw_tex - 1, iv + ih_tex - 1,
                          ix[0], iy[0], cr[0], cg[0], cb[0],
                          ix[1], iy[1], cr[1], cg[1], cb[1],
                          ix[2], iy[2], cr[2], cg[2], cb[2],
                          ix[3], iy[3], cr[3], cg[3], cb[3],
                          GFX_TRANS_NONE);
    }

    /* 3. Card Frame (back or rarity/trick front) */
    if (!front_facing) {
        fu = SPR_CARD_BACK_U;
        fv = SPR_CARD_BACK_V;
    } else if (kind == CARD_KIND_TRICK) {
        fu = SPR_CARD_TRICK_U;
        fv = SPR_CARD_TRICK_V;
    } else if (rarity >= 2) {
        fu = SPR_CARD_RARE_U;
        fv = SPR_CARD_RARE_V;
    } else if (rarity == 1) {
        fu = SPR_CARD_UNCOMMON_U;
        fv = SPR_CARD_UNCOMMON_V;
    } else {
        fu = SPR_CARD_COMMON_U;
        fv = SPR_CARD_COMMON_V;
    }

    gfx_draw_poly_gt4(z, GFX_TEX_CARDS,
                      fu, fv,
                      fu + SPR_CARD_FRAME_W - 1, fv,
                      fu, fv + SPR_CARD_FRAME_H - 1,
                      fu + SPR_CARD_FRAME_W - 1, fv + SPR_CARD_FRAME_H - 1,
                      px[0], py[0], cr[0], cg[0], cb[0],
                      px[1], py[1], cr[1], cg[1], cb[1],
                      px[2], py[2], cr[2], cg[2], cb[2],
                      px[3], py[3], cr[3], cg[3], cb[3],
                      GFX_TRANS_NONE);

    /* 2. Selection gold rim */
    if (selected) {
        int rh = hw + 3;
        int rv = hh + 3;
        int rx[4], ry[4];
        int pulse = (room3d_sin_deg((int)tick * 8) * 20) >> 8;
        uint8_t g_hi = clamp_u8(232 + pulse);
        uint8_t b_hi = clamp_u8(88 + (pulse >> 1));

        project_card_pt(-rh, -rv, cx, cy, cyaw, syaw, cpitch, spitch, &rx[0], &ry[0]);
        project_card_pt( rh, -rv, cx, cy, cyaw, syaw, cpitch, spitch, &rx[1], &ry[1]);
        project_card_pt(-rh,  rv, cx, cy, cyaw, syaw, cpitch, spitch, &rx[2], &ry[2]);
        project_card_pt( rh,  rv, cx, cy, cyaw, syaw, cpitch, spitch, &rx[3], &ry[3]);

        if (!front_facing) {
            swap_horiz_corners(rx, ry);
        }

        gfx_draw_poly_g4(z,
                         rx[0], ry[0], 255, g_hi, b_hi,
                         rx[1], ry[1], 255, g_hi, b_hi,
                         rx[2], ry[2], 235, (uint8_t)(g_hi - 42), 36,
                         rx[3], ry[3], 235, (uint8_t)(g_hi - 42), 36,
                         GFX_TRANS_NONE);
    }

    /* 1. Soft floating card drop shadow */
    for (i = 0; i < 4; i++) {
        sx[i] = px[i] + 4;
        sy[i] = py[i] + 5;
    }
    gfx_draw_poly_f4(z,
                     sx[0], sy[0], sx[1], sy[1],
                     sx[2], sy[2], sx[3], sy[3],
                     10, 10, 16, GFX_TRANS_HALF);
}
