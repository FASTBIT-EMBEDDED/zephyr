/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_APP_LCD_HELLO_H_
#define FASTBIT_APP_LCD_HELLO_H_

/*
 * Standalone LCD bring-up test.
 * Fills the SDRAM framebuffer green and draws "LCD OK" directly into it
 * with the bare-metal 5x7 bitmap font (lcd_text.c) -- no LVGL involved
 * in this active path; see lcd_hello.c for the LVGL implementation kept
 * disabled for reference. Never returns. Used only for Check 6 hardware
 * verification before the full lcd_status module is implemented.
 * Compiled only when CONFIG_APP_LCD_TEST=y.
 */
void lcd_hello_run(void);

#endif /* FASTBIT_APP_LCD_HELLO_H_ */
