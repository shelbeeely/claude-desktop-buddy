# M5Paper v1.1 board notes

`env:m5paper_v11` (platformio.ini) — the classic ESP32 (ESP32-D0WDQ6) M5Paper v1.1,
540x960 16-gray ED047TC1 panel behind an IT8951E timing controller. Third MCU family
in this firmware (X3/X4 are ESP32-C3, X4 Pro is ESP32-S3), so it's always its own
binary; same `main.cpp` as the other boards.

## BLE: restoring the Bluedroid stack in `src/ble_bridge.cpp`

arduino-esp32's BLE library backs onto one of two IDF Bluetooth host stacks,
chosen by the active core's sdkconfig, not by board. The pioarduino/ESP-IDF 5.x
core defaults ESP32-C3/S3 targets (env:xteink, env:xteink_x4pro) to NimBLE
(smaller footprint, and the only stack on BLE-only silicon like the C3). Classic
ESP32 (this board) has no such constraint and defaults to Bluedroid instead — the
same stack this project's BLE bridge used before the X4 port dropped M5StickCPlus
support and simplified `ble_bridge.cpp` down to NimBLE-only (see
`git log --oneline -- src/ble_bridge.cpp`, commit "Drop M5StickCPlus support —
Xteink X4 only").

This unit restores the Bluedroid branch, re-adapted into the current file's
structure rather than pasted back verbatim. The two stacks hand different
parameter types to the same callback names, verified against the vendored
`framework-arduinoespressif32` headers directly (not from memory of the old
code) — `/root/.platformio/packages/framework-arduinoespressif32/libraries/BLE/src/`:

| Callback | Bluedroid signature | NimBLE signature |
|---|---|---|
| `BLEServerCallbacks::onMtuChanged` | `(BLEServer*, esp_ble_gatts_cb_param_t*)` | `(BLEServer*, ble_gap_conn_desc*, uint16_t)` |
| `BLESecurityCallbacks::onAuthenticationComplete` | `(esp_ble_auth_cmpl_t)` | `(ble_gap_conn_desc*)` |
| `BLESecurity::setEncryptionLevel` | exists (`esp_ble_sec_act_t`) | no equivalent — MITM/bonding is fully specified via `setAuthenticationMode()` |
| bond clearing | `esp_ble_get_bond_device_num()` + `esp_ble_get_bond_device_list()` + `esp_ble_remove_bond_device()` per bond (`esp_gap_ble_api.h`) | `ble_store_clear()` (`host/ble_store.h`) |

(BLEServer.h:278/289, BLESecurity.h:117/209/219, esp_gap_ble_api.h:3387/3397/3412.)

`src/ble_bridge.cpp` now `#include`s `<sdkconfig.h>` and branches every
stack-specific spot on `CONFIG_BLUEDROID_ENABLED` / `CONFIG_NIMBLE_ENABLED`
(the pattern the pre-simplification version already used), matching this
project's own rule that main.cpp capability branches are runtime checks, not
`#ifdef` — this file is the documented exception, since stack selection is a
compile-time toolchain choice, not a runtime board capability.

The constants shared by both branches (`ESP_GATT_PERM_READ_ENCRYPTED`,
`ESP_IO_CAP_OUT`, `ESP_LE_AUTH_REQ_SC_MITM_BOND`, `ESP_BLE_ENC_KEY_MASK`,
`ESP_BLE_ID_KEY_MASK`) resolve natively under Bluedroid (`esp_gap_ble_api.h`,
`esp_bt_defs.h`, `esp_gatt_defs.h`) and are defined as NimBLE-compat aliases
under NimBLE (`BLESecurity.h:56-76`), so the unguarded parts of `bleInit()`
compile unchanged against either stack.

Verified by build: `pio run -e m5paper_v11` links the Bluedroid branch
(the binary contains `"[ble] cleared %d bond(s)"`), while `pio run -e xteink`
and `pio run -e xteink_x4pro` still link the NimBLE branch (`"[ble] cleared
bonds"`) — confirmed with `strings` on each `firmware.elf`. All three build
clean.

## Power latch (GPIO2)

M5Paper latches its own power through a MOSFET on GPIO2 — if it isn't driven
HIGH within the first few ms of boot, the board powers off the instant USB is
unplugged (freeink-sdk `platformio.sample.ini` "POWER LATCH" comment;
`BoardConfig.h:1173-1178,1226-1228`, profile field `power.latch0 = {2}`).

This is already an SDK-level concept, not something this firmware needed to
invent: `BoardConfig::holdPowerRails()` (`BoardConfig.h:1663-1682`) asserts
every board's `power.latch0`/`latch1` pins and is a documented no-op when
both are `PIN_UNASSIGNED` — true for X3/X4/X4 Pro's profiles, so it was never
called anywhere in this project's `main.cpp` before this unit. `main.cpp
setup()` now calls `BoardConfig::holdPowerRails()` as its very first line
(before `Serial.begin()`), unconditionally — a runtime no-op on every board
except M5Paper, so no `#ifdef` was needed, matching this project's
capability-branching convention.

## Classic-ESP32 gotchas checked against `freeink-sdk/docs/consumer-mcu-portability.md`

That doc describes chip-specific patterns found in a *different* consumer
(CrossPoint's `lib/hal/*`), not in this repo or in the freeink-sdk libraries
this project links. Checked each pattern against this project's own source
and the linked SDK libs:

- **Deep-sleep GPIO wakeup ("any low" mode).** `freeink-sdk/libs/hardware/
  PowerManager/src/PowerManager.cpp:14-33` already branches on
  `SOC_PM_SUPPORT_EXT1_WAKEUP` vs. `SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP`, and
  within the ext1 branch further branches on `CONFIG_IDF_TARGET_ESP32`
  (classic ESP32 gets `ESP_EXT1_WAKEUP_ALL_LOW`, since that SoC's RTC has no
  "any low" mode; S2/S3 get `ESP_EXT1_WAKEUP_ANY_LOW` directly) —
  `PowerManager.cpp:18-26`. `main.cpp`'s `powerOffSequence()` calls
  `freeink::PowerManager::deepSleepUntilPowerButton()` (`main.cpp:969`)
  unchanged; no consumer-side change was needed or made.
- **Hardcoded C3 flash/IO pin (`GPIO_NUM_13`).** Searched this project's
  `src/` and every freeink-sdk lib in this env's `lib_deps`
  (`grep -rn GPIO_NUM_13`) — no match. The pattern the doc flags lives in
  CrossPoint's `HalPowerManager.cpp`, which is not part of this codebase.
  Nothing to fix.
- **Panic backtrace.** The doc's RISC-V-specific
  `__wrap_panic_print_backtrace` override is also CrossPoint-only; this
  project has never defined one (`grep -rn panic_print_backtrace` across
  `src/` and the linked libs: no match), so classic ESP32 gets the stock
  arduino-esp32/ESP-IDF Xtensa panic handler by default — nothing to add.

## PSRAM

M5Paper's 960x540 landscape framebuffer (~63 KB) doesn't fit in classic
ESP32's ~300 KB DRAM alongside the firmware, WiFi, and IDF. FreeInk
auto-enables `FREEINK_FB_PSRAM` for this device (`BoardConfig.h:295`,
`#define FREEINK_FB_PSRAM (FREEINK_DEVICE_M5PAPER || FREEINK_DEVICE_PAPERMONO)`)
and allocates the framebuffer from the PSRAM heap in `begin()`
(freeink-sdk README "Framebuffer placement (`FREEINK_FB_PSRAM`)"). That only
needs `-DBOARD_HAS_PSRAM` turned on at build time — no `main.cpp` code — which
`env:m5paper_v11` sets. Confirmed by a clean `pio run -e m5paper_v11` build
(RAM 22.7%, Flash 29.3% used — see PR description for full size tables).

## Serial / USB build flags

`env:xteink`/`env:xteink_x4pro` set `-DARDUINO_USB_MODE=1
-DARDUINO_USB_CDC_ON_BOOT=1` because the C3/S3 have a native USB-OTG/HW-CDC-JTAG
peripheral and those flags pick which personality `Serial` gets
(arduino-esp32 `cores/esp32/HardwareSerial.h:424-439`). Classic ESP32 has no
such peripheral; setting `ARDUINO_USB_CDC_ON_BOOT=1` there routes `Serial` to
`HWCDCSerial`, which this core doesn't compile in for a non-native-USB target
— confirmed by a failing build (`'HWCDCSerial' was not declared in this
scope`) before removing the flags. `env:m5paper_v11` leaves both unset, so
`Serial` falls through to `Serial0` (UART0) — the correct default for this
board's USB-UART bridge.

## Input coverage: UP/DOWN/CONFIRM/POWER work, BACK/LEFT/RIGHT do not

M5Paper's only physical input is a 3-position rotary wheel (`InputStyle::
DigitalButtons`, `BoardConfig.h:1182`). Its pin map
(`BoardConfig.h:1199`, `{back, confirm, left, right, up, down, power,
activeLow}` order per the struct at `BoardConfig.h:471-477`) is
`{PIN_UNASSIGNED, 38, PIN_UNASSIGNED, PIN_UNASSIGNED, 37, 39, 38, false}`:

- `confirm` = GPIO38, `up` = GPIO37, `down` = GPIO39, `power` = GPIO38
  (shared with `confirm` by design — the wheel only has 3 electrical
  positions: push, left, right — `BoardConfig.h:1191-1199`).
- `back`/`left`/`right` are `PIN_UNASSIGNED` — no wheel GPIO exists for them.

`main.cpp`'s existing `popPress()`-based button handling
(`BoardConfig::ACTIVE.input.*` → `InputManager::BTN_*`) needs no board-specific
code to pick these up, so UP/DOWN/CONFIRM/POWER work unmodified on this board.

BACK/LEFT/RIGHT would need to come from the GT911 touch panel instead, the
way the X4 Pro gets BACK from `input.wasHomeKeyTapped()`. Checked M5Paper's
`TouchConfig` (`BoardConfig.h:1214-1216`) against the X4 Pro's
(`BoardConfig.h:1429-1448`) field-by-field
(`TouchConfig` layout at `BoardConfig.h:482-519`):

| Field | M5Paper v1.1 | X4 Pro |
|---|---|---|
| `synthesizeConfirm` | `false` | `false` |
| `hasHomeKey` | `false` (default, unset) | `true` |

Neither board synthesizes CONFIRM from a touch tap at the SDK level today
(`synthesizeConfirm` is `false` on both), and M5Paper's `hasHomeKey` is also
`false` — unlike the X4 Pro, there is no home-key bit this board's GT911
firmware reports that `InputManager::wasHomeKeyTapped()` could surface. So,
per this batch's "scope down and document rather than invent an unverified
tap-zone system" guidance: **BACK/LEFT/RIGHT are not reachable on M5Paper
v1.1 in this pass.** Wiring them up would mean adding a genuinely new
touch-to-button mapping (e.g. reading raw GT911 tap coordinates and defining
on-screen zones) that doesn't exist anywhere else in this codebase yet and
wasn't verified against real hardware — left out of scope here rather than
guessed at, so it doesn't compound the risk of the BLE stack restoration
above (this unit's primary risk).
