// claude-desktop-buddy — Xteink X4 port.
//
// Presentation + input layer rewritten for FreeInk (e-paper, InputManager,
// BatteryMonitor); the BLE/protocol/state-machine core is unchanged from the
// M5StickCPlus build:
//   - ble_bridge.cpp/h   — Nordic UART Service, verbatim
//   - data.h             — wire protocol / JSON parsing, verbatim except two
//                          M5-specific side effects now go through the
//                          platformTimeSync()/platformBatteryStatus() hooks
//                          this file implements below (see data.h/xfer.h)
//   - stats.h            — NVS-backed stats/settings/owner/species, verbatim
//   - xfer.h             — command dispatch, verbatim except the runtime
//                          character-push path is compiled out on this board
//                          (BUDDY_SUPPORTS_CHAR_PUSH=0 — see xfer.h and the
//                          README "Divergences" section)
//
// See README.md "Xteink X4 port" for the sprite-region size, the full-refresh
// timer interval, and the final input mapping, and for every place this file
// diverges from the M5 build's behavior because of e-ink or X4 hardware.

#include <Arduino.h>
#include <new>
#include <esp_mac.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <InputManager.h>
#include <BatteryMonitor.h>
#include <FreeInkUIDisplayTarget.h>

#include "ble_bridge.h"
#include "data.h"
#include "assets/icons_bufo.h"

using freeink::ui::Color;
using freeink::ui::DisplayTarget;
using freeink::ui::Orientation;
using freeink::ui::Paint;
using freeink::ui::Rect;
using freeink::ui::TextAlign;
using freeink::ui::TextStyle;

// ---------------------------------------------------------------------------
// species/char-push stubs — xfer.h's "species" command references these
// (the M5 build's ASCII-species roster + runtime GIF swap). Neither concept
// ported: this board ships one compiled-in character (bufo) as Icon assets.
// A "species" command from an old desktop client just acks as a no-op.
// ---------------------------------------------------------------------------
bool buddyMode = false;
bool gifAvailable = false;
void buddySetSpeciesIdx(uint8_t) {}

// ---------------------------------------------------------------------------
// Platform hooks required by data.h / xfer.h (see their declarations).
// ---------------------------------------------------------------------------

// X4 has no RTC (BoardConfig::XTEINK_X4 -> FREEINK_CAP_RTC is off — see
// BoardConfig.h's FREEINK_CAP_RTC device list, which does not include X4),
// and this port has no clock-face screen to feed (that M5 feature depended
// on IMU-based orientation detection the X4 also doesn't have — see README
// "Divergences"). Nothing on this board currently consumes wall-clock time,
// so the hook is a deliberate no-op rather than storing a value nothing
// reads. data.h still sets dataRtcValid() true after calling this, per its
// documented contract — a future consumer (e.g. a status timestamp) can
// start from here without touching data.h again.
void platformTimeSync(const struct tm&) {}

BatteryMonitor batteryMonitor;
PlatformBatteryStatus platformBatteryStatus() {
  BatteryMonitor::Status s = batteryMonitor.readStatus();
  PlatformBatteryStatus out{0, 0, 0, false};
  if (s.percentageKnown) out.pct = s.percentage;
  if (s.millivoltsKnown) out.mV = s.millivolts;
  // X4 has no charge-status pin (BoardConfig::XTEINK_X4.batteryChargeStatus
  // == PIN_UNASSIGNED), so BatteryMonitor can't report charging/mA. usbDetect
  // (GPIO20) IS wired on the profile; we read it directly here since nothing
  // in the SDK's ADC battery backend consumes it. Polarity (active-high) is
  // an assumption, NOT hardware-validated — flagged in README "Divergences".
  // The X4 Pro reverse-engineering doc explicitly notes its own VBUS/USB
  // pin was "not conclusively identified" even after a hardware RE session,
  // so treat this the same way until someone confirms it on a scope.
  if (BoardConfig::ACTIVE.usbDetect != BoardConfig::PIN_UNASSIGNED) {
    out.usb = digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
  }
  out.mA = 0;  // unknown — no current sense on this board
  return out;
}

// ---------------------------------------------------------------------------
// State machine — identical derivation to the M5 build's derive() in
// main.cpp, just renamed/re-typed here.
// ---------------------------------------------------------------------------
enum PersonaState : uint8_t { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART, P_COUNT };
static const char* const kStateNames[P_COUNT] = {
    "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"};

static PersonaState derive(const TamaState& s) {
  if (!s.connected) return P_SLEEP;
  if (s.sessionsWaiting > 0) return P_ATTENTION;
  if (s.recentlyCompleted) return P_CELEBRATE;
  if (s.sessionsRunning >= 3) return P_BUSY;
  return P_IDLE;
}

TamaState     tama;
PersonaState  baseState = P_SLEEP;
PersonaState  activeState = P_SLEEP;
uint32_t      oneShotUntil = 0;
static void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// ---------------------------------------------------------------------------
// Display + sprite region.
//
// Panel: 800x480. bufo's Icon assets top out at bufo::MAX_ICON_W x
// MAX_ICON_H = 192x200 (96px source x 2 scale — see tools/gif_to_icons.py
// and README "Asset pipeline"). SPRITE region is sized with margin around
// that and kept byte-aligned (x and w multiples of 8) so the manual
// framebuffer inversion used for the `attention` cue below never touches a
// partial byte at the edges.
// ---------------------------------------------------------------------------
static constexpr int16_t SPRITE_X = 40;
static constexpr int16_t SPRITE_Y = 40;
static constexpr int16_t SPRITE_W = 240;
static constexpr int16_t SPRITE_H = 260;
static_assert(SPRITE_W >= bufo::MAX_ICON_W && SPRITE_H >= bufo::MAX_ICON_H,
              "sprite region must fit the largest generated icon");

// Text/status panel occupies the rest of the 800x480 panel to the right of
// (and below) the sprite region.
static constexpr int16_t PANEL_X = SPRITE_X + SPRITE_W + 24;
static constexpr int16_t PANEL_W = 800 - PANEL_X - 16;

EInkDisplay display(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
                    BoardConfig::ACTIVE.display.cs, BoardConfig::ACTIVE.display.dc,
                    BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
InputManager input;

// DisplayTarget wraps the display's framebuffer pointer, which display.begin()
// only allocates at runtime (it's null before that) — constructing this as an
// eagerly-initialized global would capture a null pointer, since global
// constructors run before setup(). Built lazily in setup() after begin()
// instead; ui() below is the only accessor.
alignas(DisplayTarget) static unsigned char uiStorage[sizeof(DisplayTarget)];
static DisplayTarget* uiPtr = nullptr;
static DisplayTarget& ui() { return *uiPtr; }

// --- Mandatory DC-balance timer ---------------------------------------------
// Independent of activity: a long busy/idle stretch runs only partial/fast
// refreshes over the sprite region, which never DC-balances the panel.
// Force a FULL_REFRESH of the whole screen on this fixed cadence regardless
// of what state the pet is in. Tune this one constant, nothing else.
static constexpr uint32_t FULL_REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes
static uint32_t lastFullRefreshMs = 0;

static void invertSpriteRegionBytes() {
  uint8_t* fb = display.getFrameBuffer();
  uint16_t wb = display.getDisplayWidthBytes();
  for (int16_t y = SPRITE_Y; y < SPRITE_Y + SPRITE_H; y++) {
    for (int16_t xb = SPRITE_X / 8; xb < (SPRITE_X + SPRITE_W) / 8; xb++) {
      fb[(uint32_t)y * wb + xb] ^= 0xFF;
    }
  }
}

static void clearSpriteRegion() {
  ui().fill(Rect{SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H}, Paint::solid(Color::White));
}

static void drawIconCentered(const freeink::Icon& icon) {
  int16_t x = SPRITE_X + (SPRITE_W - (int16_t)icon.w) / 2;
  int16_t y = SPRITE_Y + (SPRITE_H - (int16_t)icon.h) / 2;
  display.drawImageTransparent(icon.bits, x, y, icon.w, icon.h);
}

// Pick the current animation frame for `state`, advancing the idle carousel
// (rotating array of clips — same "activity carousel" idea as the original
// README) and per-clip frame index by wall-clock time using each frame's own
// duration from the GIF. Returns nullptr if the state has no clips (never
// happens for the 7 required states, guarded by the manifest check in
// tools/gif_to_icons.py).
static const freeink::Icon* currentFrame(PersonaState state, uint32_t now, uint32_t stateEnteredMs) {
  const bufo::IconState& is = bufo::bufo_states[state];
  if (is.clipCount == 0) return nullptr;
  // Idle carousel: advance to the next clip each time the current one loops.
  // Non-idle states have exactly one clip, so this is a no-op there.
  uint32_t elapsed = now - stateEnteredMs;
  uint32_t clipMs = 0;
  for (uint8_t f = 0; f < is.clips[0].frameCount; f++) clipMs += is.clips[0].frames[f].durationMs;
  if (clipMs == 0) clipMs = 1;
  uint8_t clipIdx = (uint8_t)((elapsed / clipMs) % is.clipCount);
  uint32_t intoClip = elapsed % clipMs;
  const bufo::IconClip& clip = is.clips[clipIdx];
  uint32_t acc = 0;
  for (uint8_t f = 0; f < clip.frameCount; f++) {
    acc += clip.frames[f].durationMs;
    if (intoClip < acc) return clip.frames[f].icon;
  }
  return clip.frames[clip.frameCount - 1].icon;
}

// --- Refresh strategy --------------------------------------------------------
// See README "Refresh strategy" for the rationale behind each state's
// cadence/mode. Nothing here is uniform on purpose.
static uint32_t stateEnteredMs = 0;
static uint32_t lastSpriteDrawMs = 0;
static bool     sleepFrameDrawn = false;
static bool     attentionFlashOn = false;

static void renderSprite(uint32_t now) {
  switch (activeState) {
    case P_SLEEP:
      // Static frame, no refresh loop: draw once on entry, then do nothing
      // until the state changes (checked by the caller via sleepFrameDrawn).
      if (sleepFrameDrawn) return;
      clearSpriteRegion();
      drawIconCentered(*currentFrame(P_SLEEP, now, stateEnteredMs));
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      sleepFrameDrawn = true;
      return;

    case P_IDLE:
      // Slow-cadence partial refresh — e-ink reads a slow blink as
      // charming, not laggy (per the original README's framing).
      if (now - lastSpriteDrawMs < 1500) return;
      clearSpriteRegion();
      drawIconCentered(*currentFrame(P_IDLE, now, stateEnteredMs));
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    case P_BUSY:
      // Fast partial refresh, fixed region only — this is the "working"
      // animation and wants to read as active.
      if (now - lastSpriteDrawMs < 350) return;
      clearSpriteRegion();
      drawIconCentered(*currentFrame(P_BUSY, now, stateEnteredMs));
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    case P_ATTENTION: {
      // No LED on this board. Substitute cue: alternate the sprite region
      // between the normal icon and a bit-inverted version of it, on the
      // same ~400ms cadence the M5 build blinked its LED at. This reads as
      // "urgent" the way the idle blink can't, without needing a second
      // color plane.
      if (now - lastSpriteDrawMs < 400) return;
      clearSpriteRegion();
      drawIconCentered(*currentFrame(P_ATTENTION, now, stateEnteredMs));
      attentionFlashOn = !attentionFlashOn;
      if (attentionFlashOn) invertSpriteRegionBytes();
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;
    }

    case P_CELEBRATE: {
      // Infrequent (level-up, every 50K tokens) — can afford full refreshes.
      // Cycle the clip's frames, one FULL_REFRESH each, capped so a long
      // clip can't turn a celebration into a multi-second stall.
      if (now - lastSpriteDrawMs < 220) return;
      clearSpriteRegion();
      const freeink::Icon* f = currentFrame(P_CELEBRATE, now, stateEnteredMs);
      if (!f) break;
      drawIconCentered(*f);
      display.displayBuffer(EInkDisplay::FULL_REFRESH);
      lastFullRefreshMs = now;  // this refresh already DC-balanced the panel
      break;
    }

    case P_DIZZY:
      // Short-lived, fast partial — triggered by a button hold (no IMU on
      // this board; see the input-mapping comment in loop()).
      if (now - lastSpriteDrawMs < 200) return;
      clearSpriteRegion();
      drawIconCentered(*currentFrame(P_DIZZY, now, stateEnteredMs));
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    case P_HEART:
      // Similar budget to celebrate but smaller/cheaper: partial refresh,
      // not full — it fires far more often (every fast approval).
      if (now - lastSpriteDrawMs < 250) return;
      clearSpriteRegion();
      drawIconCentered(*currentFrame(P_HEART, now, stateEnteredMs));
      display.displayWindow(SPRITE_X, SPRITE_Y, SPRITE_W, SPRITE_H);
      break;

    default:
      return;
  }
  lastSpriteDrawMs = now;
}

// --- Status/text panel -------------------------------------------------------
static bool responseSent = false;
static char lastPromptId[40] = "";
static uint32_t promptArrivedMs = 0;

static void drawStatusPanel(uint32_t now) {
  ui().fill(Rect{PANEL_X, 0, PANEL_W, 480}, Paint::solid(Color::White));

  int16_t y = 12;
  char line[96];
  snprintf(line, sizeof(line), "%s%s%s", ownerName()[0] ? ownerName() : "", ownerName()[0] ? "'s " : "", petName());
  ui().text(Rect{PANEL_X, y, PANEL_W, 28}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
  y += 32;

  snprintf(line, sizeof(line), "%s", kStateNames[activeState]);
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 28;

  bool inPrompt = tama.promptId[0] && !responseSent;
  if (inPrompt) {
    uint32_t waited = (now - promptArrivedMs) / 1000;
    snprintf(line, sizeof(line), "approve? %lus", (unsigned long)waited);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 24;
    ui().text(Rect{PANEL_X, y, PANEL_W, 24}, tama.promptTool, TextStyle{0, TextAlign::Left, Color::Black, 1, true, false});
    y += 28;
    ui().text(Rect{PANEL_X, y, PANEL_W, 40}, tama.promptHint, TextStyle{0, TextAlign::Left, Color::DarkGray, 2, false, false});
    y += 44;
    ui().text(Rect{PANEL_X, y, PANEL_W / 2, 20}, "CONFIRM: approve",
            TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 22;
    ui().text(Rect{PANEL_X, y, PANEL_W / 2, 20}, "BACK: deny", TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 28;
  } else {
    snprintf(line, sizeof(line), "sessions %u  running %u  waiting %u", tama.sessionsTotal, tama.sessionsRunning,
             tama.sessionsWaiting);
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::Black, 1, false, false});
    y += 24;
    ui().text(Rect{PANEL_X, y, PANEL_W, 20}, tama.msg, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
    y += 28;
  }

  y = 340;
  PlatformBatteryStatus bat = platformBatteryStatus();
  snprintf(line, sizeof(line), "battery %d%%  %s", bat.pct, bat.usb ? "usb" : "");
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 22;
  snprintf(line, sizeof(line), "ble %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
  y += 22;
  snprintf(line, sizeof(line), "Lv %u  tokens %lu", stats().level, (unsigned long)stats().tokens);
  ui().text(Rect{PANEL_X, y, PANEL_W, 20}, line, TextStyle{0, TextAlign::Left, Color::DarkGray, 1, false, false});
}

// --- Input mapping ------------------------------------------------------------
// The X4's InputStyle::XteinkAdcLadder is NOT a 3-button layout like the
// M5Stick — it's 7 distinct semantic buttons (BACK/CONFIRM/LEFT/RIGHT/UP/
// DOWN/POWER) off two resistor-ladder ADC pins, decoded by InputManager (see
// README "Input mapping" for the full verification trail against the SDK
// headers). That's MORE inputs than the M5 build's 3 physical buttons, so
// nothing needs to be combined — every required action gets its own button:
//   CONFIRM        approve (in a prompt) / wake+advance otherwise
//   BACK           deny (in a prompt) / dismiss otherwise
//   LEFT / RIGHT   scroll transcript
//   UP / DOWN      reserved (menu nav if a settings screen is added later)
//   DOWN long-press  manual "dizzy" trigger — substitute for the M5 build's
//                    shake gesture, which needed an IMU the X4 does not have
//                    (BoardConfig::XTEINK_X4 has ImuType::None; confirmed via
//                    InputManager/BoardConfig, not assumed)
//   POWER          reserved, not yet wired to any action
// Face-down nap (also IMU-driven) is dropped outright rather than remapped —
// there's no button gesture that means the same thing. See README.
static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static uint32_t downPressStart = 0;
static bool     downLongFired = false;

static void handleInput(uint32_t now) {
  input.update();
  bool inPrompt = tama.promptId[0] && !responseSent;

  if (input.wasPressed(InputManager::BTN_CONFIRM)) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (now - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    }
  }

  if (input.wasPressed(InputManager::BTN_BACK)) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
    }
  }

  // DOWN long-press: dizzy substitute for shake (see comment above).
  if (input.isPressed(InputManager::BTN_DOWN)) {
    if (downPressStart == 0) downPressStart = now;
    if (!downLongFired && now - downPressStart > 800 && (int32_t)(now - oneShotUntil) >= 0) {
      downLongFired = true;
      triggerOneShot(P_DIZZY, 2000);
    }
  } else {
    downPressStart = 0;
    downLongFired = false;
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  statsLoad();
  settingsLoad();
  petNameLoad();

  display.begin();
  uiPtr = new (uiStorage) DisplayTarget(display.getFrameBuffer(), display.getDisplayWidth(),
                                        display.getDisplayHeight(), display.getDisplayWidthBytes(),
                                        Orientation::LandscapeCounterClockwise);
  display.clearScreen(0xFF);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);
  lastFullRefreshMs = millis();

  input.begin();

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  static char btName[16];
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);

  stateEnteredMs = millis();
}

void loop() {
  uint32_t now = millis();

  dataPoll(&tama);
  handleInput(now);

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId) - 1);
    lastPromptId[sizeof(lastPromptId) - 1] = 0;
    responseSent = false;
    if (tama.promptId[0]) promptArrivedMs = now;
  }

  baseState = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  static PersonaState lastRenderedState = P_COUNT;  // force first-tick transition
  if (activeState != lastRenderedState) {
    stateEnteredMs = now;
    sleepFrameDrawn = false;
    lastRenderedState = activeState;
  }

  // Panel drawn first: renderSprite() can trigger a FULL_REFRESH (celebrate),
  // which pushes the whole framebuffer — the panel text must already be
  // current in it when that happens, not from a stale previous tick.
  drawStatusPanel(now);
  renderSprite(now);

  // Mandatory DC-balance full refresh, independent of activity/state.
  if (now - lastFullRefreshMs > FULL_REFRESH_INTERVAL_MS) {
    display.displayBuffer(EInkDisplay::FULL_REFRESH);
    lastFullRefreshMs = now;
  } else if (activeState != P_CELEBRATE) {
    // The status panel is drawn every tick above but only pushed to the
    // panel when something in it can plausibly have changed, to avoid
    // hammering the (still-visible) partial-refresh region with redundant
    // waveforms. displayWindow() over the panel's own rect keeps this
    // independent of the sprite region's refresh cadence.
    static uint32_t lastPanelDrawMs = 0;
    if (now - lastPanelDrawMs > 1000) {
      display.displayWindow(PANEL_X, 0, 800 - PANEL_X, 480);
      lastPanelDrawMs = now;
    }
  }

  delay(16);
}
