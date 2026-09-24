/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_APP_LCD_LOGO_F746G_H_
#define FASTBIT_APP_LCD_LOGO_F746G_H_

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fastbit logo (chip icon + wordmark), 180x54, RGB565 -- splash screen
 *  size, shared by F429 and F746G. See lcd_logo.c for how this was
 *  generated. Draw with lv_img_create() + lv_img_set_src(img, &fastbit_logo_dsc). */
extern const lv_img_dsc_t fastbit_logo_dsc;

/** Fastbit logo (different source image, chip icon + wordmark),
 *  110x31, RGB565 -- small corner-logo size, F746G Device Monitor
 *  header, v1 only. */
extern const lv_img_dsc_t fastbit_logo_small_dsc;

/** Same logo, 300x90, RGB565 -- bigger splash-screen size, F746G only. */
extern const lv_img_dsc_t fastbit_logo_big_dsc;

#ifdef __cplusplus
}
#endif

#endif /* FASTBIT_APP_LCD_LOGO_F746G_H_ */
