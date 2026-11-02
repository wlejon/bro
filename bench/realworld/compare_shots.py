#!/usr/bin/env python3
"""Compare two raw RGBA readPixels dumps (bottom-up rows, as WebGL returns them).

  compare_shots.py a.rgba b.rgba --size 1280x720 [--tol 8] [--out prefix]

Prints the max per-channel difference, the mean absolute difference, and the
count of pixels whose largest channel difference exceeds --tol. With --out it
writes <prefix>_a.png, <prefix>_b.png (flipped upright) and <prefix>_diff.png
(per-pixel max channel difference, amplified x4; red where over tol).
No third-party modules: PNGs are written with zlib.
"""

import argparse
import struct
import sys
import zlib


def write_png(path, w, h, rows_rgb):
    raw = b"".join(b"\x00" + r for r in rows_rgb)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def upright_rgb(buf, w, h):
    rows = []
    for y in range(h - 1, -1, -1):
        row = buf[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3] = row[0::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[2::4]
        rows.append(bytes(rgb))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--tol", type=int, default=8)
    ap.add_argument("--out", default="")
    ap.add_argument("--alpha", action="store_true", help="include the alpha channel in the comparison")
    args = ap.parse_args()
    w, h = map(int, args.size.split("x"))
    A = open(args.a, "rb").read()
    B = open(args.b, "rb").read()
    if len(A) != w * h * 4 or len(B) != w * h * 4:
        sys.exit(f"size mismatch: {len(A)} / {len(B)} bytes, expected {w * h * 4}")
    nch = 4 if args.alpha else 3
    maxd = 0
    total = 0
    over = 0
    differ = 0
    diff_rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray(w * 3)
        base = y * w * 4
        for x in range(w):
            i = base + x * 4
            d = 0
            for c in range(nch):
                dc = abs(A[i + c] - B[i + c])
                total += dc
                if dc > d:
                    d = dc
            if d:
                differ += 1
            if d > maxd:
                maxd = d
            if d > args.tol:
                over += 1
                row[x * 3] = 255
                row[x * 3 + 1] = min(255, d)
            else:
                v = min(255, d * 4)
                row[x * 3:x * 3 + 3] = bytes((v, v, v))
        diff_rows.append(bytes(row))
    px = w * h
    print(f"pixels {px}  differing {differ} ({100.0 * differ / px:.3f}%)  max channel diff {maxd}  "
          f"mean abs diff {total / (px * nch):.4f}  over tol {args.tol}: {over} ({100.0 * over / px:.3f}%)")
    if args.out:
        write_png(args.out + "_a.png", w, h, upright_rgb(A, w, h))
        write_png(args.out + "_b.png", w, h, upright_rgb(B, w, h))
        write_png(args.out + "_diff.png", w, h, diff_rows)
        print("wrote", args.out + "_{a,b,diff}.png")


if __name__ == "__main__":
    main()
