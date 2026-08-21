# M5Stack PaperColor board notes

**True 6-color capability is NOT used here — this firmware renders
dithered monochrome only, the same as every other board.** The panel is a
genuine 6-color (Spectra 6) ED2208-controller EPD, but this firmware's UI
renders through FreeInkUI's `DisplayTarget`
(`freeink-sdk/libs/ui/FreeInkUI/include/FreeInkUIDisplayTarget.h`), which is
1-bit-only: grays are reproduced with an ordered Bayer dither
(`FreeInkUIDisplayTarget.h:17`), and `DisplayTarget::plot()` only ever
writes a black/white bit into the framebuffer (`FreeInkUIDisplayTarget.h:267-301`).
Real color needs a second, separate rendering path —
`FreeInkDisplay::setAccentPlaneSlot()` (`freeink-sdk/libs/display/FreeInkDisplay/include/FreeInkDisplay.h:47-61`),
which recolors 1-bit accent-plane overlays but is only honored on
complete-waveform refreshes and has no existing colored character art to
drive it — and building that path is explicitly out of scope for this
change. If color rendering is ever wanted, it's new work on top of this,
not a flag flip.

Board: `Board::M5StackPaperColor`, profile `M5STACK_PAPER_COLOR`
(`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:887-907`) — ESP32-S3,
ED2208 controller, 400×600 panel, `InputStyle::DigitalConfirmBackHold`,
`NO_TOUCH`, `NO_FRONTLIGHT`, `NO_GAUGE` (no ADC/I2C battery reading — see
`BatteryMonitor.cpp`), ES8311 codec + AW8737A amp (`M5_PAPERCOLOR_AUDIO`), two
GRB LEDs on GPIO21 behind the M5PM1's LDO3V3 rail (`M5_PAPERCOLOR_LEDS`,
`BoardConfig.h:402-404`).

## Build

`pio run -e papercolor` (`platformio.ini`, appended after `env:xteink_x4pro`).
Named `papercolor`, not `m5paper` — `m5paper` is reserved in this repo for the
M5Paper v1.1 unit (classic ESP32, IT8951), a different device; freeink-sdk's
own `platformio.sample.ini` happens to name this same board's sample env
`[env:m5paper]` (`freeink-sdk/platformio.sample.ini:86-97`), which this repo's
env deliberately does not reuse to avoid the collision.

The env mirrors the SDK sample's board/mcu/flash strapping
(`esp32-s3-devkitc1-n16r8`, `board_build.mcu = esp32s3`,
`board_build.flash_mode = qio`, `board_build.arduino.memory_type = qio_opi` —
note this differs from `env:xteink_x4pro`'s `dio`, matching the SDK sample
exactly for this board) under this repo's own `build_flags`/`lib_deps` style.
It builds `-DFREEINK_DEVICE_M5=1` **without** `-DFREEINK_M5_OFFICIAL=1`, i.e.
the fast native ED2208 driver, not the M5Unified/M5GFX/M5PM1 vendor stack —
matching this project's dependency-minimal convention on every other board.
`LedManager` is linked (per the SDK sample, `platformio.sample.ini:96-97`) for
build-composition parity even though main.cpp does not drive the LEDs — see
"LEDs and speaker" below.

## Storage — shared SPI bus, same as X3/X4 (not SDMMC)

PaperColor's SD card is **not** native SDMMC — `M5STACK_PAPER_COLOR.sdmmc` is
`NO_SDMMC` (`busWidth = 0`, `BoardConfig.h:906`). It's wired the same way
X3/X4's is: `M5STACK_PAPER_COLOR.sd = {sclk=15, miso=14, mosi=13, cs=47, ...,
separateSpi=false}` (`BoardConfig.h:895`) shares `sclk`(15)/`mosi`(13) with
the display's own SPI pins (`.display = {sclk=15, mosi=13, ...}`,
`BoardConfig.h:893`) and only adds a dedicated `miso`(14) and `cs`(47) — the
same shared-bus shape as `BoardConfig::XTEINK_X4.sd`. So this board needs
`setup()`'s pre-claim of the display's SPI bus with the SD card's MISO pin
folded in (right before `display.begin()`), exactly like X3/X4, for the same
reason described at that call site.

The pre-claim's guard was `if (!BoardConfig::isX4Pro())` — correct in effect
for this board (PaperColor isn't X4 Pro, so the claim still ran), but wrong
in form: it's a per-board special case for "boards that don't need the
shared-bus claim," when what actually determines that is whether the board
is SDMMC at all (`BoardConfig::ACTIVE.sdmmc.busWidth == 0`,
`BoardConfig.h:442`: "0 = not an SDMMC board (use SdPins/SPI), 1 or 4 =
SDMMC") — true for every shared-SPI-bus board (X3/X4/PaperColor), false for
every genuine native-SDMMC board (X4 Pro, and others among this batch's
sibling boards). Generalized to that check here, matching the form the other
sibling board PRs in this batch converged on — behavior for every
already-supported board is unchanged; only the reasoning the condition
encodes is now board-capability-based instead of naming one specific board.

## DC-balance fix (`src/main.cpp`)

Every board has a "mandatory DC-balance timer" — `FULL_REFRESH_INTERVAL_MS`,
independent of activity, exists specifically so a long busy/idle stretch that
only runs partial/fast refreshes can't let the panel's DC balance silently
degrade (comment at `src/main.cpp:271-276`). On every other board a plain
`display.displayBuffer(EInkDisplay::FULL_REFRESH)` *is* that DC-balancing
pass. **On PaperColor it is not, by default:** the native ED2208 driver
interrupts every refresh — including `FULL_REFRESH` — at ~340 ms for
reading-speed monochrome output, and an interrupted refresh is not
DC-balanced; only a complete OTP waveform (~15 s) is
(`freeink-sdk/README.md:194-213`, `FreeInkDisplay.h:38-45`). Left as a plain
`FULL_REFRESH` call, this timer's "the panel's DC balance never degrades"
comment would be silently false on this one board — the panel would darken
over hours exactly the way the timer exists to prevent.

Fix: gated on `BoardConfig::isM5StackPaperColor()` (`BoardConfig.h:1632`),
call `display.requestCompleteWaveformNextRefresh()` (`FreeInkDisplay.h:38`)
immediately before this timer's `FULL_REFRESH` call — a one-shot that
promotes just that refresh to the complete waveform. Every other
`FULL_REFRESH` call site (menu navigation, celebrate, etc.) is untouched and
stays on the fast interrupted path, and every other board's refresh behavior
is unchanged (the call is a no-op there — `FreeInkDisplay.h:38` doc comment).

The promotion itself runs on its own cadence,
`PAPERCOLOR_COMPLETE_WAVEFORM_INTERVAL_MS` (1 hour, tracked in
`lastCompleteWaveformMs`) — **not** on `FULL_REFRESH_INTERVAL_MS`'s 5-minute
cadence, even though the check lives inside that timer's `if` block and only
ever runs when it fires. `loop()` is fully synchronous
(`display.displayBuffer()` blocks the whole call, then `delay(16)`); a
complete waveform takes ~15 s, so tying the promotion to the 5-minute timer
directly would mean every button press or BLE round-trip risks landing in one
of those ~15 s blocking windows roughly 12x as often as needed. The
README's "roughly hourly" is honored by promoting on its own ~hourly timer:
`FULL_REFRESH_INTERVAL_MS` still fires the (fast, interrupted) `FULL_REFRESH`
every 5 minutes as before, and separately, whichever one of those firings is
also due for the complete-waveform promotion gets the one-shot request first.
`setFullRefreshCompletesWaveform(true)` (`FreeInkDisplay.h:40-45`) was
considered instead of the one-shot call, but it would silently turn *every*
`FULL_REFRESH` call **anywhere** in main.cpp into a ~15 s blocking
complete-waveform pass (menu navigation, celebrate), not just this one
timer — the one-shot `requestCompleteWaveformNextRefresh()` call is the
narrower, correct fix for "keep other boards' [and this board's other]
refresh behavior unchanged."

## Portrait panel, landscape framebuffer — no layout change needed

PaperColor's *physical* panel is 400×600 — portrait, taller than it is wide,
unlike every other supported board (`BoardConfig.h:891-892`,
`M5STACK_PAPER_COLOR.display.width/height = 400, 600`). An earlier version of
this change assumed that meant `display.getDisplayWidth()/Height()` would
report 400×600 at runtime too, and added a `PANEL_TOTAL_H > PANEL_TOTAL_W`
branch in `setup()` to stack the sprite region above the text panel instead
of beside it in that case. That assumption was wrong, so the branch was
removed — it could never actually trigger:

`Ed2208M5Driver::geometry()`
(`freeink-sdk/libs/display/FreeInkDisplay/src/driver/Ed2208M5Driver.h:79-81,
.cpp:40`) hardcodes `LOGICAL_W = 600, LOGICAL_H = 400` regardless of the
physical panel's orientation — the driver comment states this directly: "app
draws 600x400" even though "physical panel is 400x600". `FreeInkDisplay::
begin()` (`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:
201-204`) sets `displayWidth`/`displayHeight` from exactly that driver
`geometry()`, not from `BoardConfig::ACTIVE.display.width/height` — so
`display.getDisplayWidth()/Height()` is **600×400, landscape**, on this
board, same shape class as every other board here. The driver rotates the
600×400 logical framebuffer into the panel's native 400×600 orientation
internally at refresh time (`Ed2208M5Driver.cpp:163-170,273-282` walk the
framebuffer with a 90°-rotated coordinate transform); the app-facing side —
this firmware's whole UI, including `main.cpp`'s sprite/panel layout — never
sees portrait at all.

With the real runtime dimensions (600×400), the existing fixed
landscape layout already works, with one fix (below): `PANEL_W =
PANEL_TOTAL_W - PANEL_X - 16 = 600 - (40+240+24) - 16 = 280` px — narrower
than X4/X4 Pro's 480px or X3's 472px, but a normal, usable width for the
transcript/menu/info text `drawStatusPanel()` renders there, not the ~80px
that would result if the physical 400px width were used directly. No
board-specific branch, runtime variables, or `PANEL_TOP` concept were needed;
`SPRITE_X`/`Y` and `PANEL_X` stay `constexpr` exactly as they were before
this change, on every board including this one.

Menu/settings/reset/passkey overlays (`drawMenu()`, `drawSettings()`,
`drawReset()`, `drawPasskey()`) already center within the full
`PANEL_TOTAL_W`/`H` and never read `SPRITE_X`/`PANEL_X`, so nothing about
them depends on this either.

**One vertical-fit fix was still needed:** `drawStatusPanel()`'s bottom
block (battery/BLE/level, `src/main.cpp:980-988`) is bottom-anchored at a
fixed `y = PANEL_TOTAL_H - 140`, on the assumption that the content above it
never reaches that far down. That holds on X4/X4 Pro (480-140=340) and X3
(528-140=388), but not on PaperColor: the default HUD page's transcript
(`settings().hud` branch, up to `TRANSCRIPT_VISIBLE = 6` fixed lines), the
stats list in `drawPetPage()`'s page 0, and several of `drawInfoPage()`'s
denser pages can all lay out content well past `400 - 140 = 260` on this
board's shorter 400px-tall panel — a normal state, not an edge case, so the
two blocks would routinely overlap left as a fixed `y = PANEL_TOTAL_H - 140`.

Fix, in two parts:

1. `drawClock()`, `drawPetPage()`, and `drawInfoPage()` (previously `void`,
   taking `y` by value) now return the real `y` just below the last thing
   each one drew (`src/main.cpp:617,643,707`), and `drawStatusPanel()`
   captures that into its own `y` at each call site instead of discarding it.
   The bottom block then anchors to `y = max(y, PANEL_TOTAL_H - 140)`
   (`src/main.cpp:977`) — below the content above wherever that's true (every
   board, and PaperColor on its shorter/emptier pages), but yielding to the
   actual content extent when a page runs deeper than that, rather than
   guessing a single fixed offset per board.
2. Even with that adjustment, a couple of `drawInfoPage()`'s densest pages on
   this board's 400px panel run close enough to the panel's actual bottom
   edge that `max(y, PANEL_TOTAL_H - 140)` still doesn't leave room for the
   3-line battery/BLE/level block without clipping or overlapping the content
   above. Rather than draw a clipped or overlapping block in that case,
   `src/main.cpp:978-989` gates the whole block on
   `y + kBottomBlockH <= PANEL_TOTAL_H` (`kBottomBlockH = 64`, the block's
   fixed height at 3 lines x 22px plus a 20px-tall last line) and skips
   drawing it for that one frame instead. Nothing is permanently hidden: the
   same battery and BLE state are always visible on `drawInfoPage()`'s own
   battery (page 3/4) and BLE (page 4) pages.

On every other board (X4/X4 Pro/X3), none of `drawClock()`/`drawPetPage()`/
`drawInfoPage()` ever draws anywhere near `PANEL_TOTAL_H - 140`, and the
block's own height always fits below it, so both parts of this fix are no-ops
there — behavior is unchanged from before this file's `void`-returning
helpers were given real return values.

## Input coverage — `InputStyle::DigitalConfirmBackHold`

Two independent things gate whether a given semantic `BTN_*`
(`freeink-sdk/libs/hardware/InputManager/include/InputManager.h:57-63`) is
ever emitted: whether the **input style's code** (`InputManager.cpp`) has a
path to it at all, and whether **this board's profile** wires a GPIO for it
(`isDigitalPressed(pin)` is unconditionally `false` for `pin < 0` —
`InputManager.cpp:264`). On M5 PaperColor, both bite:

`M5STACK_PAPER_COLOR.input` = `{back=1, confirm=1, left=-1, right=-1, up=10,
down=9, power=1, powerActiveHigh=false}` (`BoardConfig.h:896`, field order in
`InputPins` at `BoardConfig.h:470-479`, `-1` = `PIN_UNASSIGNED` at
`BoardConfig.h:406`). `back`/`confirm`/`power` all name the same GPIO (1) —
the board has one physical button; `DigitalConfirmBackHold` reads it only
through `input.confirm` and derives `BACK` from a long-hold, so the `back`
and `power` fields being non-`PIN_UNASSIGNED` here doesn't add a code path
(see below) — they're simply never read as `back`/`power` for this style.

`InputManager::updateConfirmBackHold()`
(`InputManager.cpp:316-349`) covers **4 of the 7 semantic buttons** on this
board: `CONFIRM` (short click on the shared button), `BACK` (that same
button held past `CONFIRM_BACK_HOLD_MS`), and `UP`/`DOWN` (plain digital
reads on GPIO10/9 — `getDigitalState()` always includes these regardless of
input style, `InputManager.cpp:275-278`).

- **`LEFT`/`RIGHT` — board-wiring gap, not style-specific.**
  `input.left`/`right` are `PIN_UNASSIGNED` on this board, so
  `isDigitalPressed()` never fires for them no matter what style is active
  (`InputManager.cpp:264`, `275-276`). `main.cpp` uses `BTN_LEFT`/`RIGHT` to
  scroll the transcript view back/forward one entry at a time
  (`src/main.cpp:1252-1258`); on this board `LEFT` never fires, but `BACK`'s
  transcript fallback (`dispatchBack()`'s last branch,
  `src/main.cpp:1182-1185`, wrapping forward-only, one entry per press) still
  provides forward scrolling, the same fallback pattern the README documents
  for X4 Pro's own missing `LEFT`/`RIGHT` (`README.md` "Multi-board support").
- **`BTN_POWER` — a real gap in the input style itself.**
  `getDigitalState()` explicitly excludes `BTN_POWER` for
  `DigitalConfirmBackHold` (`InputManager.cpp:279-283`), and
  `updateConfirmBackHold()` never synthesizes it either — there is no
  power-hold branch in that function at all, unlike its
  `DigitalConfirmPowerHold` sibling (`InputManager.cpp:351-396`). This holds
  regardless of what `input.power` is set to (here, `1` — the same GPIO as
  `confirm`/`back`, per above): the style's code has no path to `BTN_POWER`
  on any board that uses it. `main.cpp`'s `handleInput()` calls
  `sleepScreen()` on `BTN_POWER` (`src/main.cpp:1259-1261`); that path is
  simply never reached from a physical button press on this board. Powering
  off via the on-screen menu (`CONFIRM` → "power off" →
  `powerOffSequence()`, `src/main.cpp:972-980`) still works, since it goes
  through `CONFIRM`, not `BTN_POWER`.

Related, not fixed here: `powerOffSequence()` calls
`PowerManager::deepSleepUntilPowerButton()`
(`freeink-sdk/libs/hardware/PowerManager/src/PowerManager.cpp:101-105`),
which arms a wake source via `armPowerButtonWakeup()` by watching
`BoardConfig::ACTIVE.input.power` go active
(`PowerManager.cpp:10,36-45`) — on this board that's GPIO1, the same
physical button as `confirm`/`back`, held at whatever level
`powerActiveHigh=false` expects. Whether that reliably wakes the board (a
press indistinguishable in hardware from a `confirm`/`back` press, on the
one button this board has) is untested here and is left as an observation,
not a fix — this change does not touch `PowerManager` or the wake path.

## LEDs and speaker — present, out of scope

Two GRB LEDs on GPIO21 and an ES8311 codec + AW8737A amp are real hardware on
this board (`M5_PAPERCOLOR_LEDS`/`M5_PAPERCOLOR_AUDIO`,
`BoardConfig.h:398-404`) and both are supported by the SDK
(`freeink-sdk/libs/hardware/LedManager`, `freeink-sdk/libs/hardware/AudioManager`).
`main.cpp` does not use either — no LED status indication, no sound. This is
intentional and out of scope for this change; no LED/speaker abstraction was
added to the firmware. `LedManager` is linked in `env:papercolor`'s
`lib_deps` purely to match freeink-sdk's own sample env's build composition
(`platformio.sample.ini:96-97`), not because anything calls it.
