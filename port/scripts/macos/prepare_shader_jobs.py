#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The first-start shader job list (gpu vk_precompile.h), for run_macos.sh: prepare_shader_jobs.py
<game folder> <jobs file>. Built by tools/xpps from the player's own game files, about a minute,
once: again only when the game files or the generator change. The game then compiles the jobs behind
its "Compiling shaders" screen. Nothing here may stop the game from starting: without a jobs file
shaders compile in game. Exit status 0 when the jobs file is usable."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parents[2]


def stamp(root):
    """Hash of the generator's sources and of path, size and mtime of every package."""
    digest = hashlib.sha256()
    for path in sorted((PORT / 'tools' / 'xpps').glob('*.py')):
        digest.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
    entries = []
    for path in root.rglob('*.xpps'):
        info = path.stat()
        entries.append((path.relative_to(root).as_posix(), info.st_size, info.st_mtime_ns))
    for name, size, mtime in sorted(entries):
        digest.update(f'{name}\0{size}\0{mtime}\n'.encode())
    return digest.hexdigest(), len(entries)


def main(game, jobs):
    root = next((p for p in (game / 'art' / 'cache', game / 'art') if p.is_dir()), None)
    if root is None:
        print(f'Shader jobs: no art folder in {game}: shaders compile in game')
        return 1
    current, packages = stamp(root)
    stamp_file = jobs.with_name(jobs.name + '.stamp')
    if jobs.is_file() and stamp_file.is_file() and stamp_file.read_text() == current:
        return 0
    if not packages:
        print(f'Shader jobs: no packages in {root}: shaders compile in game')
        return 1
    jobs.parent.mkdir(parents=True, exist_ok=True)
    part = jobs.with_name(jobs.name + '.part')
    index = jobs.with_name(jobs.name + '.index.sqlite')
    print(f'Shader jobs: listing the shaders of {packages} game packages (once, about a minute)',
          flush=True)
    try:
        result = subprocess.run(
            [sys.executable, str(PORT / 'scripts' / 'build_jobs.py'), str(root), str(part), '--index',
             str(index), '--jobs', str(max(1, (os.cpu_count() or 2) - 1))],
            stdout=subprocess.DEVNULL, check=False)
        if result.returncode != 0 or not part.is_file():
            print(f'Shader jobs: the generator failed ({result.returncode}): shaders compile in game')
            return 1
        os.replace(part, jobs)
        stamp_file.write_text(current)
        return 0
    finally:
        for leftover in (part, index):
            leftover.unlink(missing_ok=True)


if __name__ == '__main__':
    try:
        sys.exit(main(Path(sys.argv[1]), Path(sys.argv[2])))
    except OSError as error:
        print(f'Shader jobs: {error}: shaders compile in game')
        sys.exit(1)
