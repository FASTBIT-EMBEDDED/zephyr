/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

/*
 * Entry point reproducing the LCD call pattern from the original
 * application's own main.c: CONFIG_APP_LCD_TEST gates the standalone
 * bring-up test, CONFIG_APP_LCD_STATUS gates the real OTA status
 * screen. The original main.c also wires up app_init(), the watchdog,
 * and demo-mode heartbeat -- none of that is LCD work, so it is not
 * reproduced here; only the two #if blocks below are.
 */

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_APP_LCD_STATUS)
#include "app/lcd_status_wiring.h"
#endif

#if defined(CONFIG_APP_LCD_TEST)
#include "app/lcd_hello.h"
#endif

int main(void)
{
#if defined(CONFIG_APP_LCD_TEST)
    /* Standalone LCD bring-up test. Never returns. */
    lcd_hello_run();
#endif

#if IS_ENABLED(CONFIG_APP_LCD_STATUS)
    lcd_status_wiring_init();
#endif

    while (1) {
        k_msleep(1000);
    }

    return 0;
}
