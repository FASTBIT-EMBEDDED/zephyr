/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

#ifndef FASTBIT_APP_FONT5X7_H_
#define FASTBIT_APP_FONT5X7_H_

#include <stdint.h>

/*
 * Full printable ASCII 5x7 bitmap font (0x20 space through 0x7E ~).
 * Covers uppercase, lowercase, digits, and all common special characters.
 *
 * Each glyph is 5 columns of 1 byte; bit0 of a column byte is the top
 * row, bit6 is the bottom row (7 rows tall). Any character outside
 * FIRST_CHAR..LAST_CHAR renders as blank (space).
 */
#define FONT5X7_FIRST_CHAR 0x20 /* space */
#define FONT5X7_LAST_CHAR  0x7E /* '~' */
#define FONT5X7_WIDTH      5
#define FONT5X7_HEIGHT     7

extern const uint8_t font5x7[][FONT5X7_WIDTH];

#endif /* FASTBIT_APP_FONT5X7_H_ */
