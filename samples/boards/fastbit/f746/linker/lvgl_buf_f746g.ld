/*
 * Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
 * All rights reserved.
 *
 * Proprietary and confidential. Use, reproduction, and distribution
 * restricted to the terms of the Fastbit Services Agreement.
 */

/*
 * Routes LVGL's rendering buffer (.lvgl_buf section, see
 * zephyr/modules/lvgl/lvgl.c -- Z_GENERIC_SECTION(.lvgl_buf) on the
 * buf0/buf1 arrays when CONFIG_LV_Z_VBD_CUSTOM_SECTION=y) into SDRAM1
 * on this board, the same way the framebuffer itself is already routed
 * there via `ext-sdram = <&sdram1>;` in the board overlay.
 *
 * Why this is needed: LVGL's buffer is close to full-screen size
 * (480x272x2 = 255 KB at CONFIG_LV_Z_VDB_SIZE=100 with
 * CONFIG_LV_Z_FULL_REFRESH=y), the same size class as the framebuffer.
 * It does not fit in this board's 256 KB internal SRAM (sram0) any more
 * than the framebuffer did -- see app/boards/stm32f746g_disco.overlay
 * for the full internal-SRAM investigation this project already went
 * through for the framebuffer itself.
 *
 * Adapted from a genuine, currently-shipping Zephyr upstream reference
 * solving the identical problem on a different ST board with the same
 * SDRAM-for-LVGL-buffer need:
 *   zephyr/boards/st/stm32h747i_disco/dc_ram.ld
 * (that file targets SDRAM2 on the H747I-DISCO; this one targets SDRAM1
 * on the F746G-DISCO -- otherwise identical in structure). Despite this
 * project's CLAUDE.md documenting an `app/linker/lvgl_buf.ld` from the
 * F429 investigation, no such file was ever found in this repo's git
 * history -- this file is written fresh, not copied from that reference.
 *
 * SECTION_PROLOGUE / GROUP_LINK_IN / GROUP_START / GROUP_END are Zephyr's
 * own linker-script macros (zephyr/include/zephyr/linker/linker-defs.h),
 * not raw GNU ld syntax -- required because Zephyr's main linker script
 * assembles multiple such fragments together; raw ld `SECTIONS { }
 * INSERT AFTER` does not work inside that injected structure (same
 * lesson already learned during the F429 LVGL work, per CLAUDE.md).
 */

#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdram1), okay)
GROUP_START(SDRAM1)

	SECTION_PROLOGUE(_STM32_SDRAM1_SECTION_NAME, (NOLOAD),)
	{
		*(.lvgl_buf)
	} GROUP_LINK_IN(SDRAM1)

GROUP_END(SDRAM1)
#endif
