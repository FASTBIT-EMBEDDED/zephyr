/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

/*
 * Trimmed to the two functions the LCD status screen actually calls
 * (wifi_link_is_connected(), wifi_link_set_event_cb()) -- the full
 * wifi_link.h also declares wifi_link_init(), wifi_link_connect(), and
 * wifi_link_disconnect(), which the LCD screen code never calls
 * directly (those are used by the OTA worker itself).
 */

#ifndef FASTBIT_DRIVERS_WIFI_LINK_H_
#define FASTBIT_DRIVERS_WIFI_LINK_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Query whether the WiFi link is currently associated.
 *
 * Snapshot at call time; the link can drop asynchronously.
 *
 * @retval true  associated, IP layer should be reachable.
 * @retval false not associated.
 */
bool wifi_link_is_connected(void);

/**
 * @brief Register a callback for WiFi connection state changes.
 *
 * The callback fires with connected=true after a successful connect,
 * and with connected=false after a disconnect or async link drop.
 * Pass NULL to deregister.
 *
 * @param cb  Function to call on state change. May be NULL.
 */
void wifi_link_set_event_cb(void (*cb)(bool connected));

#ifdef __cplusplus
}
#endif

#endif /* FASTBIT_DRIVERS_WIFI_LINK_H_ */
