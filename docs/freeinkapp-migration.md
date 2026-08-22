# FreeInkApp migration

`src/main.cpp` used to drive its own hand-rolled overlay-stack UI model:
`displayMode` (NORMAL/PET/INFO) plus three independent `bool` overlay flags
(`menuOpen`/`settingsOpen`/`resetOpen`), each drawn as a floating box on top
of whatever `drawStatusPanel()` was already showing, dispatched through a
hand-written `handleInput()` → `dispatchConfirmTap()`/`dispatchConfirmHold()`/
`dispatchBack()` chain that manually checked each flag in priority order.

This migrates that model onto the FreeInk SDK's `FreeInkApp` runtime
(`freeink-sdk/libs/ui/FreeInkUI/include/FreeInkApp.h`) — screens as plain
builder functions, a real `on(ActionId, handler)` dispatch table, and
`setScreen()` transitions instead of flag flips. This doc records the design
decisions that fell out of doing that on hardware the SDK itself had never
been wired to before (see the file-header comment in `main.cpp` for the
one-paragraph summary; this is the detail behind it).

## Screen model

FreeInkApp has no screen-stack concept — `setScreen()` just swaps a function
pointer (see `FreeInkApp::setScreen()`). Given that, the pre-migration state
maps onto five `Screen<N>` builder functions:

| Screen function   | Replaces                                   |
|--------------------|---------------------------------------------|
| `screenMain`       | `displayMode` NORMAL/PET/INFO + HUD/clock/prompt/passkey, all drawn by `drawStatusPanel()` |
| `screenMenu`       | `menuOpen`                                  |
| `screenSettings`   | `settingsOpen`                              |
| `screenReset`      | `resetOpen`                                 |
| `screenSleep`       | the POWER-button sleep screen (was drawn directly by `sleepScreen()`) |

`screenPowerOff` exists too, for the deep-sleep "powered off" screen
`powerOffSequence()` shows before it calls
`PowerManager::deepSleepUntilPowerButton()` and never returns.

**`displayMode` (NORMAL/PET/INFO) and the passkey-pairing display stay
sub-modes of `screenMain`, not separate FreeInkApp screens.** CONFIRM-tap
cycling through them, and the passkey check (`blePasskey() != 0`), never
involved a `menuOpen`-style flag flip pre-migration either — they're
`if`/`else` branches inside one draw function. Turning each into its own
`Screen` would have meant a `setScreen()` call (and an unwanted transition
refresh) for what's actually just "draw different content this frame," so
they stay exactly what they were: branches inside `screenMain`, gated on the
same state (`displayMode`, `blePasskey()`) as before.

A `currentScreen` enum (`SCR_MAIN`/`SCR_MENU`/`SCR_SETTINGS`/`SCR_RESET`/
`SCR_SLEEP`) tracks which of the five is active, kept in lockstep with every
`gApp->setScreen()` call. This is *not* FreeInkApp state — FreeInkApp itself
only knows the current `ScreenFn` pointer, which isn't comparable or
introspectable from outside. `currentScreen` exists purely so the hand-rolled
CONFIRM tap/hold dispatch (see below) can answer "what's on screen right now"
without maintaining a parallel copy of FreeInkApp's own dispatch logic.

## Overlay screens: the visual tradeoff

Pre-migration, the menu/settings/reset box floated *over* whatever
`drawStatusPanel()` was already showing underneath — opening the menu while
the clock face was up left the clock visible around the box's edges, since
the panel was drawn first and the overlay drawn on top of it in the same
frame.

FreeInkApp's one-active-screen model has nothing "underneath" once
menu/settings/reset become their own screens — there's no composition step
between screens, only one `ScreenFn` running per frame. `screenMenu()` (and
settings/reset) therefore clears the whole page to white first
(`drawOverlayBox()`) and draws only the centered box; whatever `screenMain`
last drew is gone, not dimmed-and-visible behind it.

This is a real, deliberate visual regression from the pre-migration behavior
— accepted rather than worked around, because reproducing the old
composited look would mean either (a) threading `menuOpen`-equivalent state
back into `screenMain` and calling it from inside `screenMenu` (defeating
the point of separate screens — the overlay boolean soup this migration
removes), or (b) giving `FreeInkApp` a screen-stack / compositing concept it
doesn't have. Given the panel is monochrome and the box already fills most
of the width, the practical loss is small (a sliver of clock/pet text at the
box's edges) against a real reduction in `main.cpp`'s state-machine
complexity.

## Input routing: what goes through FreeInkApp, what stays hand-rolled

Only **BACK** is routed through FreeInkApp's real `on()`/`ActionId` dispatch.
Every screen registers one full-screen hit (`screen.frame().hit(screen.frame().screen(),
ACT_<SCREEN>_BACK, 0, InputBack)`), and `gApp->render(gSnapshot)` — called
once per `loop()` tick with `gSnapshot.back` set from this tick's BACK
edge — fires the matching `on()` handler (`onMainBack`/`onMenuBack`/
`onSettingsBack`/`onResetBack`), which does exactly what
`dispatchBack()`'s per-overlay branch used to do inline.

**CONFIRM, UP, DOWN, LEFT, and RIGHT stay hand-rolled**, driven from
`handleInput()`'s `pollHold()` state machine exactly like pre-migration,
for two independent reasons:

1. **Tap-vs-hold disambiguation.** `InputSnapshot` (FreeInkUICore.h) has no
   hold concept for physical buttons — `confirm`/`back`/`prev`/`next` are
   plain edge-triggered `bool`s, meant for touch/tap semantics. CONFIRM's
   behavior fundamentally depends on tap (advance selection / approve /
   cycle screen) versus ~600ms hold (open the menu, or back out one level)
   being told apart, which needs the same `pollHold()` press-timing state
   this file already had. Routing CONFIRM through FreeInkApp would mean
   inventing a hold protocol FreeInkApp doesn't have, for a single button.

2. **CONFIRM's "advance selection" behavior doesn't fit FreeInkApp's
   focus-then-confirm model.** The lists in `screenMenu`/`screenSettings`/
   `screenReset` are drawn with `props.action = NO_ACTION` — deliberately
   *not* wired to `list()`'s own hit registration — because this board's
   CONFIRM button means "move to the next item," not "activate the focused
   item" (there's no separate focus-move + confirm gesture pair on 5
   physical buttons the way there is on touch). Making that fit FreeInkApp's
   model would need auto-focus bookkeeping (register every row as a
   focusable hit, track a focus index, synthesize focusNext on CONFIRM)
   for no behavioral change over what `dispatchConfirmTap()` already does
   in four lines.

LEFT/RIGHT (transcript scroll) stay hand-rolled for a narrower reason: they're
drained from `input.popPress()`'s queue in a `while` loop, so a burst of
several queued presses in one tick decrements/increments the transcript
offset once per press. Collapsing that to a single `InputSnapshot.prev`/
`.next` edge (which `route()`/`render()` consume once per call) would lose
presses queued faster than the ~16ms loop tick. POWER (screen sleep) stays
hand-rolled because it's not a FreeInkApp concern at all — it fully bypasses
`gApp->render()`/`route()` for as long as the screen is asleep (see below).

The result: `gSnapshot` (a file-level `InputSnapshot`) is rebuilt every tick
by `handleInput()` with only `.back` ever set to anything but its default,
and handed to `gApp->render()` once per `loop()` tick. Every other button's
effect on FreeInkApp state — `gApp->setScreen()`, `gApp->invalidate()` — is
called directly from the hand-rolled handlers, not through `InputSnapshot`
at all.

`freeink-sdk`'s `FreeInkUIInputManager.h` (`snapshotFrom()`) was *not* used
to build `gSnapshot`, despite existing for exactly this purpose. Its
`wasPressed()`-based edge detection isn't safe to call from `loop()` in this
codebase: `InputManager` runs its edge tracking on a background FreeRTOS task
(`input.beginAsync()`), and only `popPress()` (a queue drain) and
`isPressed()` (a level read) are documented safe to call from another task
once async polling owns the edge state — calling `wasPressed()` from
`loop()` would race the async task's own `update()`. `gSnapshot.back` is
therefore assembled by hand from `popPress()`/`wasHomeKeyTapped()`, matching
the pattern the rest of `handleInput()` already used pre-migration.

## Theme tokens

`FreeInkApp` takes a `ThemeTokens` struct as its only styling input and does
zero file/JSON I/O itself. The SDK's own migration doc
(`freeink-sdk/docs/freeink-ui.md`, "Adopting FreeInkUI in an Existing
Firmware") sketches sourcing tokens from a JSON `ThemeDocument` for apps that
already have config-file infrastructure; this firmware has none (settings
are a packed NVS struct, not JSON) and gains nothing from adding a JSON
parser just to describe five colors and a row height. `buildTheme()` instead
returns a `ThemeTokens` literal built on `freeink::ui::defaultThemeTokens(0,
0, 0)` — font slot 0 for every text role, since `DisplayTarget` points every
slot at the same bundled bitmap font unless `setFont()` is called (which this
firmware never does) — with `smallText`/`bodyText`/`titleText`/`rowHeight`
overridden to match the pre-migration hand-rolled menu's look (`Color::Black`
selected / `Color::DarkGray` unselected text, 30px rows).

## What did not change

- `BoardConfig` runtime capability checks (`hasImu()`, `hasRtc()`,
  `isX4Pro()`, `FrontlightManager::present()`) are unchanged and still used
  exactly where they were — the settings screen's brightness row is still
  gated on `frontlight.present()` at `setup()` time (`hasBrightnessItem`,
  `SETTINGS_ITEMS_LIGHT` vs `SETTINGS_ITEMS_BASE`), and shake/IMU/RTC gating
  in `handleInput()`/`clockActive()` is unchanged. No `#ifdef` was
  introduced or removed by this migration.
- The actual panel content — HUD text, clock face, pet stats page, info
  pages, credits/about, transcript — is untouched; `drawStatusPanelContent()`
  is a rename of the old `drawStatusPanel()` minus the passkey early-return
  and the overlay draw call it used to end with (both now live in
  `screenMain()` and the overlay screens respectively).
- The DC-balance full-refresh timer (`FULL_REFRESH_INTERVAL_MS`), the
  per-state sprite redraw cadence (`renderSprite()`'s per-`PersonaState`
  intervals), and the nap/dizzy/celebrate one-shot triggers are all
  unchanged in behavior — they now report a `RefreshHint` instead of calling
  `display.displayWindow()`/`displayBuffer()` directly, forwarded to
  `gApp->invalidate()` by their caller (`screenMain`), with the actual panel
  push happening once per `loop()` tick via `freeink::ui::present(display,
  gApp->lastRenderRefreshHint())`.
