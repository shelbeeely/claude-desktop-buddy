# LilyGo T5 S3 — board note

**No unit of this board has been touched by this port. Every claim below is
copied from freeink-sdk source/docs, never bench-verified against real
hardware in this repo. `pio run -e lilygo_t5s3` succeeds; nothing beyond that
has been confirmed. Treat this env as "compiles, needs hardware bring-up"
until someone flashes a real T5 S3 and reports back.**

## Correction to the work-order for this unit

The task this port was scoped from states freeink-sdk has "NO working sample
environment" and "no reference implementation anywhere" for this board. That
was true at some point, but is **not** true of the freeink-sdk commit this
port built against (submodule pinned at
`ffeaaa271231d865590f8c54ea45ec02b1342d4e`). That checkout ships:

- `freeink-sdk/docs/lilygo-t5s3-support.md` — a full support doc.
- `freeink-sdk/libs/hardware/BoardT5S3/` — a complete board-support library:
  `BoardT5S3Pins.h` (every pin/address), `LilyGoT5S3LgfxConfig.cpp` (the full
  `LgfxEpdConfig` — parallel bus pins, PCA9535 setup, TPS65185 power sequence,
  a hand-tuned fast-refresh LUT), and `BoardT5S3.cpp/h` (I2C bus bring-up,
  PCA9535 register access, the user-button hook, LoRa/GPS pin-down, SD SPI
  bus prep).
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h` — a complete
  `BoardProfile::LILYGO_T5S3` (line 1119) and `TouchConfig::LILYGO_T5_PRO_GT911`
  (line 702), both with the same "measured on hardware" / "vendor schematic"
  citations this SDK uses for boards it treats as bring-up-complete.

**What is still true and still the point of "highest risk in the batch": none
of this was verified against a physical T5 S3 by *this port*.** The SDK's own
comments (quoted below) show real hardware work went into these files at some
point, but this session did not run anything on a board — it read source and
compiled. Given that, this port made one deliberate, load-bearing decision:
**reuse freeink-sdk's `BoardT5S3` library instead of hand-authoring a new
`LgfxEpdConfig`.** The work order asked for a new `src/lilygo_t5s3_config.cpp`
under the (reasonable, given the stated premise) assumption that no such
config existed anywhere. Since one does, and it's a ~180-line struct
literal plus a PMIC register sequence, retyping it into a second file
strictly increases risk (transcription error, drift from the SDK's own
copy) for zero benefit — so `platformio.ini` links `BoardT5S3` as a
`lib_deps` symlink, exactly the pattern freeink-sdk's own `env:papers3`
uses for `BoardPaperS3` (freeink-sdk/platformio.sample.ini:216-219). If a
reviewer wants the config physically duplicated into this repo instead of
referenced from the submodule, that's a mechanical follow-up, not a
correctness question.

## Sources consulted (file:line)

- `freeink-sdk/docs/lilygo-t5s3-support.md` — narrative doc, driver + power +
  peripherals + input summary.
- `freeink-sdk/libs/hardware/BoardT5S3/include/BoardT5S3Pins.h:1-71` — every
  pin number and I2C address used below.
- `freeink-sdk/libs/hardware/BoardT5S3/src/LilyGoT5S3LgfxConfig.cpp:1-184` —
  the `LgfxEpdConfig` (bus pins, LUT) and the PCA9535+TPS65185 power sequence
  (`prepareEpdPower`/`epdPowerOn`/`epdPowerOff`).
- `freeink-sdk/libs/hardware/BoardT5S3/src/BoardT5S3.cpp:1-163` — I2C bus
  bring-up, PCA9535 register access, the user-button hook, LoRa/GPS pin-down.
- `freeink-sdk/libs/hardware/BoardT5S3/include/BoardT5S3.h:1-31` — the public
  API this port calls from `main.cpp`.
- `freeink-sdk/libs/display/FreeInkDisplay/include/LgfxEpdConfig.h:1-42` —
  the `LgfxEpdConfig`/`LgfxEpdPowerHooks` struct definitions (field order
  used to sanity-check `LilyGoT5S3LgfxConfig.cpp`'s struct literal by hand).
- `freeink-sdk/libs/display/FreeInkDisplay/src/driver/LgfxEpdDriver.cpp:319-345`
  — confirms `#if FREEINK_DEVICE_LILYGO` alone selects
  `lilygoT5S3LgfxConfig()` (the `-DFREEINK_LGFX_EPD_CONFIG=` flag this port
  also sets is redundant but harmless, and matches the SDK's own commented
  sample literally).
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:1111-1161` —
  `BoardProfile::LILYGO_T5S3` (geometry, input pins, battery gauge, RTC,
  power latch, bezel insets).
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:693-704` —
  `TouchConfig::LILYGO_T5_PRO_GT911` (touch pins/orientation,
  `synthesizeConfirm=false`, `hasHomeKey=true`).
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:174-322` — the
  `FREEINK_CAP_*`/`FREEINK_BATTERY_I2C_GAUGE`/`FREEINK_LOG_TRANSPORT` derivation
  macros that key off `FREEINK_DEVICE_LILYGO`.
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:611-619,
  1663-1680` — `PowerConfig`/`holdPowerRails()` (the GPIO2 power-latch
  requirement).
- `freeink-sdk/libs/display/FreeInkDisplay/src/driver/LgfxEpdDriver.cpp:118-140`
  — confirms the grayscale canvas is `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`,
  which is why this env's `-DBOARD_HAS_PSRAM` was added beyond what the SDK's
  commented sample literally shows.
- `freeink-sdk/platformio.sample.ini:141-162` — the commented-out
  `[env:lilygo_t5s3]` template this env's `build_flags`/`lib_deps` are modeled
  on, plus `platformio.sample.ini:216-219` (`env:papers3`) for the
  symlink-a-board-lib pattern this port copied.
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:1429-1448`
  (`XTEINK_X4_PRO`'s touch config) and this repo's `platformio.ini` (pre-change,
  `env:xteink_x4pro` comment) — cross-checked to resolve the CONFIRM-via-touch
  question below; see "Input coverage".

## What this port did

1. **`platformio.ini`**: appended `[env:lilygo_t5s3]` (ESP32-S3,
   `esp32-s3-devkitc1-n16r8`, `default_16MB.csv`, `-DFREEINK_DEVICE_LILYGO=1`,
   `-DFREEINK_LGFX_EPD_CONFIG=lilygoT5S3LgfxConfig`, `m5stack/M5GFX @ 0.2.20`,
   plus this repo's shared `lib_deps`) and one new `lib_deps` line —
   `BoardT5S3=symlink://freeink-sdk/libs/hardware/BoardT5S3` — to link the
   board-support library described above. Nothing in the header or the
   existing `[env:xteink]` / `[env:xteink_x4pro]` blocks was touched.
2. **`src/main.cpp`**, three small, board-gated additions (all cited inline
   at the call site):
   - `BoardConfig::holdPowerRails();` as the very first line of `setup()`,
     unconditional. `LILYGO_T5S3.power.latch0 = GPIO2`
     (BoardConfig.h:1155) — without driving it HIGH first, the board loses
     power the instant USB is unplugged (same requirement as Sticky/M5Paper
     v1.1, per `holdPowerRails()`'s own doc comment at BoardConfig.h:1663-1666).
     This is a no-op on every other board in this repo (their `power.latch0`
     defaults to `PIN_UNASSIGNED`), so it's safe to call unconditionally
     rather than gating it — same reasoning the SDK itself gives for
     `selectXteinkDevice()`'s unconditional call.
   - Excluded LilyGo T5 S3 from the existing
     `if (!BoardConfig::isX4Pro()) { SPI.begin(...); }` guard. That call uses
     `BoardConfig::ACTIVE.display.{sclk,mosi,cs}`, which are all
     `PIN_UNASSIGNED` for this board (the panel rides the parallel/i80 bus,
     not SPI) — calling `SPI.begin()` with `-1` pins would be wrong. This
     board's SD card is plain SPI on its own pins
     (`BoardT5S3Pins.h:27-29,34`), brought up by `BoardT5S3::begin()` instead.
   - `#if FREEINK_DEVICE_LILYGO` `BoardT5S3::begin();` `#endif`, right before
     `display.begin()`. This is the one `#ifdef` in the whole diff, and it's
     a compile-unit gate, not a behavior gate: `BoardT5S3.h` is a symbol that
     genuinely does not exist outside this one env (its `lib_deps` isn't
     linked into `env:xteink`/`env:xteink_x4pro`), the same reason
     freeink-sdk's own `LgfxEpdDriver.cpp:325` gates `<M5GFX.h>` behind
     `#if FREEINK_DEVICE_LILYGO` rather than a runtime check. It has to run
     before `display.begin()`: the EPD power hooks
     (`prepareEpdPower`/`epdPowerOn`, `LilyGoT5S3LgfxConfig.cpp:86-151`) talk
     to the PCA9535/TPS65185 over `Wire`, and `BoardT5S3::begin()` is what
     starts `Wire` (`BoardT5S3.cpp:86-91`, SDA39/SCL40 @ 400 kHz).
3. **No `LgfxEpdConfig` file was hand-authored** — see "Correction to the
   work-order" above.
4. **`docs/board-notes/lilygo-t5s3.md`** (this file).
5. **`README.md` was not touched**, per the work order.

## Left unspecified — genuinely not documented anywhere in the SDK

- **PSRAM memory type** (`board_build.arduino.memory_type`, e.g. `qio_opi`).
  freeink-sdk's `env:papers3` sample explicitly sets `qio_opi` with a comment
  that M5GFX "hard-requires OPI PSRAM" for `board_M5PaperS3`'s autodetect
  profile. Nothing in `lilygo-t5s3-support.md`, `BoardT5S3Pins.h`, or
  `LilyGoT5S3LgfxConfig.cpp` says whether the T5 S3's PSRAM is quad or octal,
  or whether M5GFX's generic (non-autodetected, since this env doesn't use
  M5Unified board detection) `Panel_EPD`/`Bus_EPD` path cares. This env
  leaves `memory_type` at the board manifest's default (matching this repo's
  `env:xteink_x4pro`, the only other env on the same `esp32-s3-devkitc1-n16r8`
  part) rather than guess `qio_opi`. **The build succeeded** with the
  default, which is weak evidence the default is fine, but a link-time
  success says nothing about whether PSRAM actually initializes correctly at
  runtime on real T5 S3 silicon — needs hardware bring-up to know for sure.
- **Whether `BOARD_HAS_PSRAM` alone is sufficient**, or whether this board
  additionally needs `-DARDUINO_RUNNING_CORE`/octal-PSRAM-specific flags some
  ESP32-S3 boards require. Not documented for this board anywhere in the SDK;
  added `-DBOARD_HAS_PSRAM` only because the driver's
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` calls
  (`LgfxEpdDriver.cpp:138-139`) are unconditional and would silently return
  `nullptr` without it — a documented *code requirement*, not a guessed
  *hardware* value. Whether that's the complete PSRAM story for this specific
  module needs a real board to confirm.
- **Panel rotation / mount orientation on real glass.** `BoardProfile::
  LILYGO_T5S3` carries `NO_FLIP` (BoardConfig.h:1141) and the
  `LgfxEpdConfig.rotation` field is `0`
  (`LilyGoT5S3LgfxConfig.cpp:169` — note this differs from the illustrative
  `/*rotation*/ 1` shown in `lilygo-t5s3-support.md`'s example snippet; the
  *real* shipped file uses `0`, which this port trusts as the authoritative
  value over the doc's example). Nothing in the SDK states this was checked
  against a lit panel. Compare freeink-sdk's own PaperS3 note
  (BoardConfig.h:1248-1251), which explicitly flags its rotation as "pending
  hardware validation" for the *same panel class* — T5 S3's rotation gets no
  such explicit caveat in the SDK, but this port isn't going to upgrade that
  silence to a confirmed claim on its own authority.
- **Everything the SDK itself defers to "board-support (outside the SDK)"**:
  the PCF8563 RTC identity (the doc says the vendor schematic shows PCF8563
  at 0x51 but the vendor's own README/product table says PCF85063 —
  `BoardConfig.h:1145-1150` keeps PCF8563 per the schematic and flags the
  discrepancy explicitly; this port did not attempt to resolve it), and
  LoRa/GPS, which this port does not use and only disables
  (`BoardT5S3::disableGpsLora()`).

## Input coverage

**Working, on paper, without any new code in this repo** (all pre-existing,
generic `main.cpp` dispatch — nothing was written to special-case this board):

- **POWER** — `BoardProfile::LILYGO_T5S3.input.power = GPIO0` (the BOOT
  button), active-low (BoardConfig.h:1131-1132). `main.cpp`'s existing
  `BTN_POWER` handling (sleep on press, `PowerManager::armPowerButtonWakeup()`
  arms the `ext1` deep-sleep wake source on it) is unconditional.
- **BACK** — `TouchConfig::LILYGO_T5_PRO_GT911.hasHomeKey = true`
  (BoardConfig.h:704): the GT911 surfaces its capacitive home-key status bit,
  and `main.cpp`'s `if (input.wasHomeKeyTapped()) dispatchBack(now);` is
  already generic (added for X4 Pro, not board-specific) — no code change
  needed.
- **DOWN** — the PCA9535-expander "real" user button the work order mentions.
  `BoardT5S3::begin()` calls `InputManager::setButtonHook(inputButtonHook)`
  (BoardT5S3.cpp:71,125), and that hook returns the `BTN_DOWN` bit
  (`InputManager::BTN_DOWN`) when the expander button is pressed. `main.cpp`'s
  `pollHold()`-based `BTN_DOWN` handling is generic across boards, so a
  short tap is drained (no bound action today, matching every other board's
  DOWN-tap behavior) and an 800 ms hold triggers the existing "dizzy"
  one-shot animation. This works because this port calls
  `BoardT5S3::begin()`; it is not something this port wrote input logic for.

**NOT mapped — no code exists for these on this board, deliberately not
added:**

- **CONFIRM, LEFT, RIGHT, UP** — `BoardProfile::LILYGO_T5S3.input` has
  `back`/`confirm`/`left`/`right`/`up` all `PIN_UNASSIGNED`
  (BoardConfig.h:1127-1128,1131) — there is no physical GPIO for any of
  these. `TouchConfig::LILYGO_T5_PRO_GT911.synthesizeConfirm = false`
  (BoardConfig.h:703) — the touch backend does **not** synthesize a CONFIRM
  button event on tap for this board. The work order suggested this might
  work "same as X4 Pro"; it does not, on either board: `XTEINK_X4_PRO`'s own
  touch config also has `synthesizeConfirm = false`
  (BoardConfig.h:1439/field 11 of the `XTEINK_X4_PRO.touch` literal) —
  every `TouchConfig` literal in this freeink-sdk checkout has
  `synthesizeConfirm = false`; none currently use the flag. (This repo's own
  pre-existing `platformio.ini` comment on `env:xteink_x4pro` says "CONFIRM
  comes from a synthesized touch tap" — that comment does not match the
  BoardConfig.h source in this SDK checkout. That's a pre-existing
  discrepancy on the X4 Pro env, out of scope for this port, but worth a
  maintainer's attention since it means CONFIRM may not actually work on X4
  Pro either.)
- Per the work order's explicit scope-down guidance, this port did **not**
  build a touch-tap-to-button or hit-testing system to cover CONFIRM/LEFT/
  RIGHT/UP — that would be new input-dispatch surface on top of an already
  high-risk display/power port, which the work order asked to avoid. A menu
  that needs CONFIRM to advance is not reachable by button on this board
  today; only BACK (cycle/dismiss) and DOWN-hold (dizzy) are.

## Battery / RTC

- `BatteryGaugeConfig` — BQ27220 fuel gauge at `0x55`, BQ25896 charger at
  `0x6B`, shared main I2C bus (SDA39/SCL40 @ 400 kHz)
  (BoardConfig.h:1143, `BoardT5S3Pins.h:13-14,23-25`). `BatteryMonitor`
  dispatches on `GaugeType::Bq27220` (the default) automatically —
  `FREEINK_BATTERY_I2C_GAUGE` is derived true for `FREEINK_DEVICE_LILYGO`
  (BoardConfig.h:240-243), no code change needed.
- `SensorsConfig` — PCF8563 RTC at `0x51`, same bus
  (BoardConfig.h:1151, see the PCF8563-vs-PCF85063 caveat above).
  `FREEINK_CAP_RTC` is derived true (BoardConfig.h:258-261); `main.cpp`'s
  existing `rtcBegin()`/`BoardConfig::hasRtc()` gating is generic.

## Code review findings and fixes

A `code-review` pass over the staged diff found and this port fixed three
real issues (a fourth — the `-DFREEINK_LGFX_EPD_CONFIG` flag being read-order
shadowed by the `#if FREEINK_DEVICE_LILYGO` branch in `LgfxEpdDriver.cpp` and
therefore inert — was already disclosed above under "Sources consulted";
`platformio.ini` now carries that same explanation inline too, next to the
flag):

- **`src/main.cpp`'s credits screen mislabeled this board's MCU.** The
  existing `BoardConfig::isX4Pro() ? "ESP32-S3" : "ESP32-C3"` ternary was a
  valid proxy for "is this the S3 board" back when X4 Pro was the only
  ESP32-S3 target; adding a second ESP32-S3 board (this one) without
  updating it would have shown "ESP32-C3" on a LilyGo T5 S3. Fixed by
  reading the actual silicon via `ESP.getChipModel()` instead, which needs
  no per-board update for the next ESP32-S3 (or any other) board either.
- **The new `bool isLilyGoT5S3 = ...` local in `setup()`** duplicates the
  `isX4Pro()`-style helper pattern every other board has in
  `BoardConfig.h`, but that header lives in the vendored `freeink-sdk`
  submodule, which this port does not edit — adding the missing helper
  there is a follow-up for whoever next touches that submodule, not
  something to hand-roll in this repo. Left as a direct enum comparison,
  now with a comment explaining why.
- **`docs/board-notes/lilygo-t5s3.md` (this file) was untracked** when the
  review ran, even though `platformio.ini`/`main.cpp` cite it by path as
  load-bearing context. `git add` it alongside the code changes so it ships
  in the same commit — see the note at the top of this file's history for
  confirmation it's part of this change, not a dangling reference.

## Build result

`pio run -e lilygo_t5s3` — **SUCCESS** (submodule pinned at
`ffeaaa271231d865590f8c54ea45ec02b1342d4e`), after the code-review fixes
above. Also re-verified `pio run -e xteink` and `pio run -e xteink_x4pro`
both still succeed (no regression from this port's changes).

```
RAM:   [==        ]  18.8% (used 61736 bytes from 327680 bytes)
Flash: [==        ]  23.4% (used 1531991 bytes from 6553600 bytes)
```

That is the entirety of what was verified. No LED blinked, no panel refreshed,
no touch was tapped, because none of that hardware exists in this session.
