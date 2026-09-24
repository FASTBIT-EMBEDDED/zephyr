/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_APP_LCD_TEXT_H_
#define FASTBIT_APP_LCD_TEXT_H_

#include <stdint.h>

/*
 * Bare-metal 5x7 bitmap-font text renderer for a linear RGB565
 * framebuffer, ported from the proven CubeMX/HAL reference project
 * (Embedded Graphics LVGL MCU3, 001Vibgyor_Bars, bsp_text.c) that
 * displays text correctly on this exact board without LVGL.
 *
 * This writes glyphs directly into the same framebuffer coordinate
 * system used by the verified-working direct-SDRAM colour fill —
 * there is no separate render buffer, no flush step, and therefore
 * no path for the LVGL VDB-vs-CFBAR transposition bug that caused
 * the 90-degree text rotation. See lcd_hello.c for that background.
 *
 * Deliberately takes the framebuffer pointer and width as parameters
 * (no hidden global/hardware access) so this stays portable and
 * host-testable per the app-layer rule in CLAUDE.md.
 */

/* Draws one character cell (background fill + glyph) at (x, y). */
void lcd_text_draw_char(uint16_t *fb, uint32_t fb_width, uint32_t x,
                         uint32_t y, char c, uint16_t fg_rgb565,
                         uint16_t bg_rgb565, uint32_t scale);

/* Draws a NUL-terminated string starting at (x, y), advancing left to
 * right. Characters are upper-cased before glyph lookup (the font only
 * defines uppercase); anything outside the font's range renders blank.
 */
void lcd_text_draw_string(uint16_t *fb, uint32_t fb_width, uint32_t x,
                           uint32_t y, const char *str, uint16_t fg_rgb565,
                           uint16_t bg_rgb565, uint32_t scale);

#endif /* FASTBIT_APP_LCD_TEXT_H_ */
