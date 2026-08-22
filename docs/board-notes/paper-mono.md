# M5Stack Paper Mono (`env:papermono`)

ESP32-S3 board (`Board::PaperMono`, freeink-sdk BoardConfig.h:944), same MCU
family as X4 Pro. Power, display-rail, SD-rail, and touch-rail sequencing all
run through two helper chips — the M5PM1 PMIC and the M5IOE1 GPIO expander —
instead of raw ESP GPIOs. All hardware claims below are cited to
`freeink-sdk/...` file:line; anything I could not confirm from source is
called out explicitly under "Unverified / flagged".

## PMIC/expander sequencing needed zero new firmware code

`freeink-sdk/libs/hardware/BoardConfig/include/PaperMonoBoard.h` (read in
full) is explicit that this is a single-owner header: "the SDK's hardware
managers can bring the board up themselves and consumer firmware never
touches the PMIC/expander directly" (PaperMonoBoard.h:6-9). Concretely:

- `EpdBus` calls `setEpdPower()`/`setEpdReset()` (PaperMonoBoard.h:54-60) —
  the profile leaves the display's `powerEnable`/`rst` pins unassigned
  (BoardConfig.h:954-955 in the `PAPER_MONO` literal) precisely so this path
  is taken instead of a raw GPIO toggle.
- `InputManager::beginFt5x06()` calls `freeink::papermono::enableTouch()`
  before probing the FT6336 (`InputManager.cpp:1296-1300`, gated
  `#if FREEINK_DEVICE_PAPERMONO`), and `InputManager::updateDigitalTwoButton()`
  polls `pollPowerButtonClicked()` into a synthesized `BTN_POWER` pulse
  (`InputManager.cpp:404-412`).
- `SDCardManager` powers the TF rail via the same `ensureBooted()`-backed
  path (`SDCardManager.cpp:10,26`, `#if FREEINK_DEVICE_PAPERMONO`) before the
  native-SDMMC mount.
- `FrontlightManager`/`LedManager` talk to the PM1/IOE1 through their own
  configs (`PAPER_MONO_FRONTLIGHT`/`PAPER_MONO_LEDS`, BoardConfig.h:932,936),
  not through this file.

Everything funnels through `ensureBooted()` (PaperMonoBoard.h:36-50), which
is idempotent — whichever manager runs first establishes the boot state.
This firmware's existing `setup()` sequence (`display.begin()` →
`SdMan.begin()` → `input.begin()`/`input.beginAsync()` → `frontlight.begin()`
in `src/main.cpp`) already calls every one of those managers in the normal
order, so **no Paper-Mono-specific setup() code was added** — confirmed by
reading `PaperMonoBoard.h` end-to-end, not assumed.

## SPI-guard generalization

`src/main.cpp`'s pre-`display.begin()` SPI claim (needed only on boards where
the SD card shares the display's SPI bus) was gated on `!BoardConfig::
isX4Pro()`, which is wrong for any other native-SDMMC board. Generalized to
a capability check: `BoardConfig::ACTIVE.sdmmc.busWidth == 0` (no SDMMC
wiring at all → SD really is on the shared SPI bus). Paper Mono's `sdmmc`
field is `{13, 12, 11, 10, 9, 8, 4}` (BoardConfig.h:968) — `busWidth = 4`, so
the guard now correctly skips the pre-claim for it too, the same as it
already did for X4 Pro (`busWidth = 1`, BoardConfig.h:1466).

**Flagged:** the task brief described Paper Mono's SD as "native 1-bit
SDMMC," but the profile's own `sdmmc` literal at BoardConfig.h:968 reads
`busWidth = 4` (4-bit). Went with what's actually in the source.

## Battery telemetry: PMIC-backed, also needed no new code

Paper Mono's `BoardProfile` carries `NO_GAUGE` (BoardConfig.h:969) and no
`batteryAdc` pin (PIN_UNASSIGNED, BoardConfig.h:954) — there is no I2C fuel
gauge and no ADC divider. Battery status instead comes from the M5PM1 PMIC's
own registers. `BatteryMonitor` already has a dedicated backend for this:
`BatteryMonitor::hasM5Pm1Backend()` returns true for
`BoardConfig::isPaperMono() || BoardConfig::isM5StackPaperColor()`
(`BatteryMonitor.cpp:255-257`), and `readPercentage()`/`readStatus()`/
`readMillivolts()`/`isCharging()` all branch to `readM5Pm1Status()`
(`BatteryMonitor.cpp:399` on) ahead of the ADC fallback. `src/main.cpp`'s
`platformBatteryStatus()` just calls `batteryMonitor.readStatus()`
unconditionally, so this works automatically — no board-specific code here
either.

## Input coverage: `InputStyle::DigitalTwoButton` — real gap, documented

Read `InputManager.cpp`'s `updateDigitalTwoButton()` (lines 398-447) in
full. It reads exactly two inputs and derives everything else from them:

- `physical` = 2-bit state of `isDigitalPressed(ACTIVE.input.up)` (bit 0) and
  `isDigitalPressed(ACTIVE.input.down)` (bit 1) (`InputManager.cpp:399-401`).
- Short press → `BTN_UP` (physical was 1) or `BTN_DOWN` (physical was 2)
  (`InputManager.cpp:420-424`).
- Held past `TWO_BUTTON_HOLD_MS` → `BTN_BACK` (physical==1), `BTN_CONFIRM`
  (physical==2), or `BTN_POWER` (physical==3) (`InputManager.cpp:442-443`).
- `auxiliaryState` (touch + `s_buttonHook` +, on this device, the PM1 power
  button) is OR'd in every tick (`InputManager.cpp:402-412`).

Paper Mono's `InputPins` literal is `{15, 14, 16, 17, PIN_UNASSIGNED, 18,
PIN_UNASSIGNED}` (BoardConfig.h:950), i.e. `back=15, confirm=14, left=16,
right=17, up=PIN_UNASSIGNED, down=18, power=PIN_UNASSIGNED`. Because
`InputManager::update()` returns immediately after calling
`updateDigitalTwoButton()` for this input style (`InputManager.cpp:471-474`),
`getDigitalState()` — the function that would read `back`/`confirm`/`left`/
`right` off raw GPIOs — is never reached for this board. Only `input.up` and
`input.down` are ever read, and `isDigitalPressed()` short-circuits to
`false` for `PIN_UNASSIGNED` (`InputManager.cpp:264`). So in practice, on
this specific board:

| Physical input | Result |
|---|---|
| Short press of the one GPIO18 button | `BTN_DOWN` (physical bit 1 only — `up` is permanently 0) |
| Long-hold of the same button | `BTN_CONFIRM` (physical==2 branch) |
| M5PM1 power-button click | `BTN_POWER`, injected via `freeink::papermono::pollPowerButtonClicked()` (`PaperMonoBoard.h:87-96`, wired at `InputManager.cpp:404-412`) |
| — (no physical `up` input exists) | `BTN_UP` **unreachable** |
| — (needs physical==1, i.e. `up` held, which never happens) | `BTN_BACK` **unreachable** |
| — (`DigitalTwoButton`'s algorithm never sets these bits, for any board) | `BTN_LEFT` / `BTN_RIGHT` **unreachable** |

Touch does **not** fill any of these gaps on this board, unlike X4 Pro's
touch-synthesized CONFIRM / capacitive-Home-key BACK (`src/main.cpp:1223-
1235`). Paper Mono's `TouchConfig` literal (BoardConfig.h:962-963) sets
`synthesizeConfirm = false` and leaves `hasHomeKey` at its struct default of
`false` (`BoardConfig.h:491,515` — the literal only supplies 18 of the 20
`TouchConfig` fields). `serviceTouch()` only ever contributes a `BTN_CONFIRM`
bit, and only when `synthesizeConfirm` is true (`InputManager.cpp:1161`);
`wasHomeKeyTapped()`/`wasHomeKeyPressed()` are driven off `touchHomeKeyEvent`
flags that only get set when `hasHomeKey` is true. Both are false here, so
`serviceTouch()` and the home-key API contribute nothing on Paper Mono.

**Net result — a real, unavoidable-without-new-code gap:** `BTN_UP`,
`BTN_BACK`, `BTN_LEFT`, and `BTN_RIGHT` are non-functional on this board with
the current wiring. `BTN_DOWN`, `BTN_CONFIRM`, and `BTN_POWER` all work.
Per this unit's scope, no ad-hoc touch tap-zone code was invented to plug the
gap — `docs/board-notes` is the place to flag it, matching how this codebase
already documents other "reserved, not yet wired" states.

`BTN_BACK` in particular backs real behavior in `src/main.cpp::dispatchBack()`
(line 1153) — denying a pending permission prompt, canceling the reset/
settings menus, and paging back through info/pet screens — all of which are
currently unreachable on Paper Mono outside of the natural "hold the one
button = CONFIRM" / "click power" paths. This is worth a follow-up (e.g. a
double-tap-hold synthesis, or claiming a small on-screen touch zone once this
board's `synthesizeConfirm`/`hasHomeKey` intent is confirmed against real
hardware) but is out of scope for this unit's size per the task brief.

**Flagged:** it's surprising for shipped hardware to leave BACK completely
unreachable; this may indicate the `PAPER_MONO` profile in freeink-sdk itself
is incomplete (e.g. `up`/`hasHomeKey`/`synthesizeConfirm` pending real-device
confirmation) rather than a deliberate design. Worth re-checking against
on-device testing or a future freeink-sdk update rather than treated as final.

## Frontlight

Real PMIC-PWM frontlight: an AW9967 boost LED driver whose CTRL input is
routed to the M5PM1's GPIO3/PWM0 engine (5 kHz, 12-bit), not an ESP GPIO
(`PAPER_MONO_FRONTLIGHT`, BoardConfig.h:929-932). `src/main.cpp`'s existing
`frontlight.begin()` / `if (frontlight.present()) { ... hasBrightnessItem =
true; ... }` block (setup(), around line 1331-1337) is already gated purely
on `FrontlightManager::present()` at runtime with no board identity check, so
it lights up the Settings > brightness item on Paper Mono automatically —
the same pattern X4 Pro's port already established, no board-specific code
needed here either.

## RTC

Real RX8130 RTC at I2C address 0x32 on SDA47/SCL48, 100 kHz
(`{47, 48, 100000, 0x32, 0, 0, 0, RtcType::Rx8130, ImuType::None}`,
BoardConfig.h:971). `Rtc`/`BoardConfig::hasRtc()` already dispatch on
`RtcType` generically (same mechanism X3's DS3231 and X4 Pro's BM8563 use),
so the clock face survives reboot on this board with no new code.

No IMU (`ImuType::None`, same line) — `Imu`'s calls all no-op via
`BoardConfig::hasImu()`, same as X4 (no shake-to-dizzy on this board; the
DOWN-hold button substitute every board keeps still applies).

## Explicitly out of scope

- **Buzzer**: real passive beeper, LEDC tone pin GPIO42, no codec
  (`PAPER_MONO_AUDIO`, BoardConfig.h:916-927). This codebase has no buzzer
  abstraction in `src/main.cpp` today (`AudioOutput`/`Buzzer` isn't wired up
  for any board) — not added here, per the task's "no invented buzzer/mic
  features" instruction.
- **PDM mic**: CLK=GPIO45/DATA=GPIO46, power rail on IOE1 IO12
  (`PAPER_MONO_MIC`, BoardConfig.h:938-941). Same story — no codebase
  abstraction, not added.
- **RGB LED**: PM1 red leg + IOE1 green/blue legs (`PAPER_MONO_LEDS`,
  BoardConfig.h:934-936, `LedManager`'s `paperMonoDiscrete` path). Not linked
  into `platformio.ini`'s `lib_deps` for `env:papermono` — `src/main.cpp`
  doesn't reference `LedManager` for any board today, so it wasn't added
  here either, matching this unit's stated `lib_deps` scope.
- **3-level grayscale**: the SSD1677/PaperMonoDriver's own internal
  fast-refresh grayscale batching is real hardware capability, but out of
  scope per the overall multi-board plan — this board ships plain 1bpp
  dithered rendering like every other board in this firmware.

## Build

`env:papermono` in `platformio.ini` mirrors `env:xteink_x4pro`'s shape
(same ESP32-S3 board id, same `lib_deps` set) with
`-DFREEINK_DEVICE_PAPERMONO=1` in place of `-DFREEINK_DEVICE_X4PRO=1`, per
`freeink-sdk/platformio.sample.ini`'s own `[env:papermono]`
(lines 271-283): `-DBOARD_HAS_PSRAM` (grayscale plane batching needs PSRAM),
`-DUSE_BLOCK_DEVICE_INTERFACE=1` (native SDMMC needs SdFat's generic
block-device interface for the FsVolume mount), `board_build.partitions =
default_16MB.csv` (real 16MB flash part, non-OTA build).
