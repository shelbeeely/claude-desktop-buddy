# M5Stack PaperS3 (env:papers3)

> **UNVERIFIED — no hardware, and the SDK's own profile says so too.**
> `BoardConfig::M5PAPER_S3` calls out panel rotation, the touch swapXY/flip
> pairing, and the battery divider as **"pending hardware validation"**
> (`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:1248-1253`).
> No PaperS3 unit has run this firmware. This port — and in particular the
> touch tap-zone layout below, which is firmware policy this project invented
> and the SDK does not define — was built from the SDK's documented pin map
> and hardware comments only. Re-verify every zone boundary and every "which
> corner is which" claim against a real unit before relying on it; a wrong
> guess here is a board a user can navigate incorrectly, not a crash.

## Why this board needed new code, not just a new env

Every other board this firmware supports has *something* firmware-readable
to drive `dispatchConfirmTap()`/`dispatchConfirmHold()`/`dispatchBack()`
with: real GPIO buttons (X4/X3), or a capacitive Home key plus a
touch-synthesized CONFIRM (X4 Pro). PaperS3 has neither:

- `BoardConfig::M5PAPER_S3.input` leaves **every** pin (`back`, `confirm`,
  `left`, `right`, `up`, `down`, `power`) `PIN_UNASSIGNED`
  (`BoardConfig.h:1265-1266`).
- `touch.synthesizeConfirm = false` (`BoardConfig.h:1277-1279`) — GT911 taps
  never turn into a `BTN_CONFIRM` edge the way they do on boards that opt in.
- `touch.hasHomeKey` is left at its default `false` (same initializer,
  contrast X4 Pro's explicit `true` at `BoardConfig.h:1447`) — no capacitive
  Home key either.
- The side button feeds a self-latching PMS150G power chip directly
  (`BoardConfig.h:1242-1246`) — it is not a GPIO this firmware, or any
  firmware, can read. Power-off is a 5-pulse GPIO44 train
  (`BoardPaperS3::powerOff()`, `freeink-sdk/libs/hardware/BoardPaperS3/
  src/BoardPaperS3.cpp`), not a latch release.

So `handleInput()` as written for every other board is a complete no-op on
PaperS3. `src/main.cpp` adds one new, clearly-marked block —
**"PaperS3 touch-only navigation"**, immediately before `handleInput()` —
containing `touchOnlyNavActive()` and `handleTouchNav()`. It reuses the
*same* `dispatchConfirmTap()` / `dispatchConfirmHold()` / `dispatchBack()`
every other board calls; only the thing that decides *when* to call them is
new.

`touchOnlyNavActive()` is a capability check, not a `isM5PaperS3()` identity
check:

```cpp
static bool touchOnlyNavActive() {
  const auto& t = BoardConfig::ACTIVE.touch;
  const auto& in = BoardConfig::ACTIVE.input;
  return t.controller != BoardConfig::TouchController::None && !t.synthesizeConfirm && !t.hasHomeKey &&
         in.back == BoardConfig::PIN_UNASSIGNED && in.confirm == BoardConfig::PIN_UNASSIGNED &&
         in.left == BoardConfig::PIN_UNASSIGNED && in.right == BoardConfig::PIN_UNASSIGNED &&
         in.up == BoardConfig::PIN_UNASSIGNED && in.down == BoardConfig::PIN_UNASSIGNED &&
         in.power == BoardConfig::PIN_UNASSIGNED;
}
```

This matters concretely: X4 Pro also has `touch.synthesizeConfirm = false`
and `input.confirm == PIN_UNASSIGNED`, and would satisfy a naive version of
this check — it's `touch.hasHomeKey` (true on X4 Pro, false on PaperS3) that
correctly keeps this block from ever firing on X4 Pro's already-working
Home-key dispatch.

`touch.hasHomeKey` alone isn't enough either, though: an earlier version of
this check also matched `BoardConfig::PAPER_MONO` (not built by any env in
this repo today, but fully modeled in the SDK) — it has
`touch.controller = Ft5x06` (not `None`), `touch.synthesizeConfirm = false`,
`touch.hasHomeKey = false` (default), *and* `input.confirm == PIN_UNASSIGNED`,
satisfying every condition above despite having two working physical GPIO
buttons (`input.up`/`input.down`, decoded by
`InputManager::updateDigitalTwoButton()`, `InputStyle::DigitalTwoButton`) that
this touch-zone dispatch has no mapping for. Caught in code review before this
port shipped; fixed by requiring **every** `InputPins` field (`back`,
`confirm`, `left`, `right`, `up`, `down`, `power`) to be `PIN_UNASSIGNED`, not
just `confirm` — which is what "this board has no buttons at all" actually
means, and is true of PaperS3 (`BoardConfig.h:1265-1266`) but not PaperMono.

## Tap-zone layout

Normalized touch coordinates (`nx`, `ny`, each 0..1) in the touch
controller's **panel-native frame** — the frame `InputManager` already
applies `swapXY`/`flipX`/`flipY` correction into before handing coordinates
to app code (`BoardConfig.h:503-509`, `InputManager::wasTouchTap`'s doc
comment). This is deliberately **not** claimed to line up with any particular
edge of the rendered screen as a *reader* would see it, because the panel
rotation and the touch axis correction that would make that claim true are
both still pending hardware validation (`BoardConfig.h:1248-1253`) — this
port was written with no unit to corner-tap-test against.

```
  +------------------------------------------------------------+
  |                                                              |
  |                                                              |
  |                     CONFIRM  (tap anywhere here)             |
  |                hold ~500ms anywhere on screen  =  MENU       |
  |                                                              |
  |                                                              |
  +----------+                                                   |
  |   BACK   |                                                   |
  |  (tap)   |                                                   |
  +----------+---------------------------------------------------+
    nx in [0, 0.20)         ^
    ny in (0.80, 1.0]       |
    (bottom-left 20% x 20% of the touch frame)
```

- **CONFIRM** — tap anywhere outside the BACK corner. Reuses
  `dispatchConfirmTap()` verbatim (cycles the display mode / advances menu
  and settings selection / approves a pending permission prompt — whatever
  `dispatchConfirmTap()` already does on every other board).
- **BACK** — tap inside the bottom-left 20%x20% corner
  (`nx < 0.20 && ny > 0.80`). Reuses `dispatchBack()` verbatim (deny /
  act-on-highlighted-item / next page / scroll transcript).
- **MENU** — press-and-hold (~500ms, `InputManager::TOUCH_LONG_PRESS_MS`)
  anywhere on the screen, told apart from a tap purely by dwell time, exactly
  like `pollHold(confirmHold, ...)` already does for CONFIRM on the button
  boards. Reuses `dispatchConfirmHold()` verbatim (opens/closes the menu,
  closing any nested overlay first).
- **Screen wake** — any touch (press or release) wakes a sleeping screen,
  mirroring "any button press wakes it" on every other board. The tap that
  woke the screen is drained/discarded, not also dispatched as CONFIRM/BACK —
  same "wake, don't also act on an unseen prompt" rule the button path
  already follows.

### Why a corner, and why so generous

A single fixed corner is the one shape that survives *not knowing yet* which
way the touch axes actually run: even if a future hardware check flips
`swapXY`/`flipX`/`flipY` in `BoardConfig::M5PAPER_S3`, "some corner is BACK,
everything else is CONFIRM" only needs that corner's location updated, not a
new zone-layout design. A 20% x 20% zone is also large enough to tolerate a
fully mirrored or transposed axis and still be *a* corner, just not
necessarily the *intended* one — worth re-verifying on hardware, but not a
usability trap in the meantime the way a precise small hit-target would be.

### Deferred: LEFT / RIGHT / UP / DOWN / POWER

Not mapped to any tap zone in this port. Per the task's own scoping
guidance: guessing at 4-7 more precise screen regions with no hardware to
validate axis orientation against is worse than mapping none — a wrong
precise zone silently does the wrong thing, where an absent one just does
nothing. Concretely, this means on PaperS3 today:

- The transcript panel cannot be scrolled (LEFT/RIGHT are transcript
  scroll on every other board).
- There is no touch equivalent of UP-tap (force full refresh), UP-hold
  (toggle nap), or DOWN-hold (trigger "dizzy" — the no-IMU shake substitute).
- There is no touch equivalent of the POWER button's "sleep the screen"
  toggle — only automatic screen-wake-on-touch is wired up; nothing puts the
  screen back to sleep from a tap. (The menu's real "Power Off" *is* still
  reachable via CONFIRM/BACK/MENU, and does something different — see next
  section.)

Adding these later is a matter of picking more zones inside the CONFIRM
region above (or a swipe gesture — `InputManager::wasSwipe()` already exists
and this SDK's touch layer supports it) once a real unit can confirm which
edge is which.

### Known gap: a MENU long-press can be dropped during a full refresh

`InputManager::wasTouchLongPress()` is a one-shot flag the async input task
(running on its own FreeRTOS task at a ~15ms cadence) clears again on its
very next poll — there is no queue buffering long-press events the way
`popTouchTap()` buffers taps. If a long-press starts and fully resolves
while the main loop is blocked inside `display.displayBuffer(FULL_REFRESH)`
(up to ~2s, per `handleInput()`'s own doc comment), `handleTouchNav()` never
gets a chance to observe it before the flag is cleared — the MENU gesture is
silently swallowed, with no fallback.

This is a pre-existing limitation of `InputManager`'s one-shot-flag pattern
for long-press events — X4 Pro's `wasHomeKeyLongPressed()` has the identical
structure — but `wasHomeKeyLongPressed()` is never actually called anywhere
in `src/main.cpp` today, so PaperS3's MENU gesture is the first code path
that actually exercises this race in practice, not just a theoretical
parallel. Fixing it properly means queueing long-press events in
`InputManager` itself (shared code, affecting every board), which is out of
scope for this board-only port. Worth revisiting if it proves to be a
real-world annoyance on hardware — the menu is still reachable, just
possibly requiring a retry if the hold lands at exactly the wrong moment.

## A real bug this port had to fix along the way: "Power Off" would have bricked the board

`menuConfirm()`'s `case 1` (the menu's "Power Off" entry) is reachable once
CONFIRM/BACK/MENU work at all, which they now do on PaperS3. Before this
port, that path was unreachable on this board because `handleInput()` was a
no-op here — now it isn't, so this had to be checked.

The existing `powerOffSequence()` called
`freeink::PowerManager::deepSleepUntilPowerButton()` unconditionally.
`PowerManager::armPowerButtonWakeup()` returns `false` — arming **no** wake
source at all — when `powerPin() < 0`
(`freeink-sdk/libs/hardware/PowerManager/src/PowerManager.cpp:36-45`), which
is exactly PaperS3's case (`input.power == PIN_UNASSIGNED`). Critically,
`deepSleepUntilPowerButton()` never checks that return value before calling
`deepSleep()` anyway. Reused as-is, selecting "Power Off" on PaperS3 would
have deep-slept the board with nothing configured to wake it — recoverable
only by a physical reflash.

Fixed in `powerOffSequence()` (`src/main.cpp`) with an `#if
FREEINK_DEVICE_PAPERS3` branch that calls the board's real power-off instead,
`BoardPaperS3::powerOff()` — a 5-pulse train on GPIO44 to the PMS150G latch
chip, which is what this board actually needs. `#if`-guarded rather than a
runtime check because the `BoardPaperS3` library is only linked into
`[env:papers3]`'s `lib_deps`, the same reason `InputManager.cpp` itself
guards its one `PaperMono`-specific board-support call with `#if
FREEINK_DEVICE_PAPERMONO` instead of a runtime branch.

## Also fixed: the X3/X4 SPI pre-claim would have run on this board too

`setup()`'s `SPI.begin(display.sclk, sd.miso, display.mosi, display.cs)`
pre-claim exists to share one SPI bus between the display and the SD card on
X3/X4 (and to skip it on X4 Pro, whose SD is native SDMMC on separate pins).
Before this port, its guard was `if (!BoardConfig::isX4Pro())` — which does
not exclude PaperS3. PaperS3's display isn't SPI at all (it's the parallel
`LgfxEpd` bus; `display.sclk`/`mosi`/`cs` are all `PIN_UNASSIGNED`,
`BoardConfig.h:1261-1262`), and its SD card has its own dedicated SPI pins
(`BoardConfig.h:1264`) that `SDCardManager::begin()` already brings up
independently. Left as-is, this call would have run `SPI.begin()` with
`PIN_UNASSIGNED` display pins on PaperS3. Fixed by adding
`&& !BoardConfig::isM5PaperS3()` to the guard.

## Rendering: dithered monochrome only — unchanged, out of scope

Like every other board in this project, `DisplayTarget` is 1bpp/dithered
only. PaperS3's ED047TC1 glass is a genuine 16-gray-level panel and
`FREEINK_DRIVER_LGFX_EPD` can drive real grayscale, but this port does not
attempt that — same scoping call as every prior board here. Nothing about
rendering changed in this port; this was purely an input-enablement change.

## Overlap with the FreeInkApp-migration branch (sibling PR, expected)

A sibling agent in this same batch is migrating `main.cpp`'s screen/input
dispatch from the hand-rolled `dispatchConfirmTap()`/`dispatchBack()`/etc.
functions to FreeInkApp's `InputSnapshot`/`on()` handler model. That
project's own research found `FreeInkUIInputManager.h`'s `snapshotFrom()`
adapter already turns touch input into `InputSnapshot` events on boards that
support it — meaning some of what `handleTouchNav()` hand-builds here (the
long-press-vs-tap disambiguation, the async-queue draining) may become
redundant with, or need to be re-expressed as, that adapter's own handling
once that migration lands.

This is expected and was called out before this port started. Deliberately
kept isolated: `src/main.cpp`'s new code lives in one clearly-delimited
region — bounded by the
`// PaperS3 touch-only navigation — SELF-CONTAINED BLOCK, start.` /
`...end.` comments, immediately before `handleInput()` — plus one call site
inside `handleInput()` and one inside its screen-wake branch. Reconciling
against the FreeInkApp migration should be a find of that block plus its two
call sites, not a rewrite of unrelated code. The tap-zone *policy* (which
corner is BACK, hold-for-menu) is expected to carry over conceptually even
if the mechanism it's expressed through changes.

## Capability matrix

| Capability | Status | Evidence |
|---|---|---|
| Display (ED047TC1 960x540, `LgfxEpd`/M5GFX) | Wired, reuses the LilyGo T5 S3 display class | `BoardConfig.h:1230-1246`, `LgfxEpdDriver.cpp:331-336` |
| GT911 touch | Wired — new tap-zone dispatch (this port) | `BoardConfig.h:1271-1279` |
| RTC (BM8563 @ 0x51) | Wired, zero new code — gated by `BoardConfig::hasRtc()` | `BoardConfig.h:1287-1291` |
| Battery (ADC GPIO3 + charge-status GPIO4) | Wired, reuses `BatteryMonitor`'s generic ADC path | `BoardConfig.h:1267-1270` |
| SD card | Wired as plain SPI, own dedicated pins (not shared with display) | `BoardConfig.h:1264` |
| Buzzer (LEDC, GPIO21) | **Out of scope** — no Buzzer abstraction in this codebase | `BoardConfig.h:753-755` |
| IMU (BMI270 @ 0x68) | **Out of scope** — not a supported `ImuType` yet (SDK-level gap, not this port's) | `BoardConfig.h:1290, 1252-1253` |
| Frontlight | N/A — PaperS3 has none (`NO_FRONTLIGHT`) | `BoardConfig.h:1280` |
| Gauge | N/A — ADC battery only (`NO_GAUGE`) | `BoardConfig.h:1285` |
| Power latch | N/A — PMS150G self-latches; software off is `BoardPaperS3::powerOff()` | `BoardConfig.h:1246, 1293` |
| Buttons | **None exist on this board** — see "Tap-zone layout" above | `BoardConfig.h:1257, 1265-1266` |

## Build

```
export CURL_CA_BUNDLE=/root/.ccr/ca-bundle.crt SSL_CERT_FILE=/root/.ccr/ca-bundle.crt REQUESTS_CA_BUNDLE=/root/.ccr/ca-bundle.crt
pio run -e papers3
```

See the PR description for the resulting RAM/Flash usage. A successful build
only proves the firmware compiles and links against the profile as
documented; it proves nothing about whether the touch axis/zone assumptions
above match a real unit, because no real unit has run this yet.
