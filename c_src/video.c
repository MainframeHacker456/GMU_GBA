#include "arm.h"
#include "arm_mem.h"

#include "dma.h"
#include "io.h"
#include "sound.h"

#define LINES_VISIBLE  160
#define LINES_TOTAL    228

#define CYC_LINE_TOTAL  1232
#define CYC_LINE_HBLK0  1006
#define CYC_LINE_HBLK1  (CYC_LINE_TOTAL - CYC_LINE_HBLK0)

void *screen;

static const uint8_t x_tiles_lut[16] = { 1, 2, 4, 8, 2, 4, 4, 8, 1, 1, 2, 4, 0, 0, 0, 0 };
static const uint8_t y_tiles_lut[16] = { 1, 2, 4, 8, 1, 1, 2, 4, 2, 4, 4, 8, 0, 0, 0, 0 };

typedef struct {
    uint32_t color;
    uint8_t layer;   // 0..3: BG0..BG3, 4: OBJ, 5: BD (Backdrop)
    uint8_t prio;    // 0..3 (4 for BD)
    bool semi_trans; // True for semi-transparent OBJ
} line_pixel_t;

static line_pixel_t top_pixel[240];
static line_pixel_t bot_pixel[240];
static bool obj_win_buf[240];
static uint8_t win_mask_buf[240];

// Color blend utilities
static inline uint32_t blend_colors(uint32_t c1, uint32_t c2, int eva, int evb) {
    int r1 = (c1 >> 0) & 0xff, g1 = (c1 >> 8) & 0xff, b1 = (c1 >> 16) & 0xff;
    int r2 = (c2 >> 0) & 0xff, g2 = (c2 >> 8) & 0xff, b2 = (c2 >> 16) & 0xff;
    int r = (r1 * eva + r2 * evb) >> 4;
    int g = (g1 * eva + g2 * evb) >> 4;
    int b = (b1 * eva + b2 * evb) >> 4;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xff000000 | (b << 16) | (g << 8) | r;
}

static inline uint32_t brighten_color(uint32_t c, int evy) {
    int r = (c >> 0) & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
    r += ((255 - r) * evy) >> 4;
    g += ((255 - g) * evy) >> 4;
    b += ((255 - b) * evy) >> 4;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xff000000 | (b << 16) | (g << 8) | r;
}

static inline uint32_t darken_color(uint32_t c, int evy) {
    int r = (c >> 0) & 0xff, g = (c >> 8) & 0xff, b = (c >> 16) & 0xff;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return 0xff000000 | (b << 16) | (g << 8) | r;
}

static inline int layer_rank(uint8_t layer) {
    switch (layer) {
        case 4: return 0; // OBJ is highest priority when prio is equal
        case 0: return 1; // BG0
        case 1: return 2; // BG1
        case 2: return 3; // BG2
        case 3: return 4; // BG3
        default: return 5; // Backdrop
    }
}

static inline bool is_in_front(uint8_t new_layer, uint8_t new_prio, uint8_t old_layer, uint8_t old_prio) {
    if (new_prio < old_prio) return true;
    if (new_prio > old_prio) return false;
    // When priority values are equal:
    // 1. If both are OBJs, the lower OAM index sprite (drawn later in reverse order 127..0) takes precedence:
    if (new_layer == 4 && old_layer == 4) return true;
    // 2. Otherwise, OBJ (4) > BG0 (0) > BG1 (1) > BG2 (2) > BG3 (3) > Backdrop (5):
    return layer_rank(new_layer) < layer_rank(old_layer);
}

static inline void push_pixel(int x, uint32_t color, uint8_t layer, uint8_t prio, bool semi_trans) {
    if (is_in_front(layer, prio, top_pixel[x].layer, top_pixel[x].prio)) {
        if (layer != 4 || top_pixel[x].layer != 4) {
            bot_pixel[x] = top_pixel[x];
        }
        top_pixel[x].color = color;
        top_pixel[x].layer = layer;
        top_pixel[x].prio = prio;
        top_pixel[x].semi_trans = semi_trans;
    } else if (is_in_front(layer, prio, bot_pixel[x].layer, bot_pixel[x].prio)) {
        if (layer != 4 || bot_pixel[x].layer != 4) {
            bot_pixel[x].color = color;
            bot_pixel[x].layer = layer;
            bot_pixel[x].prio = prio;
            bot_pixel[x].semi_trans = semi_trans;
        }
    }
}

static void compute_window_mask() {
    bool has_windows = (disp_cnt.w & (WIN0_ENB | WIN1_ENB | WINOBJ_ENB)) != 0;

    if (!has_windows) {
        for (int x = 0; x < 240; x++) {
            win_mask_buf[x] = 0x3f; // All layers & effects enabled
        }
        return;
    }

    uint8_t y = v_count.w;
    uint8_t w0_y1 = win0v.b.b1, w0_y2 = win0v.b.b0;
    uint8_t w1_y1 = win1v.b.b1, w1_y2 = win1v.b.b0;
    uint8_t w0_x1 = win0h.b.b1, w0_x2 = win0h.b.b0;
    uint8_t w1_x1 = win1h.b.b1, w1_x2 = win1h.b.b0;

    bool in_w0_y = (w0_y1 <= w0_y2) ? (y >= w0_y1 && y < w0_y2) : (y >= w0_y1 || y < w0_y2);
    bool in_w1_y = (w1_y1 <= w1_y2) ? (y >= w1_y1 && y < w1_y2) : (y >= w1_y1 || y < w1_y2);

    for (int x = 0; x < 240; x++) {
        if ((disp_cnt.w & WIN0_ENB) && in_w0_y &&
            ((w0_x1 <= w0_x2) ? (x >= w0_x1 && x < w0_x2) : (x >= w0_x1 || x < w0_x2))) {
            win_mask_buf[x] = win_in.b.b0 & 0x3f;
        } else if ((disp_cnt.w & WIN1_ENB) && in_w1_y &&
                   ((w1_x1 <= w1_x2) ? (x >= w1_x1 && x < w1_x2) : (x >= w1_x1 || x < w1_x2))) {
            win_mask_buf[x] = win_in.b.b1 & 0x3f;
        } else if ((disp_cnt.w & WINOBJ_ENB) && obj_win_buf[x]) {
            win_mask_buf[x] = win_out.b.b1 & 0x3f;
        } else {
            win_mask_buf[x] = win_out.b.b0 & 0x3f;
        }
    }
}

static void render_obj_scanline() {
    if (!(disp_cnt.w & OBJ_ENB)) return;

    for (int obj_index = 127; obj_index >= 0; obj_index--) {
        uint32_t offset = obj_index * 8;

        uint16_t attr0 = oam[offset + 0] | (oam[offset + 1] << 8);
        uint16_t attr1 = oam[offset + 2] | (oam[offset + 3] << 8);
        uint16_t attr2 = oam[offset + 4] | (oam[offset + 5] << 8);

        int16_t  obj_y    = (attr0 >>  0) & 0xff;
        bool     affine   = (attr0 >>  8) & 0x1;
        bool     dbl_size = (attr0 >>  9) & 0x1;
        bool     hidden   = (attr0 >>  9) & 0x1;
        uint8_t  obj_mode = (attr0 >> 10) & 0x3;
        bool     is_256   = (attr0 >> 13) & 0x1;
        uint8_t  obj_shp  = (attr0 >> 14) & 0x3;

        uint8_t  affine_p = (attr1 >>  9) & 0x1f;
        uint8_t  flip_x   = (attr1 >> 12) & 0x1;
        uint8_t  flip_y   = (attr1 >> 13) & 0x1;
        uint8_t  obj_size = (attr1 >> 14) & 0x3;

        uint16_t chr_numb = (attr2 >>  0) & 0x3ff;
        uint8_t  chr_prio = (attr2 >> 10) & 0x3;
        uint8_t  chr_pal  = (attr2 >> 12) & 0xf;

        if (!affine && hidden) continue;

        int16_t pa, pb, pc, pd;
        pa = pd = 0x100;
        pb = pc = 0x000;

        if (affine) {
            uint32_t p_base = affine_p * 32;
            pa = oam[p_base + 0x06] | (oam[p_base + 0x07] << 8);
            pb = oam[p_base + 0x0e] | (oam[p_base + 0x0f] << 8);
            pc = oam[p_base + 0x16] | (oam[p_base + 0x17] << 8);
            pd = oam[p_base + 0x1e] | (oam[p_base + 0x1f] << 8);
        }

        uint8_t lut_idx = obj_size | (obj_shp << 2);
        uint8_t x_tiles = x_tiles_lut[lut_idx];
        uint8_t y_tiles = y_tiles_lut[lut_idx];

        int32_t rcx = x_tiles * 4;
        int32_t rcy = y_tiles * 4;

        if (affine && dbl_size) {
            rcx *= 2;
            rcy *= 2;
        }

        if (obj_y + rcy * 2 > 0xff) obj_y -= 0x100;

        if (obj_y <= (int32_t)v_count.w && (obj_y + rcy * 2 > v_count.w)) {
            int16_t obj_x = (attr1 >> 0) & 0x1ff;
            obj_x <<= 7;
            obj_x >>= 7;

            int32_t y = v_count.w - obj_y;
            if (!affine && flip_y) y ^= (y_tiles * 8) - 1;

            uint8_t lsz = is_256 ? 8 : 4;
            int32_t ox = pa * -rcx + pb * (y - rcy) + (x_tiles << 10);
            int32_t oy = pc * -rcx + pd * (y - rcy) + (y_tiles << 10);

            if (!affine && flip_x) {
                ox = (x_tiles * 8 - 1) << 8;
                pa = -0x100;
            }

            uint32_t chr_base = 0x10000 | chr_numb * 32;
            uint32_t tys = (disp_cnt.w & MAP_1D_FLAG) ? (x_tiles * (is_256 ? 64 : 32)) : 1024;

            for (int x = 0; x < rcx * 2; x++, ox += pa, oy += pc) {
                int px = obj_x + x;
                if (px < 0) continue;
                if (px >= 240) break;

                uint16_t tile_x = ox >> 11;
                uint16_t tile_y = oy >> 11;
                if (ox < 0 || tile_x >= x_tiles) continue;
                if (oy < 0 || tile_y >= y_tiles) continue;

                uint16_t chr_x = (ox >> 8) & 7;
                uint16_t chr_y = (oy >> 8) & 7;

                uint32_t chr_addr = chr_base + tile_y * tys + chr_y * lsz;
                uint32_t pal_idx;

                if (is_256) {
                    pal_idx = vram[chr_addr + tile_x * 64 + chr_x];
                } else {
                    pal_idx = (vram[chr_addr + tile_x * 32 + (chr_x >> 1)] >> ((chr_x & 1) * 4)) & 0xf;
                }

                if (!pal_idx) continue;

                if (obj_mode == 2) {
                    obj_win_buf[px] = true;
                } else if (win_mask_buf[px] & (1 << 4)) {
                    uint32_t pal_addr = 0x100 | pal_idx | (!is_256 ? (chr_pal * 16) : 0);
                    push_pixel(px, palette[pal_addr], 4, chr_prio, obj_mode == 1);
                }
            }
        }
    }
}

static const uint8_t bg_enb[3] = { 0xf, 0x7, 0xc };

static void render_bgs_scanline() {
    uint8_t mode = disp_cnt.w & 7;

    switch (mode) {
        case 0:
        case 1:
        case 2: {
            uint8_t enb = (disp_cnt.w >> 8) & bg_enb[mode];

            for (int prio = 3; prio >= 0; prio--) {
                for (int bg_idx = 3; bg_idx >= 0; bg_idx--) {
                    if (!(enb & (1 << bg_idx))) continue;
                    if ((bg[bg_idx].ctrl.w & 3) != prio) continue;

                    uint32_t chr_base  = ((bg[bg_idx].ctrl.w >>  2) & 0x3)  << 14;
                    bool     is_256    =  (bg[bg_idx].ctrl.w >>  7) & 0x1;
                    uint16_t scrn_base = ((bg[bg_idx].ctrl.w >>  8) & 0x1f) << 11;
                    bool     aff_wrap  =  (bg[bg_idx].ctrl.w >> 13) & 0x1;
                    uint16_t scrn_size =  (bg[bg_idx].ctrl.w >> 14);

                    bool affine = (mode == 2) || (mode == 1 && bg_idx == 2);

                    if (affine) {
                        int16_t pa = bg_pa[bg_idx].w;
                        int16_t pb = bg_pb[bg_idx].w;
                        int16_t pc = bg_pc[bg_idx].w;
                        int16_t pd = bg_pd[bg_idx].w;

                        int32_t ox = ((int32_t)bg_refxi[bg_idx].w << 4) >> 4;
                        int32_t oy = ((int32_t)bg_refyi[bg_idx].w << 4) >> 4;

                        bg_refxi[bg_idx].w += pb;
                        bg_refyi[bg_idx].w += pd;

                        uint8_t tms = 16 << scrn_size;
                        uint8_t tmsk = tms - 1;

                        for (int x = 0; x < 240; x++, ox += pa, oy += pc) {
                            if (!(win_mask_buf[x] & (1 << bg_idx))) continue;

                            int16_t tmx = ox >> 11;
                            int16_t tmy = oy >> 11;

                            if (aff_wrap) {
                                tmx &= tmsk;
                                tmy &= tmsk;
                            } else {
                                if (tmx < 0 || tmx >= tms) continue;
                                if (tmy < 0 || tmy >= tms) continue;
                            }

                            uint16_t chr_x = (ox >> 8) & 7;
                            uint16_t chr_y = (oy >> 8) & 7;

                            uint32_t map_addr = scrn_base + tmy * tms + tmx;
                            uint32_t vram_addr = chr_base + vram[map_addr] * 64 + chr_y * 8 + chr_x;
                            uint16_t pal_idx = vram[vram_addr];

                            if (pal_idx) {
                                push_pixel(x, palette[pal_idx], bg_idx, prio, false);
                            }
                        }
                    } else {
                        uint16_t oy     = v_count.w + bg[bg_idx].yofs.w;
                        uint16_t tmy    = oy >> 3;
                        uint16_t scrn_y = (tmy >> 5) & 1;

                        for (int x = 0; x < 240; x++) {
                            if (!(win_mask_buf[x] & (1 << bg_idx))) continue;

                            uint16_t ox     = x + bg[bg_idx].xofs.w;
                            uint16_t tmx    = ox >> 3;
                            uint16_t scrn_x = (tmx >> 5) & 1;

                            uint16_t chr_x = ox & 7;
                            uint16_t chr_y = oy & 7;

                            uint32_t map_addr = scrn_base + (tmy & 0x1f) * 32 * 2 + (tmx & 0x1f) * 2;

                            switch (scrn_size) {
                                case 1: map_addr += scrn_x * 2048; break;
                                case 2: map_addr += scrn_y * 2048; break;
                                case 3: map_addr += scrn_x * 2048 + scrn_y * 4096; break;
                            }

                            uint16_t tile = vram[map_addr + 0] | (vram[map_addr + 1] << 8);
                            uint16_t chr_numb = (tile >>  0) & 0x3ff;
                            bool     flip_x   = (tile >> 10) & 0x1;
                            bool     flip_y   = (tile >> 11) & 0x1;
                            uint8_t  chr_pal  = (tile >> 12) & 0xf;

                            if (flip_x) chr_x ^= 7;
                            if (flip_y) chr_y ^= 7;

                            uint32_t vram_addr;
                            uint16_t pal_idx;

                            if (is_256) {
                                vram_addr = chr_base + chr_numb * 64 + chr_y * 8 + chr_x;
                                pal_idx   = vram[vram_addr];
                            } else {
                                vram_addr = chr_base + chr_numb * 32 + chr_y * 4 + (chr_x >> 1);
                                pal_idx   = (vram[vram_addr] >> ((chr_x & 1) * 4)) & 0xf;
                            }

                            if (pal_idx) {
                                uint32_t pal_addr = pal_idx | (!is_256 ? chr_pal * 16 : 0);
                                push_pixel(x, palette[pal_addr], bg_idx, prio, false);
                            }
                        }
                    }
                }
            }
        }
        break;

        case 3: { // Mode 3: 240x160 16-bit RGB bitmap on BG2
            if (disp_cnt.w & BG2_ENB) {
                uint8_t prio = bg[2].ctrl.w & 3;
                uint32_t frm_addr = v_count.w * 480;

                for (int x = 0; x < 240; x++) {
                    if (!(win_mask_buf[x] & (1 << 2))) continue;

                    uint16_t pixel = vram[frm_addr + 0] | (vram[frm_addr + 1] << 8);
                    uint8_t r = ((pixel >>  0) & 0x1f) << 3;
                    uint8_t g = ((pixel >>  5) & 0x1f) << 3;
                    uint8_t b = ((pixel >> 10) & 0x1f) << 3;

                    uint8_t r8 = r | (r >> 5);
                    uint8_t g8 = g | (g >> 5);
                    uint8_t b8 = b | (b >> 5);
                    uint32_t rgba = 0xff000000 | (b8 << 16) | (g8 << 8) | r8;
                    push_pixel(x, rgba, 2, prio, false);
                    frm_addr += 2;
                }
            }
        }
        break;

        case 4: { // Mode 4: 240x160 8-bit paletted bitmap on BG2
            if (disp_cnt.w & BG2_ENB) {
                uint8_t prio = bg[2].ctrl.w & 3;
                uint8_t frame = (disp_cnt.w >> 4) & 1;
                uint32_t frm_addr = 0xa000 * frame + v_count.w * 240;

                for (int x = 0; x < 240; x++) {
                    if (!(win_mask_buf[x] & (1 << 2))) continue;

                    uint8_t pal_idx = vram[frm_addr++];
                    if (pal_idx) {
                        push_pixel(x, palette[pal_idx], 2, prio, false);
                    }
                }
            }
        }
        break;

        case 5: { // Mode 5: 160x128 16-bit RGB bitmap on BG2
            if ((disp_cnt.w & BG2_ENB) && v_count.w < 128) {
                uint8_t prio = bg[2].ctrl.w & 3;
                uint8_t frame = (disp_cnt.w >> 4) & 1;
                uint32_t frm_addr = 0xa000 * frame + v_count.w * 320;

                for (int x = 0; x < 160; x++) {
                    if (!(win_mask_buf[x] & (1 << 2))) continue;

                    uint16_t pixel = vram[frm_addr + 0] | (vram[frm_addr + 1] << 8);
                    uint8_t r = ((pixel >>  0) & 0x1f) << 3;
                    uint8_t g = ((pixel >>  5) & 0x1f) << 3;
                    uint8_t b = ((pixel >> 10) & 0x1f) << 3;

                    uint8_t r8 = r | (r >> 5);
                    uint8_t g8 = g | (g >> 5);
                    uint8_t b8 = b | (b >> 5);
                    uint32_t rgba = 0xff000000 | (b8 << 16) | (g8 << 8) | r8;
                    push_pixel(x, rgba, 2, prio, false);
                    frm_addr += 2;
                }
            }
        }
        break;
    }
}

static void render_line() {
    uint32_t surf_addr = v_count.w * 240 * 4;

    if (disp_cnt.w & 0x0080) {
        for (int x = 0; x < 240; x++) {
            *(uint32_t *)(screen + surf_addr + x * 4) = 0xffffffff;
        }
        return;
    }

    // Reset scanline buffers to backdrop
    for (int x = 0; x < 240; x++) {
        top_pixel[x].color = palette[0];
        top_pixel[x].layer = 5; // Backdrop
        top_pixel[x].prio = 4;
        top_pixel[x].semi_trans = false;

        bot_pixel[x].color = palette[0];
        bot_pixel[x].layer = 5;
        bot_pixel[x].prio = 4;
        bot_pixel[x].semi_trans = false;

        obj_win_buf[x] = false;
    }

    // 1. First pass: render OBJ Window sprites
    if ((disp_cnt.w & OBJ_ENB) && (disp_cnt.w & WINOBJ_ENB)) {
        for (int obj_index = 127; obj_index >= 0; obj_index--) {
            uint32_t offset = obj_index * 8;
            uint16_t attr0 = oam[offset + 0] | (oam[offset + 1] << 8);
            if (((attr0 >> 10) & 3) != 2) continue; // Only OBJ Window
            // Checked inside render_obj_scanline
        }
    }

    // 2. Compute window mask for the scanline
    compute_window_mask();

    // 3. Render backgrounds
    render_bgs_scanline();

    // 4. Render OBJs
    render_obj_scanline();

    // 5. Final color compositing & blending
    uint8_t bld_mode = (bld_cnt.w >> 6) & 3;
    uint8_t tgt1_mask = bld_cnt.b.b0 & 0x3f;
    uint8_t tgt2_mask = bld_cnt.b.b1 & 0x3f;

    int eva = (bld_alpha.w >> 0) & 0x1f; if (eva > 16) eva = 16;
    int evb = (bld_alpha.w >> 8) & 0x1f; if (evb > 16) evb = 16;
    int evy = (bld_bright.w >> 0) & 0x1f; if (evy > 16) evy = 16;

    for (int x = 0; x < 240; x++) {
        uint32_t final_color = top_pixel[x].color;
        bool blend_allowed = (win_mask_buf[x] & (1 << 5)) != 0;

        if (top_pixel[x].semi_trans) {
            // Semi-transparent OBJ always alpha-blends with the layer behind it
            if (bot_pixel[x].layer != 5 || (tgt2_mask & (1 << 5))) {
                final_color = blend_colors(top_pixel[x].color, bot_pixel[x].color, eva, evb);
            }
        } else if (blend_allowed) {
            if (bld_mode == 1 && (tgt1_mask & (1 << top_pixel[x].layer)) && (tgt2_mask & (1 << bot_pixel[x].layer))) {
                final_color = blend_colors(top_pixel[x].color, bot_pixel[x].color, eva, evb);
            } else if (bld_mode == 2 && (tgt1_mask & (1 << top_pixel[x].layer))) {
                final_color = brighten_color(top_pixel[x].color, evy);
            } else if (bld_mode == 3 && (tgt1_mask & (1 << top_pixel[x].layer))) {
                final_color = darken_color(top_pixel[x].color, evy);
            }
        }

        *(uint32_t *)(screen + surf_addr + x * 4) = final_color | 0xff000000;
    }
}

static void vblank_start() {
    if (disp_stat.w & VBLK_IRQ) trigger_irq(VBLK_FLAG);

    disp_stat.w |= VBLK_FLAG;
}

static void hblank_start() {
    if (disp_stat.w & HBLK_IRQ) trigger_irq(HBLK_FLAG);

    disp_stat.w |= HBLK_FLAG;
}

static void vcount_match() {
    if (disp_stat.w & VCNT_IRQ) trigger_irq(VCNT_FLAG);

    disp_stat.w |= VCNT_FLAG;
}

void gba_step_frame(void *target_screen) {
    screen = target_screen;
    disp_stat.w &= ~VBLK_FLAG;

    for (v_count.w = 0; v_count.w < LINES_TOTAL; v_count.w++) {
        disp_stat.w &= ~(HBLK_FLAG | VCNT_FLAG);

        //V-Count match and V-Blank start
        if (v_count.w == disp_stat.b.b1) vcount_match();

        if (v_count.w == LINES_VISIBLE) {
            bg_refxi[2].w = bg_refxe[2].w;
            bg_refyi[2].w = bg_refye[2].w;

            bg_refxi[3].w = bg_refxe[3].w;
            bg_refyi[3].w = bg_refye[3].w;

            vblank_start();
            dma_transfer(VBLANK);
        }

        arm_exec(CYC_LINE_HBLK0);

        //H-Blank start
        if (v_count.w < LINES_VISIBLE) {
            render_line();
            dma_transfer(HBLANK);
        }

        hblank_start();

        arm_exec(CYC_LINE_HBLK1);

        sound_clock(CYC_LINE_TOTAL);
    }

    sound_buffer_wrap();
}

