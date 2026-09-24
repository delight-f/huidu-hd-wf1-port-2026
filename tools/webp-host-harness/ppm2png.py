#!/usr/bin/env python3
"""Convert the PPM montages written by ./render into PNGs you can actually look
at, using only the standard library.

    ./ppm2png.py render_parrot_ref.ppm [...]      # or with no args: *.ppm
"""
import glob
import os
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def chunk(tag, body):
    payload = tag + body
    return (struct.pack(">I", len(body)) + payload
            + struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF))


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)
    print(f"{path} ({w}x{h})")


def main():
    targets = sys.argv[1:] or sorted(glob.glob("*.ppm"))
    if not targets:
        print("no .ppm files found - run ./render first")
        return 1
    for src in targets:
        w, h, px = read_ppm(src)
        write_png(os.path.splitext(src)[0] + ".png", w, h, px)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
