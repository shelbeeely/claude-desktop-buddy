# claude-desktop-buddy

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet on ESP32 that lives off permission
approvals and interaction with Claude. It sleeps when nothing's happening,
wakes when sessions start, gets visibly impatient when an approval prompt is
waiting, and lets you approve or deny right from the device.

<p align="center">
  <img src="docs/device.jpg" alt="M5StickC Plus running the buddy firmware" width="500">
</p>

## Hardware

The firmware targets ESP32 with the Arduino framework. As written, it
depends on the M5StickCPlus library for its display, IMU, and button
drivers—so you'll need that board, or a fork that swaps those drivers for
your own pin layout.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -t upload
```

If you're starting from a previously-flashed device, wipe it first:

```bash
pio run -t erase && pio run -t upload
```

Once running, you can also wipe everything from the device itself: **hold A
→ settings → reset → factory reset → tap twice**.

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

If discovery isn't finding the stick:

- Make sure it's awake (any button press)
- Check the stick's settings menu → bluetooth is on

## Controls

|                         | Normal               | Pet         | Info        | Approval    |
| ----------------------- | -------------------- | ----------- | ----------- | ----------- |
| **A** (front)           | next screen          | next screen | next screen | **approve** |
| **B** (right)           | scroll transcript    | next page   | next page   | **deny**    |
| **Hold A**              | menu                 | menu        | menu        | menu        |
| **Power** (left, short) | toggle screen off    |             |             |             |
| **Power** (left, ~6s)   | hard power off       |             |             |             |
| **Shake**               | dizzy                |             |             | —           |
| **Face-down**           | nap (energy refills) |             |             |             |

The screen auto-powers-off after 30s of no interaction (kept on while an
approval prompt is up). Any button press wakes it.

## ASCII pets

Eighteen pets, each with seven animations (sleep, idle, busy, attention,
celebrate, dizzy, heart). Menu → "next pet" cycles them with a counter.
Choice persists to NVS.

## GIF pets

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the stick switches to GIF mode live. **Settings
→ delete char** reverts to ASCII mode.

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

GIFs are 96px wide; height up to ~140px stays on a 135×240 portrait screen.
Crop tight to the character — transparent margins waste screen and shrink
the sprite. `tools/prep_character.py` handles the resize: feed it source
GIFs at any sizes and it produces a 96px-wide set where the character is the
same scale in every state.

The whole folder must fit under 1.8MB —
`gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.

See `characters/bufo/` for a working example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/flash_character.py characters/bufo` stages it into `data/` and runs
`pio run -t uploadfs` directly over USB.

## The seven states

| State       | Trigger                     | Feel                        |
| ----------- | --------------------------- | --------------------------- |
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, **LED blinks**       |
| `celebrate` | level up (every 50K tokens) | confetti, bouncing          |
| `dizzy`     | you shook the stick         | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Project layout

```
src/
  main.cpp             — M5StickCPlus: loop, state machine, UI screens
  main_xteink_x4.cpp   — Xteink X4: loop, state machine, refresh strategy
  buddy.cpp            — ASCII species dispatch + render helpers (M5 only)
  buddies/             — one file per species, seven anim functions each (M5 only)
  ble_bridge.cpp        — Nordic UART service, line-buffered TX/RX (shared)
  character.cpp         — GIF decode + render (M5 only)
  data.h                — wire protocol, JSON parse (shared)
  xfer.h                — folder push receiver (shared)
  stats.h               — NVS-backed stats, settings, owner, species choice (shared)
  assets/icons_bufo.h    — generated Icon assets for the X4 (see tools/gif_to_icons.py)
characters/            — example GIF character packs
tools/                 — generators and converters
freeink-sdk/           — git submodule (Free-Ink/freeink-sdk), X4 target only
```

## Xteink X4 port

A second target, `[env:xteink_x4]` in `platformio.ini`, builds this buddy on
the **Xteink X4** (ESP32-C3, SSD1677 800×480 e-paper, no PSRAM) using the
[FreeInk SDK](https://github.com/Free-Ink/freeink-sdk), vendored as the
`freeink-sdk` git submodule (`git submodule update --init` before building).
Build it with:

```bash
pio run -e xteink_x4
```

The BLE/protocol/state-machine core (`ble_bridge.cpp/h`, `data.h`, `stats.h`,
`xfer.h`) is shared with the M5StickCPlus target; only the presentation and
input layer is new (`src/main_xteink_x4.cpp`). What follows documents the
decisions behind that port and every place it had to diverge from the
original buddy's behavior.

### Verifying against the SDK before porting (Step 0)

Claims below are cited to the actual SDK headers, not assumed from prior
device experience:

- **X4 input is not a 3-button layout.** `BoardConfig::XTEINK_X4` declares
  `InputStyle::XteinkAdcLadder`: two ADC pins, each a resistor ladder
  multiplexing several buttons (`InputManager.cpp`'s `ADC_RANGES_1`/`_2`),
  decoding to **seven** distinct semantic buttons — `BACK`, `CONFIRM`,
  `LEFT`, `RIGHT`, `UP`, `DOWN`, `POWER` — more than the M5Stick's three
  physical buttons, not fewer. `InputManager::getState()` (and
  `readButtonAdc()`, exposed for calibration/diagnostics) handles the ladder
  decode; the port routes through this semantic API, never raw
  `analogRead()` thresholds. This contradicted the initial assumption a
  "resistor ladder" implies a reduced action set — it doesn't here.
- **X4 has no IMU.** `BoardConfig::XTEINK_X4`'s `sensors` field is
  `NO_SENSORS` (`ImuType::None`), confirmed by `BoardConfig::hasImu()`
  (`sensors.imuAddr == 0`). The X3 sibling profile *does* carry a QMI8658,
  which is presumably where the "X3/X4 share a pinout" assumption could
  mislead — they do not share sensors. Shake-to-dizzy and face-down-nap
  (both IMU-driven in the M5 build) have no hardware equivalent.
- **X4 has no RTC.** `FREEINK_CAP_RTC`'s device list (`BoardConfig.h`) is
  `X3 || STICKY || X4PRO || PAPERMONO || PAPERS3 || LILYGO` — X4 is absent.
  The M5 build's charging "clock face" screen (RTC + IMU-orientation
  driven) has no path here at all; not ported, not substituted.
- **X4 has a real `usbDetect` pin (GPIO20)**, but nothing in
  `BatteryMonitor`'s ADC backend reads it — it's read directly in this
  port's `platformBatteryStatus()`, active-high, **unverified on hardware**
  (see "Divergences" below).
- **Battery is ADC-only**, `batteryChargeStatus` is `PIN_UNASSIGNED`: no
  charge-current sensing, so the status ack's `mA` field is always `0` on
  this board (X4Pro's own RE notes call its own VBUS pin "not conclusively
  identified" even after a hardware session — same caveat applies here).
- **`FreeInkDisplay::displayWindow(x,y,w,h)`** (no mode argument) is the
  windowed-partial-refresh primitive the SDK actually exposes; `RefreshMode`
  is `{FULL_REFRESH, HALF_REFRESH, FAST_REFRESH}` — there is no separate
  "PARTIAL" mode name in the SDK, unlike the wording in early drafts of this
  port's plan.

### The BLE/protocol core was not actually display-agnostic

Two of the four files the plan assumed were drop-in turned out to have
M5-specific side effects baked in, found only by trying to build against a
different board:

- **`data.h`** called `M5.Rtc.SetTime/SetDate` directly on every bridge time
  sync, and reached into `main.cpp`'s file-static `_clkLastRead` via
  `extern`. Replaced with a `platformTimeSync(const struct tm&)` hook
  declared in `data.h`, implemented once per target (`main.cpp` keeps the
  exact old behavior; `main_xteink_x4.cpp`'s implementation is a no-op — see
  above, X4 has no RTC and no clock-face consumer for it).
- **`xfer.h`** `#include`d `<M5StickCPlus.h>` directly and called
  `M5.Axp.GetBatVoltage()/GetBatCurrent()/GetVBusVoltage()` inline in the
  `"status"` command handler. Replaced with a `platformBatteryStatus()`
  hook; `main_xteink_x4.cpp`'s implementation reads `BatteryMonitor` (see
  above for what it can't report).
- **`xfer.h`**'s runtime character-pack push (BLE folder drop → LittleFS →
  on-device `AnimatedGIF` decode → color-TFT render) is itself an
  M5StickCPlus-shaped feature — it assumes spare LittleFS space for a
  second copy of the pack and a GIF decoder. Gated off entirely on this
  board (`BUDDY_SUPPORTS_CHAR_PUSH=0`, set in `platformio.ini`'s
  `xteink_x4` env) rather than ported: e-ink has no business decoding GIFs
  on-device frame-by-frame, and the offline `tools/gif_to_icons.py`
  pipeline (below) already produces the equivalent compiled-in asset.
  `char_begin` simply doesn't ack, matching the behavior REFERENCE.md
  already documents for a device that declines pushed files.
- **`ble_bridge.cpp`**, believed to be purely protocol-level, turned out to
  depend on which Bluetooth host stack the active core's `sdkconfig`
  selects — not on which board this is. arduino-esp32's `BLE` library backs
  onto Bluedroid *or* NimBLE, with different parameter types on the same
  callback names (`onWrite`, `onMtuChanged`, `onAuthenticationComplete`),
  and no bond-clearing API in common. The X4 build (ESP32-C3 via the
  pioarduino/ESP-IDF 5.x core FreeInk requires) resolves to NimBLE. Rather
  than rewrite the file for one target and risk silently breaking the
  other, `ble_bridge.cpp` now branches on `CONFIG_BLUEDROID_ENABLED` /
  `CONFIG_NIMBLE_ENABLED`: the Bluedroid branch is byte-for-byte what this
  file already did before this port; the NimBLE branch is new (see
  "Divergences" for what could and couldn't be independently re-verified
  by compiling the M5 target in this environment).
- **`stats.h`**'s `min(secondsToRespond, 65535u)` failed to compile on the
  X4 toolchain: `uint32_t` and the `unsigned` literal resolve to distinct
  types there, which makes template argument deduction for `std::min`
  ambiguous (the M5 build's older toolchain happened to unify them). Fixed
  with an explicit ternary — a real portability bug the new toolchain
  surfaced, not an X4-specific change; both targets build from the fix.
- **`main.cpp`** itself is not "a state machine plus render helpers" the
  way the project layout table implies — button handling, the settings/menu
  system, the ASCII-species/GIF dispatch, IMU orientation tracking, and the
  AXP-clock-face screen are all interleaved in one file and one `loop()`.
  The X4 port doesn't reuse any of it; `main_xteink_x4.cpp` re-derives only
  the state machine (`derive()`) from scratch, matching the original
  4-line priority logic exactly.

### Asset pipeline

`tools/gif_to_icons.py` is the new offline (host-side, not on-device) step:
it takes a character pack already normalized by `tools/prep_character.py`
(96px-wide, cropped to one consistent scale across every state) and emits
`freeink::Icon` C structs (`libs/assets/Icons/include/Icon.h`: 1-bpp,
MSB-first rows, `opticalCenterY` from the real ink pixels) directly into a
header, one clip/frame array per state, mirroring the manifest's own
"single filename or list of filenames" shape (a multi-clip state is the
idle-carousel rotation, matching the original README's description) instead
of inventing a different one. Classification per pixel: within tolerance of
the manifest's flattened background color → transparent; else dark →
ink; else (a light non-background pixel, e.g. a highlight) → also
transparent, since the sprite region is cleared to white before every
blit and a transparent pixel just lets that white show through.

```bash
python3 tools/gif_to_icons.py characters/bufo \
  --scale 2 --out src/assets/icons_bufo.h --namespace bufo
```

`src/assets/icons_bufo.h` (checked in, ~650KB of packed icon data across
139 frames) is the generated output for the bundled `bufo` character —
proof the pipeline runs end-to-end, not just a described-but-unexercised
script. `--scale 2` (192px-wide icons) was chosen over the source's native
96px because the X4's pixel pitch is much finer than the M5Stick's 1.14"
LCD (both are roughly similar PPI, but the X4's physical panel is ~4×
larger by area); at native scale the character would read as small and
lost on the 800×480 canvas relative to how it looked on the Stick.

### Sprite region and refresh strategy

The generated bufo assets top out at 192×200px (`bufo::MAX_ICON_W/H`). The
fixed animation region is `SPRITE_X=40, SPRITE_Y=40, SPRITE_W=240,
SPRITE_H=260` (`main_xteink_x4.cpp`) — byte-aligned on both axes (240/8=30)
so the `attention`-state bit-inversion trick below never touches a partial
byte at the region's edge — with the rest of the 800×480 panel to the right
as a text/status panel (session counts, transcript, approval prompt,
battery, BLE link state), drawn with `FreeInkUI`'s dependency-free
`DisplayTarget` (bundled Noto Sans bitmap font, no external font library).

| State | What runs | Why |
|---|---|---|
| `sleep` | Draw once on entry, then nothing until the state changes | static frame, no refresh loop |
| `idle` | `displayWindow()` on the sprite region, ~1.5s cadence | slow partial refresh reads as a charming blink, not lag |
| `busy` | `displayWindow()`, ~350ms cadence | fast partial, but still windowed — never touches the panel outside the sprite region |
| `attention` | `displayWindow()` at ~400ms, alternating the icon with a bit-inverted copy of the same region (manual XOR over the framebuffer bytes) | no LED on this board; the M5 build's LED blink needed a substitute that reads as urgent, distinct from idle's slower blink |
| `celebrate` | Cycles the clip's frames, one `FULL_REFRESH` (whole panel) each | infrequent (every 50K tokens) — can afford the cost; also resets the DC-balance timer below |
| `dizzy` | `displayWindow()`, ~200ms cadence, one-shot ~2s | short-lived, triggered by a button hold (see Input mapping — there is no shake gesture on this board) |
| `heart` | `displayWindow()`, ~250ms cadence, one-shot ~2s | similar budget to celebrate but partial, not full — it fires on every fast approval, far more often |

**Mandatory full-refresh timer**, independent of the table above:
`FULL_REFRESH_INTERVAL_MS` (`main_xteink_x4.cpp`, currently 5 minutes) forces
a whole-panel `FULL_REFRESH` on a fixed cadence regardless of what state the
pet is in, so a long `busy`/`idle` stretch running only partial refreshes
over the sprite region never lets the panel's DC balance degrade. It's one
named constant, checked once per `loop()` iteration, deliberately not tied
to any other timer in the file.

### Input mapping

Per the Step 0 finding above, the X4 has seven semantic buttons — enough
for a 1:1 mapping with no combining required:

| Button | Normal | In an approval prompt |
|---|---|---|
| `CONFIRM` | wake / advance | **approve** |
| `BACK` | dismiss | **deny** |
| `LEFT` / `RIGHT` | scroll transcript | — |
| `UP` / `DOWN` | reserved (menu nav, unused by this port) | — |
| `DOWN` (long-press, ~800ms) | trigger `dizzy` for 2s | — |
| `POWER` | reserved | — |

`DOWN` long-press is the substitute for the M5 build's shake-to-dizzy
gesture — the closest analogue available without an IMU. **Face-down nap
is dropped outright, not remapped**: there is no button gesture that means
the same thing, and inventing one (e.g. a timed hold) would be a
significantly different feature, not a port of this one.

### Resource budget

```
RAM:   [=         ]   8.0% (used 26244 bytes from 327680 bytes)
Flash: [==        ]  21.3% (used 1399141 bytes from 6553600 bytes)
```

From `pio run -e xteink_x4`'s size report (ESP32-C3, `default_16MB.csv`
partition scheme — the generic `esp32-c3-devkitm-1` board definition's
stock partition table targets a 4MB part and is too small for this image;
X4 is a real 16MB part per `platformio.sample.ini`, so this port's env sets
`board_build.partitions = default_16MB.csv` explicitly). The RAM figure is
static `.data`/`.bss` only, from the linker's own accounting. It does
**not** include the ~48KB single-buffer e-paper framebuffer (`displayWidth
/ 8 * displayHeight` = 100 × 480), which `FreeInkDisplay::begin()`
heap-allocates at runtime, nor NimBLE's own heap usage — both come out of
the same ~295KB of DRAM left after the static image, alongside whatever
`ArduinoJson`'s `JsonDocument` needs per incoming heartbeat.

### Divergences from the original buddy (explicit callouts)

- Shake→`dizzy` replaced with a `DOWN` long-press (no IMU on X4).
- Face-down nap dropped, not remapped (no IMU; no equivalent gesture).
- The charging "clock face" screen dropped (no RTC, and it depended on the
  same IMU-based orientation detection as the nap gesture).
- Runtime GIF character push over BLE declined; this board ships one
  compiled-in character (`bufo`) as offline-generated `freeink::Icon`
  assets instead. The ASCII-species roster (`buddy.cpp`/`buddies/*.cpp`,
  18 species) is not ported either — `main_xteink_x4.cpp` stubs the
  `species` command's globals as inert so an old desktop client's `species`
  command still acks cleanly instead of failing to link.
- Battery status's `mA` (charge current) is always `0` — X4 has no
  charge-status pin to read it from, unlike the M5Stick's AXP192 PMIC.
- Battery status's `usb` flag reads `BoardConfig::ACTIVE.usbDetect`
  (GPIO20) as active-high — this polarity is **an assumption**, not
  hardware-validated (flagged in code and here; the X4 Pro's own
  reverse-engineering notes hit the same wall on its VBUS pin).
- `ble_bridge.cpp`'s NimBLE branch (used by this X4 build) could be
  compiled and linked end-to-end in this environment. Its Bluedroid branch
  (used by the M5StickCPlus build, preserved byte-for-byte from the
  pre-port file) could **not** be independently re-verified by building
  `env:m5stickc-plus` here — that build hit an unrelated board/variant
  packaging mismatch in the pulled platform release (`m5stick-c`'s declared
  variant isn't present in this framework version's `variants/` directory),
  a pre-existing environment gap unconnected to this port's changes, present
  before any of the touched files are even compiled.
- No text UI parity with the M5 build's settings menu, info pages, or
  transcript scrollback paging beyond a plain `LEFT`/`RIGHT` scroll — the
  status panel covers session counts, the live approval prompt, and basic
  device status, not a full menu system port.

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.
