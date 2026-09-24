/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_APP_LCD_STATUS_WIRING_H_
#define FASTBIT_APP_LCD_STATUS_WIRING_H_

#include "middleware/ota_manager.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the LCD status display and register callbacks.
 *
 * Call once from main() after the display is ready. Sets up the
 * framebuffer, draws the initial screen, and registers the WiFi
 * connection event callback.
 */
void lcd_status_wiring_init(void);

/**
 * @brief OTA state-change handler — logs to serial and updates LCD.
 *
 * Wire into ota_manager_deps_t.on_state_change in ota_manager_zephyr.c.
 */
void lcd_status_on_ota_state_zephyr(ota_state_t state, ota_result_t result);

/**
 * @brief OTA download progress handler — updates the LCD progress bar.
 *
 * Wire into ota_manager_deps_t.on_progress in ota_manager_zephyr.c.
 */
void lcd_status_on_progress_zephyr(uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* FASTBIT_APP_LCD_STATUS_WIRING_H_ */
