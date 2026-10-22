#!/usr/bin/env python3
"""Decode a PSMCT32 framebuffer out of a 4 MiB GS VRAM dump (PS2X_GS_DUMP vram_*.bin).

usage: tools/gs_fb_decode.py <vram.bin> <fbp> <fbw> <width> <height> <out_prefix>
Writes <out_prefix>_rgb.ppm (colour) and <out_prefix>_alpha.pgm (alpha scaled x2).
"""
import sys

BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21],
           [2, 3, 6, 7, 18, 19, 22, 23],
           [8, 9, 12, 13, 24, 25, 28, 29],
           [10, 11, 14, 15, 26, 27, 30, 31]]
COLUMN32 = [[0, 1, 4, 5, 8, 9, 12, 13],
            [2, 3, 6, 7, 10, 11, 14, 15],
            [16, 17, 20, 21, 24, 25, 28, 29],
            [18, 19, 22, 23, 26, 27, 30, 31],
            [32, 33, 36, 37, 40, 41, 44, 45],
            [34, 35, 38, 39, 42, 43, 46, 47],
            [48, 49, 52, 53, 56, 57, 60, 61],
            [50, 51, 54, 55, 58, 59, 62, 63]]


def word_address(fbp, fbw, x, y):
    page = (y // 32) * fbw + (x // 64)
    block = BLOCK32[(y % 32) // 8][(x % 64) // 8]
    column = COLUMN32[y % 8][x % 8]
    return (fbp * 2048 + page * 2048 + block * 64 + column) % (1 << 20)


def main():
    if len(sys.argv) != 7:
        print(__doc__)
        return 1
    path, fbp, fbw, w, h, out = sys.argv[1], *map(int, sys.argv[2:6]), sys.argv[6]
    vram = open(path, 'rb').read()
    rgb = bytearray()
    alpha = bytearray()
    for y in range(h):
        for x in range(w):
            a = word_address(fbp, fbw, x, y) * 4
            r, g, b, al = vram[a], vram[a + 1], vram[a + 2], vram[a + 3]
            rgb += bytes((r, g, b))
            alpha.append(min(255, al * 2))
    open(out + '_rgb.ppm', 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + rgb)
    open(out + '_alpha.pgm', 'wb').write(b'P5\n%d %d\n255\n' % (w, h) + alpha)
    return 0


if __name__ == '__main__':
    sys.exit(main())
