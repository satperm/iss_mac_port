#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Luminance statistics of RGBA16F dumps (TLG_DIAG_IMAGES=1 during a frame capture), for the auto
exposure work: what the game's exposure compute (cs a8b68717, 15x15 groups -> the 225 log-luminance
values summed on the CPU at eboot+0x171783) reads.

    python3 exposure_stats.py <dump.raw> [<dump.raw> ...] [--tiles 15]

File names come from TextureCache::DiagDumpImage: <seq>_<tag>_<addr>_<w>x<h>_p<pitch>_b<bpp>_f<fmt>.raw.
Prints per image: NaN/Inf count, max luminance, pixels above 16/64/1024, mean log luminance, and the
tile grid's mean log luminance (where the bright pixels are)."""
import math
import re
import sys


def load(path):
    m = re.search(r'_(\d+)x(\d+)_p(\d+)_b(\d+)_f(\d+)\.raw$', path)
    if not m:
        raise SystemExit(f'not a dump name: {path}')
    w, h, pitch, bpp = (int(m.group(i)) for i in range(1, 5))
    if bpp != 8:
        raise SystemExit(f'{path}: {bpp} bytes per pixel, RGBA16F (8) expected')
    data = memoryview(open(path, 'rb').read()).cast('e')
    return w, h, pitch, data


def stats(path, tiles):
    w, h, pitch, px = load(path)
    bad = 0
    lmax = 0.0
    above = [0, 0, 0]
    sum_log = 0.0
    n = 0
    tw, th = math.ceil(w / tiles), math.ceil(h / tiles)
    grid = [[0.0, 0] for _ in range(tiles * tiles)]
    for y in range(h):
        row = y * pitch * 4
        gy = (y // th) * tiles
        for x in range(w):
            i = row + x * 4
            r, g, b = px[i], px[i + 1], px[i + 2]
            lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
            if lum != lum or lum in (math.inf, -math.inf):
                bad += 1
                continue
            lum = max(lum, 0.0)
            lmax = max(lmax, lum)
            above[0] += lum > 16
            above[1] += lum > 64
            above[2] += lum > 1024
            v = math.log(lum + 1e-4)
            sum_log += v
            n += 1
            cell = grid[gy + x // tw]
            cell[0] += v
            cell[1] += 1
    print(f'{path.rsplit("/", 1)[-1]}: {w}x{h} NaN/Inf {bad}, max {lmax:.1f}, >16 {above[0]}, '
          f'>64 {above[1]}, >1024 {above[2]}, mean log {sum_log / max(n, 1):.3f}')
    for ty in range(tiles):
        print('   ' + ' '.join(f'{(c[0] / c[1]) if c[1] else 0:6.2f}' for c in grid[ty * tiles:(ty + 1) * tiles]))


def main():
    args = sys.argv[1:]
    tiles = 15
    if '--tiles' in args:
        i = args.index('--tiles')
        tiles = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        sys.exit(2)
    for path in args:
        stats(path, tiles)


if __name__ == '__main__':
    main()
