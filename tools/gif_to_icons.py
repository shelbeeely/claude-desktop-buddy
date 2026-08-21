#!/usr/bin/env python3
"""Convert a claude-desktop-buddy character pack to freeink::Icon C structs.

Offline, host-side step in the Xteink X4 asset pipeline (see the port's
README "Asset pipeline" section). Input is a character pack that has
already been through tools/prep_character.py — a manifest.json plus
96px-wide GIFs, one (or a rotating list) per state, all cropped to the
same on-screen scale.

freeink::Icon (libs/assets/Icons/include/Icon.h) is 1-bpp: bit 1 = leave
the pixel (transparent, background shows through), bit 0 = draw black.
There is no "draw white" — a light pixel and a background pixel both come
out transparent, which is correct as long as the caller clears the sprite
region to white before blitting (this port's render loop does exactly
that; see main_xteink_x4.cpp).

Classifying a source pixel:
  - within BG_TOLERANCE of the manifest's flattened background color
    (prep_character.py flattens every frame onto manifest colors.bg,
    so there is no alpha channel left to key on) -> transparent
  - else luminance < THRESHOLD (dark ink)                -> draw black
  - else (a light non-background pixel, e.g. a highlight) -> transparent

bufo's manifest background is #000000 (near-black), so the bg-color check
matters: without it, luminance alone would classify the entire background
as "dark -> draw black" and paint a solid rectangle. Checking bg-distance
first excludes it even though its luminance is 0.

Output: one C++ header with, per state, a IconState (a rotating array of
IconClip, each an array of IconFrame{icon, durationMs}) — this mirrors the
manifest's own "single filename or list of filenames" shape (the list is
the idle-carousel rotation from the original README) so nothing about the
per-state structure is invented here, just re-expressed as compiled data.

--sd-out additionally writes a binary ".charpack" with the same frame data,
for a character stored on the SD card instead of compiled into flash — see
src/sd_character_pack.h and README.md "SD-backed character packs" for the
on-device reader and the format this binary uses (a fixed-size state/clip/
frame index, so the reader loads only the ~2-3KB index into RAM and reads
one frame's packed bits on demand, never the whole pack).

Usage:
    python3 tools/gif_to_icons.py characters/bufo \\
        --scale 2 --out src/assets/icons_bufo.h --namespace bufo \\
        --sd-out characters/bufo/bufo.charpack
"""
import argparse
import json
import struct
import sys
from pathlib import Path

from PIL import Image, ImageSequence

# .charpack binary layout (all little-endian, matches ESP32-C3's native
# endianness so the on-device reader can memcpy struct fields directly):
#
#   Header   (12B): magic="CDBK", version(B), stateCount(B), reserved(H),
#                    totalClips(H), totalFrames(H)
#   StateDir (4B * stateCount, fixed order sleep/idle/busy/attention/
#             celebrate/dizzy/heart): clipStart(H) clipCount(B) reserved(B)
#   ClipDir  (4B * totalClips): frameStart(H) frameCount(B) reserved(B)
#   FrameDir (16B * totalFrames): w(H) h(H) opticalCenterY(h) durationMs(H)
#             dataOffset(I) dataLength(I) -- dataOffset is absolute from the
#             start of the file, so the reader never needs to compute it.
#   <raw 1bpp frame bytes, back to back, referenced by FrameDir>
_HEADER_FMT = "<4sBBHHH"
_STATE_FMT = "<HBB"
_CLIP_FMT = "<HBB"
_FRAME_FMT = "<HHhHII"
_MAGIC = b"CDBK"
_VERSION = 1

STATES = ["sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"]
THRESHOLD = 128       # luminance below this (0-255) counts as ink
BG_TOLERANCE = 40      # per-channel max-diff from manifest bg counted as background


def hex_to_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def pack_frame(img, bg_rgb):
    """1-bpp pack: rows top-to-bottom, (w+7)//8 bytes/row, MSB-first.
    Returns (bytes_list, w, h, opticalCenterY)."""
    rgb = img.convert("RGB")
    w, h = rgb.size
    px = rgb.load()
    data = []
    sum_y = 0
    count = 0
    for y in range(h):
        for xb in range(0, w, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                transparent = 1
                if x < w:
                    r, g, bl = px[x, y]
                    bg_dist = max(abs(r - bg_rgb[0]), abs(g - bg_rgb[1]), abs(bl - bg_rgb[2]))
                    luminance = 0.299 * r + 0.587 * g + 0.114 * bl
                    if bg_dist > BG_TOLERANCE and luminance < THRESHOLD:
                        transparent = 0
                        sum_y += y
                        count += 1
                byte |= transparent << (7 - b)
            data.append(byte)
    center = round(sum_y / count) if count else h // 2
    return data, w, h, center


def ident(*parts):
    return "_".join(str(p) for p in parts)


def load_clip(gif_path, scale):
    im = Image.open(gif_path)
    frames = []
    for f in ImageSequence.Iterator(im):
        duration = f.info.get("duration", 100) or 100
        rgba = f.convert("RGBA")
        if scale != 1:
            rgba = rgba.resize((rgba.width * scale, rgba.height * scale), Image.LANCZOS)
        frames.append((rgba, duration))
    return frames


def write_charpack(path, bin_states):
    """bin_states: list of 7 states, each a list of clips, each a list of
    (w, h, center, data_bytes, duration_ms) frames — see the format comment
    above. Layout is fixed-size-record-first so the on-device reader can
    seek straight to any frame's index entry without parsing anything
    variable-length."""
    total_clips = sum(len(clips) for clips in bin_states)
    total_frames = sum(len(frames) for clips in bin_states for frames in clips)

    header_size = struct.calcsize(_HEADER_FMT)
    state_dir_size = struct.calcsize(_STATE_FMT) * len(bin_states)
    clip_dir_size = struct.calcsize(_CLIP_FMT) * total_clips
    frame_dir_size = struct.calcsize(_FRAME_FMT) * total_frames
    data_start = header_size + state_dir_size + clip_dir_size + frame_dir_size

    state_dir = bytearray()
    clip_dir = bytearray()
    frame_dir = bytearray()
    data = bytearray()
    clip_cursor = 0
    frame_cursor = 0
    data_offset = data_start

    for clips in bin_states:
        state_dir += struct.pack(_STATE_FMT, clip_cursor, len(clips), 0)
        clip_cursor += len(clips)
        for frames in clips:
            clip_dir += struct.pack(_CLIP_FMT, frame_cursor, len(frames), 0)
            frame_cursor += len(frames)
            for w, h, center, frame_bytes, duration in frames:
                frame_dir += struct.pack(_FRAME_FMT, w, h, center, duration, data_offset, len(frame_bytes))
                data += frame_bytes
                data_offset += len(frame_bytes)

    header = struct.pack(_HEADER_FMT, _MAGIC, _VERSION, len(bin_states), 0, total_clips, total_frames)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(header) + bytes(state_dir) + bytes(clip_dir) + bytes(frame_dir) + bytes(data))
    return len(header) + len(state_dir) + len(clip_dir) + len(frame_dir) + len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("character_dir", type=Path)
    ap.add_argument("--scale", type=int, default=2, help="integer upscale from the 96px-wide source (default 2x -> 192px)")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--namespace", required=True, help="C identifier prefix, e.g. bufo")
    ap.add_argument("--sd-out", type=Path, help="also write a .charpack binary for SD storage (see src/sd_character_pack.h)")
    args = ap.parse_args()

    manifest_path = args.character_dir / "manifest.json"
    if not manifest_path.exists():
        sys.exit(f"no manifest.json in {args.character_dir}")
    manifest = json.loads(manifest_path.read_text())
    bg_rgb = hex_to_rgb(manifest.get("colors", {}).get("bg", "#000000"))
    ns = args.namespace

    missing = [s for s in STATES if s not in manifest["states"]]
    if missing:
        sys.exit(f"manifest is missing required state(s): {missing}")

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/gif_to_icons.py from a claude-desktop-buddy character",
        f"// pack ({args.character_dir.name}). Do not edit by hand — regenerate instead.",
        f'// Source: {args.character_dir}  scale: {args.scale}x',
        "",
        '#include "Icon.h"',
        "#include <cstdint>",
        "",
        f"namespace {ns} {{",
        "",
        "struct IconFrame {",
        "  const freeink::Icon* icon;",
        "  uint16_t durationMs;",
        "};",
        "struct IconClip {",
        "  const IconFrame* frames;",
        "  uint8_t frameCount;",
        "};",
        "struct IconState {",
        "  const IconClip* clips;",
        "  uint8_t clipCount;  // >1 = rotating carousel (advances on loop-end)",
        "};",
        "",
    ]

    max_w = max_h = 0
    state_clip_syms = {}
    # Mirrors the .h structure exactly, for --sd-out: bin_states[state] is a
    # list of clips, each a list of (w, h, center, data_bytes, duration).
    bin_states = []

    for state in STATES:
        entry = manifest["states"][state]
        clip_files = entry if isinstance(entry, list) else [entry]
        clip_syms = []
        bin_clips = []
        for ci, fname in enumerate(clip_files):
            gif_path = args.character_dir / fname
            if not gif_path.exists():
                sys.exit(f"missing {gif_path} (state '{state}')")
            frames = load_clip(gif_path, args.scale)
            frame_syms = []
            bin_frames = []
            for fi, (rgba, duration) in enumerate(frames):
                data, w, h, center = pack_frame(rgba, bg_rgb)
                max_w, max_h = max(max_w, w), max(max_h, h)
                bits_sym = ident(ns, state, ci, fi, "bits")
                icon_sym = ident(ns, state, ci, fi, "icon")
                body = ", ".join(f"0x{b:02X}" for b in data)
                lines.append(f"static const uint8_t {bits_sym}[] = {{{body}}};")
                lines.append(
                    f"static const freeink::Icon {icon_sym} = {{{w}, {h}, {center}, {bits_sym}}};"
                )
                frame_syms.append((icon_sym, duration))
                bin_frames.append((w, h, center, bytes(data), duration))
            frames_sym = ident(ns, state, ci, "frames")
            frame_list = ", ".join(f"{{&{sym}, {dur}}}" for sym, dur in frame_syms)
            lines.append(f"static const IconFrame {frames_sym}[] = {{{frame_list}}};")
            clip_sym = ident(ns, state, ci, "clip")
            lines.append(
                f"static const IconClip {clip_sym} = {{{frames_sym}, {len(frame_syms)}}};"
            )
            lines.append("")
            clip_syms.append(clip_sym)
            bin_clips.append(bin_frames)
        clips_array_sym = ident(ns, state, "clips")
        clips_list = ", ".join(clip_syms)
        lines.append(f"static const IconClip {clips_array_sym}[] = {{{clips_list}}};")
        lines.append("")
        state_clip_syms[state] = (clips_array_sym, len(clip_syms))
        bin_states.append(bin_clips)

    lines.append(f"// Indexed by the 7-state enum, in the order {STATES}.")
    lines.append(f"static const IconState {ns}_states[] = {{")
    for state in STATES:
        sym, n = state_clip_syms[state]
        lines.append(f"    {{{sym}, {n}}},  // {state}")
    lines.append("};")
    lines.append("")
    lines.append(f"// Largest single frame across every state/clip — size the fixed")
    lines.append(f"// sprite displayWindow to at least this (w={max_w}, h={max_h}).")
    lines.append(f"static constexpr uint16_t MAX_ICON_W = {max_w};")
    lines.append(f"static constexpr uint16_t MAX_ICON_H = {max_h};")
    lines.append("")
    lines.append(f"}}  // namespace {ns}")
    lines.append("")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines))
    total_bytes = sum(len(l) for l in lines)
    print(f"wrote {args.out} (~{total_bytes/1024:.1f}KB source, max icon {max_w}x{max_h})")

    if args.sd_out:
        packed_bytes = write_charpack(args.sd_out, bin_states)
        print(f"wrote {args.sd_out} ({packed_bytes/1024:.1f}KB)")


if __name__ == "__main__":
    main()
