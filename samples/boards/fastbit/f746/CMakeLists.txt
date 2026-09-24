# Copyright (c) 2026 Fastbit Embedded Technologies Pvt Ltd.
# All rights reserved.
#
# Proprietary and confidential. Use, reproduction, and distribution
# restricted to the terms of the Fastbit Services Agreement.
#
# LCD work for the F746G-DISCO, copied from the ota-lcd project. Source
# reference only -- no entry point (src/main.c) or OTA/WiFi backend
# stand-ins included, so this target does not build/flash standalone.

cmake_minimum_required(VERSION 3.20.5)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fastbit_f746_lcd_work LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/app/lcd_status_wiring_f746g_zephyr.c
    src/app/lcd_logo.c
    src/app/lcd_icon_thermometer.c
    src/app/lcd_hello_f746g_zephyr.c
    src/app/lcd_text.c
    src/app/font5x7.c
)

if(CONFIG_LV_Z_VBD_CUSTOM_SECTION)
    zephyr_linker_sources_ifdef(CONFIG_LV_Z_VBD_CUSTOM_SECTION
        SECTIONS linker/lvgl_buf_f746g.ld)
endif()

target_include_directories(app PRIVATE src)
