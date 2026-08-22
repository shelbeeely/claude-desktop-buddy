#include "sd_character_pack.h"

#include <cstring>

// On-disk record shapes — see tools/gif_to_icons.py's _HEADER_FMT/_STATE_FMT/
// _CLIP_FMT/_FRAME_FMT comment for the authoritative format. #pragma pack
// matches the Python struct module's packed little-endian layout exactly
// (no compiler-inserted padding); ESP32-C3 is little-endian so no byte
// swapping is needed either.
namespace {
constexpr char kMagic[4] = {'C', 'D', 'B', 'K'};
constexpr uint8_t kVersion = 1;

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint8_t version;
  uint8_t stateCount;
  uint16_t reserved;
  uint16_t totalClips;
  uint16_t totalFrames;
};
struct StateRec {
  uint16_t clipStart;
  uint8_t clipCount;
  uint8_t reserved;
};
struct ClipRec {
  uint16_t frameStart;
  uint8_t frameCount;
  uint8_t reserved;
};
struct FrameRec {
  uint16_t w;
  uint16_t h;
  int16_t center;
  uint16_t durationMs;
  uint32_t dataOffset;
  uint32_t dataLength;
};
#pragma pack(pop)
}  // namespace

bool SdCharacterPack::open(const char* path) {
  close();

  if (strlen(path) >= sizeof(path_)) return false;

  FsFile f = SdMan.open(path, O_RDONLY);
  if (!f) return false;

  Header hdr;
  if (f.read(&hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
    f.close();
    return false;
  }
  if (memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0 || hdr.version != kVersion ||
      hdr.stateCount != STATE_COUNT) {
    f.close();
    return false;
  }
  if (hdr.totalClips > MAX_CLIPS || hdr.totalFrames > MAX_FRAMES) {
    f.close();  // pack too large for this device's bounded index arrays
    return false;
  }

  for (uint8_t i = 0; i < STATE_COUNT; i++) {
    StateRec rec;
    if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec)) {
      f.close();
      return false;
    }
    states_[i] = {rec.clipStart, rec.clipCount};
  }
  for (uint16_t i = 0; i < hdr.totalClips; i++) {
    ClipRec rec;
    if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec)) {
      f.close();
      return false;
    }
    clips_[i] = {rec.frameStart, rec.frameCount};
  }
  maxFrameBytes_ = 0;
  for (uint16_t i = 0; i < hdr.totalFrames; i++) {
    FrameRec rec;
    if (f.read(&rec, sizeof(rec)) != (int)sizeof(rec)) {
      f.close();
      return false;
    }
    frames_[i] = {rec.w, rec.h, rec.center, rec.durationMs, rec.dataOffset, rec.dataLength};
    if (rec.dataLength > maxFrameBytes_) maxFrameBytes_ = rec.dataLength;
  }

  totalClips_ = hdr.totalClips;
  totalFrames_ = hdr.totalFrames;
  f.close();
  strncpy(path_, path, sizeof(path_) - 1);
  isOpen_ = true;
  return true;
}

void SdCharacterPack::close() {
  isOpen_ = false;
  path_[0] = 0;
  totalClips_ = 0;
  totalFrames_ = 0;
  maxFrameBytes_ = 0;
}

bool SdCharacterPack::getFrame(uint8_t state, uint32_t elapsedMs, freeink::Icon& out, uint8_t* buf,
                               size_t bufCap) {
  if (!isOpen_ || state >= STATE_COUNT) return false;
  const StateEntry& se = states_[state];
  if (se.clipCount == 0) return false;

  const ClipEntry& clip0 = clips_[se.clipStart];
  uint32_t clip0Ms = 0;
  for (uint8_t f = 0; f < clip0.frameCount; f++) clip0Ms += frames_[clip0.frameStart + f].durationMs;
  if (clip0Ms == 0) clip0Ms = 1;
  uint16_t clipIdx = (uint16_t)(se.clipStart + (elapsedMs / clip0Ms) % se.clipCount);
  uint32_t intoClip = elapsedMs % clip0Ms;

  const ClipEntry& clip = clips_[clipIdx];
  uint32_t acc = 0;
  uint16_t frameIdx = clip.frameStart;
  for (uint8_t f = 0; f < clip.frameCount; f++) {
    frameIdx = (uint16_t)(clip.frameStart + f);
    acc += frames_[frameIdx].durationMs;
    if (intoClip < acc) break;
  }

  const FrameEntry& fe = frames_[frameIdx];
  if (fe.dataLength > bufCap) return false;  // buffer too small — see maxFrameBytes()

  FsFile f = SdMan.open(path_, O_RDONLY);
  if (!f) return false;
  bool ok = f.seekSet(fe.dataOffset) && f.read(buf, fe.dataLength) == (int)fe.dataLength;
  f.close();
  if (!ok) return false;

  out = freeink::Icon{fe.w, fe.h, fe.center, buf};
  return true;
}
