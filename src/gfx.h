#ifndef TSINGTAO_GFX_H
#define TSINGTAO_GFX_H

#include <stdint.h>

/* PAL 50 Hz display dimensions */
#define GFX_SCREEN_W      320
#define GFX_SCREEN_H      256
#define GFX_OT_LEN        128

/* PS1 GPU color modulation neutral value (128 = 100% texture brightness) */
#define GFX_COLOR_NEUTRAL 128

/*
 * Ordering Table (OT) Z-order convention (standard PS1 DrawOTag reverse order):
 *   Higher Z = drawn FIRST (background)
 *   Lower  Z = drawn LAST  (foreground / HUD)
 * Valid range: 0 .. (GFX_OT_LEN - 1)
 */
#define GFX_Z_OVERLAY     2
#define GFX_Z_HUD_TEXT    6
#define GFX_Z_HUD_BG      10
#define GFX_Z_POPUP       16
#define GFX_Z_VFX         22
#define GFX_Z_BARTENDER   30
#define GFX_Z_DRINK       36
#define GFX_Z_CUSTOMER    44
#define GFX_Z_COUNTER     56
#define GFX_Z_DECOR       92
#define GFX_Z_SKYLINE     108
#define GFX_Z_SKY         124

/* Semi-transparency modes */
#define GFX_TRANS_NONE    0 /* Opaque */
#define GFX_TRANS_HALF    1 /* 50% back + 50% front (ABR 0) */
#define GFX_TRANS_ADD     2 /* 100% back + 100% front additive (ABR 1) */
#define GFX_TRANS_SUB     3 /* 100% back - 100% front subtractive (ABR 2) */
#define GFX_TRANS_ADD25   4 /* 100% back + 25% front additive (ABR 3) */

/* Embedded TIM texture sheets loaded into VRAM by gfx_init() */
typedef enum {
    GFX_TEX_REAL    = 0, /* real_tsingtao.tim: 8-bit digitized logo, cans, bottle */
    GFX_TEX_SPRITES = 1, /* sprites.tim: 8-bit mascot, customers, bags, VFX */
    GFX_TEX_ENV_UI  = 2, /* env_ui.tim: 4-bit bar, taps, lanterns, font, UI */
    GFX_TEX_CARDS   = 3  /* cards.tim: 8-bit card frames, coaster/drink/trick icons */
} GfxTexSheet;

/* Pure-C hardware TPage / CLUT packing helpers (matches PS1 GPU bitfields) */
#define GFX_MAKE_TPAGE(tp, abr, x, y) \
    ((uint16_t)((((tp) & 3) << 7) | (((abr) & 3) << 5) | (((y) & 0x100) >> 4) | (((x) & 0x3FF) >> 6)))

#define GFX_MAKE_CLUT(x, y) \
    ((uint16_t)((((y) & 0x1FF) << 6) | (((x) >> 4) & 0x3F)))

/* Initialize PAL 320x256 double-buffered GPU and upload embedded .TIM textures to VRAM */
void gfx_init(void);

/* Begin a new frame: clear OT, reset primitive packet pointer, update shake */
void gfx_begin_frame(uint8_t clear_r, uint8_t clear_g, uint8_t clear_b);

/* Finish frame: DrawSync(0), VSync(0), swap buffers, DrawOTag */
void gfx_end_frame(void);

/* Monotonic count of rendered frames (50 Hz) */
uint32_t gfx_frame_count(void);

/* Trigger screen shake for `frames` VSyncs with max pixel offset `magnitude` */
void gfx_set_shake(int frames, int magnitude);

/* Toggle whether subsequent draw calls apply the current frame's shake offset (default: 1) */
void gfx_set_shake_apply(int apply);

/* Query current frame's shake offset in pixels */
void gfx_get_shake(int *out_dx, int *out_dy);

/* Query VRAM TPage (with ABR mode 0..3) and CLUT packed values for loaded TIM sheets */
uint16_t gfx_get_tpage(GfxTexSheet sheet, int abr);
uint16_t gfx_get_clut(GfxTexSheet sheet);

/* Full-width (0..320) vertical color gradient strip (POLY_G4) for dynamic Qingdao skies */
void gfx_draw_sky_grad(int z, int y0, int y1,
                       uint8_t r0, uint8_t g0, uint8_t b0,
                       uint8_t r1, uint8_t g1, uint8_t b1);

/* Arbitrary vertical color gradient rectangle (POLY_G4) */
void gfx_draw_grad_rect(int z, int x, int y, int w, int h,
                        uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1,
                        int semi_trans);

/* Solid or semi-transparent rectangle */
void gfx_draw_rect(int z, int x, int y, int w, int h,
                   uint8_t r, uint8_t g, uint8_t b, int semi_trans);

/*
 * Draw a 1:1 textured sprite at (x, y).
 * - Uses SPRT + DR_TPAGE when flip_x == 0.
 * - Uses POLY_FT4 with swapped U coordinates when flip_x != 0 (PS1 SPRT cannot flip).
 * - r, g, b = 128, 128, 128 draws un-tinted original texture colors.
 */
void gfx_draw_sprt(int z, uint16_t tpage, uint16_t clut,
                   int u, int v, int w, int h,
                   int x, int y,
                   uint8_t r, uint8_t g, uint8_t b,
                   int flip_x, int semi_trans);

/* Draw a scaled textured sprite (always emits POLY_FT4) */
void gfx_draw_sprt_scaled(int z, uint16_t tpage, uint16_t clut,
                          int u, int v, int w, int h,
                          int x, int y, int dst_w, int dst_h,
                          uint8_t r, uint8_t g, uint8_t b,
                          int flip_x, int semi_trans);

/* Convenience wrapper using loaded TIM sheet index instead of raw tpage/clut */
void gfx_draw_sheet_sprt(int z, GfxTexSheet sheet,
                         int u, int v, int w, int h,
                         int x, int y,
                         uint8_t r, uint8_t g, uint8_t b,
                         int flip_x, int semi_trans);

/* Convenience wrapper for scaled textured sprite using loaded TIM sheet index */
void gfx_draw_sheet_sprt_scaled(int z, GfxTexSheet sheet,
                                int u, int v, int w, int h,
                                int x, int y, int dst_w, int dst_h,
                                uint8_t r, uint8_t g, uint8_t b,
                                int flip_x, int semi_trans);

/*
 * 4-vertex hardware polygons (PS1 Z-order: 0=Top-Left, 1=Top-Right, 2=Bottom-Left, 3=Bottom-Right).
 * Note: POLY_FT4/POLY_GT4 embed tpage & clut directly; POLY_F4/POLY_G4 emit DR_TPAGE when semi_trans > 0.
 */
void gfx_draw_poly_f4(int z,
                      int x0, int y0, int x1, int y1,
                      int x2, int y2, int x3, int y3,
                      uint8_t r, uint8_t g, uint8_t b, int semi_trans);

void gfx_draw_poly_g4(int z,
                      int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0,
                      int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1,
                      int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2,
                      int x3, int y3, uint8_t r3, uint8_t g3, uint8_t b3,
                      int semi_trans);

void gfx_draw_poly_ft4(int z, GfxTexSheet sheet,
                       int u0, int v0, int u1, int v1,
                       int u2, int v2, int u3, int v3,
                       int x0, int y0, int x1, int y1,
                       int x2, int y2, int x3, int y3,
                       uint8_t r, uint8_t g, uint8_t b, int semi_trans);

void gfx_draw_poly_gt4(int z, GfxTexSheet sheet,
                       int u0, int v0, int u1, int v1,
                       int u2, int v2, int u3, int v3,
                       int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0,
                       int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1,
                       int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2,
                       int x3, int y3, uint8_t r3, uint8_t g3, uint8_t b3,
                       int semi_trans);

/* Draw ASCII string using the 8x8 bitmap font in env_ui.tim */
void gfx_draw_text(int z, int x, int y, const char *str,
                   uint8_t r, uint8_t g, uint8_t b);

/* Draw ASCII string with a 1px dark drop shadow for crisp readability over bright skies */
void gfx_draw_text_shadow(int z, int x, int y, const char *str,
                          uint8_t r, uint8_t g, uint8_t b);

/* Draw horizontally centered ASCII string on the 320px PAL screen */
void gfx_draw_text_centered(int z, int y, const char *str,
                            uint8_t r, uint8_t g, uint8_t b);

/* Draw unsigned/signed integer with optional zero-padding (min_digits: 0..10) */
void gfx_draw_int(int z, int x, int y, int32_t value, int min_digits,
                  uint8_t r, uint8_t g, uint8_t b);

/* Glossy PS1 / Xbox button glyphs & toggleable drink badges */
typedef enum {
    BTN_STYLE_PS1  = 0,
    BTN_STYLE_XBOX = 1
} GfxBtnStyle;

typedef enum {
    GLYPH_CROSS    = 0, /* PS1 Cross / Xbox A */
    GLYPH_CIRCLE   = 1, /* PS1 Circle / Xbox B */
    GLYPH_SQUARE   = 2, /* PS1 Square / Xbox X */
    GLYPH_TRIANGLE = 3, /* PS1 Triangle / Xbox Y */
    GLYPH_L1       = 4, /* PS1 L1 / Xbox LB (18x12) */
    GLYPH_R1       = 5, /* PS1 R1 / Xbox RB (18x12) */
    GLYPH_DPAD     = 6  /* Shared 14x14 D-Pad */
} GfxBtnGlyph;

void        gfx_set_btn_style(GfxBtnStyle style);
GfxBtnStyle gfx_get_btn_style(void);

void gfx_set_drink_glyphs(int enabled);
int  gfx_get_drink_glyphs(void);

/* Draw a glossy 14x14 (or 18x12 for L1/R1) button cap matching current style */
void gfx_draw_btn(int z, int x, int y, GfxBtnGlyph glyph);

/* Draw a 10x10 mini drink button badge (drink_type: 0=Can, 1=Bottle, 2=DraftBag) */
void gfx_draw_drink_mini_btn(int z, int x, int y, int drink_type);
void gfx_draw_drink_mini_btn_ex(int z, int x, int y, int drink_type, int force);

#endif /* TSINGTAO_GFX_H */
