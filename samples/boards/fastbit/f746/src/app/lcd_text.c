/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#include "lcd_text.h"
#include "font5x7.h"

static inline uint16_t *fb_pixel(uint16_t *fb, uint32_t fb_width, uint32_t x,
                                  uint32_t y)
{
    return &fb[y * fb_width + x];
}

static void fb_fill_rect(uint16_t *fb, uint32_t fb_width, uint32_t x,
                          uint32_t width, uint32_t y, uint32_t height,
                          uint16_t rgb565)
{
    for (uint32_t row = 0; row < height; row++) {
        uint16_t *p = fb_pixel(fb, fb_width, x, y + row);

        for (uint32_t col = 0; col < width; col++) {
            p[col] = rgb565;
        }
    }
}

void lcd_text_draw_char(uint16_t *fb, uint32_t fb_width, uint32_t x,
                         uint32_t y, char c, uint16_t fg_rgb565,
                         uint16_t bg_rgb565, uint32_t scale)
{
    if (c < FONT5X7_FIRST_CHAR || c > FONT5X7_LAST_CHAR) {
        c = ' ';
    }

    const uint8_t *glyph = font5x7[(uint8_t)c - FONT5X7_FIRST_CHAR];

    /* Background for the whole character cell first, same order as the
     * reference bsp_draw_char() — keeps inter-glyph gaps and empty
     * (blank) glyphs painted in the background colour.
     */
    fb_fill_rect(fb, fb_width, x, FONT5X7_WIDTH * scale, y,
                 FONT5X7_HEIGHT * scale, bg_rgb565);

    for (uint32_t col = 0; col < FONT5X7_WIDTH; col++) {
        uint8_t col_bits = glyph[col];

        for (uint32_t row = 0; row < FONT5X7_HEIGHT; row++) {
            if (col_bits & (1U << row)) {
                fb_fill_rect(fb, fb_width, x + col * scale, scale,
                             y + row * scale, scale, fg_rgb565);
            }
        }
    }
}

void lcd_text_draw_string(uint16_t *fb, uint32_t fb_width, uint32_t x,
                           uint32_t y, const char *str, uint16_t fg_rgb565,
                           uint16_t bg_rgb565, uint32_t scale)
{
    uint32_t cursor_x = x;

    while (*str) {
        lcd_text_draw_char(fb, fb_width, cursor_x, y, *str, fg_rgb565,
                            bg_rgb565, scale);
        cursor_x += (FONT5X7_WIDTH + 1) * scale;
        str++;
    }
}
