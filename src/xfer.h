#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "ble_bridge.h"
#include <mbedtls/base64.h>
#include <ArduinoJson.h>

// Runtime character-pack push (BLE folder drop -> LittleFS -> AnimatedGIF
// decode) is an M5StickCPlus-era feature: it assumes a color TFT, an
// on-device GIF decoder (character.cpp), and spare LittleFS space for a
// second copy of the pack. The Xteink X4 port ships its character art as
// compiled-in freeink::Icon structs generated offline (see
// tools/gif_to_icons.py) instead — no on-device decode, no flicker-prone
// runtime asset install on e-ink. Default on for the original M5 target;
// the xteink_x4 env sets this to 0 (see platformio.ini), which makes
// char_begin decline per REFERENCE.md ("If your device doesn't want
// pushed files, don't ack char_begin. The desktop times out ...").
#ifndef BUDDY_SUPPORTS_CHAR_PUSH
#define BUDDY_SUPPORTS_CHAR_PUSH 1
#endif

#if BUDDY_SUPPORTS_CHAR_PUSH
static File     _xFile;
static uint32_t _xExpected = 0, _xWritten = 0;
static char     _xCharName[24] = "";
static bool     _xActive = false;
static uint32_t _xTotal = 0, _xTotalWritten = 0;
#endif

// Ack goes to both streams — we don't track which one delivered the command,
// and writes to a clientless SerialBT just drop. The bridge listens on
// whichever port it opened.
static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n", what, ok?"true":"false", (unsigned long)n);
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}

#if BUDDY_SUPPORTS_CHAR_PUSH
static uint32_t _xWipeDir(const char* dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) { LittleFS.mkdir(dir); return 0; }
  uint32_t freed = 0;
  File f = d.openNextFile();
  while (f) {
    freed += f.size();
    char p[80];
    snprintf(p, sizeof(p), "%s/%s", dir, f.name());
    f.close();
    LittleFS.remove(p);
    f = d.openNextFile();
  }
  d.close();
  return freed;
}

// Only one character lives on the device at a time. Installing a new one
// under a different name would otherwise leave the old one's files eating
// space. Wipe everything under /characters/, return total bytes reclaimed.
static uint32_t _xWipeAllChars() {
  File root = LittleFS.open("/characters");
  if (!root || !root.isDirectory()) { LittleFS.mkdir("/characters"); return 0; }
  uint32_t freed = 0;
  File sub = root.openNextFile();
  while (sub) {
    if (sub.isDirectory()) {
      char p[64];
      snprintf(p, sizeof(p), "/characters/%s", sub.name());
      sub.close();
      freed += _xWipeDir(p);
      LittleFS.rmdir(p);
    } else {
      sub.close();
    }
    sub = root.openNextFile();
  }
  root.close();
  return freed;
}
#endif  // BUDDY_SUPPORTS_CHAR_PUSH

// Called from data.h when incoming JSON has a "cmd" key. Returns true if
// it was a transfer command (caller should skip state-update parsing).
// Needs characterClose()/characterInit() declared before this include.
void characterClose();
bool characterInit(const char* name);
void petNameSet(const char* name);
const char* petName();
void ownerSet(const char* name);
const char* ownerName();
#include "stats.h"

// Platform hook: battery/power telemetry for the "status" ack below.
// M5StickCPlus reads it off the AXP192 PMIC; a board with a plain ADC
// divider (e.g. Xteink X4 — no PMIC, no charge-status pin, see
// BoardConfig::XTEINK_X4) reads BatteryMonitor instead and always
// reports usb=false (X4 has no charge-status GPIO to sense it — see
// README "Divergences from the original buddy"). Declared here (not
// defined) so xfer.h stays board-agnostic; exactly one platform main.cpp
// must define it.
struct PlatformBatteryStatus {
  int pct;   // 0-100
  int mV;    // battery millivolts
  int mA;    // battery current, negative = charging (0 if unknown)
  bool usb;  // external power present
};
PlatformBatteryStatus platformBatteryStatus();

inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "species") == 0) {
    extern bool buddyMode, gifAvailable;
    extern void buddySetSpeciesIdx(uint8_t);
    uint8_t idx = doc["idx"] | 0xFF;
    speciesIdxSave(idx);
    buddyMode = !(gifAvailable && idx == 0xFF);
    if (buddyMode) buddySetSpeciesIdx(idx);
    _xAck("species", true);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // Dump everything the info screens show. Manual printf rather than
    // ArduinoJson serialize — less heap churn, and the shape is fixed.
    PlatformBatteryStatus bat = platformBatteryStatus();
    char b[320];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":%d,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%u,\"fsFree\":%lu,\"fsTotal\":%lu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      bat.pct, bat.mV, bat.mA, bat.usb ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap(),
#if BUDDY_SUPPORTS_CHAR_PUSH
      (unsigned long)(LittleFS.totalBytes() - LittleFS.usedBytes()),
      (unsigned long)LittleFS.totalBytes(),
#else
      0ul, 0ul,  // no LittleFS character store on this board — see BUDDY_SUPPORTS_CHAR_PUSH
#endif
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  if (strcmp(cmd, "char_begin") == 0) {
#if !BUDDY_SUPPORTS_CHAR_PUSH
    // Decline silently (no ack): the desktop times out and reports the
    // push failed, exactly as REFERENCE.md documents for devices that
    // don't want pushed files. This board's character art is baked in.
    return true;
#else
    const char* name = doc["name"] | "pet";
    _xTotal = doc["total"] | 0;

    // Fit check: free space after wiping everything under /characters/.
    // Do the math before touching the filesystem so a failed check leaves
    // the current character intact.
    uint32_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
    uint32_t reclaimable = 0;
    {
      File r = LittleFS.open("/characters");
      if (r && r.isDirectory()) {
        File s = r.openNextFile();
        while (s) {
          if (s.isDirectory()) {
            File f = s.openNextFile();
            while (f) { reclaimable += f.size(); f.close(); f = s.openNextFile(); }
          }
          s.close(); s = r.openNextFile();
        }
        r.close();
      }
    }
    // Headroom for LittleFS metadata overhead — it's not byte-for-byte.
    uint32_t available = free + reclaimable;
    if (_xTotal > 0 && _xTotal + 4096 > available) {
      char b[96];
      int len = snprintf(b, sizeof(b),
        "{\"ack\":\"char_begin\",\"ok\":false,\"n\":%lu,\"error\":\"need %luK, have %luK\"}\n",
        (unsigned long)available, (unsigned long)(_xTotal/1024), (unsigned long)(available/1024)
      );
      Serial.write(b, len);
      bleWrite((const uint8_t*)b, len);
      return true;
    }

    strncpy(_xCharName, name, sizeof(_xCharName)-1); _xCharName[sizeof(_xCharName)-1]=0;
    characterClose();
    _xWipeAllChars();
    char dir[48]; snprintf(dir, sizeof(dir), "/characters/%s", _xCharName);
    LittleFS.mkdir(dir);
    _xTotalWritten = 0;
    _xActive = true;
    _xAck("char_begin", true);
    return true;
#endif  // BUDDY_SUPPORTS_CHAR_PUSH
  }

#if BUDDY_SUPPORTS_CHAR_PUSH
  if (!_xActive) return strcmp(cmd, "permission") != 0;  // permission cmd is not ours

  if (strcmp(cmd, "file") == 0) {
    const char* path = doc["path"];
    _xExpected = doc["size"] | 0;
    _xWritten = 0;
    if (!path) { _xAck("file", false); return true; }
    char full[80]; snprintf(full, sizeof(full), "/characters/%s/%s", _xCharName, path);
    _xFile = LittleFS.open(full, "w");
    _xAck("file", (bool)_xFile);
    return true;
  }

  if (strcmp(cmd, "chunk") == 0) {
    const char* b64 = doc["d"];
    if (!b64 || !_xFile) { _xAck("chunk", false); return true; }
    uint8_t buf[300];
    size_t outLen = 0;
    int rc = mbedtls_base64_decode(buf, sizeof(buf), &outLen,
                                   (const uint8_t*)b64, strlen(b64));
    if (rc != 0) { _xAck("chunk", false); return true; }
    _xFile.write(buf, outLen);
    _xWritten += outLen;
    _xTotalWritten += outLen;
    // Ack every chunk — LittleFS writes can block on flash erase and the
    // UART RX buffer is only ~256 bytes. Without this the sender overruns it.
    _xAck("chunk", true, _xWritten);
    return true;
  }

  if (strcmp(cmd, "file_end") == 0) {
    bool ok = _xFile && (_xWritten == _xExpected || _xExpected == 0);
    if (_xFile) _xFile.close();
    _xAck("file_end", ok, _xWritten);
    return true;
  }

  if (strcmp(cmd, "char_end") == 0) {
    _xActive = false;
    bool ok = characterInit(_xCharName);
    extern bool buddyMode, gifAvailable;
    if (ok) { buddyMode = false; gifAvailable = true; speciesIdxSave(0xFF); }
    _xAck("char_end", ok);
    return true;
  }
#endif  // BUDDY_SUPPORTS_CHAR_PUSH

  return false;
}

#if BUDDY_SUPPORTS_CHAR_PUSH
inline bool xferActive() { return _xActive; }
inline uint32_t xferProgress() { return _xTotalWritten; }
inline uint32_t xferTotal() { return _xTotal; }
#else
inline bool xferActive() { return false; }
inline uint32_t xferProgress() { return 0; }
inline uint32_t xferTotal() { return 0; }
#endif
