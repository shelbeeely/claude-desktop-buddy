# de-link — board notes

`env:delink` in the top-level `platformio.ini`. ESP32-S3 board running the
same `src/main.cpp` as `env:xteink` (X4/X3) and `env:xteink_x4pro` (X4 Pro).
All claims below cite `freeink-sdk/libs/hardware/BoardConfig/include/
BoardConfig.h` (`DE_LINK` profile, `constexpr BoardProfile DE_LINK = {...}`
starting at BoardConfig.h:1084) unless noted otherwise. Line numbers are
current as of freeink-sdk submodule commit `ffeaaa2`; they will drift as the
SDK moves.

## What's real

- **Display**: SSD1677 controller, 800x480 (BoardConfig.h:1087-1089). Same
  controller/driver as the Xteink X4 — the profile comment (BoardConfig.h:
  1076-1078) calls de-link an "X4-class GDEQ0426T82 panel on ESP32-S3": same
  glass/driver, different board (S3 MCU, SDMMC SD, PWM frontlight). Display
  SPI pins `{8, 10, 21, 4, 5, 6, PIN_UNASSIGNED}` at BoardConfig.h:1090,
  identical to the X4's own display pin set (BoardConfig.h:787) — the SPI
  bus wiring for the panel is shared hardware between the two boards.
  `displaySpiHz` is 0 (BoardConfig.h:1091), i.e. the SSD1677 driver's own
  default (40 MHz per the comment there).

- **Input**: `InputStyle::XteinkAdcLadder` (BoardConfig.h:1086), the same
  input style as X3/X4 (BoardConfig.h:362, 783). The `InputPins` values
  `{0, 1, 2, 3, 4, 5, 3, true}` (BoardConfig.h:1096) are numerically
  identical to the X4's `{0, 1, 2, 3, 4, 5, 3, false}` (BoardConfig.h:790)
  except the trailing `powerActiveHigh` flag: de-link's power button reads
  active-HIGH / `INPUT_PULLDOWN` (BoardConfig.h:1096 comment), the X4's
  reads active-LOW. Firmware-side this means de-link's buttons behave like
  the X4's under `InputManager` with no code changes — main.cpp's existing
  X4/X3 button handling (`popPress()`, etc.) applies unmodified.

- **SD card**: native 4-bit SDMMC, not SPI. Pin set `{39, 40, 38, 48, 42, 41,
  4}` (clk/cmd/d0/d1/d2/d3/busWidth) at BoardConfig.h:1108. `busWidth=4`
  means `SDCardManager` mounts an `FsVolume` on a native esp-idf SDMMC block
  device rather than driving the card over SPI/SdFat (see the `SdmmcPins`
  comment at BoardConfig.h:434-442 and the `FREEINK_SD_SDMMC` capability
  derivation at BoardConfig.h:298-306, which lists de-link as one of the
  SDMMC boards). The profile's `SdPins` field (`sd`, BoardConfig.h:1095) is
  populated with SPI-shaped values but the comment there says explicitly
  "These SPI sd pins are unused" — SDMMC is the real transport.
  `-DUSE_BLOCK_DEVICE_INTERFACE=1` is required in the build (set in
  `env:delink` in `platformio.ini`) for SdFat's generic block-device
  interface, matching `env:xteink_x4pro`'s own X4-Pro SDMMC handling.

- **Frontlight**: single-channel PWM, GPIO5, 20 kHz / 8-bit, active-HIGH
  (BoardConfig.h:1104, with the comment at 1102-1103 noting the profile's
  warm/cool/rail/fault pins GPIO6/7/17/18 are declared but not driven —
  only the primary brightness PWM is real). `FREEINK_CAP_FRONTLIGHT`
  auto-enables for `FREEINK_DEVICE_DELINK` (BoardConfig.h:180-183), and
  `FrontlightManager.h`'s own doc comment (line 9) names "de-link primary
  LED" as the canonical example of its single-channel topology. Because
  `main.cpp`'s settings menu already gates its "brightness" item on
  `FrontlightManager::present()` at runtime (`src/main.cpp`, `setup()`,
  around the `frontlight.present()` check) rather than on
  `BoardConfig::isX4Pro()`, de-link automatically gets a working
  "brightness" settings item with **no main.cpp changes** — this was
  verified by reading the existing code path, not assumed.

- **Battery**: ADC sense on GPIO4, divider multiplier 2.0 (BoardConfig.h:
  1097, 1099). No charge-status pin, no USB-detect pin (both
  `PIN_UNASSIGNED`, BoardConfig.h:1098, 1100). No I2C fuel gauge —
  `NO_GAUGE` (BoardConfig.h:1109) — so `BatteryMonitor` reads the ADC pin
  directly, same shape as the X4's battery setup.

## What's not present

- **Touch**: `NO_TOUCH` (BoardConfig.h:1101). `BoardConfig::hasTouch()`
  returns false; every touch-gated code path in `main.cpp` (X4 Pro's
  synthesized CONFIRM tap, Home-key BACK, etc.) is a no-op on this board.

- **RTC / IMU**: neither is set — the profile omits the `sensors` field
  entirely, so it takes the struct's own default `NO_SENSORS`-equivalent
  (`SensorsConfig` default-initialized to all-unassigned pins/
  `RtcType::None`/`ImuType::None`, BoardConfig.h:659). `BoardConfig::
  hasImu()`/`hasRtc()` (referenced from `main.cpp`'s file-header comment)
  return false; the clock face and shake-to-interact paths behave as they
  do on the plain X4 (no IMU/RTC substitutes are wired for this board).

- **Battery gauge**: `NO_GAUGE` (BoardConfig.h:1109) — see "Battery" above;
  there is no I2C fuel gauge, only the ADC pin.

- **Audio / LEDs**: `NO_AUDIO`, `NO_LEDS` (BoardConfig.h:1105-1106).

## Orientation caveat (unverified on hardware)

The profile ships `NO_FLIP` (BoardConfig.h:1107), i.e. the same orientation
as the X4. The comment directly above the profile (BoardConfig.h:1080-1082)
says this assumes the newer/next PCB revision that matches the X4's mount;
it explicitly flags that the *current* de-link PCB mounts the panel upside
down relative to the X4, and that a board on that older PCB should set
`ROTATE_180` (or a mirror) in its own profile instead of a firmware-side
software rotate. `freeink-sdk/platformio.sample.ini`'s own `[env:delink]`
comment (lines 126-129) repeats this same caveat. **This firmware ships
with the SDK's default (`NO_FLIP`) and does not attempt to detect which PCB
revision a given board is** — if display output appears upside down on real
hardware, the fix is a profile-level `ROTATE_180` change in the SDK/board
profile, not a `main.cpp` change.

## Build

`env:delink` in `platformio.ini`, modeled on `env:xteink_x4pro` (same
ESP32-S3 board id `esp32-s3-devkitc1-n16r8`, same 16 MB partition-table
reasoning) and on `freeink-sdk/platformio.sample.ini`'s own `[env:delink]`
block (same `-DFREEINK_DEVICE_DELINK=1` / `-DUSE_BLOCK_DEVICE_INTERFACE=1`
flags). Unlike X4 Pro, de-link does not need `-DBOARD_HAS_PSRAM`: the SDK's
`FREEINK_FB_PSRAM` capability derivation (BoardConfig.h:294-296) only forces
PSRAM framebuffer placement for M5Paper v1.1 and Paper Mono, not de-link.

## Unverifiable from source alone

- The orientation caveat above (which PCB revision ships in practice, and
  whether `ROTATE_180` is actually needed) cannot be resolved from
  `BoardConfig.h` — it depends on which physical unit a builder has. Flagged
  above, not guessed at.
- No hardware exists in this environment to confirm any of the above at
  runtime; verification here is `pio run -e delink` build success only (see
  the PR description for the resulting RAM/Flash usage).
