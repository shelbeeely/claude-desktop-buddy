# Free Ink Claude Buddy

This is a [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) project
first: the SDK's e-paper display, input, and board-abstraction layers do
the actual work of driving the panel, and everything specific to Claude is
a layer on top of that. What it happens to speak is Claude's BLE desk-pet
protocol — Claude for macOS and Windows can connect Claude Cowork and
Claude Code to maker devices over BLE, so developers and makers can build
hardware that displays permission prompts, recent messages, and other
interactions. We've been impressed by the creativity of the maker
community around Claude - providing a lightweight, opt-in API is our way
of making it easier to build fun little hardware devices that integrate
with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet that lives off permission approvals and
interaction with Claude. It sleeps when nothing's happening, wakes when
sessions start, gets visibly impatient when an approval prompt is waiting,
and lets you approve or deny right from the device.

## Hardware

The firmware targets three **[Xteink](https://github.com/Free-Ink/freeink-sdk)**
boards via the FreeInk SDK, vendored as the `freeink-sdk` git submodule
(`git submodule update --init` before building):

| | X4 | X3 | X4 Pro |
|---|---|---|---|
| MCU | ESP32-C3 | ESP32-C3 | ESP32-S3 (PSRAM) |
| Panel | SSD1677, 800×480 | UC8253/UC8279d, 792×528 | SSD1677/UC8179/UC8279, 800×480 |
| Input | 7-button ADC ladder | 7-button ADC ladder | 2 digital nav buttons + GT911 touch |
| IMU | none | QMI8658 | none |
| RTC | none | DS3231 | BM8563 |
| Battery | ADC estimate | BQ27220 gauge | CW2017 gauge |
| Frontlight | none | none | dual warm/cool PWM |
| Build | `env:xteink` | `env:xteink` | `env:xteink_x4pro` |

X4 and X3 share the ESP32-C3 and a pinout, so **one binary drives both** —
this is the FreeInk SDK's own documented pattern (its README's "Supported
devices" table), not something specific to this firmware. See "Multi-board
support" below for how `main.cpp` adapts to whichever board actually
booted. X4 Pro is a different MCU family entirely and builds as its own
target.

A few things about the ESP32-C3 boards (X4/X3) shape parts of the firmware
regardless of which one is running:

- **Seven-button ADC ladder, not a handful of discrete GPIOs.**
  `InputStyle::XteinkAdcLadder`: two ADC pins, each a resistor ladder
  multiplexing several buttons, decoding to seven semantic buttons —
  `BACK`, `CONFIRM`, `LEFT`, `RIGHT`, `UP`, `DOWN`, `POWER`.
  `InputManager::getState()` handles the decode; the firmware always
  routes through this semantic API, never raw `analogRead()` thresholds.
  X4 Pro uses a different input style — see "Multi-board support".
- **The SD card slot shares the display's SPI bus** (`sclk`/`mosi`
  unassigned, `separateSpi=false` — only `miso` (GPIO7) and `cs` (GPIO12)
  are unique to the card). `FreeInkDisplay::begin()` only wires MISO into
  the bus when the panel driver needs it, which none of X4/X3's panel
  controllers do — so `setup()` claims the SPI bus once, MISO included,
  before `display.begin()` runs its own `SPI.begin()`. This is the same
  sequence [Free-Ink's own X4 app, inkdeck](https://github.com/Free-Ink/inkdeck/blob/main/src/main.cpp),
  uses for the same board. X4 Pro's SD card is native SDMMC on entirely
  separate pins, so it skips this claim — see "Multi-board support".

## Multi-board support

`main.cpp` is one file for all three boards. Where a board has real
hardware, the firmware uses it; where it doesn't, the same button/software
substitute from the original X4-only version remains — nothing is board-
specific by `#ifdef`, it's all runtime checks (`BoardConfig::hasImu()`,
`hasRtc()`, `isX4Pro()`, `FrontlightManager::present()`), so one binary
(X4/X3) or one shared source file (X4 Pro) can serve every capability
level without duplicating logic:

- **X3 vs. X4 detection.** `freeink::selectXteinkDevice()` (`main.cpp`
  `setup()`, before any display/SD bring-up) I2C-fingerprints X3-only
  peripherals (BQ27220 gauge, DS3231 RTC, QMI8658 IMU) on a shared probe
  bus. A match switches `BoardConfig::ACTIVE` to the X3 profile and calls
  `display.setDisplayX3()`; everything downstream — panel size
  (`PANEL_TOTAL_W`/`H`, read from `display.getDisplayWidth()`/`Height()`
  rather than hardcoded, since X3's panel is a different size than X4's),
  `BatteryMonitor`, `Imu`, `Rtc` — then reads the correct board
  automatically. A no-op on `env:xteink_x4pro` (X4 Pro is a different MCU
  family; the function's I2C probe is C3-pinout-specific and would be
  unsafe there, so it's compiled out).
- **Shake → `dizzy`.** On X3 (`BoardConfig::hasImu()`), a real accelerometer
  reading (rolling-baseline magnitude delta, same idea as the earlier
  M5-era generation's shake detection) triggers `dizzy` directly, running
  alongside — not instead of — the `DOWN`-held button substitute every
  board keeps. X4 and X4 Pro have no IMU, so only the button works there.
- **Clock face.** On X3 and X4 Pro (`BoardConfig::hasRtc()`), the `Rtc`
  library reads real wall-clock time that survives a reboot, and
  `platformTimeSync()` writes through to it on every bridge sync so it
  stays accurate. X4 has no RTC and falls back to the RAM-only software
  clock described in "Menu system" below — `getSoftClock()` tries the real
  RTC first and only falls back when there isn't one (or a read fails).
- **Battery.** `BatteryMonitor` already picks ADC (X4) vs. I2C fuel gauge
  (X3's BQ27220, X4 Pro's CW2017) at runtime from
  `BoardConfig::ACTIVE.batteryGauge` — `platformBatteryStatus()` needed no
  board-specific code, just reading whichever fields the active backend
  fills in (`chargingKnown`/`externalPowerKnown`) before falling back to a
  manual `usbDetect` GPIO read on boards without a gauge.
- **Frontlight.** Only X4 Pro has one — `FrontlightManager::present()`
  gates a `brightness` item that appears in Settings only on that board
  (see "Menu system"); inert (and absent from the menu) everywhere else.
- **X4 Pro's input is genuinely different**, not just a config variant:
  `InputStyle::DigitalButtons`, not the ADC ladder. Per its `BoardConfig`
  profile, `back`/`confirm`/`left`/`right` are all `PIN_UNASSIGNED` — the
  two physical nav buttons wire to the semantic `UP`/`DOWN` slots instead,
  so what reads as "hold UP to nap" or "hold DOWN to get dizzy" elsewhere
  is, on this board, physically the two nav buttons. `CONFIRM` is
  synthesized from a touch tap (`InputManager::getState()` ORs in
  `serviceTouch()`'s result automatically — no extra code needed). `BACK`
  has no physical button or synthesized `BTN_BACK` bit at all — the
  capacitive Home key is its own separate API
  (`wasHomeKeyTapped()`/`wasHomeKeyPressed()`), not part of the `BTN_*`
  system `popPress()` drains, so `handleInput()` calls it explicitly and
  feeds it into the same `InputSnapshot.back` edge every other board's
  physical `BACK` button sets (see "Menu system" and
  `docs/freeinkapp-migration.md` for how that edge gets routed to a
  screen's BACK handler). `LEFT`/`RIGHT` have no physical or synthesized path
  on this board at all; transcript scrolling still works via `BACK`'s
  "next page" behavior (see "Controls"), just without the fine-grained
  back-and-forth `LEFT`/`RIGHT` gives on X4/X3.

## Building and flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then, for X4 or X3:

```bash
pio run -e xteink
pio run -e xteink -t upload   # over USB, once built
```

or for X4 Pro:

```bash
pio run -e xteink_x4pro
pio run -e xteink_x4pro -t upload
```

### Flashing from the browser

`.github/workflows/firmware.yml` builds both targets on every push to
`main` and publishes them to GitHub Pages as one one-click web installer
([ESP Web Tools](https://esphome.github.io/esp-web-tools/), Web Serial —
desktop Chrome or Edge only). The manifest carries both chip families
(ESP32-C3 for X4/X3, ESP32-S3 for X4 Pro) in one `builds` array; ESP Web
Tools reads the connected chip and installs the matching entry
automatically, so the page needs only one Install button for all three
boards. For each target, the workflow merges the bootloader, partition
table, `boot_app0`, and app into one image at the offsets `pio run -t
upload` itself would use (`0x0`/`0x8000`/`0xe000`/`0x10000` — identical
between the two chips in this Arduino core, confirmed by capturing pio's
own planned `esptool` invocation for both `env:xteink` and
`env:xteink_x4pro` separately, not assumed), so the browser only has to
write one file. The same job also uploads both merged images (and the
unmerged parts) as downloadable build artifacts on every push and PR, not
just `main`.

**One-time setup this workflow can't do for you:** in the repo's **Settings
→ Pages**, set **Source** to **GitHub Actions**. Until that's set, the
`deploy-pages` job fails with a clear error rather than silently no-op'ing.

If you're starting from a previously-flashed device, wipe it first:

```bash
pio run -e xteink -t erase && pio run -e xteink -t upload   # (env:xteink_x4pro for X4 Pro)
```

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the device:

- Make sure it's awake (any button press)
- Check that it hasn't been power-cycled off since its last pairing

## Beyond Claude

The wire protocol (see REFERENCE.md) doesn't care which app is on the
other end — Claude's desktop apps are just the reference sender this
device shipped pairing with. **[adapters/](adapters/)** has ready-made
bridges for GitHub Copilot CLI, OpenAI Codex CLI, and Aider (plus a
template for wiring up anything else) over USB serial, so the same
firmware can show activity from — and, for tools with a permission hook,
forward approval prompts from — other AI coding tools too.

## Controls

This firmware aims to match the earlier M5StickCPlus-based desktop-buddy's
button semantics as closely as this board's very different button set
allows: `CONFIRM` and `BACK` play the same roles that generation's two
physical buttons (`A`/`B`) did — tap `CONFIRM` to advance/approve, hold it
to open the menu; tap `BACK` to act/deny. The X4's extra buttons
(`LEFT`/`RIGHT`/`UP`/`DOWN`/`POWER`) cover things that generation had no
spare button for.

| Button                    | Normal                                                                    | On an approval prompt |
| -------------------------- | -------------------------------------------------------------------------- | ---------------------- |
| `CONFIRM` tap               | advance selection in a menu, else cycle screen (home → pet → info)         | **approve**             |
| `CONFIRM` hold (~600ms)     | open the menu; closes a nested settings/reset panel first if one is open   | —                        |
| `BACK` tap                  | act on the highlighted menu item, else next page/scroll transcript         | **deny**                |
| `LEFT` / `RIGHT`            | scroll the transcript panel                                                | —                        |
| `UP` tap                    | force a full e-paper refresh now (clears ghosting instead of waiting for the 5-minute timer) | —          |
| `UP` hold (~800ms)          | toggle nap — pauses the pet and accumulates nap time, same mechanic as the earlier generation's face-down nap | —          |
| `DOWN` hold (~800ms)        | trigger `dizzy` for 2s — the closest substitute for a shake gesture, since the X4 has no IMU | —          |
| `POWER` tap                 | toggle screen sleep — blanks the panel and halts rendering; any button press wakes it | —          |

A sleeping screen always just wakes on the first press — that press never
also fires the button's normal action, so waking the device can't
accidentally approve or deny a prompt you haven't seen yet. An incoming
approval prompt wakes a sleeping screen automatically so it's never missed.

**On X4 Pro**, this table's semantic buttons map to different physical
controls — see "Multi-board support" for the full explanation. In short:
`CONFIRM` is a touch tap anywhere on the panel, `BACK` is the capacitive
Home key (there is no `LEFT`/`RIGHT` on this board — transcript scrolling
falls back to `BACK`'s "next page" behavior), and the two physical nav
buttons are wired to the semantic `UP`/`DOWN` slots, so "hold UP to nap"
and "hold DOWN for dizzy" are physically those two buttons rather than a
directional pair. `POWER` and `CONFIRM`-hold-for-menu work the same as
X4/X3.

**Input polling is async, not synchronous with the render loop.**
`freeink::ui::present()` (the FreeInkApp-era wrapper around
`display.displayBuffer()`) blocks the main loop for anywhere from ~50ms (a
fast partial refresh) to ~2s (a full refresh) — a CONFIRM/BACK
press-and-release landing entirely inside one of those calls would be
silently dropped by a synchronous `input.update()`/`wasPressed()` loop. `setup()` calls `input.beginAsync()` (spawns `InputManager`'s own
FreeRTOS polling task) and `handleInput()` drains edges via `popPress()`
instead — the same pattern Free-Ink's inkdeck uses for this exact reason.
`isPressed()` (a plain level read, used for the `DOWN` long-press timer
above) stays safe to call from the main loop even with async polling
active, since the background task's own `update()` call keeps it current.
Only `wasPressed()`/`update()` are unsafe to call yourself once the async
task owns the edge state.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, bit-inverted blink   |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | `DOWN` held, or a real shake on X3 | spiral eyes, wobbling |
| `heart`     | approved in under 5s        | floating hearts             |

## Menu system

The status panel has three screens (`CONFIRM` tap cycles through them),
plus menu/settings/reset screens reachable from it via `CONFIRM`-hold — the
same navigation structure the earlier M5-based generation used, adapted to
this board's buttons and single fixed panel (there's no "peek" scaling: the
pet sprite keeps animating in its own region regardless of which screen is
showing beside it). Screens are built on the FreeInk SDK's `FreeInkApp`
runtime rather than a hand-rolled overlay-flag state machine (see
`docs/freeinkapp-migration.md`); menu/settings/reset each replace the whole
panel while open rather than floating over the screen underneath — a
deliberate tradeoff of that migration, see the doc's "Overlay screens"
section.

- **home** — session counts, the latest message, and a scrollable
  transcript (`LEFT`/`RIGHT`), or the approval prompt when one's pending.
- **pet** — two pages (`BACK` cycles them): mood/fed/energy pips, level,
  approvals/denials/nap time/tokens; then a how-to page.
- **info** — six pages (`BACK` cycles them): about, button reference,
  Claude session/link status, device/battery status, Bluetooth pairing
  info, and credits.

Holding `CONFIRM` opens the **menu** (`settings`, `turn off`, `help`,
`about`, `demo`, `close`) — `CONFIRM` taps advance the selection, `BACK`
acts on it, matching the earlier generation's `A`-advances/`B`-selects
pattern exactly. `settings` opens a submenu (`hud`, `flash`, `character`,
`brightness` — X4 Pro only, see below —, `reset`, `back`): `hud` toggles
the home screen's session/transcript content, `flash` toggles the
bit-inverted blink during `attention` (the substitute for that
generation's LED, still used even on boards that could theoretically have
a real one, since none of these three do), `character` cycles through the
compiled-in `bufo` plus every `.charpack` found on the SD card at boot
(persisted to NVS). `reset` opens a tap-twice-to-confirm submenu:
`delete char` reverts to the compiled-in `bufo` (without touching the SD
card's files — unlike the earlier generation, this firmware doesn't own
that storage), and `factory reset` clears NVS (stats, owner, pet name,
settings, character choice) and BLE bonds, then restarts.

**Brightness** (X4 Pro only, appears in Settings only when
`FrontlightManager::present()`) cycles the real dual warm/cool frontlight
through 5 steps (20–100%) via `FrontlightManager::setBrightness()` — the
one setting from the earlier M5-era generation that has a genuine hardware
match here, restored rather than substituted (that generation's own
brightness control had no equivalent on plain X4/X3, which have no
frontlight at all).

**Turn off** puts the device into real ESP32 deep sleep
(`PowerManager::deepSleepUntilPowerButton()`), woken by the `POWER`
button — none of these three boards has a PMIC hard-off like the earlier
generation's AXP192, so deep sleep is the equivalent off state on all of
them.

**Clock face.** On X3 and X4 Pro, `Rtc` reads real wall-clock time that
survives a reboot; `platformTimeSync()` (`main.cpp`) writes every bridge
time-sync through to it. X4 has no RTC, so it falls back to a software
clock kept in RAM: one synced moment (from the bridge's `time` heartbeat)
plus the `millis()` it arrived at, with "now" derived by adding elapsed
time on every read — resets on reboot, accurate only while powered (see
"Multi-board support"). When nothing's happening (home screen, no
sessions, no overlay open, on USB power, and the clock has a value —
immediately on boot for X3/X4 Pro if the RTC was ever set, or after the
first bridge sync this session on X4), the home screen shows this clock
instead of session info, and the pet's mood follows the same hour-of-day/
weekday table the earlier generation used (a pure function of the time,
so it needed no changes).

**Nap** (`UP` held) is this firmware's substitute for that generation's
face-down-to-nap gesture, which needed an IMU — used on every board here,
even X3 (which does have a real IMU): shake already covers "something
physical happened to the device," and repurposing that same sensor for
"user deliberately wants a nap" would conflate two different intents, so
nap stays a deliberate button hold everywhere. It pauses the pet's
animation and accumulates nap time toward the same stat the gesture did;
holding `UP` again ends it.

### Sprite region and refresh strategy

The generated bufo assets top out at 192×200px (`bufo::MAX_ICON_W/H`). The
fixed animation region is `SPRITE_X=40, SPRITE_Y=40, SPRITE_W=240,
SPRITE_H=260` (`main.cpp`) — byte-aligned on both axes (240/8=30) so the
`attention`-state bit-inversion trick below never touches a partial byte at
the region's edge — with the rest of the panel to the right as a
text/status panel (session counts, transcript, approval prompt, battery,
BLE link state), drawn with `FreeInkUI`'s dependency-free `DisplayTarget`
(bundled Noto Sans bitmap font, no external font library). The sprite
region's fixed size comfortably fits every board's panel (800×480 on
X4/X4 Pro, 792×528 on X3); everything panel-relative around it
(`PANEL_TOTAL_W`/`H`, `PANEL_W`, every full-screen overlay) reads the real
size from `display.getDisplayWidth()`/`getDisplayHeight()` at boot instead
of assuming 800×480 — see "Multi-board support".

Each state's `renderSprite()` case redraws the sprite region on its own
cadence and reports a `RefreshHint` (`Fast` for a partial refresh, `Full`
for a whole-panel one); `screenMain()` forwards that to `gApp->invalidate()`,
and `loop()` pushes it to the panel once via `freeink::ui::present()` — see
`docs/freeinkapp-migration.md`. The cadence and refresh strength per state:

| State | What runs | Why |
|---|---|---|
| `sleep` | Draw once on entry, then nothing until the state changes | static frame, no refresh loop |
| `idle` | Fast (partial) refresh over the sprite region, ~1.5s cadence | slow partial refresh reads as a charming blink, not lag |
| `busy` | Fast refresh, ~350ms cadence | fast partial, but still windowed — never touches the panel outside the sprite region |
| `attention` | Fast refresh at ~400ms, alternating the icon with a bit-inverted copy of the same region (manual XOR over the framebuffer bytes) | no LED on this board — this reads as urgent, distinct from idle's slower blink |
| `celebrate` | Cycles the clip's frames, one Full (whole-panel) refresh each | infrequent (every 50K tokens) — can afford the cost; also resets the DC-balance timer below |
| `dizzy` | Fast refresh, ~200ms cadence, one-shot ~2s | short-lived, triggered by a `DOWN` long-press |
| `heart` | Fast refresh, ~250ms cadence, one-shot ~2s | similar budget to celebrate but partial, not full — it fires on every fast approval, far more often |

**Mandatory full-refresh timer**, independent of the table above:
`FULL_REFRESH_INTERVAL_MS` (`main.cpp`, currently 5 minutes) forces a
whole-panel `FULL_REFRESH` on a fixed cadence regardless of what state the
pet is in, so a long `busy`/`idle` stretch running only partial refreshes
over the sprite region never lets the panel's DC balance degrade. It's one
named constant, checked once per `loop()` iteration, deliberately not tied
to any other timer in the file.

## Characters

The firmware ships one compiled-in character (`bufo`) as `freeink::Icon`
assets, and can optionally load a different character from the SD card at
boot.

### Asset pipeline

`tools/gif_to_icons.py` is the offline (host-side, not on-device) step: it
takes a character pack already normalized by `tools/prep_character.py`
(96px-wide, cropped to one consistent scale across every state) and emits
`freeink::Icon` C structs (`libs/assets/Icons/include/Icon.h`: 1-bpp,
MSB-first rows, `opticalCenterY` from the real ink pixels) directly into a
header, one clip/frame array per state, mirroring a manifest's own "single
filename or list of filenames" shape (a multi-clip state is an idle-carousel
rotation). Classification per pixel: within tolerance of the manifest's
flattened background color → transparent; else dark → ink; else (a light
non-background pixel, e.g. a highlight) → also transparent, since the
sprite region is cleared to white before every blit and a transparent pixel
just lets that white show through.

```bash
python3 tools/gif_to_icons.py characters/bufo \
  --scale 2 --out src/assets/icons_bufo.h --namespace bufo
```

`src/assets/icons_bufo.h` (checked in, ~650KB of packed icon data across
139 frames) is the generated output for the bundled `bufo` character —
proof the pipeline runs end-to-end, not just a described-but-unexercised
script. `--scale 2` (192px-wide icons) reads better than the source's
native 96px on the X4's 800×480 canvas, which is much larger by area than
a typical small color LCD.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_0.gif", "idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide; height up to ~140px keeps the character at a consistent
scale across states. Crop tight to the character — transparent margins
waste screen and shrink the sprite. `tools/prep_character.py` handles the
resize: feed it source GIFs at any sizes and it produces a 96px-wide set
where the character is the same scale in every state.

See `characters/bufo/` for a working example.

### SD-backed character packs

The compiled-in `bufo` above is the guaranteed fallback; the X4 also has a
real SD card slot (shared with the display's SPI bus — see "Hardware"
above), and `--sd-out` on the same conversion pass writes a second output,
a binary `.charpack`, so a character doesn't have to live in flash to be
usable.

```bash
python3 tools/gif_to_icons.py characters/bufo \
  --scale 2 --out src/assets/icons_bufo.h --namespace bufo \
  --sd-out characters/bufo/bufo.charpack
```

**Install:** copy the `.charpack` file(s) to `/characters/` on the SD card
and insert it — no rebuild needed. This is deliberately the same shape as
[CrossPoint's SD-card-font install](https://github.com/crosspoint-reader/crosspoint-reader/blob/main/docs/sd-card-fonts.md#option-3-manual-sd-card-copy):
discovered and loaded at boot, not pushed over BLE (see below). `main.cpp`'s
`scanCharacters()` enumerates every `*.charpack` file under `/characters`
alongside the compiled-in `bufo`; Settings > `character` cycles between
them (see "Menu system"), and the choice persists to NVS across reboots. No
SD card, or none found — it falls back to the compiled-in `bufo`
automatically.

**Format** (`src/sd_character_pack.h`/`.cpp`, generated by the same
`write_charpack()` that produces the `.h` from identical per-frame data —
verified byte-for-byte identical during development, not just "should
match"): a fixed-size state→clip→frame index (12B header + 4B/state + 4B/
clip + 16B/frame — ~2.3KB total for bufo's 139 frames) followed by the raw
packed bits, referenced by absolute file offset. The device reads only that
small index into RAM at `open()` (bounded static arrays, not a heap
allocation) and seeks+reads one frame's bytes on demand per `getFrame()`
call, so an SD-backed character costs a few KB of RAM regardless of how
large the pack is on disk.

**Heap discipline.** This follows crosspoint-reader's own
[`.skills/heap-discipline`](https://github.com/crosspoint-reader/crosspoint-reader/blob/main/.skills/heap-discipline/SKILL.md)
guidance, written for the same hardware class (ESP32-C3, no PSRAM, one
full-frame-sized buffer as the tightest resource): the index arrays in
`SdCharacterPack` are fixed-size statics (`MAX_CLIPS=48`, `MAX_FRAMES=400`,
generous over bufo's 15/139), not `std::vector`, so there's no growth-
triggered fragmentation; the frame buffer (`SD_FRAME_BUF_CAP=16000` bytes,
sized for up to ~320×400px) is one static array allocated once, never
per-frame; and the SD file handle is never held open across calls (SdFat's
`FsFile` has a deleted copy/move assignment operator in this SDK's bundled
version, so `getFrame()` reopens the short on-SD path per call instead of
fighting that — at this render cadence, hundreds of ms to seconds between
frames, that reopen is well inside the frame budget).

**What this is not — yet.** BLE folder-push (`char_begin`/`file`/`chunk`/
`file_end`/`char_end` in `xfer.h`) isn't wired to write `.charpack` files to
SD; the manual-copy path above is the only install method right now — see
"Known limitations" below. There's also no runtime check that an SD pack's
frame dimensions fit the sprite region (`SPRITE_W`×`SPRITE_H`, 240×260) —
pick `--scale` to match, same constraint as the compiled-in asset.

## Resource budget

`env:xteink` (X4/X3, ESP32-C3, from `pio run -e xteink`):

```
RAM:   [==        ]  15.5% (used 50940 bytes from 327680 bytes)
Flash: [==        ]  22.3% (used 1461593 bytes from 6553600 bytes)
```

`env:xteink_x4pro` (ESP32-S3, from `pio run -e xteink_x4pro`):

```
RAM:   [==        ]  18.8% (used 61740 bytes from 327680 bytes)
Flash: [==        ]  23.1% (used 1511286 bytes from 6553600 bytes)
```

Both use `default_16MB.csv` for `board_build.partitions` — the generic
board definitions' stock partition tables (both C3 and S3) target a
smaller flash part than either board's real 16MB, and X4 Pro's own OEM
dual-OTA scheme isn't needed since this isn't an OTA-updated build (see
`platformio.ini`). The RAM figures are static `.data`/`.bss` only, from
the linker's own accounting. They do **not** include the e-paper
framebuffer (`displayWidth / 8 * displayHeight` — ~48KB on X4/X4 Pro's
800×480, ~52KB on X3's 792×528), which `FreeInkDisplay::begin()`
heap-allocates at runtime, nor NimBLE's own heap usage — both come out of
the DRAM left after the static image, alongside whatever `ArduinoJson`'s
`JsonDocument` needs per incoming heartbeat. SD card support adds SdFat's
own static footprint plus `SdCharacterPack`'s fixed index arrays and its
16KB frame buffer (see "SD-backed character packs") — all static, none of
it heap. X4 Pro's slightly higher numbers are the touch/frontlight/RTC/
gauge libraries this board actually uses, plus PSRAM bring-up code from
`-DBOARD_HAS_PSRAM`.

## Known limitations

**On X4 (no IMU, no RTC, no frontlight — see the "Hardware" table):**

- No shake gesture — `DOWN` held substitutes for triggering `dizzy`
  (X3 gets a real shake in addition to this — see "Multi-board support").
- No wall-clock time across reboots — the clock face (see "Menu system")
  runs on a software clock that resets to unsynced on every boot until the
  bridge sends its next `time` heartbeat, and drifts with `millis()`
  between syncs rather than ticking off real hardware time (X3 and X4 Pro
  have a real RTC instead — see "Multi-board support").
- No brightness control — no frontlight to control (only X4 Pro has one).
- Battery status's charge current (`mA`) always reads `0`, and `usb`
  reads `BoardConfig::ACTIVE.usbDetect` (GPIO20) as active-high — this
  polarity is **an assumption**, not hardware-validated. X3 and X4 Pro's
  I2C fuel gauges report real percentage/voltage, though charging current
  isn't observable from either gauge either (no charger IC on their I2C
  bus — confirmed for X4 Pro's CW2017 in freeink-sdk's own hardware
  bring-up doc; assumed the same for X3's BQ27220, untested).

**On all three boards:**

- No buzzer, no LED — none of X4/X3/X4 Pro has either
  (`BoardConfig` declares `NO_AUDIO`/`NO_LEDS` on all three). There's no
  substitute; the earlier M5-era generation's sound feedback is dropped
  rather than faked, and its LED-blink-on-`attention` is replaced
  everywhere with the bit-inverted sprite flash (see "The seven states").
- The 18 hand-tuned ASCII-species characters from the earlier generation
  aren't ported — they were pixel-position-tuned text art for a 135×240
  color LCD at 5fps, and re-tuning all of them for monochrome e-paper's
  much slower partial-refresh cadence would be a from-scratch effort per
  species regardless of which of these three boards it targeted. Their
  answer to "multiple characters" is the compiled-in `bufo` plus SD
  `.charpack` cycling instead (see "SD-backed character packs" and "Menu
  system").
- BLE folder-push writes only the compiled-in flash path today, not SD —
  see "What this is not — yet" under "SD-backed character packs" above.
- The browser flasher (`site/index.html`) assumes Web Serial can reset
  each board into its ROM bootloader automatically. All three use their
  native USB port for serial (`ARDUINO_USB_MODE=1`), not a separate
  USB-UART bridge chip — whether Arduino-ESP32's app-side USB CDC driver
  answers the reset request the same way a bridge chip would is **not
  hardware-verified** for any of them. The page documents the manual
  fallback (hold BOOT while plugging in) so flashing still works either
  way.

**X4 Pro-specific, unverified:**

- Touch axis flip (`flipX`/`flipY`) is still unconfirmed on hardware per
  freeink-sdk's own bring-up doc — corner-tap accuracy for the automatic
  `CONFIRM` touch-tap synthesis is untested.
- `wasHomeKeyTapped()`'s async-polling safety hasn't been independently
  re-verified the way the generic per-button `wasPressed()`/`popPress()`
  split has (see "Controls" > "Input polling is async"); it's called from
  the same place and trust level as the confirmed-safe `isPressed()`
  calls, but the SDK's own async-safety documentation was written with
  the generic button API in mind, not the Home key's separate one.
- No X4 Pro unit has been used to test this firmware — everything above
  is built from freeink-sdk's own hardware bring-up documentation
  (`docs/xteink-x4pro-support.md`), the same rigor applied to the original
  X4 port's "Step 0" SDK verification, not assumed.

## Project layout

One `src/` tree builds all three boards (`env:xteink` for X4/X3,
`env:xteink_x4pro` for X4 Pro — see "Multi-board support"):

```
src/
  main.cpp                 — loop, state machine, rendering, input, setup
  ble_bridge.cpp/h          — Nordic UART service, line-buffered TX/RX
  data.h                    — wire protocol, JSON parse
  xfer.h                    — command handling (name/owner/status/unpair)
  stats.h                   — NVS-backed stats, pet name, owner name, settings
  assets/icons_bufo.h        — generated Icon assets (see tools/gif_to_icons.py)
  sd_character_pack.h/.cpp   — reads an SD-card .charpack at runtime
characters/                — example GIF character packs (bufo/bufo.charpack is the
                             SD-card form of the same character — see "SD-backed
                             character packs")
tools/                     — generators and converters
freeink-sdk/                — git submodule (Free-Ink/freeink-sdk)
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.
