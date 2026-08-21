#pragma once
#include <Icon.h>
#include <SDCardManager.h>

#include <cstddef>
#include <cstdint>

// Reads a .charpack binary (see tools/gif_to_icons.py --sd-out) from the SD
// card and serves one Icon frame at a time into a caller-owned buffer, so
// only the small fixed-size index lives in RAM — never the whole pack. See
// README.md "SD-backed character packs" for the on-disk format and the
// heap-discipline reasoning this follows (crosspoint-reader's
// .skills/heap-discipline/SKILL.md, which this port takes as precedent):
// bounded static arrays sized at compile time, one FsFile kept open for the
// pack's lifetime instead of reopened per frame, and no allocation at all
// on the render path — the caller supplies the frame buffer once.
class SdCharacterPack {
 public:
  // Fixed order: sleep, idle, busy, attention, celebrate, dizzy, heart —
  // matches PersonaState in main_xteink_x4.cpp and STATES in gif_to_icons.py.
  static constexpr uint8_t STATE_COUNT = 7;
  // Bounds the index arrays below (kept static, not heap — see
  // heap-discipline). bufo has 15 clips / 139 frames; this leaves headroom
  // for a hand-authored pack with more idle variety. A pack exceeding
  // either bound fails open() cleanly rather than overflowing.
  static constexpr uint16_t MAX_CLIPS = 48;
  static constexpr uint16_t MAX_FRAMES = 400;

  // Reads and validates the pack's index (a few hundred bytes to ~6.5KB for
  // the bounds above) into the static arrays below, then closes the file —
  // FsFile has no working copy/move assignment in this SdFat version, so
  // rather than fight that to keep one open for the pack's lifetime,
  // getFrame() below just reopens the (short, stack-local) path on each
  // call; matches how Free-Ink's own inkdeck (src/Document.cpp) uses
  // SDCardManager. At this render cadence (hundreds of ms between frames,
  // never per-tick) that reopen is well under the frame budget. Returns
  // false (and leaves the pack closed) on a missing file, bad magic/
  // version, wrong state count, or an index too large for MAX_CLIPS/
  // MAX_FRAMES.
  bool open(const char* path);
  bool isOpen() const { return isOpen_; }
  void close();

  // Largest single frame's packed byte length across the whole pack — size
  // the caller's frame buffer to at least this before calling getFrame().
  // 0 when not open.
  uint32_t maxFrameBytes() const { return maxFrameBytes_; }

  // Fills `out` with the icon for `state` (0..6) at `elapsedMs` into that
  // state, using the same clip-carousel-by-total-duration and per-frame-
  // duration timing as main_xteink_x4.cpp's currentFrame() for the
  // compiled-in path — deliberately bug-for-bug identical (including
  // sizing the carousel cycle off clip 0's duration even when other clips
  // differ) so behavior doesn't change depending on which source an
  // animation happens to come from. Reads the selected frame's packed bits
  // into `buf` (must be >= maxFrameBytes()). Returns false if not open,
  // the state has no clips, `buf` is too small, or the SD read fails.
  bool getFrame(uint8_t state, uint32_t elapsedMs, freeink::Icon& out, uint8_t* buf, size_t bufCap);

 private:
  struct StateEntry {
    uint16_t clipStart;
    uint8_t clipCount;
  };
  struct ClipEntry {
    uint16_t frameStart;
    uint8_t frameCount;
  };
  struct FrameEntry {
    uint16_t w;
    uint16_t h;
    int16_t center;
    uint16_t durationMs;
    uint32_t dataOffset;
    uint32_t dataLength;
  };

  char path_[80] = {0};
  bool isOpen_ = false;
  uint16_t totalClips_ = 0;
  uint16_t totalFrames_ = 0;
  uint32_t maxFrameBytes_ = 0;
  StateEntry states_[STATE_COUNT] = {};
  ClipEntry clips_[MAX_CLIPS] = {};
  FrameEntry frames_[MAX_FRAMES] = {};
};
