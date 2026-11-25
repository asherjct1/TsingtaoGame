#include "gfx.h"
#include "../assets/gen/assets_layout.h"
#include "../assets/gen/tim_blobs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>

#define GFX_PRIM_BUF_SIZE (72u * 1024u)
#define GFX_TEX_COUNT     4

/* Double-buffered display/draw environments, ordering tables, and packet arenas */
static DISPENV  s_disp[2];
static DRAWENV  s_draw[2];
static uint32_t s_ot[2][GFX_OT_LEN];
static uint8_t  s_prim_buf[2][GFX_PRIM_BUF_SIZE] __attribute__((aligned(4)));

static uint8_t *s_next_prim   = NULL;
static uint8_t *s_prim_end    = NULL;
static int      s_db          = 0;
static uint32_t s_frame_count = 0;

/* Loaded TIM sheet metadata from GetTimInfo() */
static uint16_t s_sheet_clut[GFX_TEX_COUNT];
static int16_t  s_sheet_vx[GFX_TEX_COUNT];
static int16_t  s_sheet_vy[GFX_TEX_COUNT];
static uint8_t  s_sheet_tp[GFX_TEX_COUNT];

/* Screen shake state (pure integer deterministic PRNG) */
static int      s_shake_frames = 0;
static int      s_shake_mag    = 0;
static int      s_shake_dx     = 0;
static int      s_shake_dy     = 0;
static int      s_shake_apply  = 1;
static uint32_t s_shake_rng    = 0x19960815u;

static uint32_t shake_rand(void) {
    uint32_t x = s_shake_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_shake_rng = x;
    return x;
}

static inline int clamp_z(int z) {
    if (z < 0) return 0;
    if (z >= GFX_OT_LEN) return GFX_OT_LEN - 1;
    return z;
}

static inline uint8_t clamp_uv(int uv) {
    if (uv < 0) return 0;
    if (uv > 255) return 255;
    return (uint8_t)uv;
}

static inline void ot_add_prim(uint32_t *ot_slot, uint32_t *prim_tag) {
    *prim_tag = (*prim_tag & 0xFF000000u) | (*ot_slot & 0x00FFFFFFu);
    *ot_slot  = (*ot_slot  & 0xFF000000u) | ((uint32_t)(uintptr_t)prim_tag & 0x00FFFFFFu);
}

static inline int has_prim_space(size_t bytes) {
    size_t aligned = (bytes + 3u) & ~3u;
    return (s_next_prim != NULL) && ((size_t)(s_prim_end - s_next_prim) >= aligned);
}

static inline void *alloc_prim(size_t bytes) {
    size_t aligned = (bytes + 3u) & ~3u;
    uint8_t *ptr;

    if (!s_next_prim || (size_t)(s_prim_end - s_next_prim) < aligned) {
        return NULL;
    }
    ptr = s_next_prim;
    s_next_prim += aligned;
    return (void *)ptr;
}

static void upload_tim_sheet(GfxTexSheet sheet, const uint8_t *tim_blob) {
    TIM_IMAGE tim;

    if (!tim_blob || GetTimInfo((const uint32_t *)tim_blob, &tim) != 0) {
        return;
    }

    LoadImage(tim.prect, tim.paddr);
    DrawSync(0);

    s_sheet_tp[sheet] = (uint8_t)(tim.mode & 3u);
    s_sheet_vx[sheet] = tim.prect->x;
    s_sheet_vy[sheet] = tim.prect->y;

    if (tim.mode & 0x8u) {
        LoadImage(tim.crect, tim.caddr);
        DrawSync(0);
        s_sheet_clut[sheet] = getClut(tim.crect->x, tim.crect->y);
    } else {
        s_sheet_clut[sheet] = 0;
    }
}

void gfx_init(void) {
    int i;

    ResetGraph(0);
    SetVideoMode(MODE_PAL);

    /*
     * Double-buffered PAL 320x256:
     *   s_db == 0: draw to (0, 0),   display (0, 256)
     *   s_db == 1: draw to (0, 256), display (0, 0)
     */
    SetDefDrawEnv(&s_draw[0], 0,   0, GFX_SCREEN_W, GFX_SCREEN_H);
    SetDefDispEnv(&s_disp[0], 0, 256, GFX_SCREEN_W, GFX_SCREEN_H);

    SetDefDrawEnv(&s_draw[1], 0, 256, GFX_SCREEN_W, GFX_SCREEN_H);
    SetDefDispEnv(&s_disp[1], 0,   0, GFX_SCREEN_W, GFX_SCREEN_H);

    for (i = 0; i < 2; i++) {
        /* Unclipped PAL 320x256 vertical positioning (centered at 0xA3 without +16 shift) */
        s_disp[i].screen.x = 0;
        s_disp[i].screen.y = 0;
        s_disp[i].screen.h = GFX_SCREEN_H;

        s_draw[i].isbg = 1;
        s_draw[i].dtd  = 1;
        s_draw[i].dfe  = 1;
        setRGB0(&s_draw[i], 0, 0, 0);
    }

    /* Upload all 4 embedded .TIM sheets into non-overlapping VRAM regions */
    upload_tim_sheet(GFX_TEX_REAL,    g_tim_real_tsingtao);
    upload_tim_sheet(GFX_TEX_SPRITES, g_tim_sprites);
    upload_tim_sheet(GFX_TEX_ENV_UI,  g_tim_env_ui);
    upload_tim_sheet(GFX_TEX_CARDS,   g_tim_cards);

    /* Clear both framebuffers to black before enabling video output */
    PutDrawEnv(&s_draw[0]);
    DrawSync(0);
    PutDrawEnv(&s_draw[1]);
    DrawSync(0);

    s_db          = 0;
    s_frame_count = 0;
    s_shake_frames = 0;
    s_shake_mag    = 0;
    s_shake_dx     = 0;
    s_shake_dy     = 0;
    s_shake_apply  = 1;

    ClearOTagR(s_ot[0], GFX_OT_LEN);
    ClearOTagR(s_ot[1], GFX_OT_LEN);
    s_next_prim = s_prim_buf[0];
    s_prim_end  = s_prim_buf[0] + GFX_PRIM_BUF_SIZE;

    PutDispEnv(&s_disp[0]);
    VSync(0);
    SetDispMask(1);
}

void gfx_begin_frame(uint8_t clear_r, uint8_t clear_g, uint8_t clear_b) {
    if (s_shake_frames > 0 && s_shake_mag > 0) {
        int span = (s_shake_mag * 2) + 1;
        s_shake_dx = (int)(shake_rand() % (uint32_t)span) - s_shake_mag;
        s_shake_dy = (int)(shake_rand() % (uint32_t)span) - s_shake_mag;
        s_shake_frames--;
        if (s_shake_frames == 0) {
            s_shake_mag = 0;
        }
    } else {
        s_shake_dx = 0;
        s_shake_dy = 0;
    }

    s_shake_apply = 1;

    setRGB0(&s_draw[s_db], clear_r, clear_g, clear_b);
    {
        int i;
        for (i = GFX_OT_LEN - 1; i > 0; i--) {
            s_ot[s_db][i] = (uint32_t)(uintptr_t)&s_ot[s_db][i - 1] & 0x00FFFFFFu;
        }
        s_ot[s_db][0] = 0x00FFFFFFu;
    }
    s_next_prim = s_prim_buf[s_db];
    s_prim_end  = s_prim_buf[s_db] + GFX_PRIM_BUF_SIZE;
}

void gfx_end_frame(void) {
    DrawSync(0);
    VSync(0);

    PutDispEnv(&s_disp[s_db]);
    DrawOTagEnv(&s_ot[s_db][GFX_OT_LEN - 1], &s_draw[s_db]);
    DrawSync(0);

    s_db ^= 1;
    s_frame_count++;
}

uint32_t gfx_frame_count(void) {
    return s_frame_count;
}

void gfx_set_shake(int frames, int magnitude) {
    if (frames <= 0 || magnitude <= 0) {
        s_shake_frames = 0;
        s_shake_mag    = 0;
        s_shake_dx     = 0;
        s_shake_dy     = 0;
        return;
    }
    if (frames > s_shake_frames) {
        s_shake_frames = frames;
    }
    if (magnitude > s_shake_mag) {
        s_shake_mag = magnitude;
    }
}

void gfx_set_shake_apply(int apply) {
    s_shake_apply = apply ? 1 : 0;
}

void gfx_get_shake(int *out_dx, int *out_dy) {
    if (out_dx) *out_dx = s_shake_dx;
    if (out_dy) *out_dy = s_shake_dy;
}

uint16_t gfx_get_tpage(GfxTexSheet sheet, int abr) {
    if ((int)sheet < 0 || (int)sheet >= GFX_TEX_COUNT) {
        return 0;
    }
    return getTPage(s_sheet_tp[sheet], abr & 3, s_sheet_vx[sheet], s_sheet_vy[sheet]);
}

uint16_t gfx_get_clut(GfxTexSheet sheet) {
    if ((int)sheet < 0 || (int)sheet >= GFX_TEX_COUNT) {
        return 0;
    }
    return s_sheet_clut[sheet];
}

static inline void apply_shake(int *x, int *y) {
    if (s_shake_apply) {
        *x += s_shake_dx;
        *y += s_shake_dy;
    }
}

static inline uint16_t patch_tpage_abr(uint16_t tpage, int semi_trans) {
    if (semi_trans > 0) {
        uint16_t abr = (uint16_t)((semi_trans - 1) & 3);
        tpage = (uint16_t)((tpage & ~(3u << 5)) | (abr << 5));
    }
    return tpage;
}

void gfx_draw_sky_grad(int z, int y0, int y1,
                       uint8_t r0, uint8_t g0, uint8_t b0,
                       uint8_t r1, uint8_t g1, uint8_t b1) {
    POLY_G4 *p;
    int oz = clamp_z(z);

    p = (POLY_G4 *)alloc_prim(sizeof(POLY_G4));
    if (!p) return;

    setPolyG4(p);
    setSemiTrans(p, 0);
    setRGB0(p, r0, g0, b0);
    setRGB1(p, r0, g0, b0);
    setRGB2(p, r1, g1, b1);
    setRGB3(p, r1, g1, b1);
    setXY4(p, 0, y0, GFX_SCREEN_W, y0, 0, y1, GFX_SCREEN_W, y1);

    ot_add_prim(&s_ot[s_db][oz], &p->tag);
}

void gfx_draw_grad_rect(int z, int x, int y, int w, int h,
                        uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1,
                        int semi_trans) {
    POLY_G4 *p;
    int oz = clamp_z(z);

    if (w <= 0 || h <= 0) return;
    apply_shake(&x, &y);

    if (semi_trans > 0) {
        DR_TPAGE *tp;
        if (!has_prim_space(sizeof(POLY_G4) + sizeof(DR_TPAGE))) return;
        p  = (POLY_G4 *)alloc_prim(sizeof(POLY_G4));
        tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));

        setPolyG4(p);
        setSemiTrans(p, 1);
        setRGB0(p, r0, g0, b0);
        setRGB1(p, r0, g0, b0);
        setRGB2(p, r1, g1, b1);
        setRGB3(p, r1, g1, b1);
        setXYWH(p, x, y, w, h);

        setDrawTPage(tp, 0, 1, getTPage(0, (semi_trans - 1) & 3, 0, 0));

        ot_add_prim(&s_ot[s_db][oz], &p->tag);
        ot_add_prim(&s_ot[s_db][oz], &tp->tag);
    } else {
        p = (POLY_G4 *)alloc_prim(sizeof(POLY_G4));
        if (!p) return;

        setPolyG4(p);
        setSemiTrans(p, 0);
        setRGB0(p, r0, g0, b0);
        setRGB1(p, r0, g0, b0);
        setRGB2(p, r1, g1, b1);
        setRGB3(p, r1, g1, b1);
        setXYWH(p, x, y, w, h);

        ot_add_prim(&s_ot[s_db][oz], &p->tag);
    }
}

void gfx_draw_rect(int z, int x, int y, int w, int h,
                   uint8_t r, uint8_t g, uint8_t b, int semi_trans) {
    TILE *t;
    int oz = clamp_z(z);

    if (w <= 0 || h <= 0) return;
    apply_shake(&x, &y);

    if (semi_trans > 0) {
        DR_TPAGE *tp;
        if (!has_prim_space(sizeof(TILE) + sizeof(DR_TPAGE))) return;
        t  = (TILE *)alloc_prim(sizeof(TILE));
        tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));

        setTile(t);
        setSemiTrans(t, 1);
        setRGB0(t, r, g, b);
        setXY0(t, x, y);
        setWH(t, w, h);

        setDrawTPage(tp, 0, 1, getTPage(0, (semi_trans - 1) & 3, 0, 0));

        ot_add_prim(&s_ot[s_db][oz], &t->tag);
        ot_add_prim(&s_ot[s_db][oz], &tp->tag);
    } else {
        t = (TILE *)alloc_prim(sizeof(TILE));
        if (!t) return;

        setTile(t);
        setSemiTrans(t, 0);
        setRGB0(t, r, g, b);
        setXY0(t, x, y);
        setWH(t, w, h);

        ot_add_prim(&s_ot[s_db][oz], &t->tag);
    }
}

void gfx_draw_sprt_scaled(int z, uint16_t tpage, uint16_t clut,
                          int u, int v, int w, int h,
                          int x, int y, int dst_w, int dst_h,
                          uint8_t r, uint8_t g, uint8_t b,
                          int flip_x, int semi_trans) {
    POLY_FT4 *p;
    int oz = clamp_z(z);
    uint8_t u0, u1, v0, v1;

    if (w <= 0 || h <= 0 || dst_w <= 0 || dst_h <= 0) return;
    apply_shake(&x, &y);

    p = (POLY_FT4 *)alloc_prim(sizeof(POLY_FT4));
    if (!p) return;

    tpage = patch_tpage_abr(tpage, semi_trans);

    setPolyFT4(p);
    setShadeTex(p, 0);
    setSemiTrans(p, semi_trans > 0);
    setRGB0(p, r, g, b);
    setXYWH(p, x, y, dst_w, dst_h);

    v0 = clamp_uv(v);
    v1 = clamp_uv(v + h);

    if (flip_x) {
        /* Stay strictly inside [u, u+w-1] on reverse UV interpolation to avoid bleed */
        u0 = clamp_uv(u + w - 1);
        u1 = clamp_uv(u);
    } else {
        u0 = clamp_uv(u);
        u1 = clamp_uv(u + w);
    }

    setUV4(p, u0, v0, u1, v0, u0, v1, u1, v1);
    p->clut  = clut;
    p->tpage = tpage;

    ot_add_prim(&s_ot[s_db][oz], &p->tag);
}

void gfx_draw_sprt(int z, uint16_t tpage, uint16_t clut,
                   int u, int v, int w, int h,
                   int x, int y,
                   uint8_t r, uint8_t g, uint8_t b,
                   int flip_x, int semi_trans) {
    SPRT *s;
    DR_TPAGE *tp;
    int oz;

    if (w <= 0 || h <= 0) return;

    /* PS1 SPRT hardware cannot flip horizontally; route flip_x through POLY_FT4 */
    if (flip_x) {
        gfx_draw_sprt_scaled(z, tpage, clut, u, v, w, h, x, y, w, h, r, g, b, 1, semi_trans);
        return;
    }

    oz = clamp_z(z);
    apply_shake(&x, &y);

    if (!has_prim_space(sizeof(SPRT) + sizeof(DR_TPAGE))) return;

    s  = (SPRT *)alloc_prim(sizeof(SPRT));
    tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));

    tpage = patch_tpage_abr(tpage, semi_trans);

    setSprt(s);
    setShadeTex(s, 0);
    setSemiTrans(s, semi_trans > 0);
    setRGB0(s, r, g, b);
    setXY0(s, x, y);
    setUV0(s, clamp_uv(u), clamp_uv(v));
    setWH(s, (uint16_t)w, (uint16_t)h);
    s->clut = clut;

    setDrawTPage(tp, 0, 1, tpage);

    /* Link SPRT first, then DR_TPAGE so DR_TPAGE executes before SPRT */
    ot_add_prim(&s_ot[s_db][oz], &s->tag);
    ot_add_prim(&s_ot[s_db][oz], &tp->tag);
}

void gfx_draw_sheet_sprt(int z, GfxTexSheet sheet,
                         int u, int v, int w, int h,
                         int x, int y,
                         uint8_t r, uint8_t g, uint8_t b,
                         int flip_x, int semi_trans) {
    int abr = (semi_trans > 0) ? ((semi_trans - 1) & 3) : 0;
    gfx_draw_sprt(z, gfx_get_tpage(sheet, abr), gfx_get_clut(sheet),
                  u, v, w, h, x, y, r, g, b, flip_x, semi_trans);
}

void gfx_draw_sheet_sprt_scaled(int z, GfxTexSheet sheet,
                                int u, int v, int w, int h,
                                int x, int y, int dst_w, int dst_h,
                                uint8_t r, uint8_t g, uint8_t b,
                                int flip_x, int semi_trans) {
    int abr = (semi_trans > 0) ? ((semi_trans - 1) & 3) : 0;
    gfx_draw_sprt_scaled(z, gfx_get_tpage(sheet, abr), gfx_get_clut(sheet),
                         u, v, w, h, x, y, dst_w, dst_h, r, g, b, flip_x, semi_trans);
}

static void link_semi_tpage(int oz, int semi_trans) {
    DR_TPAGE *tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));
    if (!tp) return;
    setDrawTPage(tp, 0, 1, getTPage(0, (semi_trans - 1) & 3, 0, 0));
    ot_add_prim(&s_ot[s_db][oz], &tp->tag);
}

void gfx_draw_poly_f4(int z,
                      int x0, int y0, int x1, int y1,
                      int x2, int y2, int x3, int y3,
                      uint8_t r, uint8_t g, uint8_t b, int semi_trans) {
    POLY_F4 *p;
    int oz = clamp_z(z);
    size_t need = sizeof(POLY_F4) + ((semi_trans > 0) ? sizeof(DR_TPAGE) : 0u);

    if (!has_prim_space(need)) return;

    apply_shake(&x0, &y0);
    apply_shake(&x1, &y1);
    apply_shake(&x2, &y2);
    apply_shake(&x3, &y3);

    p = (POLY_F4 *)alloc_prim(sizeof(POLY_F4));
    setPolyF4(p);
    setSemiTrans(p, semi_trans > 0);
    setRGB0(p, r, g, b);
    setXY4(p, x0, y0, x1, y1, x2, y2, x3, y3);
    ot_add_prim(&s_ot[s_db][oz], &p->tag);

    if (semi_trans > 0) {
        link_semi_tpage(oz, semi_trans);
    }
}

void gfx_draw_poly_g4(int z,
                      int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0,
                      int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1,
                      int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2,
                      int x3, int y3, uint8_t r3, uint8_t g3, uint8_t b3,
                      int semi_trans) {
    POLY_G4 *p;
    int oz = clamp_z(z);
    size_t need = sizeof(POLY_G4) + ((semi_trans > 0) ? sizeof(DR_TPAGE) : 0u);

    if (!has_prim_space(need)) return;

    apply_shake(&x0, &y0);
    apply_shake(&x1, &y1);
    apply_shake(&x2, &y2);
    apply_shake(&x3, &y3);

    p = (POLY_G4 *)alloc_prim(sizeof(POLY_G4));
    setPolyG4(p);
    setSemiTrans(p, semi_trans > 0);
    setRGB0(p, r0, g0, b0);
    setRGB1(p, r1, g1, b1);
    setRGB2(p, r2, g2, b2);
    setRGB3(p, r3, g3, b3);
    setXY4(p, x0, y0, x1, y1, x2, y2, x3, y3);
    ot_add_prim(&s_ot[s_db][oz], &p->tag);

    if (semi_trans > 0) {
        link_semi_tpage(oz, semi_trans);
    }
}

void gfx_draw_poly_ft4(int z, GfxTexSheet sheet,
                       int u0, int v0, int u1, int v1,
                       int u2, int v2, int u3, int v3,
                       int x0, int y0, int x1, int y1,
                       int x2, int y2, int x3, int y3,
                       uint8_t r, uint8_t g, uint8_t b, int semi_trans) {
    POLY_FT4 *p;
    int oz = clamp_z(z);
    int abr = (semi_trans > 0) ? ((semi_trans - 1) & 3) : 0;

    p = (POLY_FT4 *)alloc_prim(sizeof(POLY_FT4));
    if (!p) return;

    apply_shake(&x0, &y0);
    apply_shake(&x1, &y1);
    apply_shake(&x2, &y2);
    apply_shake(&x3, &y3);

    setPolyFT4(p);
    setShadeTex(p, 0);
    setSemiTrans(p, semi_trans > 0);
    setRGB0(p, r, g, b);
    setXY4(p, x0, y0, x1, y1, x2, y2, x3, y3);
    setUV4(p,
           clamp_uv(u0), clamp_uv(v0),
           clamp_uv(u1), clamp_uv(v1),
           clamp_uv(u2), clamp_uv(v2),
           clamp_uv(u3), clamp_uv(v3));
    p->clut  = gfx_get_clut(sheet);
    p->tpage = gfx_get_tpage(sheet, abr);

    ot_add_prim(&s_ot[s_db][oz], &p->tag);
}

void gfx_draw_poly_gt4(int z, GfxTexSheet sheet,
                       int u0, int v0, int u1, int v1,
                       int u2, int v2, int u3, int v3,
                       int x0, int y0, uint8_t r0, uint8_t g0, uint8_t b0,
                       int x1, int y1, uint8_t r1, uint8_t g1, uint8_t b1,
                       int x2, int y2, uint8_t r2, uint8_t g2, uint8_t b2,
                       int x3, int y3, uint8_t r3, uint8_t g3, uint8_t b3,
                       int semi_trans) {
    POLY_GT4 *p;
    int oz = clamp_z(z);
    int abr = (semi_trans > 0) ? ((semi_trans - 1) & 3) : 0;

    p = (POLY_GT4 *)alloc_prim(sizeof(POLY_GT4));
    if (!p) return;

    apply_shake(&x0, &y0);
    apply_shake(&x1, &y1);
    apply_shake(&x2, &y2);
    apply_shake(&x3, &y3);

    setPolyGT4(p);
    setShadeTex(p, 0);
    setSemiTrans(p, semi_trans > 0);
    setRGB0(p, r0, g0, b0);
    setRGB1(p, r1, g1, b1);
    setRGB2(p, r2, g2, b2);
    setRGB3(p, r3, g3, b3);
    setXY4(p, x0, y0, x1, y1, x2, y2, x3, y3);
    setUV4(p,
           clamp_uv(u0), clamp_uv(v0),
           clamp_uv(u1), clamp_uv(v1),
           clamp_uv(u2), clamp_uv(v2),
           clamp_uv(u3), clamp_uv(v3));
    p->clut  = gfx_get_clut(sheet);
    p->tpage = gfx_get_tpage(sheet, abr);

    ot_add_prim(&s_ot[s_db][oz], &p->tag);
}

static void draw_text_pass(int oz, int x, int y, const char *str,
                           uint8_t r, uint8_t g, uint8_t b,
                           uint16_t clut, int *out_emitted) {
    int cx = x;
    int emitted = 0;

    while (*str) {
        unsigned char c = (unsigned char)*str++;
        if (c == '\n') {
            cx = x;
            y += FONT_CHAR_H + 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (unsigned char)(c - ('a' - 'A'));
        }
        if (c > FONT_FIRST_CHAR && c <= FONT_LAST_CHAR) {
            SPRT_8 *s;
            /* Always leave room for the trailing DR_TPAGE */
            if (!has_prim_space(sizeof(SPRT_8) + sizeof(DR_TPAGE))) {
                break;
            }
            s = (SPRT_8 *)alloc_prim(sizeof(SPRT_8));
            setSprt8(s);
            setShadeTex(s, 0);
            setSemiTrans(s, 0);
            setRGB0(s, r, g, b);
            setXY0(s, cx, y);
            setUV0(s, (uint8_t)FONT_CHAR_U(c), (uint8_t)FONT_CHAR_V(c));
            s->clut = clut;
            ot_add_prim(&s_ot[s_db][oz], &s->tag);
            emitted++;
        }
        cx += FONT_CHAR_W;
    }

    if (out_emitted) {
        *out_emitted += emitted;
    }
}

void gfx_draw_text(int z, int x, int y, const char *str,
                   uint8_t r, uint8_t g, uint8_t b) {
    int oz;
    int emitted = 0;
    uint16_t clut;

    if (!str || !*str) return;
    oz = clamp_z(z);
    apply_shake(&x, &y);

    clut = gfx_get_clut(GFX_TEX_ENV_UI);
    draw_text_pass(oz, x, y, str, r, g, b, clut, &emitted);

    if (emitted > 0) {
        DR_TPAGE *tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));
        if (tp) {
            setDrawTPage(tp, 0, 1, gfx_get_tpage(GFX_TEX_ENV_UI, 0));
            ot_add_prim(&s_ot[s_db][oz], &tp->tag);
        }
    }
}

void gfx_draw_text_shadow(int z, int x, int y, const char *str,
                          uint8_t r, uint8_t g, uint8_t b) {
    int oz;
    int emitted = 0;
    uint16_t clut;

    if (!str || !*str) return;
    oz = clamp_z(z);
    apply_shake(&x, &y);

    clut = gfx_get_clut(GFX_TEX_ENV_UI);

    /*
     * OT insertion is LIFO within the same slot:
     *   1. Add foreground glyphs (drawn LAST, on top)
     *   2. Add shadow glyphs     (drawn BEFORE foreground)
     *   3. Add single DR_TPAGE   (executed FIRST for all glyphs)
     */
    draw_text_pass(oz, x, y, str, r, g, b, clut, &emitted);
    draw_text_pass(oz, x + 1, y + 1, str, 16, 16, 24, clut, &emitted);

    if (emitted > 0) {
        DR_TPAGE *tp = (DR_TPAGE *)alloc_prim(sizeof(DR_TPAGE));
        if (tp) {
            setDrawTPage(tp, 0, 1, gfx_get_tpage(GFX_TEX_ENV_UI, 0));
            ot_add_prim(&s_ot[s_db][oz], &tp->tag);
        }
    }
}

void gfx_draw_text_centered(int z, int y, const char *str,
                            uint8_t r, uint8_t g, uint8_t b) {
    int len;
    int x;

    if (!str || !*str) return;
    len = (int)strlen(str);
    x   = (GFX_SCREEN_W - (len * FONT_CHAR_W)) / 2;
    gfx_draw_text_shadow(z, x, y, str, r, g, b);
}

void gfx_draw_int(int z, int x, int y, int32_t value, int min_digits,
                  uint8_t r, uint8_t g, uint8_t b) {
    char buf[16];
    char rev[16];
    int neg = 0;
    uint32_t uval;
    int n = 0;
    int pos = 0;

    if (min_digits < 1)  min_digits = 1;
    if (min_digits > 10) min_digits = 10;

    if (value < 0) {
        neg  = 1;
        uval = (uint32_t)(-(value + 1)) + 1u;
    } else {
        uval = (uint32_t)value;
    }

    do {
        rev[n++] = (char)('0' + (uval % 10u));
        uval /= 10u;
    } while (uval > 0u && n < 12);

    while (n < min_digits && n < 12) {
        rev[n++] = '0';
    }

    if (neg) {
        buf[pos++] = '-';
    }
    while (n > 0) {
        buf[pos++] = rev[--n];
    }
    buf[pos] = '\0';

    gfx_draw_text(z, x, y, buf, r, g, b);
}

static GfxBtnStyle s_btn_style    = BTN_STYLE_PS1;
static int         s_drink_glyphs = 1;

void gfx_set_btn_style(GfxBtnStyle style) {
    s_btn_style = (style == BTN_STYLE_XBOX) ? BTN_STYLE_XBOX : BTN_STYLE_PS1;
}

GfxBtnStyle gfx_get_btn_style(void) {
    return s_btn_style;
}

void gfx_set_drink_glyphs(int enabled) {
    s_drink_glyphs = enabled ? 1 : 0;
}

int gfx_get_drink_glyphs(void) {
    return s_drink_glyphs;
}

void gfx_draw_btn(int z, int x, int y, GfxBtnGlyph glyph) {
    static const uint16_t k_ps_u[7] = {
        SPR_GLYPH_PS_CROSS_U, SPR_GLYPH_PS_CIRCLE_U, SPR_GLYPH_PS_SQUARE_U,
        SPR_GLYPH_PS_TRIANGLE_U, SPR_GLYPH_PS_L1_U, SPR_GLYPH_PS_R1_U,
        SPR_GLYPH_DPAD_U
    };
    static const uint16_t k_ps_v[7] = {
        SPR_GLYPH_PS_CROSS_V, SPR_GLYPH_PS_CIRCLE_V, SPR_GLYPH_PS_SQUARE_V,
        SPR_GLYPH_PS_TRIANGLE_V, SPR_GLYPH_PS_L1_V, SPR_GLYPH_PS_R1_V,
        SPR_GLYPH_DPAD_V
    };
    static const uint16_t k_xb_u[7] = {
        SPR_GLYPH_XB_A_U, SPR_GLYPH_XB_B_U, SPR_GLYPH_XB_X_U,
        SPR_GLYPH_XB_Y_U, SPR_GLYPH_XB_LB_U, SPR_GLYPH_XB_RB_U,
        SPR_GLYPH_DPAD_U
    };
    static const uint16_t k_xb_v[7] = {
        SPR_GLYPH_XB_A_V, SPR_GLYPH_XB_B_V, SPR_GLYPH_XB_X_V,
        SPR_GLYPH_XB_Y_V, SPR_GLYPH_XB_LB_V, SPR_GLYPH_XB_RB_V,
        SPR_GLYPH_DPAD_V
    };
    int idx = (int)glyph;
    int u, v, w, h;

    if (idx < 0 || idx > (int)GLYPH_DPAD) return;
    if (s_btn_style == BTN_STYLE_XBOX) {
        u = (int)k_xb_u[idx];
        v = (int)k_xb_v[idx];
    } else {
        u = (int)k_ps_u[idx];
        v = (int)k_ps_v[idx];
    }
    if (glyph == GLYPH_L1 || glyph == GLYPH_R1) {
        w = SPR_GLYPH_SHLD_W;
        h = SPR_GLYPH_SHLD_H;
    } else {
        w = SPR_GLYPH_W;
        h = SPR_GLYPH_H;
    }
    gfx_draw_sheet_sprt(z, GFX_TEX_SPRITES, u, v, w, h, x, y,
                        128, 128, 128, 0, GFX_TRANS_NONE);
}

void gfx_draw_drink_mini_btn_ex(int z, int x, int y, int drink_type, int force) {
    static const uint16_t k_ps_u[3] = {
        SPR_MINI_PS_CROSS_U, SPR_MINI_PS_CIRCLE_U, SPR_MINI_PS_SQUARE_U
    };
    static const uint16_t k_ps_v[3] = {
        SPR_MINI_PS_CROSS_V, SPR_MINI_PS_CIRCLE_V, SPR_MINI_PS_SQUARE_V
    };
    static const uint16_t k_xb_u[3] = {
        SPR_MINI_XB_A_U, SPR_MINI_XB_B_U, SPR_MINI_XB_X_U
    };
    static const uint16_t k_xb_v[3] = {
        SPR_MINI_XB_A_V, SPR_MINI_XB_B_V, SPR_MINI_XB_X_V
    };
    int u, v;

    if (!s_drink_glyphs && !force) return;
    if (drink_type < 0 || drink_type > 2) return;

    if (s_btn_style == BTN_STYLE_XBOX) {
        u = (int)k_xb_u[drink_type];
        v = (int)k_xb_v[drink_type];
    } else {
        u = (int)k_ps_u[drink_type];
        v = (int)k_ps_v[drink_type];
    }
    gfx_draw_sheet_sprt(z, GFX_TEX_SPRITES, u, v,
                        SPR_MINI_GLYPH_W, SPR_MINI_GLYPH_H,
                        x, y, 128, 128, 128, 0, GFX_TRANS_NONE);
}

void gfx_draw_drink_mini_btn(int z, int x, int y, int drink_type) {
    gfx_draw_drink_mini_btn_ex(z, x, y, drink_type, 0);
}
