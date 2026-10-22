#!/usr/bin/env python3
"""Convert a video into a 1-bit .bap file for the Bad Apple module.

Usage: python3 make_bap.py input.mp4 output.bap [width height fps]
Defaults: 96x72 at 15 fps (works well for a 4:3 source such as 480x360).

.bap format (little-endian):
  "BAP1", u16 width, u16 height, u16 fps, u16 reserved, u32 frameCount,
  then frameCount * ceil(width*height/8) bytes, row-major, MSB first, 1 = white.
"""
import struct
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 96
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 72
    fps = int(sys.argv[5]) if len(sys.argv) > 5 else 15

    cmd = [
        "ffmpeg", "-v", "error", "-i", src,
        "-vf", f"fps={fps},scale={w}:{h}:flags=area,format=gray",
        "-f", "rawvideo", "-pix_fmt", "gray", "-",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    frame_size = w * h
    packed_size = (frame_size + 7) // 8
    frames = []
    while True:
        raw = proc.stdout.read(frame_size)
        if len(raw) < frame_size:
            break
        out = bytearray(packed_size)
        for i, v in enumerate(raw):
            if v >= 128:
                out[i >> 3] |= 1 << (7 - (i & 7))
        frames.append(bytes(out))
    proc.stdout.close()
    code = proc.wait()
    if code != 0 or not frames:
        print(f"ffmpeg failed (exit {code}) or produced no frames", file=sys.stderr)
        return 1

    with open(dst, "wb") as f:
        f.write(b"BAP1")
        f.write(struct.pack("<HHHH", w, h, fps, 0))
        f.write(struct.pack("<I", len(frames)))
        for fr in frames:
            f.write(fr)
    size_mb = (16 + len(frames) * packed_size) / 1e6
    print(f"Wrote {dst}: {len(frames)} frames, {w}x{h} @ {fps} fps, {size_mb:.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
