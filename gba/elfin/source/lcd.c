/*
 * LCD renderer - see lcd.h. Compiled as ARM code into IWRAM.
 *
 * Every screen pixel covered by a segment (or by a segment's drop shadow)
 * has a precomputed list of contributors (assets.c). When a segment's
 * brightness level changes, only its pixels are recomposed.
 */
#include <tonc.h>
#include "lcd.h"
#include "assets.h"

#define LEVELS    16                /* brightness levels 0..16 */
#define FULL      (LEVELS << 8)     /* fixed point intensity */

static uint16_t intensity[SEG_COUNT];   /* 0..FULL */
static uint8_t level[SEG_COUNT];        /* 0..LEVELS as drawn */
static uint8_t pending[SEG_COUNT];
static int blur = 0;

EWRAM_BSS static uint16_t stamp[PIX_COUNT];
static uint16_t frame = 1;

void lcd_set_motion_blur(int on) { blur = on; }

void lcd_draw_background(void) {
    dma3_cpy(vid_mem, bg_bitmap, 240 * 160 * 2);
    for (int s = 0; s < SEG_COUNT; s++)
        level[s] = 0;
}

void lcd_force_redraw(void) {
    lcd_draw_background();
    for (int s = 0; s < SEG_COUNT; s++)
        level[s] = 0xFF;    /* different from any real level */
}

void lcd_init(void) {
    for (int s = 0; s < SEG_COUNT; s++) {
        intensity[s] = 0;
        level[s] = 0;
    }
    lcd_draw_background();
}

static inline void compose(int p) {
    uint16_t bgc = pix_bg[p];
    int r = bgc & 31, g = (bgc >> 5) & 31, b = (bgc >> 10) & 31;
    int first = pix_first[p], n = pix_n[p];
    for (int i = 0; i < n; i++) {
        uint16_t cs = c_seg[first + i];
        int lv = level[cs & 0x1FF];
        if (!lv)
            continue;
        int a = (c_alpha[first + i] * lv) >> 4;     /* 0..255 */
        if (cs & 0x8000) {
            r -= (r * a) >> 8;
            g -= (g * a) >> 8;
            b -= (b * a) >> 8;
        } else {
            r += (((INK_R >> 3) - r) * a) >> 8;
            g += (((INK_G >> 3) - g) * a) >> 8;
            b += (((INK_B >> 3) - b) * a) >> 8;
        }
    }
    vid_mem[pix_off[p]] = (uint16_t)(r | (g << 5) | (b << 10));
}

void lcd_update(const uint8_t *lcd_ram, int enabled) {
    int changed = 0;
    for (int s = 0; s < SEG_COUNT; s++) {
        if (seg_start[s] == seg_start[s + 1])
            continue;
        int target = (enabled && ((lcd_ram[s >> 3] >> (s & 7)) & 1)) ? FULL : 0;
        int v = intensity[s];
        if (v != target) {
            if (blur) {
                /* v += 0.4 * (target - v), BrickEmuPy's k = 1 - 0.6 */
                v += ((target - v) * 102) >> 8;
                if (v > target - 16 && v < target + 16)
                    v = target;
            } else {
                v = target;
            }
            intensity[s] = (uint16_t)v;
        }
        int lv = (v + 128) >> 8;
        if (lv != level[s]) {
            level[s] = (uint8_t)lv;
            pending[s] = 1;
            changed = 1;
        }
    }
    if (!changed)
        return;

    frame++;
    if (frame == 0) {
        for (int i = 0; i < PIX_COUNT; i++)
            stamp[i] = 0;
        frame = 1;
    }
    for (int s = 0; s < SEG_COUNT; s++) {
        if (!pending[s])
            continue;
        pending[s] = 0;
        for (int i = seg_start[s]; i < seg_start[s + 1]; i++) {
            int p = seg_list[i];
            if (stamp[p] == frame)
                continue;
            stamp[p] = frame;
            compose(p);
        }
    }
}
