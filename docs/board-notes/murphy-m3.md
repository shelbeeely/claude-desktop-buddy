# Murphy M3 board note

ESP32-S3, UC8253 e-paper controller, CHSC6x capacitive touch, PWM frontlight,
ADC battery, ES8388-compatible audio codec (present on the board, not wired
up by this firmware — no color/grayscale/audio rendering work is in scope
here). Build with `pio run -e murphy` (`-DFREEINK_DEVICE_MURPHY=1`, see
`platformio.ini` `[env:murphy]`).

Board profile: `BoardConfig::MURPHY_M3`,
`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:981-1007`.

## Panel rotation — 240x416 physical, reported as 416x240 landscape

The UC8253 controller is wired to a **240x416 portrait** panel, but
`MURPHY_M3.displayWidth`/`displayHeight` are **416/240**
(`BoardConfig.h:988-989`). The comment directly above it explains why:

> "Framebuffer is landscape 416x240: the panel is a 240x416 controller held
> rotated 90°, and the Murphy driver rotates each plane into controller RAM."
> — `BoardConfig.h:986-987`

So every consumer of `display.getDisplayWidth()`/`getDisplayHeight()`
(this firmware included) sees a 416-wide x 240-tall landscape panel; the
90° rotation into controller RAM is handled entirely inside the Murphy
UC8253 driver (`freeink-sdk/libs/display/FreeInkDisplay/src/driver/
Uc8253MurphyDriver.cpp`) and never needs to be accounted for at the app
level. No firmware change was needed for this part — it's confirmed
correct as shipped in the SDK.

## THE BUG — `invertSpriteRegionBytes()` out-of-bounds heap write

`src/main.cpp`'s sprite region is laid out with fixed constants sized for
the >=300px-tall panels this firmware originally targeted:

```
SPRITE_X = 40, SPRITE_Y = 40, SPRITE_W = 240, SPRITE_H = 260   (main.cpp:232-235)
```

`invertSpriteRegionBytes()` — the manual framebuffer-byte XOR used for the
`attention` state's flash cue (`main.cpp:445-461`, gated on
`settings().flash`) — looped:

```cpp
for (int16_t y = SPRITE_Y; y < SPRITE_Y + SPRITE_H; y++) {   // y: 40..299
  for (int16_t xb = SPRITE_X / 8; xb < (SPRITE_X + SPRITE_W) / 8; xb++) {
    fb[(uint32_t)y * wb + xb] ^= 0xFF;
  }
}
```

`SPRITE_Y + SPRITE_H` is `300`. On every board this firmware supported
before Murphy M3 (X4/X3, X4 Pro, de-link, Sticky, Paper Mono), the runtime
panel height (`PANEL_TOTAL_H`, set in `setup()` from
`display.getDisplayHeight()` — `main.cpp:1315-1316`) is comfortably above
300, so the loop stayed inside the allocated framebuffer. **Murphy M3's
`getDisplayHeight()` returns 240** (`BoardConfig.h:989`), so the unclamped
loop wrote rows `y = 240..299` — **60 rows past the end of the
runtime-allocated framebuffer** — on every `attention`-state flash tick.
`getFrameBuffer()`'s backing allocation is sized to the *runtime* panel
(`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:368-373`,
"MEMFIX-PORT: runtime-sized framebuffer... Sized to the RUNTIME panel, not
MAX_BUFFER_SIZE"), so this is a genuine heap buffer overflow, not a
harmless over-scan — it corrupts whatever heap allocation follows the
framebuffer, every time `attention` flashes with the flash cue enabled
(the default; `settings().flash` starts true unless the user turns the
"flash" setting off).

**Fix** (`main.cpp:271-284`): clamp the loop's upper bound to the real
panel height, mirroring how `PANEL_TOTAL_H` itself is already set at
runtime from `display.getDisplayHeight()`:

```cpp
int16_t yEnd = SPRITE_Y + SPRITE_H;
if (yEnd > PANEL_TOTAL_H) yEnd = PANEL_TOTAL_H;
for (int16_t y = SPRITE_Y; y < yEnd; y++) { ... }
```

**No-op on every other board**: X4/X3 (480/528), X4 Pro (480), de-link,
Sticky, and Paper Mono all report `PANEL_TOTAL_H` well above 300 (confirmed
via their `BoardConfig.h` profiles' `displayHeight` fields), so
`SPRITE_Y + SPRITE_H (300) > PANEL_TOTAL_H` is false on all of them and
`yEnd` stays `300` — identical behavior to before the fix. Verified by
building `pio run -e xteink` and `pio run -e xteink_x4pro` after the change
(see PR description for the build output) — both compile and link
unchanged; nothing about this fix is gated on `FREEINK_DEVICE_MURPHY`, it's
a plain runtime bounds check that degrades to a no-op when the panel is
tall enough.

## Does SPRITE_H=260 actually fit Murphy's 240px height? No.

Clamping the OOB write stops the heap corruption, but it does not make the
sprite *box* fit the panel: `SPRITE_Y + SPRITE_H = 300` is still 60px
taller than Murphy's 240px panel even with the fix — the box is simply
truncated at the panel edge instead of running off the end of memory.
Concretely, on Murphy M3:

- `ui().fill(Rect{SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H}, ...)` and
  `drawImageTransparent()` (used by `clearSpriteRegion()`/
  `drawIconCentered()`, `main.cpp:281-289`) are both safe against this —
  `DisplayTarget::plot()` bounds-checks against the logical panel size
  before writing a pixel (`freeink-sdk/libs/ui/FreeInkUI/include/
  FreeInkUIDisplayTarget.h:269`, `x < 0 || y < 0 || x >= w_ || y >= h_`),
  and `FreeInkDisplay::blitImage()` bounds-checks `destY >= displayHeight`
  and breaks the row loop (`freeink-sdk/libs/display/FreeInkDisplay/src/
  FreeInkDisplay.cpp:250` and `:281`). So these calls silently *clip*
  rather than corrupt memory — no second OOB bug here.
- `display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H)`
  (`main.cpp:421` etc.) is also safe on Murphy specifically: Murphy's
  UC8253 driver (`Uc8253MurphyDriver.cpp`) does not override
  `PanelDriver::displayWindow()`, so it falls back to the base class
  default, which ignores the x/y/w/h window entirely and does a plain full
  `display()` refresh (`freeink-sdk/libs/display/FreeInkDisplay/src/
  driver/PanelDriver.h:53-56`). The oversized height parameter is simply
  never used on this board.
- **Visual effect**: `drawIconCentered()` centers an icon within the
  nominal `SPRITE_H=260` box starting at `SPRITE_Y=40`
  (`main.cpp:285-288`, `y = SPRITE_Y + (SPRITE_H - icon.h) / 2`). The
  largest generated icon is 200px tall (`src/assets/icons_bufo.h:376`,
  `MAX_ICON_H = 200`), which places it at `y = 40 + 30 = 70`, spanning
  `70..270` — the bottom 30px of a max-height icon fall below Murphy's
  240px panel edge and get clipped. Icons render visibly low/off-center
  rather than centered in the 200px of panel actually available below
  `SPRITE_Y` (`240 - 40 = 200`).
- **`renderNapFrame()`'s "napping..." tag is fully invisible on Murphy**:
  it's drawn at `Rect{SPRITE_X, SPRITE_Y + SPRITE_H - 24, ...}`
  (`main.cpp:507`) = y=276, which is entirely below the 240px panel — this
  text never appears on Murphy M3 at all.

This is a real, visible layout defect, but not a memory-safety issue (the
bounds checks above establish that), and fixing it properly means either
giving `SPRITE_H`/icon-centering/text-anchor logic a runtime-aware
"effective sprite height" or reworking the sprite-box layout for short
panels — both of which touch `renderSprite()`/`renderNapFrame()`/
`drawIconCentered()`, i.e. exactly the screen-rendering call sites the
concurrent FreeInkApp migration unit is moving in this same file. Reworking
that layout here would guarantee a merge conflict rather than the
mechanical clamp described above, so it's left as a documented follow-up
rather than fixed in this PR: **on Murphy M3, expect the sprite icon to
render low/bottom-clipped for tall (>=200px) frames, and the "napping..."
tag to never be visible.** Both are cosmetic, not crashes, and both are new
information (not previously suspected) surfaced by actually reading the
call sites against Murphy's real panel height.

## Input — `InputStyle::DigitalFiveKey`

`MURPHY_M3.inputStyle` is `InputStyle::DigitalFiveKey`
(`BoardConfig.h:984`, declared at `BoardConfig.h:366`: "3 physical GPIO
keys + synthesized events (Murphy M3)"). `main.cpp`'s `handleInput()`
(`main.cpp:1200-1283`) needed **no changes** — it only ever consumes the
board-agnostic `InputManager::BTN_*` abstraction via `popPress()`/
`isPressed()`, never branching on board or input style itself.

However, actually reading `InputManager.cpp` (not just trusting the enum's
doc comment) turned up that `DigitalFiveKey` **has no dedicated handling
there at all** — `grep`ing the file for the enum value returns zero
matches. `InputManager::update()` only special-cases
`DigitalConfirmBackHold`, `DigitalConfirmPowerHold`, and `DigitalTwoButton`
(`InputManager.cpp:463-474`); every other style, `DigitalFiveKey` included,
falls through to the generic `getDigitalState()` path
(`InputManager.cpp:266-286`), which just reads each of
`back`/`confirm`/`left`/`right`/`up`/`down`/`power` off its configured GPIO
pin directly — there is no "synthesize 7 buttons from 3 keys" logic
anywhere in this style's code path.

Murphy's `input` pin config (`BoardConfig.h:993`,
`{PIN_UNASSIGNED, 0, PIN_UNASSIGNED, PIN_UNASSIGNED, 1, 2, 0, false}` against
`struct InputPins{back, confirm, left, right, up, down, power,
powerActiveHigh}`, `BoardConfig.h:470-479`) wires exactly 3 real GPIOs:

| Semantic | GPIO |
|---|---|
| `back` | unassigned |
| `confirm` | 0 |
| `left` | unassigned |
| `right` | unassigned |
| `up` | 1 |
| `down` | 2 |
| `power` | 0 (**same pin as `confirm`**) |

Two consequences, both verified by tracing the actual code paths rather
than assumed from the "5 key" name:

1. **`BTN_BACK`/`BTN_LEFT`/`BTN_RIGHT` are never reachable via GPIO on
   Murphy.** `getDigitalState()` only sets those bits from
   `isDigitalPressed(pin)`, which is `false` whenever `pin < 0`
   (`InputManager.cpp:264`), and Murphy's touch config has
   `synthesizeConfirm = false` and `hasHomeKey = false`
   (`BoardConfig.h:998`, positional against `struct TouchConfig`,
   `BoardConfig.h:482-520`) — i.e. touch does not synthesize any button
   events on this board either. There is currently no live input path to
   those three semantic buttons on Murphy M3.
2. **`confirm` and `power` share GPIO0, but `DigitalFiveKey` doesn't get
   the dual-role disambiguation that styles like
   `DigitalConfirmPowerHold` provide.** `getDigitalState()` sets *both*
   `BTN_CONFIRM` and `BTN_POWER` from the same GPIO0 read whenever the
   style isn't `DigitalConfirmBackHold`/`DigitalConfirmPowerHold`
   (`InputManager.cpp:271-283`), so pressing that one physical button
   fires both button IDs on the same tick. `asyncPoll()`'s `kButtons`
   scan order is `{BACK, CONFIRM, LEFT, RIGHT, UP, DOWN, POWER}`
   (`InputManager.cpp:188`), so both get queued, `CONFIRM` first. But
   `main.cpp:1249-1262`'s `popPress()` drain loop has no `BTN_CONFIRM`
   case (confirm is handled separately below, via `isPressed()` +
   `pollHold()`, `main.cpp:1266-1268`) — it silently discards the
   `CONFIRM` queue entry and then hits
   `else if (btn == InputManager::BTN_POWER) { sleepScreen(now); return; }`
   (`main.cpp:1259-1261`), which returns immediately. **Net effect: on
   real Murphy M3 hardware, pressing the confirm/power key currently
   always puts the screen to sleep and never registers as a confirm
   tap/hold**, because the `BTN_POWER` edge always accompanies it and is
   handled first, before `main.cpp` ever reaches the confirm-hold poll for
   that frame.

This is an upstream `freeink-sdk` gap (`BoardConfig`/`InputManager`) — the
*real* fix is either switching `MURPHY_M3.inputStyle` to
`DigitalConfirmPowerHold` (which already implements exactly this "short
press = confirm click, hold = power" disambiguation for a shared pin,
`InputManager.cpp:351-396`) or adding a genuine `DigitalFiveKey`-specific
synthesis path to `InputManager.cpp`. Both are submodule-level changes
outside this PR's scope (app-level `main.cpp`/`platformio.ini`/docs).

Because losing CONFIRM entirely makes the device unusable (no way to accept
a menu selection at all) whereas losing the *quick-press-to-sleep* shortcut
is a minor loss (the menu still has a "turn off" item), `main.cpp:1259-1273`
adds a narrow, board-scoped mitigation: in the `BTN_POWER` branch of
`handleInput()`'s press-drain loop, when `BoardConfig::ACTIVE.inputStyle ==
InputStyle::DigitalFiveKey` **and** `input.power == input.confirm` (true
only for Murphy M3 today — Sticky's `DigitalConfirmPowerHold` shares a pin
too but already disambiguates upstream, verified in `BoardConfig.h:1314`
and `1327-1329`, so its `BTN_POWER` events are never spurious and this
guard never triggers for it), the spurious `BTN_POWER` edge is dropped
instead of sleeping the screen, letting the CONFIRM tap/hold poll just
below (`main.cpp:1274-1276`) handle that same press as a normal confirm.
This is a runtime `BoardConfig` check, not a `#ifdef`, per this codebase's
convention. **Net result after this PR: UP (GPIO1) and DOWN (GPIO2) work as
documented; the shared CONFIRM/POWER key (GPIO0) now works as CONFIRM
(tap/hold); there is no GPIO-triggered instant sleep on this board (use the
menu's "turn off" instead); BACK/LEFT/RIGHT still have no GPIO or touch
path at all** — that part needs the upstream `InputManager` fix described
above and is out of scope here.

## SD — SPI pre-claim guard generalized from `isX4Pro()`

`MURPHY_M3.sdmmc` is `NO_SDMMC` (`BoardConfig.h:1006`, `busWidth` 0), so like
X3/X4, Murphy's SD card shares the display's SPI bus rather than using
native SDMMC (contrast X4 Pro, which has genuine native SDMMC on separate
pins). `setup()`'s SPI pre-claim — which wires the SD card's MISO pin into
the bus before `display.begin()` runs its own `SPI.begin()`, since a second
`SPI.begin()` with different pins is unreliable once the bus is already up
— was gated on `if (!BoardConfig::isX4Pro())`. That was already wrong in
spirit before this PR (it encodes "not X4 Pro" as a proxy for "shares an
SPI bus", true only because X4 Pro happened to be the sole native-SDMMC
board in the tree so far) and would have been actively wrong for Murphy:
`isX4Pro()` is `false` on Murphy, so the old guard would have (correctly,
but only by accident) still run the pre-claim — but the *next* native-SDMMC
board added under the old guard would have silently skipped a pre-claim it
needed, or run one it didn't.

This PR generalizes the guard to `BoardConfig::ACTIVE.sdmmc.busWidth == 0`
— the same native-SDMMC test `SDCardManager` itself uses
(`BoardConfig.h:1656-1657`) — so the condition is actually "does this board
share one SPI bus between display and SD", not a board-name check. This is
a no-op on every existing board (X3/X4/X4 Pro all keep their current
behavior, verified via each profile's `sdmmc.busWidth` in `BoardConfig.h`)
and correctly keeps Murphy on the SPI pre-claim path. The info screen's
ESP32-S3/C3 hardware label (`main.cpp`, "hardware" section of the About
page) had the same `isX4Pro()`-only bug for the MCU family and is
generalized alongside it to `isX4Pro() || isMurphyM3()`.

## Frontlight

Murphy M3 has a PWM frontlight (`MURPHY_M3.frontlight = {48, 25000, 10,
true}`, `BoardConfig.h:999`), and `FREEINK_CAP_FRONTLIGHT` auto-enables
from `-DFREEINK_DEVICE_MURPHY=1` (`BoardConfig.h:180-183`). This firmware's
`FrontlightManager` usage is already fully runtime-gated —
`frontlight.present()` decides at `setup()` time whether the Settings menu
exposes the `"brightness"` item at all (`main.cpp:549-556`), and
`cycleBrightness()`/`FrontlightManager::setBrightness()`
(`main.cpp:154-162`) are unconditionally safe to call regardless of board
(no-ops where absent). No `main.cpp` change was needed — Murphy M3 gets a
working brightness setting automatically once `-DFREEINK_DEVICE_MURPHY=1`
is set, the same way X4 Pro already does.

## Touch, audio, battery, LEDs (not in scope here)

- CHSC6x touch (`BoardConfig.h:998`) is wired and `CAP_TOUCH` auto-enables,
  but (per the Input section above) it's a plain coordinate touchscreen on
  this board — it doesn't synthesize button events, so it isn't part of
  this firmware's button-driven UI beyond whatever generic touch
  passthrough `InputManager` already provides to every touch-capable
  board.
- The ES8388-compatible audio codec (`MURPHY_AUDIO`, `BoardConfig.h:726-730`
  and the constant's definition following it) is present on the board but
  not wired into this firmware — no audio playback code exists here, per
  the "no color/grayscale rendering work" and general minimal-scope
  guidance for this port.
- Battery: ADC pin 9, no charge-status pin, divider multiplier 3.030303f
  (`BoardConfig.h:994-996`) — read through the existing board-agnostic
  `BatteryMonitor`, no firmware change needed.
- LEDs: `NO_LEDS` (`BoardConfig.h:1004`) — same substitute-cue path
  (`attention` flash) as every other LED-less board already in this
  firmware; this is what the bug above was found in.
