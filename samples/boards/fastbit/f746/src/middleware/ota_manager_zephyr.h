/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_MIDDLEWARE_OTA_MANAGER_ZEPHYR_H_
#define FASTBIT_MIDDLEWARE_OTA_MANAGER_ZEPHYR_H_

/*
 * Zephyr-only public API for the OTA worker (see ota_manager_zephyr.c).
 * Declared separately from the portable middleware/ota_manager.h
 * because this has no meaning on host builds -- there is no worker
 * thread or check-interval wait to interrupt there.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wake the OTA worker thread immediately instead of waiting for
 *        the rest of CONFIG_APP_OTA_CHECK_INTERVAL_MS to elapse.
 *
 * Safe to call from any context (2026-09-16, M3: called from the LCD's
 * touch button click handler, which runs on the LVGL ticker thread).
 * A no-op if the worker is already awake/busy -- it just means the
 * next iteration's wait is skipped once.
 */
void ota_manager_trigger_check(void);

/**
 * @brief Confirm a download the user was shown, unblocking the worker.
 *
 * Called from the LCD's DWD button (2026-09-17, MP3), shown only while
 * OTA state is OTA_STATE_UPDATE_AVAILABLE. The worker calls
 * ota_manager_check() then waits here (v1 only -- v2 has no button and
 * downloads autonomously) before actually starting the download, so
 * "check for an update" and "download it" are two deliberate, separate
 * touches rather than one. Safe to call from any context, same as
 * ota_manager_trigger_check(); a no-op if the worker is not currently
 * waiting for it.
 */
void ota_manager_trigger_download(void);

#ifdef __cplusplus
}
#endif

#endif /* FASTBIT_MIDDLEWARE_OTA_MANAGER_ZEPHYR_H_ */
