#!/usr/bin/env python3
"""Convert a video into a .bap file (grayscale, 2/4/16 levels) for the Bad Apple module.

Usage: python3 make_bap.py input.mp4 output.bap [width height fps levels]
Defaults: 96x72 at 15 fps, 4 gray levels.

levels: 2 = pure black/white, 4 = black, dark gray, light gray, white, 16 = fine gray.

.bap (BAP2) format, little-endian:
  "BAP2", u16 width, u16 height, u16 fps, u16 levels, u32 frameCount,
  then frameCount frames, each ceil(width*height*bpp/8) bytes where bpp is
  1 (2 levels), 2 (4 levels) or 4 (16 levels). Pixels are row-major, packed
  MSB first; value 0 = black, levels-1 = white.
"""
import struct
import subprocess
import sys


def bpp_for(levels: int) -> int:
    return 1 if levels <= 2 else 2 if levels <= 4 else 4


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 96
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 72
    fps = int(sys.argv[5]) if len(sys.argv) > 5 else 15
    levels = int(sys.argv[6]) if len(sys.argv) > 6 else 4
    if levels not in (2, 4, 16):
        print("levels must be 2, 4 or 16", file=sys.stderr)
        return 2
    bpp = bpp_for(levels)

    cmd = [
        "ffmpeg", "-v", "error", "-i", src,
        "-vf", f"fps={fps},scale={w}:{h}:flags=area,format=gray",
        "-f", "rawvideo", "-pix_fmt", "gray", "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    frame_size = w * h
    packed_size = (frame_size * bpp + 7) // 8
    lut = [min(levels - 1, (v * (levels - 1) + 127) // 255) for v in range(256)]
    frames = []
    while True:
        raw = proc.stdout.read(frame_size)
        if len(raw) < frame_size:
            break
        out = bytearray(packed_size)
        for i, v in enumerate(raw):
            q = lut[v]
            if q:
                bit = i * bpp
                out[bit >> 3] |= q << (8 - bpp - (bit & 7))
        frames.append(bytes(out))
    proc.stdout.close()
    code = proc.wait()
    if code != 0 or not frames:
        print(f"ffmpeg failed (exit {code}) or produced no frames", file=sys.stderr)
        return 1

    with open(dst, "wb") as f:
        f.write(b"BAP2")
        f.write(struct.pack("<HHHH", w, h, fps, levels))
        f.write(struct.pack("<I", len(frames)))
        for fr in frames:
            f.write(fr)
    size_mb = (16 + len(frames) * packed_size) / 1e6
    print(f"Wrote {dst}: {len(frames)} frames, {w}x{h} @ {fps} fps, {levels} levels, {size_mb:.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
