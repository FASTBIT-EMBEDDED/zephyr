# F746G-DISCO LCD work (source reference)

Source files implementing the F746G-DISCO LCD status display screen,
copied here for reference.

## Normal LCD work (display bring-up, no OTA involved)

Standalone hardware verification that the display itself works,
before the real OTA status screen was built on top of it.

- `src/app/lcd_hello_f746g_zephyr.c` -- Check 6-style bring-up test:
  fills the framebuffer and draws confirmation text directly, to prove
  the panel/LTDC/SDRAM chain works before any real UI is built.
- `src/app/lcd_hello.h` -- shared interface header (same one both the
  F429 and F746G bring-up tests use).
- `src/app/lcd_text.c` / `src/app/lcd_text.h` -- bare-metal 5x7 bitmap
  font renderer, used by the bring-up test above.
- `src/app/font5x7.c` / `src/app/font5x7.h` -- the actual font glyph
  data `lcd_text.c` renders from.

## OTA LCD work (the real status screen)

The actual demo UI: buttons, temperature/humidity graph, DHT connection
status, uptime, WiFi icon, and OTA state/progress display.

- `src/app/lcd_status_wiring_f746g_zephyr.c` -- main screen logic. Builds
  the whole screen out of LVGL widgets, reacts to OTA state/progress
  callbacks, polls the DHT11 sensor, drives the WiFi icon.
- `src/app/lcd_status_wiring.h` -- shared interface header exposing the
  three public entry points (`lcd_status_wiring_init()`,
  `lcd_status_on_ota_state_zephyr()`, `lcd_status_on_progress_zephyr()`).
- `src/app/lcd_logo.c` / `src/app/lcd_logo.h` -- Fastbit logo image asset
  (splash screen + small header logo).
- `src/app/lcd_icon_thermometer.c` / `src/app/lcd_icon_thermometer.h` --
  thermometer icon image asset shown next to the temperature reading.

## Entry point

- `src/main.c` -- reproduces the exact LCD call pattern from the
  original application's own `main.c`: `CONFIG_APP_LCD_TEST` gates the
  bring-up test (`lcd_hello_run()`), `CONFIG_APP_LCD_STATUS` gates the
  real status screen (`lcd_status_wiring_init()`). The original
  `main.c` also wires up `app_init()`, the watchdog, and demo-mode
  heartbeat -- none of that is LCD work, so it is not reproduced here,
  only these two `#if` blocks are.

## Supporting headers (trimmed to only what the LCD work actually calls)

Not LCD work -- these declare the pre-existing OTA/WiFi middleware
types and functions that `lcd_status_wiring_f746g_zephyr.c` calls into.
Each header below has been trimmed down from the original (which also
declares a lot of unrelated OTA-only API surface) to just the
declarations the LCD screen code actually references -- confirmed by
checking every call site, then verified the trim compiles to the exact
same object code size as the untrimmed version.

- `src/drivers/wifi_link.h` -- trimmed to `wifi_link_is_connected()` and
  `wifi_link_set_event_cb()` only (the full header also declares
  `wifi_link_init()`, `wifi_link_connect()`, `wifi_link_disconnect()`,
  none of which the LCD screen calls directly).
- `src/middleware/ota_manager.h` -- trimmed to just the `ota_state_t` /
  `ota_result_t` enum definitions the screen displays (the full header
  is 665 lines: manifest types, policy/deps structs, and the entire OTA
  manager function API, none of which the LCD screen references).
- `src/middleware/ota_manager_zephyr.h` -- unchanged, already minimal:
  declares only `ota_manager_trigger_check()` and
  `ota_manager_trigger_download()`, both called from the screen's
  CHECK/DWD button handlers.

## Build/hardware scaffolding

Not LCD logic -- configuration and hardware wiring the LCD work depends
on to actually build and run on real F746G-DISCO hardware.

- `boards/stm32f746g_disco.overlay` -- devicetree: the DHT11 sensor's
  GPIO wiring (PA0) and the LTDC black background color (hides brief
  FIFO underruns against this dark UI).
- `linker/lvgl_buf_f746g.ld` -- routes LVGL's rendering buffer into
  external SDRAM; it does not fit in this board's internal SRAM
  alongside the rest of the app.
- `CMakeLists.txt` -- lists which of the above source files to compile.
- `prj.conf` -- enables the Kconfig options the LCD work depends on
  (display, LVGL, DHT/sensor subsystem, touch input).
- `Kconfig` -- declares four small app-level symbols
  (`APP_LOG_LEVEL`, `APP_OTA_DEVICE_VERSION`, `APP_LCD_TEST`,
  `APP_LCD_STATUS`) that the copied source files reference directly and
  which normally come from the original application's own
  `app/Kconfig`.

## Build status (verified)

| Build config | Result |
|---|---|
| Default (`CONFIG_APP_LCD_TEST=n`, `CONFIG_APP_LCD_STATUS=n`) | Builds clean |
| `-DCONFIG_APP_LCD_TEST=y` (normal LCD bring-up) | Builds clean, flashes and runs standalone on real hardware, no OTA/WiFi dependency at all |
| `-DCONFIG_APP_LCD_STATUS=y` (OTA status screen) | Fails to link -- undefined references to `wifi_link_is_connected`, `wifi_link_set_event_cb`, `ota_manager_trigger_check`, `ota_manager_trigger_download` |

## Why the OTA status screen doesn't build, and why that can't be trimmed away

`lcd_status_wiring_f746g_zephyr.c` genuinely calls those four functions.
Unlike the headers above, their real implementations
(`wifi_link_esp_at_zephyr.c`, `ota_manager_zephyr.c`) cannot be trimmed
down the same way: each function body reads/writes shared internal
state (a connection-status struct, semaphores) that is only meaningful
alongside the rest of that file's real logic (the actual WiFi driver,
the actual OTA worker thread). Pulling just these four function bodies
out, without that surrounding logic, would not compile (the shared
state they reference is declared nowhere else) -- and even if it were
made to compile, it would do nothing real (a semaphore nobody waits on,
a status flag nothing ever updates).

So making `CONFIG_APP_LCD_STATUS=y` build here requires one of:

1. Fake stand-in bodies for those four functions -- display-only, no
   real network/OTA behavior, or
2. The original application's own `main.c` plus the real, complete
   `wifi_link_esp_at_zephyr.c` and `ota_manager_zephyr.c`
   implementations -- full real OTA/WiFi behavior.

Neither is included in this copy.
