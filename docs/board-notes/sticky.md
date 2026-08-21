# Sticky (env:sticky)

> **UNVERIFIED — "Upcoming Device."** freeink-sdk's own supported-devices
> table lists Sticky as `(Upcoming Device)`, and its `BoardConfig.h` profile
> says outright that orientation, SD bus-sharing, and the PDM mic pins are
> **"pending hardware validation"** (`freeink-sdk/libs/hardware/BoardConfig/
> include/BoardConfig.h:1304-1310`). No Sticky unit has run this firmware, or
> any freeink-sdk firmware — this port was built from the SDK's documentation
> and a vendor pin-config demo only, the same way this project's X4 Pro
> support was built before it, but with one more degree of speculation on
> top: even freeink-sdk's own authors haven't validated Sticky on real
> hardware yet. Treat every claim below as "what the SDK's docs say," not
> "what a Sticky does." Re-verify against a real unit before relying on this
> for anything that matters — in particular the SD SPI-bus-sharing note and
> the GPIO0 strapping-pin note near the bottom, which are real ways to brick
> or corrupt a boot if wrong.

## What this port wires up

Everything below reuses code paths this project already built for X3 (real
RTC/IMU support) and X4 Pro (a second ESP32-S3 env) this session. No new
`src/` logic was needed for RTC, IMU, or battery — only the new
`[env:sticky]` block in `platformio.ini` and a couple of generalizations to
`src/main.cpp` that previously hardcoded "X4 Pro" where they meant something
more general (see "main.cpp changes" below).

| Capability | Status | Evidence |
|---|---|---|
| Display (SSD1677, 800x480) | Wired, reuses the X4/de-link SSD1677 driver | `BoardConfig.h:1295-1322` |
| GT911 touch (own I2C bus) | Wired, reuses X4 Pro's GT911 code path | `BoardConfig.h:1334-1342` |
| RTC (PCF8563 @ 0x51) | Wired, zero new code — gated by `BoardConfig::hasRtc()` | `BoardConfig.h:1358-1359` |
| IMU (LSM6DS3TR-C @ 0x6A) | Wired, zero new code — gated by `BoardConfig::hasImu()` | `BoardConfig.h:1358-1359` |
| Battery gauge (BQ27220 @ 0x55, Wire1) | Wired, reuses BatteryMonitor's I2C-gauge path | `BoardConfig.h:1348-1353` |
| SD card | Wired **as SPI**, not SDMMC — see below | `BoardConfig.h:1300, 1322-1326, 1347` |
| Power latch (GPIO45/46) | Wired, reuses the generic `holdPowerRails()` | `BoardConfig.h:1361-1363` |
| Buzzer (LEDC, GPIO48) | **Out of scope** — no Buzzer abstraction in this codebase | `BoardConfig.h:747-752` |
| PDM microphone (GPIO19/20/38) | **Out of scope** — no Microphone abstraction in this codebase | `BoardConfig.h:1354-1356` |
| Temp/humidity (SHT40 @ 0x44) | **Out of scope** — no EnvironmentSensor abstraction in this codebase | `BoardConfig.h:1358-1359` |
| Frontlight | N/A — Sticky has none (`NO_FRONTLIGHT`; the charge LED is board-support, not a frontlight) | `BoardConfig.h:1343` |

Sound and environment-sensor support were dropped from this codebase earlier
this session along with M5 support, and re-adding a Microphone/Buzzer/
EnvironmentSensor abstraction is explicitly out of scope for this change —
this port only gets display, touch, RTC, IMU, battery, SD, and BLE working.

## Important: SD card is SPI, sharing the display's bus — NOT SDMMC

freeink-sdk's own `platformio.sample.ini` and the top-level task brief that
kicked off this port both describe Sticky's storage loosely as "native
SDMMC." **That is not what `BoardConfig.h`'s Sticky profile actually says.**
The profile is explicit and consistent on this point:

- `NO_SDMMC,  // SD is SPI, not 4-bit SDMMC` (`BoardConfig.h:1347`)
- The `sd` field's `separateSpi` is `false` and its `sclk`/`mosi` pins
  (13/14) are the *same* pins as the display's `sclk`/`mosi` (`BoardConfig.h:
  1318, 1326`) — i.e. the SD card is wired onto the display's own SPI bus,
  exactly like X3/X4.
- The profile's own comment: *"MicroSD shares the display SPI bus; the
  vendor demo doesn't exercise SD, so bus-sharing / CS arbitration with the
  panel needs a hardware check."* (`BoardConfig.h:1307-1308`)

So Sticky needs the same "pre-claim the shared SPI bus with SD's MISO pin
before `display.begin()`" sequence X3/X4 use, **not** the "skip the pre-claim,
SD lives on entirely separate SDMMC pins" path X4 Pro/de-link/Paper Mono use.
`src/main.cpp`'s `setup()` guard was generalized accordingly (see below) —
Sticky was deliberately **left inside** the pre-claim branch, not excluded
from it. If a real Sticky unit's SD turns out not to arbitrate cleanly with
the panel on a shared bus (the exact risk the SDK's own comment flags), that
is a hardware bring-up problem to debug on real hardware, not something this
firmware can route around by pretending the card is SDMMC when the profile
says it isn't.

## main.cpp changes

Two small generalizations, both because Sticky is the first non-X4-Pro
ESP32-S3 board and the first non-X3/X4 board sharing the display/SD SPI bus:

1. **SPI pre-claim guard** (`setup()`): previously `if
   (!BoardConfig::isX4Pro())`, which really meant "does this board need the
   shared-bus pre-claim." Generalized to check
   `BoardConfig::ACTIVE.sdmmc.busWidth == 0` — the same "is this board native
   SDMMC" test `SDCardManager` itself uses — instead of hardcoding another
   board name. This correctly keeps Sticky on the pre-claim path (see above)
   while still skipping it for X4 Pro (`sdmmc.busWidth == 1`,
   `freeink-sdk/docs/xteink-x4pro-support.md` "Storage"). Two sibling PRs
   (de-link, Paper Mono) generalize this same guard independently — expect a
   merge conflict on this hunk, which is expected and fine to resolve by
   keeping the `sdmmc.busWidth` check.
2. **About-screen MCU label**: previously `BoardConfig::isX4Pro() ? "ESP32-S3"
   : "ESP32-C3"`. Sticky is also ESP32-S3 (`BoardConfig.h:1295` header:
   "ESP32-S3R8"), so the ternary now checks `isX4Pro() || isSticky()`.

RTC and IMU needed **no changes**: `rtcBegin()`/`imuBegin()`/
`getSoftClock()`/`checkShake()` already gate on `BoardConfig::hasRtc()`/
`hasImu()`, which read `ACTIVE.sensors.rtcAddr != 0` /
`ACTIVE.sensors.imuAddr != 0`. Sticky's `sensors` field sets `rtcAddr =
0x51` and `imuAddr = 0x6A` (`BoardConfig.h:1358-1359`), so both resolve to
real hardware automatically. Confirmed by the `pio run -e sticky` build
succeeding with no source changes needed beyond the two generalizations
above.

## GPIO0 strapping-pin caveat (real risk if this is ever built for hardware)

The BQ27220 fuel gauge's I2C bus uses `SDA=GPIO1, SCL=GPIO0`
(`BoardConfig.h:1348-1350`). **GPIO0 is an ESP32-S3 boot-strapping pin** —
its level during reset selects the boot mode. The profile's own comment
warns: *"the board init must not leave a pull state that corrupts boot
mode."* This firmware does nothing today that would drive or pull GPIO0
outside of the standard I2C `Wire1.begin()` call the gauge code makes after
boot has already completed, so there is no known conflict in the code as
written — but this is exactly the kind of thing that only shows up on real
hardware (a gauge driver retry loop that toggles the pin unexpectedly, a
brown-out reset mid-I2C-transaction, etc.). Anyone bringing this up on actual
Sticky hardware should watch for boot-mode corruption symptoms (device
failing to start normally after a reset that lands mid-I2C-transaction on
this bus) before assuming the gauge code is otherwise fine.

## Also pending hardware validation (inherited from the SDK, unchanged here)

These are called out in `BoardConfig.h:1304-1310` and this port does nothing
to resolve them — they are genuine unknowns until someone has a unit:

- **Panel orientation**: mount transform unknown; ships `NO_FLIP`. If a real
  unit's "up" doesn't match the reader's expected up, set `ROTATE_180` (or a
  mirror) in the profile once confirmed.
- **PDM mic pins** (GPIO19/20/38): sourced from schematic/spec only, no
  vendor demo exercises the mic. Moot for this port since mic support is out
  of scope here regardless.

## Build

```
export CURL_CA_BUNDLE=/root/.ccr/ca-bundle.crt SSL_CERT_FILE=/root/.ccr/ca-bundle.crt REQUESTS_CA_BUNDLE=/root/.ccr/ca-bundle.crt
pio run -e sticky
```

`pio run -e sticky` builds successfully (ESP32-S3, `esp32-s3-devkitc1-n16r8`,
16 MB flash / 8 MB PSRAM, `default_16MB.csv` partitions) — see the PR
description for the RAM/Flash usage lines. A successful build only proves the
firmware compiles and links against the profile as documented; it proves
nothing about whether that profile matches a real board, because no real
board has run this yet.
