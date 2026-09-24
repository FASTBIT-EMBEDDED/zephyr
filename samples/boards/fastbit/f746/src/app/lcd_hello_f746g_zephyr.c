/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#include "lcd_hello.h"
#include "lcd_text.h"

#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_LVGL)
#include <lvgl.h> /* Only needed for the active LVGL path below. Header
                    * kept out of the build when CONFIG_LVGL=n (the
                    * commented-out raw-pixel path further down), same
                    * guard pattern already used in the F429 file. */
#endif

LOG_MODULE_REGISTER(lcd_hello_f746g, CONFIG_APP_LOG_LEVEL);

/*
 * STM32F746G-DISCO: full 480x272 resolution, SDRAM framebuffer.
 *
 * Panel is RK043FN48H(-CT), native 480x272, RGB565 (confirmed against
 * UM1907 and the real v3.7.0 Zephyr board DTS). Two internal-SRAM
 * approaches were tried and ruled out (see app/boards/
 * stm32f746g_disco.overlay for the full story: full resolution
 * overflowed even after aggressive stack trimming; a shrunk 100x50
 * window built/flashed but desynced the panel's physical scan pattern
 * on real hardware). SDRAM is required for the framebuffer here.
 */
#define LCD_W     480
#define LCD_H     272
#define COL_GREEN 0x07E0u
#define COL_BLACK 0x0000u

/*
 * --- ACTIVE PATH: LVGL, drawing through the official display API ---
 *
 * Unlike the raw-pixel version below (kept for reference, not deleted),
 * this path never touches the framebuffer address directly -- LVGL
 * renders into its OWN buffer (routed to SDRAM1 via
 * app/linker/lvgl_buf_f746g.ld, see that file and
 * ci/prj_lcd_test_f746g.conf's CONFIG_LV_Z_VBD_CUSTOM_SECTION), then
 * hands it to Zephyr's official display_write() to copy into the real
 * LTDC framebuffer.
 *
 * No manual cache flush needed here (unlike the raw-pixel path): checked
 * Zephyr's own display_stm32_ltdc.c write() implementation directly --
 * it already calls sys_cache_data_flush_range() itself, per row, as part
 * of the official copy. The earlier fix was only needed because the
 * raw-pixel path bypassed that official path entirely.
 */
void lcd_hello_run(void)
{
    const struct device *display_dev =
        DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display_dev)) {
        LOG_ERR("display not ready");
        return;
    }

    /* Black background this time (matches the LTDC underrun-masking
     * colour already set in the board overlay -- consistent look, and
     * an underrun would be invisible against it either way), white
     * icons for contrast.
     */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    /* LV_SYMBOL_WIFI / LV_SYMBOL_DOWNLOAD are built-in LVGL icon glyphs
     * (src/font/lv_symbol_def.h), included in the already-enabled
     * Montserrat 14 font -- no image files or extra Kconfig needed.
     * Proves icons/symbols work at all before Stage 4 wires up the real
     * OTA status content (real wifi state, real download progress) on
     * top of this same mechanism.
     */
    lv_obj_t *wifi_icon = lv_label_create(lv_scr_act());
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xFFFFFF),
                                 LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_14,
                                LV_PART_MAIN);
    lv_obj_align(wifi_icon, LV_ALIGN_CENTER, 0, -20);

    /* Progress bar: rectangle outline (LV_PART_MAIN) + green fill
     * (LV_PART_INDICATOR) that grows with the value, standard LVGL
     * built-in widget (CONFIG_LV_USE_BAR=y, already on by default --
     * confirmed in the generated .config, no Kconfig change needed).
     * Replaces the plain LV_SYMBOL_DOWNLOAD glyph from the earlier icon
     * test -- proves an actual progress indicator, not just a static
     * icon, before Stage 4 wires this to real OTA download percentage.
     */
    lv_obj_t *bar = lv_bar_create(lv_scr_act());
    lv_obj_set_size(bar, 200, 20);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 20);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    /* LV_ANIM_OFF, not ON: LV_ANIM_ON starts a smooth animated fill that
     * only progresses while lv_task_handler() keeps being called over
     * time. Our own loop below already provides the visible "progressive
     * fill" effect (discrete steps + pauses), so LV_ANIM_ON's animation
     * was redundant AND buggy here -- each step's animation got
     * interrupted by the next lv_bar_set_value() call before finishing,
     * and the FINAL step's animation was never given another
     * lv_task_handler() call after the loop ended, leaving the bar
     * visibly short of full on real hardware (confirmed by photo,
     * 2026-09-05). LV_ANIM_OFF jumps directly to the exact value every
     * time -- no partial-animation state possible.
     */
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    /* Percentage label, placed to the right of the bar, text updated in
     * lockstep with the bar's value in the loop below.
     */
    lv_obj_t *percent_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_color(percent_label, lv_color_hex(0xFFFFFF),
                                 LV_PART_MAIN);
    lv_obj_set_style_text_font(percent_label, &lv_font_montserrat_14,
                                LV_PART_MAIN);
    lv_label_set_text(percent_label, "0%");
    lv_obj_align_to(percent_label, bar, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    /* First render at 0%, then turn the screen on -- same "draw
     * everything before enabling the display" principle as the raw-pixel
     * path, just via LVGL's own render+flush instead of a manual fill
     * loop.
     */
    lv_task_handler();
    display_blanking_off(display_dev);

    LOG_INF("F746G LCD progress bar test: starting 0-100%% in 10%% steps");

    /* Steps through 0/10/20/.../100 with a visible pause between each,
     * so the fill is actually seen progressing on real hardware, not
     * jumping straight to full. Stage 4 replaces this fake timed loop
     * with the real OTA download percentage callback -- this proves
     * only the bar mechanism itself works.
     */
    for (int percent = 0; percent <= 100; percent += 10) {
        lv_bar_set_value(bar, percent, LV_ANIM_OFF);
        lv_label_set_text_fmt(percent_label, "%d%%", percent);
        lv_obj_align_to(percent_label, bar, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
        lv_task_handler();
        k_msleep(800);
    }

    LOG_INF("F746G LCD progress bar test: reached 100%%");

    while (1) {
        k_msleep(1000);
    }
}

#if 0
/*
 * --- LVGL "LCD OK" TEXT PATH: superseded, kept for reference ---
 *
 * Proven working on real hardware, 2026-09-05 (Stage 3 milestone -- solid
 * text, no cache-flush needed since LVGL goes through the official
 * display_write() path). Uncomment this and comment out the icon version
 * above to restore this exact milestone if ever needed (e.g. to isolate
 * whether a future problem is icon/symbol-specific vs. general LVGL text
 * rendering).
 */
void lcd_hello_run(void)
{
    const struct device *display_dev =
        DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display_dev)) {
        LOG_ERR("display not ready");
        return;
    }

    /* lv_color_hex() takes 24-bit RGB888; LVGL converts to the display's
     * native RGB565 internally (0x00FF00 pure green -> 0x07E0), same
     * conversion already proven correct in the F429 file's own
     * (superseded) LVGL attempt.
     */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x00FF00),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "LCD OK");
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_task_handler();

    display_blanking_off(display_dev);

    LOG_INF("F746G LCD Stage 3 (LVGL) bring-up: background + label drawn "
            "via LVGL, blanking off");

    while (1) {
        k_msleep(1000);
    }
}
#endif

#if 0
/*
 * --- RAW-PIXEL PATH: superseded, kept for reference (not deleted) ---
 *
 * Proven working on real hardware, 2026-09-04/05 (including the
 * Cortex-M7 cache-flush fix documented below). Uncomment this block and
 * comment out the LVGL version above, plus flip CONFIG_LVGL back to n
 * in ci/prj_lcd_test_f746g.conf (already commented there, not deleted),
 * to restore this known-good baseline -- e.g. to isolate whether a
 * future problem is LVGL-specific by reverting to a version already
 * confirmed correct without LVGL involved at all.
 *
 * Framebuffer address: hardcoded, MUST be re-verified against
 * zephyr.map for every rebuild of THIS specific config, before flashing.
 *
 * Same situation as the F429 Check 6 file (lcd_hello_zephyr.c) and for
 * the same reason: Zephyr 3.7's STM32 LTDC driver
 * (drivers/display/display_stm32_ltdc.c) does not implement
 * `.get_framebuffer` in its DEVICE_API table, so display_get_framebuffer()
 * returns NULL unconditionally on this Zephyr version. The driver's
 * internal frame_buffer_0 array is `static` in that .c file -- no
 * `extern` linkage path exists from application code.
 *
 * With SDRAM (ext-sdram = &sdram1 in the board overlay), the driver
 * places the buffer in a dedicated `.stm32_sdram1` linker section
 * instead of plain .bss, so `grep frame_buffer_0` finds nothing -- search
 * for the section name instead:
 *   grep -A2 stm32_sdram1 build_lcd_test_f746g/zephyr/zephyr.map
 *
 * Confirmed for that build: 0xc0000000 (start of SDRAM1), size 0x3fc00
 * (261120 bytes = 480*272*2). Re-verify after every rebuild that changes
 * any global/BSS layout before trusting this value again.
 */
#define LCD_FB_ADDR 0xc0000000UL

/*
 * Why write directly into the framebuffer instead of using display_write
 * row by row: same FMC-bus-style race condition documented in the F429
 * Check 6 file applies in spirit here too (display_blanking_on does not
 * stop LTDC's continuous scanning). Writing the entire framebuffer in one
 * tight loop before calling display_blanking_off guarantees LTDC's first
 * unmasked scan sees a complete, consistent frame.
 *
 * Why an explicit cache flush is required here, unlike the F429 file:
 * F429's Cortex-M4 has no data cache at all, so a raw pointer write is
 * immediately visible to LTDC's DMA reads. F746G's Cortex-M7 DOES have a
 * data cache (CONFIG_DCACHE=y here, confirmed in the generated .config),
 * and Zephyr's own STM32_LTDC Kconfig already auto-selects
 * CONFIG_CACHE_MANAGEMENT specifically because of this -- but that only
 * provides the flush API, it does not call it automatically for raw
 * application writes to the framebuffer (only Zephyr's own
 * display_write() driver path does that). Without an explicit flush,
 * written pixel data can sit in the CPU's data cache without having
 * reached physical SDRAM yet, so LTDC (a separate bus master) reads
 * stale/incomplete data. Confirmed on real hardware: a solid-colour fill
 * looked fine (every cache line holds the same value regardless of flush
 * timing), but text -- alternating foreground/background within the same
 * cache lines -- showed visible dashed gaps in every glyph. A dedicated
 * SDRAM isolation test (bypassing LTDC entirely) proved the SDRAM chip
 * itself is healthy (0/130560 words wrong, 0/17 address bits aliasing),
 * ruling out a hardware fault and pointing squarely at this cache-
 * coherency gap instead.
 */
void lcd_hello_run(void)
{
    const struct device *display_dev =
        DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display_dev)) {
        LOG_ERR("display not ready");
        return;
    }

    volatile uint16_t * const fb = (volatile uint16_t *)LCD_FB_ADDR;

    LOG_INF("filling %dx%d framebuffer with green (0x%04X)", LCD_W, LCD_H,
            COL_GREEN);

    for (int i = 0; i < LCD_W * LCD_H; i++) {
        fb[i] = COL_GREEN;
    }

    /* Centered by hand for a 4x-scaled 5x7 font: "LCD OK" is 6 cells
     * wide (6 * (5 + 1) * 4 = 144 px) and 1 cell tall (7 * 4 = 28 px)
     * on a 480x272 panel.
     */
    lcd_text_draw_string((uint16_t *)fb, LCD_W, (LCD_W - 144) / 2,
                          (LCD_H - 28) / 2, "LCD OK", COL_BLACK, COL_GREEN,
                          4);

    sys_cache_data_flush_range((void *)fb, LCD_W * LCD_H * sizeof(uint16_t));

    display_blanking_off(display_dev);

    LOG_INF("F746G LCD Stage 1 (full resolution) bring-up: fill + text "
            "drawn, blanking off");

    while (1) {
        k_msleep(1000);
    }
}
#endif
