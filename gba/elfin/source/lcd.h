/*
 * LCD renderer (mode 3). Segments fade in and out like the real
 * reflective LCD (BrickEmuPy's "motion blur" of 0.6 per 60 Hz frame).
 */
#ifndef LCD_H
#define LCD_H

#include <stdint.h>

void lcd_init(void);
void lcd_draw_background(void);                 /* full redraw */
void lcd_update(const uint8_t *lcd_ram, int enabled);  /* once per frame */
void lcd_force_redraw(void);
void lcd_set_motion_blur(int on);

#endif
