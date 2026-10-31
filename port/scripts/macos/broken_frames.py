#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Counts broken frames among a diag run's shots (diag_run.sh SHOT_SECONDS=1): the bridge after
the car crash shows them as scenery washed out to white with 8x8 black blocks below. Signature:
the mean brightness of the upper third of the image above 185 (good frames there stay at or
below ~182, broken ones 193-209; checked against hand counts of five runs, 2026-10-31).

    python3 scripts/macos/broken_frames.py out/diag/<run>/shots   (needs Pillow)
"""
import os
import sys

from PIL import Image

THRESHOLD = 185


def top_brightness(path):
    image = Image.open(path).convert('L').resize((192, 108))
    pixels = image.load()
    return sum(pixels[x, y] for y in range(36) for x in range(192)) / (192 * 36)


def main():
    folder = sys.argv[1]
    shots = sorted(f for f in os.listdir(folder) if f.endswith('.jpg'))
    values = [(name, top_brightness(os.path.join(folder, name))) for name in shots]
    broken = [name for name, value in values if value > THRESHOLD]
    print(f"{len(broken)}/{len(values)} broken frames ({100 * len(broken) / max(1, len(values)):.1f}%)")
    for name in broken:
        print(f"  {name}")


if __name__ == '__main__':
    main()
