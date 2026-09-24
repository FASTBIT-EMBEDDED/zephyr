/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_APP_LCD_ICON_THERMOMETER_H_
#define FASTBIT_APP_LCD_ICON_THERMOMETER_H_

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Thermometer icon, 22x22, RGB565. See lcd_icon_thermometer.c for how
 *  this was generated. Draw with lv_img_create() +
 *  lv_img_set_src(img, &thermometer_icon_dsc). */
extern const lv_img_dsc_t thermometer_icon_dsc;

#ifdef __cplusplus
}
#endif

#endif /* FASTBIT_APP_LCD_ICON_THERMOMETER_H_ */
