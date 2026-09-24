/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

/*
 * Zephyr wiring for the OTA status display, STM32F746G-DISCO variant.
 *
 * Unlike the F429 wiring (lcd_status_wiring_f429_zephyr.c), this file does
 * NOT use the shared bare-metal lcd_status.c/lcd_icon.c modules. It
 * builds the whole status screen directly out of LVGL widgets instead.
 *
 * Why: the WiFi icon needed a real, clean WiFi glyph (dot + arcs) with
 * a proper outline-vs-filled look, matching a reference image the user
 * provided. LVGL ships a built-in WiFi symbol (LV_SYMBOL_WIFI, in
 * src/font/lv_symbol_def.h, part of the Montserrat font's bundled
 * FontAwesome-derived symbol set -- no extra Kconfig or image asset
 * needed). But LVGL owns the whole screen's render/refresh cycle; it
 * cannot cleanly coexist on the same framebuffer with code that also
 * pokes raw pixels outside LVGL's own widget tree (LVGL's redraws do
 * not know those regions exist and can overwrite them). So using LVGL
 * for the icon meant using LVGL for the entire screen here, not just
 * the icon -- confirmed as the right tradeoff with the user before
 * doing this rewrite.
 *
 * This mirrors the exact LV_SYMBOL_WIFI + lv_bar pattern already
 * proven on real F746G hardware in lcd_hello_f746g_zephyr.c's Stage 3/4
 * bring-up experiments (see that file for the original confirmation).
 *
 * F429 is unaffected: it keeps using lcd_status.c/lcd_icon.c and
 * CONFIG_LVGL=n, unchanged.
 *
 * No manual cache flush needed here (unlike this board's earlier
 * bare-metal version of this file): LVGL renders through Zephyr's
 * official display_write() path, which already flushes the CPU data
 * cache itself -- confirmed by reading display_stm32_ltdc.c directly,
 * same finding already documented in lcd_hello_f746g_zephyr.c.
 *
 * Three public symbols, same interface as the F429 wiring (see
 * lcd_status_wiring.h) so main.c and ota_manager_zephyr.c stay
 * board-agnostic:
 *   lcd_status_wiring_init()
 *   lcd_status_on_ota_state_zephyr()
 *   lcd_status_on_progress_zephyr()
 */

#include "app/lcd_icon_thermometer.h"
#include "app/lcd_logo.h"
#include "app/lcd_status_wiring.h"
#include "drivers/wifi_link.h"
#include "middleware/ota_manager.h"
#include "middleware/ota_manager_zephyr.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(lcd_status_wiring_f746g, CONFIG_APP_LOG_LEVEL);

/* ============================================================================
 * LVGL ticker (2026-09-16, touch/animation foundation)
 *
 * Previously, lv_task_handler() was only called from inside the OTA
 * state/progress callbacks -- meaning LVGL was only ever "pumped" at the
 * moments an OTA event happened to fire. That is not enough for touch
 * input (which needs to be polled continuously to feel responsive) or
 * for animations (which need repeated lv_task_handler() calls over time
 * to advance). This dedicated thread ticks LVGL every 30ms regardless of
 * what the OTA state machine is doing.
 *
 * g_lvgl_lock protects every access to LVGL's object tree: the ticker
 * thread and the OTA worker thread (which calls the callbacks below)
 * are now two different threads both touching LVGL, and LVGL itself is
 * not thread-safe without external locking.
 *
 * Started manually from the end of lcd_status_wiring_init(), not via
 * K_THREAD_DEFINE's auto-start-at-boot -- this guarantees the ticker
 * never runs concurrently with the screen still being built during
 * init(), so init() itself does not need to take the lock.
 * ========================================================================== */

K_MUTEX_DEFINE(g_lvgl_lock);
/* 2048 -> 8192 (2026-09-17, MP4 crash fix): confirmed on real hardware as
 * a stack overflow ("Stack overflow on CPU 0", MPU fault) the instant
 * this thread's first tick ran on v2 -- 2048 bytes had been adequate for
 * labels/bars/buttons/spinner since M0, but lv_chart (MP4's temperature
 * history graph) is LVGL's most complex built-in widget: line-segment
 * drawing, anti-aliasing, and LV_DRAW_COMPLEX invalidation/redraw logic,
 * all running on this same thread every time update_sensor_reading()
 * calls lv_chart_set_next_value(). 8192 gives real headroom rather than
 * just enough to limp past this one crash. */
static K_THREAD_STACK_DEFINE(lvgl_ticker_stack, 8192);
static struct k_thread lvgl_ticker_thread_data;

/* Per-version "what's running" animation (2026-09-17). v1 shows orbiting
 * dots around a center icon (idea "4" from the animation-ideas preview,
 * app/src/app/lcd_hello_f746g_zephyr.c in the ota_lcd_test sandbox); v2
 * shows LVGL's built-in spinner (idea "1"). Deliberately different so a
 * customer/reviewer can tell which firmware is running at a glance, not
 * just by reading the version cell's text. Decided at runtime from
 * CONFIG_APP_OTA_DEVICE_VERSION's first character ('1' vs anything else)
 * -- see s_is_v1 below, set once in lcd_status_wiring_init().
 *
 * The spinner animates itself (LVGL widget, no per-frame code needed);
 * the orbiting dots do not, so update_orbit_anim() is called from this
 * same ticker every tick, same manual-math style already proven in the
 * sandbox preview. s_anim_phase accumulates by a fixed step per tick
 * (not from absolute uptime) so the animation speed stays correct
 * regardless of how long the board has been running -- same reasoning
 * as the sandbox wave demo's own phase accumulator. */
/* Full-width single-column "Device Monitor" layout (2026-09-17, MP1) --
 * SUPERSEDES the earlier narrow inset-column "fake portrait" attempt
 * (black bars either side): confirmed on real hardware that wasn't what
 * was wanted -- the whole 480px width should be used, just organised as
 * one stacked column (portrait STYLE, not portrait SHAPE). CONTENT_W is
 * the shared content width with a 10px margin either side, matching the
 * original landscape design's own margin.
 *
 * ANIM_CY intentionally overlaps the START OTA button's row: the v1
 * orbiting-dot animation is only ever visible during active OTA (button
 * hidden then), and the button is only ever visible at idle (animation
 * hidden then) -- they never appear at the same time, so sharing one
 * screen region costs nothing and is the only way everything fits
 * (title + 3 boxes + 2 status lines + button leaves very little spare
 * vertical space in 272px). */
#define CONTENT_X 10
#define CONTENT_W 460

/* v1 layout Y-positions (2026-09-18, follow-up) -- two sets: the
 * normal one (idle / update-available, CHECK or DOWNLOAD button
 * visible) and the "downloading" one (button row empty, progress bar
 * showing) which spreads the bar/box/uptime out to fill that empty
 * space instead of leaving it blank. Confirmed with the user via an
 * ASCII layout mockup before implementing. */
#define V1_BAR_Y_NORMAL 90
#define V1_SENSOR_Y_NORMAL 110
#define V1_UPTIME_Y_NORMAL 192
/* Downloading-state gaps widened (2026-09-18, second follow-up) --
 * confirmed via a hand-drawn sketch that the bar needs real breathing
 * room above AND below it, not just a shift down: ~24px clear above
 * (TXP/FW/OTA row ends at y=80, bar starts at 104) and ~24px clear
 * below (bar ends at 118, box starts at 142), rather than the earlier
 * 6px gap that had the bar sitting tight against the box. */
#define V1_BAR_Y_DOWNLOADING 104
#define V1_SENSOR_Y_DOWNLOADING 142
#define V1_UPTIME_Y_DOWNLOADING 236

#define ANIM_CX 240
#define ANIM_CY 240

static bool s_is_v1;
/* Even/odd major-version graph swap (2026-09-18, generalised from a
 * v3-only check per explicit request: "for every even version ...
 * temp graph, every odd version ... hum graph"). Every non-v1 Device
 * Monitor screen is otherwise identical (same left-column layout,
 * buttons, bar) -- only the history graph's data source, Y range,
 * title, and colour change: temperature for even major versions
 * (v2, v4, v6, ...), humidity for odd ones (v3, v5, v7, ...). Decided
 * once in lcd_status_wiring_init() via device_version_major_is_odd(). */
static bool s_show_humidity_graph;
static lv_obj_t *s_anim_icon;
static lv_obj_t *s_anim_dots[3];
static float s_anim_phase;

/* v1's orbiting-dot animation is gated to the OTA activity window
 * (2026-09-17): hidden while genuinely idle, shown from the moment the
 * user touches START (state leaves OTA_STATE_IDLE) through checking,
 * downloading, installing, all the way to reboot -- see
 * lcd_status_on_ota_state_zephyr() below, which flips this and resets
 * the phase for a clean restart each time it (re)appears. v2's spinner
 * is NOT gated this way -- it is the "this is v2, already updated,
 * running normally" indicator and stays on continuously once created,
 * same as the original always-on design. */
static bool s_anim_active;

static void update_orbit_anim(void)
{
    s_anim_phase += 0.06f;

    for (int i = 0; i < 3; i++) {
        float angle = s_anim_phase + i * (2.0f * 3.14159265f / 3.0f);
        int x = ANIM_CX + (int)(70.0f * cosf(angle)) - 6;
        int y = ANIM_CY + (int)(28.0f * sinf(angle)) - 6;
        lv_obj_set_pos(s_anim_dots[i], x, y);
    }
}

/* ============================================================================
 * DHT11 temperature+humidity (2026-09-17, Device Monitor OTA demo
 * milestone). Verified working standalone in the ota_lcd_test sandbox
 * (CONFIG_APP_DHT_TEST) before being wired in here.
 *
 * Polled every 2 seconds from this same ticker thread, not every tick --
 * the DHT11 datasheet specifies a minimum ~1s interval between reads, and
 * polling it at the ticker's 30ms rate would both violate that and be
 * pointless (the sensor itself has nowhere near that update rate). Timed
 * with k_uptime_get() against s_last_sensor_read_ms rather than a tick
 * counter, so the interval stays correct regardless of ticker jitter.
 * ========================================================================== */

#define SENSOR_POLL_INTERVAL_MS 2000

/* Temperature history graph window size (2026-09-17, MP4) -- 10 points
 * at 2s each is a 20-second-wide visible window, per explicit request
 * (shrunk from an earlier 30-point/60-second window: "in right corner
 * ... I need 20 ... total 5 time units from starting 0,5,10,15,20").
 * One named constant instead of the literal 30 repeated in several
 * places (point count, fill-vs-scroll threshold, NONE-initialisation
 * loop bound) so the window size only ever needs changing in one spot. */
#define CHART_POINT_COUNT 10

static const struct device *s_dht;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_humidity_label;
/* Shared between v1 and v2+ (only one branch's widget tree is ever
 * built, same pattern as s_start_btn below) -- reflects the DHT11's
 * actual last read result instead of a static "RUNNING" that never
 * changed regardless of real sensor state. */
static lv_obj_t *s_dht_status_label;
static lv_obj_t *s_uptime_label;
static lv_obj_t *s_temp_chart;
static lv_chart_series_t *s_temp_chart_series;
/* v2 only (2026-09-18, follow-up) -- hidden/shown together (as two
 * separate objects, NOT a wrapper container -- see
 * lcd_status_wiring_init()'s v2 branch for why) while a download is in
 * progress: the graph and the progress bar occupy the same screen
 * area, so only one can be visible at a time. */
static lv_obj_t *s_graph_title;
/* v1 only (2026-09-18, follow-up) -- repositioned while downloading so
 * the progress bar, sensor box, and UPTIME spread out and fill the
 * space the (now-hidden) CHECK/DOWNLOAD button row leaves empty,
 * instead of leaving a big empty gap at the bottom -- see the
 * V1_*_Y_DOWNLOADING constants and lcd_status_on_ota_state_zephyr()
 * below. */
static lv_obj_t *s_sensor_cell;
/* Fill-then-scroll counter (2026-09-17) -- see update_sensor_reading()'s
 * own comment below for the full reasoning. Counts every successful
 * sensor read since boot, uncapped (not reset back to 0 once the chart
 * is full) -- both the fill/scroll decision and the X-axis label
 * callback (chart_x_tick_label_event_cb) need to know the true total,
 * not just "how many of the visible 30 slots are used". */
static uint32_t s_chart_total_samples;
static int64_t s_last_sensor_read_ms;
static int64_t s_last_uptime_update_ms;
static int64_t s_last_dl_dots_update_ms;
/* v2 only for now (2026-09-18, follow-up) -- "DOWNLOADING NEW
 * FIRMWARE" text with an animated dot count, shown alongside the
 * progress bar while a download is actually happening (a spinner was
 * tried alongside this and removed per explicit request). NULL on v1
 * (not added there yet -- v1's screen has much less free vertical
 * space around its bar). Declared here (not with the other widget
 * handles further below) since update_download_dots_label() and the
 * ticker both need it before that point in the file. */
static lv_obj_t *s_dl_label;

/* Parses the leading integer of CONFIG_APP_OTA_DEVICE_VERSION (the
 * major version, e.g. "4" out of "4.0.0") and reports whether it is
 * odd. Handles multi-digit majors (v10, v11, ...) correctly, unlike a
 * single-character check against '1'/'3' -- needed once this stopped
 * being a v2-vs-v3-only distinction (2026-09-18, follow-up: "for every
 * even version ... temp graph, every odd version ... hum graph"). */
static bool device_version_major_is_odd(void)
{
    const char *v = CONFIG_APP_OTA_DEVICE_VERSION;
    int major = 0;

    while (*v >= '0' && *v <= '9') {
        major = major * 10 + (*v - '0');
        v++;
    }

    return (major % 2) != 0;
}

static void update_sensor_reading(void)
{
    int rc = sensor_sample_fetch(s_dht);

    if (rc != 0) {
        LOG_WRN("DHT11 sensor_sample_fetch rc=%d", rc);
        lv_label_set_text(s_dht_status_label,
                           s_is_v1 ? "DHT STATUS: #ff3333 NO CONN#" : "DHT: #ff3333 NO CONN#");
        /* No reading while disconnected -- a stale last-good value left
         * on screen would look like a live reading. Graph history is
         * untouched here on purpose: it's a record of past readings,
         * not a live gauge, and appending resumes normally once the
         * sensor reconnects (see the success path below). */
        lv_label_set_text(s_temp_label, s_is_v1 ? "TEMP: --.-- C" : "T = --.-- C");
        lv_label_set_text(s_humidity_label, s_is_v1 ? "HUM: --.-- %" : "H = --.-- %");
        return;
    }

    struct sensor_value temperature;
    struct sensor_value humidity;

    rc = sensor_channel_get(s_dht, SENSOR_CHAN_AMBIENT_TEMP, &temperature);
    if (rc == 0) {
        rc = sensor_channel_get(s_dht, SENSOR_CHAN_HUMIDITY, &humidity);
    }
    if (rc != 0) {
        LOG_WRN("DHT11 sensor_channel_get rc=%d", rc);
        lv_label_set_text(s_dht_status_label,
                           s_is_v1 ? "DHT STATUS: #ff3333 NO CONN#" : "DHT: #ff3333 NO CONN#");
        lv_label_set_text(s_temp_label, s_is_v1 ? "TEMP: --.-- C" : "T = --.-- C");
        lv_label_set_text(s_humidity_label, s_is_v1 ? "HUM: --.-- %" : "H = --.-- %");
        return;
    }

    lv_label_set_text(s_dht_status_label, s_is_v1 ? "DHT STATUS: RUNNING" : "DHT: RUNNING");

    lv_label_set_text_fmt(s_temp_label, "TEMP: %d.%02d C",
                           temperature.val1, temperature.val2 / 10000);
    lv_label_set_text_fmt(s_humidity_label, "HUM: %d.%02d %%",
                           humidity.val1, humidity.val2 / 10000);

    /* Temperature history graph (2026-09-17, MP4), v2 only -- one point
     * per successful read, whole degrees (temperature.val1) since the
     * DHT11's real resolution is about 1 C anyway and lv_chart values
     * are integers.
     *
     * Fill-then-scroll (2026-09-17, third and correct attempt, per
     * direct user clarification after the previous two were wrong on
     * real hardware):
     *   - First CHART_POINT_COUNT readings (s_chart_total_samples
     *     0..CHART_POINT_COUNT-1): written by explicit index via
     *     lv_chart_set_value_by_id(), filling left-to-right starting at
     *     index 0 (the chart's leftmost x-position) -- no shifting yet,
     *     so the very first reading genuinely appears at the left edge,
     *     not the right.
     *   - From reading CHART_POINT_COUNT+1 on: lv_chart_set_next_value() (LVGL's
     *     native shift mode) takes over -- this is confirmed correct
     *     for ongoing behaviour: newest enters at the RIGHT, existing
     *     history shifts LEFT, oldest ages off the left edge. Since
     *     internal series storage was never touched by set_next_value
     *     during the fill phase (its start_point stays at 0 throughout,
     *     only advanced by set_next_value itself), the very first
     *     set_next_value call correctly overwrites index 0 -- the
     *     oldest reading -- continuing the aging pattern with no jump
     *     at the transition. */
    if (!s_is_v1) {
        /* Odd major versions feed humidity into the chart instead of
         * temperature -- see the chart's own creation comment for the
         * matching Y-range/colour change. */
        int32_t chart_value = s_show_humidity_graph ? humidity.val1 : temperature.val1;

        if (s_chart_total_samples < CHART_POINT_COUNT) {
            lv_chart_set_value_by_id(s_temp_chart, s_temp_chart_series,
                                      (uint16_t)s_chart_total_samples, chart_value);
        } else {
            lv_chart_set_next_value(s_temp_chart, s_temp_chart_series, chart_value);
        }
        s_chart_total_samples++;
    }
}

/* Custom X-axis tick label text (2026-09-17, per explicit user
 * clarification): LVGL's default label for each X-axis tick is a fixed
 * point-index number (e.g. always "0 1 2 3 4" left to right) that never
 * changes even as the chart scrolls -- explicitly not what was wanted
 * ("0 should become 1, 1 -> 2 ... as time keeps increasing"). This
 * overrides that text with the real elapsed time (in seconds) that
 * tick's screen position actually represents right now, computed from
 * s_chart_total_samples (how many readings have been taken since boot)
 * and SENSOR_POLL_INTERVAL_MS (how many seconds apart each one is).
 *
 * LVGL calls this once per major tick during the chart's own draw pass,
 * with dsc->value already set to that tick's index among the 13 major
 * ticks (0..12, since major_cnt=13, minor_cnt=1 in the set_axis_tick
 * call -- a 60-second window labelled every 5 seconds needs 13 labels)
 * and dsc->text pointing at a small stack buffer LVGL is about to
 * render -- overwriting its contents here is the documented way to
 * customise a chart's tick labels (LV_EVENT_DRAW_PART_BEGIN). */
static void chart_x_tick_label_event_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);

    if (dsc->part != LV_PART_TICKS || dsc->id != LV_CHART_AXIS_PRIMARY_X || dsc->text == NULL) {
        return;
    }

    /* Computed directly from the tick's own index (2026-09-17, per
     * explicit request for a clean 0,5,10,15...60 sequence) -- an
     * earlier version derived this from an approximate point-window
     * position instead, which only ever lands on clean multiples of 5
     * by chance (29 points does not divide evenly by the tick count),
     * so it was rounded afterwards. That rounding gave clean-looking
     * individual numbers but not a clean fixed step between them once
     * the tick count changed. Since the tick MARKS' actual screen
     * positions are already spaced evenly by LVGL's own generic math
     * regardless of what text we show, there is no need to derive the
     * text from a point position at all: dsc->value (0..12, since
     * major_cnt=13) times the fixed 5-second step, offset by the
     * left edge's own real elapsed time (rounded to the nearest 5 for
     * the same "clean number" reason), is both simpler and exact. */
    uint32_t leftmost_sample = (s_chart_total_samples > CHART_POINT_COUNT) ?
                                (s_chart_total_samples - CHART_POINT_COUNT) : 0U;
    uint32_t leftmost_seconds = leftmost_sample * (SENSOR_POLL_INTERVAL_MS / 1000U);
    uint32_t leftmost_rounded = ((leftmost_seconds + 2U) / 5U) * 5U;
    uint32_t seconds = leftmost_rounded + (uint32_t)dsc->value * 5U;

    /* dsc->text_length is LVGL's own buffer size for this text (a
     * private 16-byte constant inside lv_chart.c, not exported as a
     * macro) -- using the field it already gives us instead of
     * hardcoding that number. */
    lv_snprintf(dsc->text, dsc->text_length, "%u", (unsigned)seconds);
}

/* Uptime counter (2026-09-17, MP2) -- HH:MM:SS since this boot,
 * k_uptime_get()-based like everything else time-driven in this file.
 * Updated once a second, not every ticker pass -- a clock only needs to
 * visibly move once a second, and recomputing/re-rendering the label 33x
 * more often than that would be pure waste. Resets to 00:00:00 on every
 * reboot, including a post-OTA one -- that reset is part of the point,
 * it is what proves the board actually rebooted into new firmware
 * rather than just changing a version string. */
#define UPTIME_UPDATE_INTERVAL_MS 1000

static void update_uptime_label(void)
{
    uint32_t total_seconds = (uint32_t)(k_uptime_get() / 1000);
    uint32_t hours = total_seconds / 3600U;
    uint32_t minutes = (total_seconds % 3600U) / 60U;
    uint32_t seconds = total_seconds % 60U;

    lv_label_set_text_fmt(s_uptime_label, "UPTIME: %02u:%02u:%02u", hours, minutes, seconds);
}

/* Animated dot count for s_dl_label (2026-09-18, follow-up -- replaces
 * a spinner that was tried and removed per explicit request). Cycles
 * "DOWNLOADING NEW FIRMWARE" -> "." -> ".." -> "..." -> back to no
 * dots, one step at a time, giving the appearance of dots appearing
 * one by one and then resetting together -- the same visual idea as
 * the requested "1 dot appears, 2nd appears, 3rd appears, all fade,
 * repeat", without needing a real per-character fade animation. Only
 * called while s_dl_label is actually visible (see the ticker below),
 * so this is a no-op cost the rest of the time. */
#define DOWNLOAD_DOTS_UPDATE_INTERVAL_MS 400

static void update_download_dots_label(void)
{
    static uint8_t dot_count;

    /* LV_SYMBOL_BULLET (2026-09-18, follow-up) instead of "." -- a
     * solid-circle glyph from the same Montserrat symbol font already
     * linked in (used elsewhere for LV_SYMBOL_WIFI etc.), so no extra
     * flash cost, but visibly bigger/rounder than a plain period per
     * explicit request ("make the dot size more, it should be
     * visible"). A zoom transform was tried earlier for general text
     * size and broke rendering entirely -- this sidesteps that by
     * picking a bigger-looking glyph at the same font size instead of
     * scaling anything. Space-separated, one " •" pair per dot. */
    static const char *const variants[4] = {
        "",
        " " LV_SYMBOL_BULLET,
        " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET,
        " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET,
    };

    dot_count = (dot_count + 1U) % 4U;

    char buf[48];
    (void)snprintf(buf, sizeof(buf), "DOWNLOADING NEW FIRMWARE%s", variants[dot_count]);
    lv_label_set_text(s_dl_label, buf);
}

static void lvgl_ticker_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        k_mutex_lock(&g_lvgl_lock, K_FOREVER);
        /* Animation temporarily disabled (2026-09-17) -- see the widget
         * creation block's own comment in lcd_status_wiring_init(). */
#if 0
        if (s_is_v1 && s_anim_active) {
            update_orbit_anim();
        }
#endif
        int64_t now = k_uptime_get();
        if (now - s_last_sensor_read_ms >= SENSOR_POLL_INTERVAL_MS) {
            s_last_sensor_read_ms = now;
            update_sensor_reading();
        }
        if (now - s_last_uptime_update_ms >= UPTIME_UPDATE_INTERVAL_MS) {
            s_last_uptime_update_ms = now;
            update_uptime_label();
        }
        if (s_dl_label != NULL && !lv_obj_has_flag(s_dl_label, LV_OBJ_FLAG_HIDDEN) &&
            now - s_last_dl_dots_update_ms >= DOWNLOAD_DOTS_UPDATE_INTERVAL_MS) {
            s_last_dl_dots_update_ms = now;
            update_download_dots_label();
        }
        lv_task_handler();
        k_mutex_unlock(&g_lvgl_lock);
        k_msleep(30);
    }
}

/* ============================================================================
 * Widget handles -- created once in lcd_status_wiring_init(), updated by
 * the callbacks below.
 * ========================================================================== */

static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_ota_label;
static lv_obj_t *s_version_label;
static lv_obj_t *s_bar;
static lv_obj_t *s_pct_label;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_dwd_btn;

/* True 90-degree rotation via LVGL's per-object transform (2026-09-17,
 * MP1 follow-up). The panel itself CANNOT physically rotate (no smart
 * controller chip on this panel, and this Zephyr version's bundled LVGL
 * has zero rotation Kconfig support -- confirmed by grepping both before
 * attempting this), so every real widget is parented under this one
 * container instead of lv_scr_act() directly, then the WHOLE container
 * is rotated + scaled down as a single unit. All existing x/y alignment
 * math elsewhere in this file is untouched -- it was already relative
 * to a 480x272 logical area, which is exactly what this container's own
 * size still is; only its parent-level transform changes how that area
 * finally lands on the physical panel. Rotating 480x272 by 90 degrees
 * produces a 272x480 bounding box, taller than the physical 272px
 * screen height, so it must also shrink by 272/480 (~0.567, zoom=145 in
 * LVGL's 256=100% units) to fit at all -- confirmed with the user this
 * trade-off (smaller content) is acceptable to get genuine vertical
 * reading order. Touch hit-testing under this transform is UNVERIFIED
 * -- LVGL's rotation is primarily a rendering-time visual effect; if
 * the START OTA button becomes hard/impossible to touch accurately
 * after this, that is a separate follow-up (input coordinate
 * remapping), not a sign this rotation itself is wrong. */
static lv_obj_t *s_content;

/* Guards every callback below against firing before the widgets exist.
 *
 * ota_manager_zephyr.c's SYS_INIT(ota_zephyr_sys_init, APPLICATION, 90)
 * hook does not itself connect WiFi or block -- it only spawns a
 * separate "ota_worker" thread and returns immediately. That worker
 * thread is what actually calls wifi_link_init()/wifi_link_connect()
 * and posts the first OTA state, running concurrently with main()
 * (which is what calls lcd_status_wiring_init()). So this is a genuine
 * race between two independent threads, not a fixed ordering -- the
 * OTA worker's first state announcement can reach
 * lcd_status_on_ota_state_zephyr() before this file's widgets exist,
 * confirmed on real hardware as a BUS FAULT at boot (s_ota_label still
 * NULL, lv_label_set_text(NULL, ...) dereferencing invalid memory).
 * The original bare-metal lcd_status.c already guards every one of its
 * public functions against exactly this kind of race
 * (`if (s_fb == NULL) { return; }`); this flag is the equivalent for
 * this LVGL rewrite. The same race is why the WiFi icon/text also
 * check the real current connection state at init time below, instead
 * of always assuming "not yet connected". */
static bool s_ready;

/* ============================================================================
 * Helpers
 * ========================================================================== */

static const char *ota_state_label(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_IDLE:                return "IDLE";
    case OTA_STATE_UPDATE_AVAILABLE:    return "UPDATE AVAILABLE";
    case OTA_STATE_PRECHECK:            return "CHECKING";
    case OTA_STATE_DOWNLOAD:            return "DOWNLOADING";
    case OTA_STATE_VERIFY:              return "VERIFYING";
    case OTA_STATE_INSTALL_TO_2ND_SLOT: return "INSTALLING";
    case OTA_STATE_REBOOT_PENDING:      return "REBOOT PENDING";
    case OTA_STATE_BOOT_NEW_IMAGE:      return "BOOT NEW IMAGE";
    case OTA_STATE_CONFIRM_IMAGE:       return "CONFIRMING";
    case OTA_STATE_SUCCESS:             return "SUCCESS";
    default:                            return "UNKNOWN";
    }
}

static const char *ota_failure_label(ota_result_t result)
{
    switch (result) {
    case OTA_RESULT_ERR_TRANSPORT:    return "FAIL: TRANSPORT";
    case OTA_RESULT_ERR_VERIFY:       return "FAIL: VERIFY";
    case OTA_RESULT_ERR_INSTALL:      return "FAIL: INSTALL";
    case OTA_RESULT_ERR_PRECONDITION: return "FAIL: PRECONDITION";
    case OTA_RESULT_ERR_NO_UPDATE:    return "NO UPDATE";
    case OTA_RESULT_ERR_BOOT:         return "FAIL: BOOT";
    default:                          return "FAILED";
    }
}

/* Visual polish (2026-09-16, event demo): colour-codes the OTA status
 * line so its state is readable at a glance from across a room, not
 * just from the text itself. Red is reserved strictly for a genuine
 * failure -- OTA_RESULT_ERR_NO_UPDATE is deliberately NOT treated as a
 * failure here (checked and found nothing newer is a normal, expected
 * outcome, not an error), which is why it is checked separately from
 * the other OTA_RESULT_ERR_* codes below. */
static lv_color_t ota_state_color(ota_state_t state, ota_result_t result)
{
    if (state == OTA_STATE_IDLE) {
        if (result == OTA_RESULT_ERR_NO_UPDATE) {
            return lv_color_hex(0x00FF00); /* green: checked, nothing newer -- not a failure */
        }
        if (result != OTA_RESULT_OK) {
            return lv_color_hex(0xFF3B30); /* red: genuine failure */
        }
        return lv_color_hex(0xFFFFFF); /* white: freshly idle, no check yet */
    }
    switch (state) {
    case OTA_STATE_UPDATE_AVAILABLE:
    case OTA_STATE_PRECHECK:
        return lv_color_hex(0x3B9CFF); /* blue: checking */
    case OTA_STATE_DOWNLOAD:
    case OTA_STATE_VERIFY:
    case OTA_STATE_INSTALL_TO_2ND_SLOT:
        return lv_color_hex(0x00FF00); /* green: actively working (2026-09-16: was orange) */
    case OTA_STATE_SUCCESS:
        return lv_color_hex(0x00FF00); /* green: done */
    default:
        /* cyan, not white: post-reboot confirm flow (2026-09-16) --
         * previously shared white with idle, making the two
         * indistinguishable at a glance. Every state now has its own
         * colour; red stays reserved for failure only. */
        return lv_color_hex(0x00CFFF);
    }
}

/* Transport in use, known at compile time -- exactly one of these two
 * Kconfig choice members is ever set (app/Kconfig's APP_OTA_TRANSPORT
 * choice), matching how CONFIG_APP_OTA_DEVICE_VERSION is baked in per
 * build. (2026-09-16, M1) */
#if defined(CONFIG_APP_OTA_TRANSPORT_HTTPS)
#define OTA_TRANSPORT_NAME "HTTPS"
#elif defined(CONFIG_APP_OTA_TRANSPORT_MQTT)
#define OTA_TRANSPORT_NAME "MQTT"
#else
#define OTA_TRANSPORT_NAME "UNKNOWN"
#endif

/* Bordered "cell" look (2026-09-16, M1), matching the grid-panel
 * reference the user provided -- a plain outlined box, transparent
 * background (screen stays black), framing one piece of information.
 * Border colour switched from plain grey to the app's blue accent
 * (2026-09-17), matching the glowing blue box borders in the newer
 * reference mockup -- previously a flat, dim grey with no accent at
 * all. Border thickened (1->2px) and a faint blue fill added
 * (2026-09-17, follow-up) -- the plain outline alone still looked too
 * flat next to the reference mockup's glowing blue panels; a low-
 * opacity blue background gives the same light-blue-glow look while
 * keeping the screen's black base visible through it. */
static lv_obj_t *create_cell(int16_t x, int16_t y, int16_t w, int16_t h)
{
    lv_obj_t *cell = lv_obj_create(s_content);
    lv_obj_set_size(cell, w, h);
    lv_obj_align(cell, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x3B9CFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_border_color(cell, lv_color_hex(0x3B9CFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    return cell;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

/* Icon-only WiFi indicator (2026-09-16, M1) -- no "WiFi: CONNECTED" /
 * "Connecting..." text any more, just the glyph colour (green/red),
 * fixed in the top-right corner (set once at creation in
 * lcd_status_wiring_init(); this function only ever changes its
 * colour, never its position, unlike the old text+icon pairing which
 * had to re-anchor the icon every time the text width changed). */
static void set_wifi_display(bool connected)
{
    lv_obj_set_style_text_color(s_wifi_icon,
        connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000),
        LV_PART_MAIN);
}

static void on_wifi_event(bool connected)
{
    if (!s_ready) {
        return;
    }
    k_mutex_lock(&g_lvgl_lock, K_FOREVER);
    set_wifi_display(connected);
    k_mutex_unlock(&g_lvgl_lock);
}

/* "CHECK" button click handler (2026-09-16, M3; relabelled from "START
 * OTA" in MP3 -- see s_start_btn's own comment). Runs on the LVGL
 * ticker thread (that is what calls lv_task_handler(), which is what
 * dispatches LVGL input events) -- ota_manager_trigger_check() is safe
 * to call from any context (see that function's own doc comment), so
 * no extra locking is needed here beyond LVGL's own event dispatch
 * already holding g_lvgl_lock via the ticker. */
static void start_btn_event_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    LOG_INF("CHECK button pressed");
    ota_manager_trigger_check();
}

/* "DWD" button click handler (2026-09-17, MP3) -- confirms the download
 * shown at OTA_STATE_UPDATE_AVAILABLE. See
 * ota_manager_trigger_download()'s own doc comment. */
static void dwd_btn_event_cb(lv_event_t *e)
{
    ARG_UNUSED(e);
    LOG_INF("DWD button pressed");
    ota_manager_trigger_download();
}

void lcd_status_wiring_init(void)
{
    const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(display_dev)) {
        LOG_ERR("display not ready");
        return;
    }

    /* Splash screen (2026-09-16, event demo): turns the display on with
     * the Fastbit logo image as early as possible, instead of the full
     * status screen (which takes longer to build widget-by-widget). The
     * panel shows raw/garbled content briefly during MCUboot and early
     * kernel boot, before this code ever runs -- calling
     * display_blanking_off() here, immediately, rather than only after
     * the entire status screen below is built, gets a clean picture on
     * screen sooner and shortens that noisy window. Purely cosmetic; no
     * OTA behaviour depends on this. Logo asset: lcd_logo.c/.h.
     *
     * Screen background matches the logo's own navy-blue background
     * (2026-09-17, follow-up) -- per explicit request to fill the whole
     * screen with the logo. A real full 480x272 bitmap would cost
     * ~255KB flash (this build has under 64KB headroom left), so
     * instead the screen bg is set to the SAME navy colour the logo
     * artwork already has baked in, and the logo image is zoomed up to
     * fill most of the width -- the two blend into one seamless graphic
     * with no extra flash cost. (Earlier this was white, since the
     * OLD icon-only logo's dark artwork needed a light background to
     * be visible -- not a constraint with this navy-background asset.)
     * Switched to black below, right after the splash is removed, for
     * the rest of the status screen. */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0E184B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    /* fastbit_logo_big_dsc (300x90 source), not the shared
     * fastbit_logo_dsc (180x54, F429's splash) -- per explicit feedback
     * the smaller size looked too small on this board's larger 480x272
     * screen. Zoomed further here (390/256 =~ 1.52x -> ~456x137) to
     * fill most of the screen width, reusing the same embedded bitmap
     * (LVGL's zoom is a render-time scale, no extra flash data). */
    lv_obj_t *splash = lv_img_create(lv_scr_act());
    lv_img_set_src(splash, &fastbit_logo_big_dsc);
    lv_img_set_zoom(splash, 390);
    lv_obj_center(splash);

    lv_task_handler();
    display_blanking_off(display_dev);

    k_msleep(1200);

    lv_obj_del(splash);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    /* Content container (2026-09-17, MP1) -- true 90-degree rotation was
     * attempted here and reverted: it requires LVGL to render the whole
     * 480x272 subtree into an offscreen buffer before rotating it, and
     * that buffer (~261KB minimum) does not come close to fitting in
     * LVGL's 16KB memory pool -- confirmed on real hardware as a
     * completely blank screen, no error logged (LVGL logging is not
     * even enabled in this build, so the allocation failure was
     * silent). No longer transformed; kept as a plain container (rather
     * than folding everything back onto lv_scr_act() directly) so the
     * structure stays ready if a real fix (much larger LVGL heap,
     * routed to SDRAM) is ever attempted later. Vertical reading order
     * -- title, then each box, top to bottom -- comes from the widgets'
     * own stacked y-coordinates below, not from any transform. */
    s_content = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_content, 480, 272);
    lv_obj_align(s_content, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_content, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* Decided here (2026-09-17, MP4) -- v1 and v2 now use two genuinely
     * different layouts below, not just shifted positions of the same
     * widgets: v1 keeps the original title/boxes/buttons screen; v2 is
     * a two-column layout (every metric stacked in a left column, the
     * temperature history graph filling the right side), confirmed with
     * the user on real hardware after the graph looked too cramped
     * squeezed between v1-style boxes. */
    s_is_v1 = (CONFIG_APP_OTA_DEVICE_VERSION[0] == '1');
    s_show_humidity_graph = device_version_major_is_odd();

    s_dht = DEVICE_DT_GET(DT_NODELABEL(dht11));
    if (!device_is_ready(s_dht)) {
        LOG_ERR("DHT11 device not ready");
    }

    if (s_is_v1) {
        /* ---- v1 layout (2026-09-17, reworked per a reference mockup
         * the user provided): small logo top-left, title, WiFi icon
         * top-right (its own single connect/disconnect indicator --
         * no separate "WiFi: CONNECTED" box needed, confirmed with the
         * user); a row of 3 small colour-bordered boxes (TXP/FW/OTA,
         * WiFi dropped from this row for the same reason); progress
         * bar; T/H as two separate boxes instead of one shared box;
         * DHT STATUS/UPTIME as a plain text row; CHECK/DWD buttons
         * unchanged.
         *
         * No thermometer/humidity/Ethernet/cloud icons -- confirmed by
         * grepping LVGL's bundled symbol font directly, none of those
         * exist in it. Real icon images matching the reference would
         * need custom bitmap assets, which was ruled out given this
         * build's flash usage was already ~91-92%%. LV_SYMBOL_UPLOAD is
         * used for OTA (closest built-in match to a cloud-upload icon,
         * already free -- part of the font already linked in); T/H
         * stay text-only, matching that same reasoning. */

        /* Logo, small corner size, v1 only. Replaced (2026-09-18) with
         * a new source image ("fastbit embedded logo.png", real alpha
         * transparency, composited onto black at conversion time) --
         * see lcd_logo.c's own header comment for the full asset
         * history. No backing chip needed: white chip icon + wordmark
         * on a black-composited background reads cleanly on this
         * screen's own black background. */
        lv_obj_t *logo = lv_img_create(s_content);
        lv_img_set_src(logo, &fastbit_logo_small_dsc);
        lv_obj_align(logo, LV_ALIGN_TOP_LEFT, CONTENT_X, 8);

        /* Centred (2026-09-17, follow-up), not next to the logo -- per
         * explicit request to move it to the middle of the header. */
        lv_obj_t *title = lv_label_create(s_content);
        lv_label_set_text(title, "DEVICE MONITOR");
        lv_obj_set_style_text_color(title, lv_color_hex(0x3B9CFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

        /* WiFi -- "WiFi: " text plus the icon (2026-09-17, follow-up:
         * icon-only was tried, then explicitly asked back with its
         * label), top-right, the screen's single connect/disconnect
         * indicator (green/red via set_wifi_display()). */
        s_wifi_icon = lv_label_create(s_content);
        lv_label_set_text(s_wifi_icon, "WiFi: " LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_RIGHT, -8, 12);

        /* Real current state, not an assumed "not connected yet": WiFi
         * connects on a separate thread that races with this function
         * (see s_ready's comment above) and had often already finished
         * by the time this runs, confirmed on real hardware -- checking
         * here avoids getting stuck showing "Connecting..." forever
         * when it is actually already connected. */
        set_wifi_display(wifi_link_is_connected());

        /* TXP / FW / OTA -- three small boxes, one row (2026-09-17).
         * OTA gets more width than the other two (192 vs 126) since its
         * text is the longest and most variable ("OTA: REBOOT PENDING"
         * etc.) -- 126+126+192+2*8(gaps) = 460 = CONTENT_W exactly. */
        lv_obj_t *txp_cell = create_cell(CONTENT_X, 44, 126, 36);
        lv_obj_t *transport_label = lv_label_create(txp_cell);
        lv_label_set_text(transport_label, "TXP: " OTA_TRANSPORT_NAME);
        lv_obj_set_style_text_color(transport_label, lv_color_hex(0xCC66FF), LV_PART_MAIN);
        lv_obj_set_style_text_font(transport_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(transport_label);

        lv_obj_t *fw_cell = create_cell(CONTENT_X + 126 + 8, 44, 126, 36);
        s_version_label = lv_label_create(fw_cell);
        lv_label_set_text(s_version_label, "FW: " CONFIG_APP_OTA_DEVICE_VERSION);
        lv_obj_set_style_text_color(s_version_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_version_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(s_version_label);

        /* OTA -- LV_SYMBOL_UPLOAD (closest built-in match to a cloud-
         * upload icon; a static neutral colour, not the dynamic state
         * colour, to keep this simple) plus the existing dynamic
         * "OTA: <state>" text next to it, left-aligned so the longest
         * state names have maximum room to the right. */
        lv_obj_t *ota_cell = create_cell(CONTENT_X + 126 + 8 + 126 + 8, 44, 192, 36);
        lv_obj_t *ota_icon = lv_label_create(ota_cell);
        lv_label_set_text(ota_icon, LV_SYMBOL_UPLOAD);
        lv_obj_set_style_text_color(ota_icon, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
        lv_obj_set_style_text_font(ota_icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(ota_icon, LV_ALIGN_LEFT_MID, 6, 0);

        s_ota_label = lv_label_create(ota_cell);
        lv_label_set_text(s_ota_label, "OTA: IDLE");
        lv_obj_set_style_text_color(s_ota_label, ota_state_color(OTA_STATE_IDLE, OTA_RESULT_OK), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ota_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_ota_label, LV_ALIGN_LEFT_MID, 26, 0);

        /* Progress bar + percentage label. v1 only -- see the "DWD"
         * button below: v1 is the only version that ever actually
         * downloads anything through this screen. */
        s_bar = lv_bar_create(s_content);
        lv_obj_set_size(s_bar, CONTENT_W - 60, 14);
        lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, CONTENT_X, V1_BAR_Y_NORMAL);
        lv_bar_set_range(s_bar, 0, 100);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
        /* LV_ANIM_OFF: LV_ANIM_ON's animation only progresses across
         * repeated lv_task_handler() calls over time and was confirmed
         * buggy for this kind of discrete-step update during the
         * earlier Stage 4 bring-up (each step's animation got
         * interrupted by the next value change, leaving the bar
         * visibly short of full on real hardware). */
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

        s_pct_label = lv_label_create(s_content);
        lv_obj_set_style_text_color(s_pct_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_pct_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_pct_label, "0%");
        lv_obj_align_to(s_pct_label, s_bar, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

        /* Hidden until an actual download is in progress (DOWNLOAD/
         * VERIFY/INSTALL) -- see lcd_status_on_ota_state_zephyr() below.
         * Not shown at idle/no-update, and not worth showing for the
         * post-reboot confirm states either: the screen resets on
         * reboot and those states settle within the same early-boot
         * race already described above, before a human would
         * realistically see them anyway. */
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pct_label, LV_OBJ_FLAG_HIDDEN);

        /* DHT11 temperature+humidity+status, all in ONE box now
         * (2026-09-17, follow-up) -- TEMP and HUM side by side on the
         * top row, DHT STATUS centred below, replacing the earlier
         * two-separate-boxes-plus-plain-text-row layout per explicit
         * request ("keep the temp hum and dht status in single box"). */
        s_sensor_cell = create_cell(CONTENT_X, V1_SENSOR_Y_NORMAL, CONTENT_W, 74);

        /* Thermometer icon (2026-09-17) -- real bitmap, since LVGL's
         * built-in symbol font has no thermometer glyph (confirmed).
         * Humidity uses LV_SYMBOL_TINT (a water-drop shape, LVGL's
         * closest built-in match) instead of a custom bitmap -- free,
         * no extra flash cost. */
        lv_obj_t *temp_icon = lv_img_create(s_sensor_cell);
        lv_img_set_src(temp_icon, &thermometer_icon_dsc);
        lv_obj_align(temp_icon, LV_ALIGN_TOP_LEFT, 10, 6);

        s_temp_label = lv_label_create(s_sensor_cell);
        lv_label_set_text(s_temp_label, "TEMP: --.-- C");
        lv_obj_set_style_text_color(s_temp_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_temp_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_temp_label, LV_ALIGN_TOP_LEFT, 10 + 22 + 6, 9);

        lv_obj_t *hum_icon = lv_label_create(s_sensor_cell);
        lv_label_set_text(hum_icon, LV_SYMBOL_TINT);
        lv_obj_set_style_text_color(hum_icon, lv_color_hex(0x66CCFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(hum_icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(hum_icon, LV_ALIGN_TOP_LEFT, 250, 8);

        s_humidity_label = lv_label_create(s_sensor_cell);
        lv_label_set_text(s_humidity_label, "HUM: --.-- %");
        lv_obj_set_style_text_color(s_humidity_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_humidity_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_humidity_label, LV_ALIGN_TOP_LEFT, 250 + 20, 9);

        s_dht_status_label = lv_label_create(s_sensor_cell);
        lv_label_set_recolor(s_dht_status_label, true);
        lv_label_set_text(s_dht_status_label, "DHT STATUS: RUNNING");
        lv_obj_set_style_text_color(s_dht_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_dht_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_dht_status_label, LV_ALIGN_BOTTOM_MID, 0, -8);

        /* UPTIME -- kept outside the sensor box (only the temp/hum/dht
         * trio was asked to move into one box). Centred below the box
         * (2026-09-17, follow-up) -- alone at the right edge looked
         * lopsided/orphaned per explicit feedback ("the placement that
         * is present now is not good"); centred reads as its own
         * balanced row instead. */
        s_uptime_label = lv_label_create(s_content);
        lv_label_set_text(s_uptime_label, "UPTIME: --:--:--");
        lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_uptime_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_uptime_label, LV_ALIGN_TOP_MID, 0, V1_UPTIME_Y_NORMAL);

        /* "CHECK" button -- unchanged (per explicit request: "check and
         * dwd touch button keep as it is") -- shown only while idle (see
         * lcd_status_on_ota_state_zephyr() below). Triggers a manifest
         * check only -- see the DWD button just below for the separate,
         * second touch that actually starts a download. */
        s_start_btn = lv_btn_create(s_content);
        lv_obj_set_size(s_start_btn, 220, 32);
        lv_obj_align(s_start_btn, LV_ALIGN_TOP_MID, 0, 224);
        lv_obj_set_style_bg_color(s_start_btn, lv_color_hex(0x3B9CFF), LV_PART_MAIN);
        lv_obj_add_event_cb(s_start_btn, start_btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *start_btn_label = lv_label_create(s_start_btn);
        lv_label_set_text(start_btn_label, "UPDATE");
        lv_obj_set_style_text_font(start_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(start_btn_label);

        /* "DWD" button -- the second, separate touch that confirms a
         * download the CHECK button found. Same position as
         * s_start_btn (they are never shown at the same time -- CHECK
         * only at idle, DWD only at OTA_STATE_UPDATE_AVAILABLE, see
         * lcd_status_on_ota_state_zephyr() below). Green rather than
         * CHECK's blue, matching the "actively working" colour already
         * used for download/verify/install states. The "OTA: UPDATE
         * AVAILABLE" text already shown in the OTA cell above covers
         * "new version available" -- no separate label needed here. */
        s_dwd_btn = lv_btn_create(s_content);
        lv_obj_set_size(s_dwd_btn, 220, 32);
        lv_obj_align(s_dwd_btn, LV_ALIGN_TOP_MID, 0, 224);
        lv_obj_set_style_bg_color(s_dwd_btn, lv_color_hex(0x00FF00), LV_PART_MAIN);
        lv_obj_add_event_cb(s_dwd_btn, dwd_btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *dwd_btn_label = lv_label_create(s_dwd_btn);
        lv_label_set_text(dwd_btn_label, "DOWNLOAD");
        lv_obj_set_style_text_color(dwd_btn_label, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_text_font(dwd_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(dwd_btn_label);
        lv_obj_add_flag(s_dwd_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* ---- v2 layout (2026-09-17, MP4): every metric stacked in a
         * left column, temperature history graph filling the right
         * side. No title, no boxes, no buttons -- v2 has nothing to
         * touch and the graph is the whole point of this screen, so
         * every pixel not spent on a metric goes to it. Confirmed with
         * the user on real hardware that squeezing the graph into a
         * ~56px strip between v1-style boxes looked wrong; this
         * dedicates the graph its own side of the screen instead. */
        #define V2_LEFT_X   6
        #define V2_LEFT_W   150
        #define V2_ROW_H    32

        lv_obj_t *wifi_row = lv_label_create(s_content);
        lv_label_set_text(wifi_row, "WiFi: " LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(wifi_row, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(wifi_row, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 0 * V2_ROW_H + 8);
        s_wifi_icon = wifi_row;
        set_wifi_display(wifi_link_is_connected());

        lv_obj_t *transport_row = lv_label_create(s_content);
        lv_label_set_text(transport_row, "TXP: " OTA_TRANSPORT_NAME);
        lv_obj_set_style_text_color(transport_row, lv_color_hex(0xCC66FF), LV_PART_MAIN);
        lv_obj_set_style_text_font(transport_row, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(transport_row, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 1 * V2_ROW_H + 8);

        s_version_label = lv_label_create(s_content);
        lv_label_set_text(s_version_label, "FW: " CONFIG_APP_OTA_DEVICE_VERSION);
        lv_obj_set_style_text_color(s_version_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_version_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_version_label, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 2 * V2_ROW_H + 8);

        /* OTA -- back in its original column slot (2026-09-17, second
         * follow-up: moving it below the chart was NOT what was wanted;
         * the actual request was to wrap a long failure line onto a
         * second line instead, v2 only -- see
         * lcd_status_on_ota_state_zephyr()'s v2 formatting below, which
         * turns "OTA: FAIL: TRANSPORT" into "OTA: FAIL -\nTRANSPORT"
         * so it never needs to be wider than this column. */
        s_ota_label = lv_label_create(s_content);
        lv_label_set_text(s_ota_label, "OTA: IDLE");
        lv_obj_set_style_text_color(s_ota_label, ota_state_color(OTA_STATE_IDLE, OTA_RESULT_OK), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_ota_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_ota_label, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 3 * V2_ROW_H + 8);

        /* Orange/cyan (2026-09-18, follow-up) -- v2/v3(+) only, per
         * explicit request ("dont apply for v1"): a fixed colour per
         * value, consistent across every non-v1 version, so TEMP/HUM
         * are visually identifiable at a glance regardless of which
         * version is running. v1 keeps its plain white TEMP/HUM text
         * (see the v1 branch further above, unchanged). */
        s_temp_label = lv_label_create(s_content);
        lv_label_set_text(s_temp_label, "T = --.-- C");
        lv_obj_set_style_text_color(s_temp_label, lv_color_hex(0xFF9933), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_temp_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_temp_label, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 4 * V2_ROW_H + 8);

        s_humidity_label = lv_label_create(s_content);
        lv_label_set_text(s_humidity_label, "H = --.-- %");
        lv_obj_set_style_text_color(s_humidity_label, lv_color_hex(0x66FFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_humidity_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_humidity_label, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 5 * V2_ROW_H + 8);

        /* Shortened from "DHT STATUS: RUNNING" to "DHT: RUNNING" -- the
         * v1 wording does not fit this column's ~150px width. */
        s_dht_status_label = lv_label_create(s_content);
        lv_label_set_recolor(s_dht_status_label, true);
        lv_label_set_text(s_dht_status_label, "DHT: RUNNING");
        lv_obj_set_style_text_color(s_dht_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_dht_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_dht_status_label, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 6 * V2_ROW_H + 8);

        s_uptime_label = lv_label_create(s_content);
        lv_label_set_text(s_uptime_label, "UPTIME: --:--:--");
        lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_uptime_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_uptime_label, LV_ALIGN_TOP_LEFT, V2_LEFT_X, 7 * V2_ROW_H + 8);

        /* v2 now has its own progress bar too (2026-09-18, follow-up),
         * created further below in this branch, in the same screen area
         * the graph occupies -- see s_graph_title's declaration comment
         * and s_bar's creation comment for the full reasoning. */

        /* Temperature history graph -- fills the entire right side of
         * the screen, full height, not squeezed into a strip between
         * boxes. This is the actual new application feature the whole
         * OTA demo hinges on: v1's binary contains no chart code at
         * all, so this appearing only after the OTA reboot proves real
         * new code shipped, not just a version-string change. lv_chart,
         * LVGL's built-in scrolling line-chart widget
         * (CONFIG_LV_USE_CHART, already on by default).
         *
         * Fixed Y range (15-40 C) rather than auto-scaling to the
         * observed min/max: auto-scaling would rescale the whole
         * graph's vertical proportions every time a new extreme value
         * arrived, which reads as jumpy/unstable on a live display; a
         * fixed, generous indoor-temperature range keeps the scale
         * visually stable while the line itself still clearly shows
         * real rises and falls.
         *
         * Point count 30, one point added per successful sensor read
         * (every SENSOR_POLL_INTERVAL_MS = 2s) -- matches this sensor's
         * own already-established, DHT11-safe polling rate rather than
         * reading it faster just for the graph (see the DHT11
         * read-reliability project memory note; faster polling would
         * only make those failures more frequent). 30 points at 2s each
         * is one minute of rolling history. */
        #define V2_GRAPH_X (V2_LEFT_X + V2_LEFT_W + 10)
        #define V2_GRAPH_W (480 - V2_GRAPH_X - 8)

        /* Reverted to exactly the original structure (2026-09-18): a
         * wrapper container was tried here to hide title+chart as one
         * unit, but it broke the chart's own axis-number rendering --
         * confirmed on real hardware, Y and X axis numbers vanished
         * (gridlines only) even though nothing about the chart's own
         * config changed. Root cause: the X-axis tick labels draw
         * OUTSIDE the chart widget's own bounding box (see the height
         * comment below), and the container's bounds clipped that
         * away -- LVGL clips a widget's children to the parent's own
         * rectangle. Title and chart are direct children of s_content
         * again, unchanged from before; s_graph_title/s_temp_chart are
         * hidden/shown as two separate objects instead of one
         * container (see lcd_status_on_ota_state_zephyr() below). */
        /* Title colour matches its own value's colour (2026-09-18,
         * follow-up) -- orange for TEMPERATURE HISTORY (same as the
         * TEMP row), cyan for HUMIDITY HISTORY (same as the HUM row),
         * instead of the generic blue every other title uses. */
        s_graph_title = lv_label_create(s_content);
        lv_label_set_text(s_graph_title,
                           s_show_humidity_graph ? "HUMIDITY HISTORY" : "TEMPERATURE HISTORY");
        lv_obj_set_style_text_color(
            s_graph_title, lv_color_hex(s_show_humidity_graph ? 0x66FFFF : 0xFF9933), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_graph_title, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_graph_title, LV_ALIGN_TOP_LEFT, V2_GRAPH_X, 8);

        /* CHECK / DWD buttons, v2 (2026-09-17, follow-up) -- same touch-
         * driven two-step flow as v1 (CHECK triggers a manifest check;
         * DWD is the separate second touch that confirms the download),
         * just smaller and placed in the free space at the top-right of
         * the graph area (to the right of the "TEMPERATURE HISTORY"
         * title, above the chart itself) instead of v1's full-width
         * bottom button -- there is no equivalent free row at the
         * bottom of v2's layout. Same event callbacks as v1; the two
         * are mutually exclusive by construction, same as v1 (see
         * lcd_status_on_ota_state_zephyr() below, guard now applies to
         * both versions since both have real (non-NULL) buttons). */
        s_start_btn = lv_btn_create(s_content);
        lv_obj_set_size(s_start_btn, 100, 22);
        lv_obj_align(s_start_btn, LV_ALIGN_TOP_RIGHT, -8, 6);
        lv_obj_set_style_bg_color(s_start_btn, lv_color_hex(0x3B9CFF), LV_PART_MAIN);
        lv_obj_add_event_cb(s_start_btn, start_btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *v2_start_btn_label = lv_label_create(s_start_btn);
        lv_label_set_text(v2_start_btn_label, "UPDATE");
        lv_obj_set_style_text_font(v2_start_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(v2_start_btn_label);

        s_dwd_btn = lv_btn_create(s_content);
        lv_obj_set_size(s_dwd_btn, 100, 22);
        lv_obj_align(s_dwd_btn, LV_ALIGN_TOP_RIGHT, -8, 6);
        lv_obj_set_style_bg_color(s_dwd_btn, lv_color_hex(0x00FF00), LV_PART_MAIN);
        lv_obj_add_event_cb(s_dwd_btn, dwd_btn_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *v2_dwd_btn_label = lv_label_create(s_dwd_btn);
        lv_label_set_text(v2_dwd_btn_label, "DOWNLOAD");
        lv_obj_set_style_text_color(v2_dwd_btn_label, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_text_font(v2_dwd_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(v2_dwd_btn_label);
        lv_obj_add_flag(s_dwd_btn, LV_OBJ_FLAG_HIDDEN);

        /* Progress bar (2026-09-18, follow-up) -- v2 now downloads
         * through this screen too (since it got a real DWD button), but
         * there is nowhere to put a bar alongside the graph without
         * overlapping it. Per explicit request: hide the graph while a
         * download is actually happening and show this bar in the same
         * area instead (see lcd_status_on_ota_state_zephyr() and
         * lcd_status_on_progress_zephyr() below, both now version-
         * agnostic). Sits in the same rectangle s_graph_title/
         * s_temp_chart occupy -- only one side is ever visible at a
         * time. */
        /* Shifted right +10 and widened +10 (2026-09-18, follow-up) --
         * per explicit feedback it sat too close to the left column's
         * "OTA: DOWNLOADING" text and felt short. Right edge is still
         * comfortably clear of the pct label (40px margin left for it,
         * enough for "100%"). */
        s_bar = lv_bar_create(s_content);
        lv_obj_set_size(s_bar, V2_GRAPH_W - 50, 14);
        lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, V2_GRAPH_X + 10, 110);
        lv_bar_set_range(s_bar, 0, 100);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_bar, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

        s_pct_label = lv_label_create(s_content);
        lv_obj_set_style_text_color(s_pct_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_pct_label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_label_set_text(s_pct_label, "0%");
        lv_obj_align_to(s_pct_label, s_bar, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

        /* "DOWNLOADING NEW FIRMWARE" text with animated dots (2026-09-18,
         * follow-up -- spinner tried and removed per explicit request,
         * "will look into that later"). Base text is set here; the
         * animated dot count is written by update_download_dots_label()
         * from the ticker while this label is visible -- see that
         * function's own comment for how the animation works. */
        s_dl_label = lv_label_create(s_content);
        lv_label_set_text(s_dl_label, "DOWNLOADING NEW FIRMWARE");
        /* Green (2026-09-18, follow-up) -- matches the bar's own
         * indicator colour and the DWD button, the app's existing
         * "actively working" colour, rather than plain white.
         *
         * A transform_zoom (109%) was tried here for a bit more size
         * per explicit request, then reverted -- confirmed on real
         * hardware it made the label stop rendering at all (blank,
         * not just clipped). Not worth debugging further given a
         * bigger font back (Montserrat 24) would cost ~28KB flash
         * anyway; text stays at its normal size for now. */
        lv_obj_set_style_text_color(s_dl_label, lv_color_hex(0x00FF00), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_dl_label, &lv_font_montserrat_14, LV_PART_MAIN);
        /* Left-aligned within the graph area, not LV_ALIGN_TOP_MID --
         * TOP_MID would centre on the full screen width (this label's
         * parent is s_content, the whole screen), which would overlap
         * the left column of metrics instead of staying inside the
         * graph's own area. y=90, right above the bar at y=110
         * (2026-09-18, follow-up: moved down from y=40 -- per explicit
         * request, "keep just top of the progress bar" instead of sitting
         * far above it with a big empty gap). */
        lv_obj_align(s_dl_label, LV_ALIGN_TOP_LEFT, V2_GRAPH_X + 10, 90);

        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pct_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dl_label, LV_OBJ_FLAG_HIDDEN);

        /* Height shrunk from 236 to 206 (2026-09-17): confirmed on real
         * hardware that the X-axis tick labels were drawing correctly
         * in every other respect but landing entirely BELOW the
         * physical 272px screen edge -- LVGL draws X-axis ticks/labels
         * relative to the chart widget's own bottom edge
         * (obj->coords.y2), not inside some reserved inner margin, so
         * at height 236 (y2 = 30 + 236 = 266) there were only 6px left
         * before the screen's bottom edge at 272 -- nowhere near enough
         * for a tick line plus a 14pt label. 206 (y2 = 236) leaves 36px
         * of real on-screen room below the chart for them. */
        /* v3 (2026-09-18, follow-up): humidity history instead of
         * temperature -- same widget, same fill-then-scroll mechanism
         * (see update_sensor_reading() below), just a different Y
         * range (0-100%% instead of 15-40 C, still 6 clean divisions:
         * 0/20/40/60/80/100) and series colour -- now the exact same
         * colour as its own value's row and title (2026-09-18,
         * follow-up: cyan for humidity, orange for temperature) rather
         * than the earlier blue/green pairing, so the whole chain
         * (row text -> section title -> graph line) reads as one
         * consistent colour per metric. */
        s_temp_chart = lv_chart_create(s_content);
        lv_obj_set_size(s_temp_chart, V2_GRAPH_W, 206);
        lv_obj_align(s_temp_chart, LV_ALIGN_TOP_LEFT, V2_GRAPH_X, 30);
        lv_chart_set_type(s_temp_chart, LV_CHART_TYPE_LINE);
        if (s_show_humidity_graph) {
            lv_chart_set_range(s_temp_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        } else {
            lv_chart_set_range(s_temp_chart, LV_CHART_AXIS_PRIMARY_Y, 15, 40);
        }
        lv_chart_set_point_count(s_temp_chart, CHART_POINT_COUNT);
        lv_obj_set_style_bg_opa(s_temp_chart, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(s_temp_chart, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_temp_chart, 1, LV_PART_MAIN);
        lv_obj_set_style_size(s_temp_chart, 0, LV_PART_INDICATOR);
        s_temp_chart_series = lv_chart_add_series(
            s_temp_chart, lv_color_hex(s_show_humidity_graph ? 0x66FFFF : 0xFF9933),
            LV_CHART_AXIS_PRIMARY_Y);

        /* Axis tick labels (2026-09-17) -- Celsius on the left, point
         * index (roughly time, one point per SENSOR_POLL_INTERVAL_MS =
         * 2s) along the bottom, matching the requested reference
         * sketch. lv_chart's own built-in tick/label drawing
         * (lv_chart_set_axis_tick) reserves its label space from the
         * chart WIDGET's own padding, not by shrinking the outer
         * lv_obj_set_size above -- hence the explicit left/bottom
         * padding here, which is what actually shrinks the plotted
         * area to make room for the numbers. 6 Y-axis divisions (15-40
         * range in fixed steps of 5: 15/20/25/30/35/40); 5 X-axis
         * divisions across the 30-point window.
         *
         * Text colour explicitly set on LV_PART_TICKS (2026-09-17,
         * confirmed on real hardware as the actual bug for the Y axis):
         * the labels were never missing, they were rendering in LVGL's
         * default dark tick-label colour, invisible against this
         * screen's black background. White makes them match every
         * other label here. The X axis had a second, separate bug --
         * see the chart height comment just above. */
        lv_obj_set_style_pad_left(s_temp_chart, 34, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(s_temp_chart, 18, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_temp_chart, lv_color_hex(0xFFFFFF), LV_PART_TICKS);
        lv_obj_set_style_line_color(s_temp_chart, lv_color_hex(0x888888), LV_PART_TICKS);
        lv_chart_set_axis_tick(s_temp_chart, LV_CHART_AXIS_PRIMARY_Y, 5, 0, 6, 1, true, 34);
        /* major_cnt 13 (2026-09-17, per explicit request): a 60-second-
         * wide window labelled every 5 seconds needs 13 labels
         * (0,5,10...60), not 5 -- see chart_x_tick_label_event_cb's own
         * comment for how the label text itself is now computed
         * directly from the tick index rather than from point position,
         * which is what makes a clean 0,5,10,15... sequence possible at
         * all. */
        /* major_cnt 5 (2026-09-17, per explicit request): a 20-second-
         * wide window (CHART_POINT_COUNT=10 points at 2s each) labelled
         * every 5 seconds needs exactly 5 labels (0,5,10,15,20). */
        lv_chart_set_axis_tick(s_temp_chart, LV_CHART_AXIS_PRIMARY_X, 5, 0, 5, 1, true, 18);

        /* Custom X-axis label text (2026-09-17) -- see
         * chart_x_tick_label_event_cb's own comment for why: without
         * this, LVGL's default X-axis labels are fixed point-index
         * numbers (always "0 1 2 3 4" e.g.) that never change even as
         * the chart scrolls, which does not match "time keeps
         * increasing" as explicitly requested. */
        lv_obj_add_event_cb(s_temp_chart, chart_x_tick_label_event_cb, LV_EVENT_DRAW_PART_BEGIN,
                             NULL);

        /* All CHART_POINT_COUNT slots start at LV_CHART_POINT_NONE (not
         * 0, which is below this chart's 15-40 range and would
         * otherwise draw a visible flat line at the bottom for
         * not-yet-written points) so only genuinely-read points ever
         * appear during the fill phase -- see update_sensor_reading()'s
         * own comment. */
        for (int i = 0; i < CHART_POINT_COUNT; i++) {
            lv_chart_set_value_by_id(s_temp_chart, s_temp_chart_series, i, LV_CHART_POINT_NONE);
        }

        #undef V2_LEFT_X
        #undef V2_LEFT_W
        #undef V2_ROW_H
        #undef V2_GRAPH_X
        #undef V2_GRAPH_W
    }

    /* Animation widgets temporarily disabled (2026-09-17), per explicit
     * user request after MP3 testing -- "let it be commented out for
     * now", not removed. s_is_v1 above is still computed and still
     * drives CHECK/DWD button visibility elsewhere in this file; only
     * the animation widget creation is skipped. Uncomment this block to
     * restore the v1 orbiting-dots / v2 spinner animation. */
#if 0
    if (s_is_v1) {
        s_anim_icon = lv_label_create(s_content);
        lv_label_set_text(s_anim_icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(s_anim_icon, lv_color_hex(0x3B9CFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_anim_icon, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_align(s_anim_icon, LV_ALIGN_TOP_LEFT, ANIM_CX - 8, ANIM_CY - 8);

        static const uint32_t dot_colors[3] = { 0x00FF00, 0xFFAA00, 0x3B9CFF };
        for (int i = 0; i < 3; i++) {
            s_anim_dots[i] = lv_obj_create(s_content);
            lv_obj_remove_style_all(s_anim_dots[i]);
            lv_obj_set_style_radius(s_anim_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_anim_dots[i], lv_color_hex(dot_colors[i]), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_anim_dots[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_size(s_anim_dots[i], 12, 12);
        }

        /* Hidden until OTA activity actually starts (2026-09-17) -- see
         * lcd_status_on_ota_state_zephyr() below. */
        lv_obj_add_flag(s_anim_icon, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++) {
            lv_obj_add_flag(s_anim_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        /* Shrunk from 70x70 (2026-09-17, portrait layout) to fit within
         * the narrow column's remaining vertical space below STATUS. */
        lv_obj_t *spinner = lv_spinner_create(s_content, 1000, 90);
        lv_obj_set_size(spinner, 60, 60);
        lv_obj_align(spinner, LV_ALIGN_TOP_LEFT, ANIM_CX - 30, ANIM_CY - 30);
    }
#endif

    lv_task_handler();
    display_blanking_off(display_dev);

    wifi_link_set_event_cb(on_wifi_event);

    s_ready = true;

    /* Start the ticker only now -- screen construction above is finished,
     * so there is no window where the ticker could run concurrently with
     * init() still building widgets. */
    k_thread_create(&lvgl_ticker_thread_data, lvgl_ticker_stack,
                     K_THREAD_STACK_SIZEOF(lvgl_ticker_stack), lvgl_ticker_fn,
                     NULL, NULL, NULL, 8, 0, K_NO_WAIT);

    LOG_INF("LCD status display initialised (F746G, LVGL)");
}

void lcd_status_on_ota_state_zephyr(ota_state_t state, ota_result_t result)
{
    LOG_INF("OTA state -> %d (last_result=%d)", (int)state, (int)result);

    if (!s_ready) {
        return;
    }

    char buf[48];
    if (state == OTA_STATE_IDLE && result != OTA_RESULT_OK) {
        const char *label = ota_failure_label(result);
        /* v2 only (2026-09-17, follow-up): a failure label like
         * "FAIL: TRANSPORT" is wider than v2's ~150px left column and
         * was overflowing into the graph next to it. Break it onto a
         * second line at the colon instead -- "OTA: FAIL -\nTRANSPORT"
         * -- so it never needs more width than the column has. v1 has
         * its own dedicated wide OTA box and keeps the single-line
         * form unchanged. */
        const char *colon = s_is_v1 ? NULL : strchr(label, ':');
        if (colon != NULL) {
            (void)snprintf(buf, sizeof(buf), "OTA: %.*s -\n%s",
                            (int)(colon - label), label, colon + 2);
        } else {
            (void)snprintf(buf, sizeof(buf), "OTA: %s", label);
        }
    } else {
        const char *label = ota_state_label(state);
        /* v2 only (2026-09-18, follow-up): same overflow problem as the
         * FAIL: labels above, this time for plain state labels with a
         * space in them (e.g. "UPDATE AVAILABLE", "REBOOT PENDING") --
         * "OTA: UPDATE AVAILABLE" touched the graph's left edge.
         * Wrapped onto a second line at the first space, same
         * mechanism as the failure-label case. */
        const char *space = s_is_v1 ? NULL : strchr(label, ' ');
        if (space != NULL) {
            (void)snprintf(buf, sizeof(buf), "OTA: %.*s\n%s",
                            (int)(space - label), label, space + 1);
        } else {
            (void)snprintf(buf, sizeof(buf), "OTA: %s", label);
        }
    }

    k_mutex_lock(&g_lvgl_lock, K_FOREVER);

    lv_label_set_text(s_ota_label, buf);
    lv_obj_set_style_text_color(s_ota_label, ota_state_color(state, result), LV_PART_MAIN);

    /* Progress bar only appears for the states where it means anything
     * (a download is actually in flight or about to be applied). Every
     * other state -- idle, no-update, post-reboot confirm flow, etc. --
     * hides it, per the reasoning in lcd_status_wiring_init(). Now
     * version-agnostic (2026-09-18, follow-up) -- v2 has its own s_bar/
     * s_pct_label too since it got a real DWD button. On v2, the graph
     * (s_graph_title + s_temp_chart) shares the exact same screen area
     * as the bar, so both are hidden while the bar shows and restored
     * once the bar hides again -- per explicit request ("graph should
     * disappear and the download progress bar should be shown"). Two
     * separate objects, not one wrapper container -- a container was
     * tried and reverted, see s_graph_title's declaration comment and
     * lcd_status_wiring_init()'s v2 branch for why. Both are NULL on
     * v1, so that half is skipped there. */
    bool show_bar = (state == OTA_STATE_DOWNLOAD || state == OTA_STATE_VERIFY ||
                      state == OTA_STATE_INSTALL_TO_2ND_SLOT);
    if (show_bar) {
        lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pct_label, LV_OBJ_FLAG_HIDDEN);
        if (s_graph_title != NULL) {
            lv_obj_add_flag(s_graph_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_temp_chart, LV_OBJ_FLAG_HIDDEN);
        }
        /* v1 only (2026-09-18, follow-up): spread the bar/sensor box/
         * UPTIME out to fill the space the now-empty CHECK/DOWNLOAD
         * button row leaves, per explicit request confirmed via an
         * ASCII layout mockup -- see the V1_*_Y_DOWNLOADING constants'
         * own comment. */
        if (s_is_v1) {
            lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, CONTENT_X, V1_BAR_Y_DOWNLOADING);
            lv_obj_align_to(s_pct_label, s_bar, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
            lv_obj_align(s_sensor_cell, LV_ALIGN_TOP_LEFT, CONTENT_X, V1_SENSOR_Y_DOWNLOADING);
            lv_obj_align(s_uptime_label, LV_ALIGN_TOP_MID, 0, V1_UPTIME_Y_DOWNLOADING);
        }
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pct_label, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_pct_label, "0%");
        lv_obj_align_to(s_pct_label, s_bar, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
        if (s_graph_title != NULL) {
            lv_obj_clear_flag(s_graph_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_temp_chart, LV_OBJ_FLAG_HIDDEN);
        }
        /* v1 only -- restore the normal (idle/update-available)
         * positions, the mirror of the block above. */
        if (s_is_v1) {
            lv_obj_align(s_bar, LV_ALIGN_TOP_LEFT, CONTENT_X, V1_BAR_Y_NORMAL);
            lv_obj_align_to(s_pct_label, s_bar, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
            lv_obj_align(s_sensor_cell, LV_ALIGN_TOP_LEFT, CONTENT_X, V1_SENSOR_Y_NORMAL);
            lv_obj_align(s_uptime_label, LV_ALIGN_TOP_MID, 0, V1_UPTIME_Y_NORMAL);
        }
    }

    /* "DOWNLOADING NEW FIRMWARE" text (2026-09-18) -- only for the
     * actual download step, not verify/install (the text would be
     * misleading there); NULL-guarded since this only exists on v2 for
     * now (see its declaration comment). */
    if (s_dl_label != NULL) {
        if (state == OTA_STATE_DOWNLOAD) {
            lv_obj_clear_flag(s_dl_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dl_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_dl_label, "DOWNLOADING NEW FIRMWARE");
        }
    }

    /* CHECK button only makes sense while idle -- hidden the moment a
     * check/download/install actually starts, so it can't be tapped
     * again mid-cycle; reappears once back to idle (including
     * "no update" and failure, both of which are OTA_STATE_IDLE).
     *
     * DWD button (2026-09-17, MP3) only makes sense at
     * OTA_STATE_UPDATE_AVAILABLE -- the window between CHECK finding a
     * genuinely newer version and the worker actually starting the
     * download (see ota_manager_zephyr.c's confirm-gate). The two
     * buttons are mutually exclusive by construction: state is never
     * both IDLE and UPDATE_AVAILABLE at once.
     *
     * Now shown on v2 too (2026-09-17, follow-up: v2 originally had no
     * buttons at all -- autonomous by design -- but was explicitly
     * asked to get the same touch-driven CHECK/DWD flow as v1, just
     * placed differently; see lcd_status_wiring_init()'s v2 branch and
     * ota_manager_zephyr.c's now-unconditional download-confirm gate). */
    if (state == OTA_STATE_IDLE) {
        lv_obj_clear_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_start_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (state == OTA_STATE_UPDATE_AVAILABLE) {
        lv_obj_clear_flag(s_dwd_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_dwd_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* Animation visibility gating temporarily disabled (2026-09-17) --
     * see the widget creation block's own comment in
     * lcd_status_wiring_init(). Uncomment together with that block to
     * restore. */
#if 0
    if (s_is_v1) {
        bool show_anim = (state != OTA_STATE_IDLE);
        if (show_anim && !s_anim_active) {
            s_anim_phase = 0.0f;
        }
        s_anim_active = show_anim;
        if (show_anim) {
            lv_obj_clear_flag(s_anim_icon, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                lv_obj_clear_flag(s_anim_dots[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_anim_icon, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                lv_obj_add_flag(s_anim_dots[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
#endif

    k_mutex_unlock(&g_lvgl_lock);
}

void lcd_status_on_progress_zephyr(uint8_t pct)
{
    if (!s_ready) {
        return;
    }
    if (pct > 100U) {
        pct = 100U;
    }

    k_mutex_lock(&g_lvgl_lock, K_FOREVER);
    /* Version-agnostic (2026-09-18, follow-up) -- v2 has its own s_bar/
     * s_pct_label now too (see lcd_status_wiring_init()'s v2 branch). */
    lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_pct_label, "%u%%", (unsigned)pct);
    lv_obj_align_to(s_pct_label, s_bar, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    k_mutex_unlock(&g_lvgl_lock);
}
