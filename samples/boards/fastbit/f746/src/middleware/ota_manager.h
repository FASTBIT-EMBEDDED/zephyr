/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

/*
 * Trimmed to the two type definitions the LCD status screen actually
 * uses (ota_state_t, ota_result_t) -- the full ota_manager.h also
 * declares ota_manifest_t, ota_backoff_t, ota_manager_policy_t,
 * ota_manager_deps_t, and the full OTA manager function API, none of
 * which the LCD screen code references.
 */

#ifndef FASTBIT_MIDDLEWARE_OTA_MANAGER_H_
#define FASTBIT_MIDDLEWARE_OTA_MANAGER_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Lifecycle state.
 *
 * The state machine progresses through these on the happy path:
 * IDLE -> UPDATE_AVAILABLE -> PRECHECK -> DOWNLOAD -> VERIFY ->
 * INSTALL_TO_2ND_SLOT -> REBOOT_PENDING -> BOOT_NEW_IMAGE ->
 * CONFIRM_IMAGE -> SUCCESS -> IDLE.
 */
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_UPDATE_AVAILABLE,
    OTA_STATE_PRECHECK,
    OTA_STATE_DOWNLOAD,
    OTA_STATE_VERIFY,
    OTA_STATE_INSTALL_TO_2ND_SLOT,
    OTA_STATE_REBOOT_PENDING,
    OTA_STATE_BOOT_NEW_IMAGE,
    OTA_STATE_CONFIRM_IMAGE,
    OTA_STATE_SUCCESS,
} ota_state_t;

/**
 * @brief Result code returned by every API call.
 *
 * OTA_RESULT_OK indicates the operation completed cleanly. All other
 * codes carry a specific failure category.
 */
typedef enum {
    OTA_RESULT_OK = 0,
    OTA_RESULT_ERR_INVALID_ARG,
    OTA_RESULT_ERR_INVALID_STATE,
    OTA_RESULT_ERR_DEPENDENCY,
    OTA_RESULT_ERR_PRECONDITION,
    OTA_RESULT_ERR_TRANSPORT,
    OTA_RESULT_ERR_VERIFY,
    OTA_RESULT_ERR_INSTALL,
    OTA_RESULT_ERR_BOOT,
    OTA_RESULT_ERR_NO_UPDATE,
} ota_result_t;

#ifdef __cplusplus
}
#endif

#endif /* FASTBIT_MIDDLEWARE_OTA_MANAGER_H_ */
