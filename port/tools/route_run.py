#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One scripted run of a recorded gameplay route (Windows and Linux): same save, same inputs, so two
configurations can be compared on the same scene.

    python tools/route_run.py <label> [KEY=value ...]

The save snapshot is copied over the test user's savedata, the game starts through the launcher's
`--play` (so it gets the same environment as a player), the route (ISS_PAD_REPLAY) plays from gameplay,
and the game is closed when the route ends. Prints the gameplay windows' numbers (windows with more
than 1000 draws a frame) and appends one JSON line to <results>/route_results.jsonl.

Paths come from the environment, with the defaults of the author's workspace:
  ISS_ROUTE_USER   the test user's folder (ISS_USER_DIR)        ~/decomp/iss_test_user_11pct
  ISS_ROUTE_SAVE   a folder holding the pristine `savedata`     ~/decomp/infamous_save_backup_20261020/iss_ref_pgo_user_11pct_20261019
  ISS_ROUTE_FILE   the recorded route                           ~/decomp/iss_rota/rota_11pct.txt
  ISS_ROUTE_GAME   the game folder (ISS_GAME_DIR)               ~/decomp/infamous_pc/game/CUSA00223_win
  ISS_ROUTE_OUT    where the results and run logs are kept      <user>/route_results

KEY=value arguments are put in the game's environment (ISS_DYN_CULL=2, ISS_DRAW_BUDGET=50, ...).
The game is never started while another tlg-probe is running.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
HOME_DECOMP = Path(os.environ.get('ISS_ROUTE_ROOT', Path.home() / 'decomp'))
USER = Path(os.environ.get('ISS_ROUTE_USER', HOME_DECOMP / 'iss_test_user_11pct'))
SAVE = Path(os.environ.get('ISS_ROUTE_SAVE',
                           HOME_DECOMP / 'infamous_save_backup_20261020' / 'iss_ref_pgo_user_11pct_20261019'))
ROUTE = Path(os.environ.get('ISS_ROUTE_FILE', HOME_DECOMP / 'iss_rota' / 'rota_11pct.txt'))
GAME = Path(os.environ.get('ISS_ROUTE_GAME', HOME_DECOMP / 'infamous_pc' / 'game' / 'CUSA00223_win'))
OUT = Path(os.environ.get('ISS_ROUTE_OUT', USER / 'route_results'))
LOGS = PORT / 'out' / 'logs'
WINDOWS = os.name == 'nt'

# Frame stats: "28.5 FPS, worst frame 50.8 ms (vblank 60 Hz); ... GPU thread 0.00 us/draw, 451 draws/frame"
STATS = re.compile(r'Frame stats: ([0-9.]+) FPS, worst frame ([0-9.]+) ms.*?([0-9]+) draws/frame')
LEVELS = re.compile(r'Dynamic quality: level (-?\d+), draw lists max (\d+)/(\d+) KB, samples per level(.*)')


def running():
    name = 'tlg-probe.exe' if WINDOWS else 'tlg-probe'
    if WINDOWS:
        out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}'], capture_output=True, text=True).stdout
        return name.lower() in out.lower()
    return subprocess.run(['pgrep', '-x', 'tlg-probe'], capture_output=True).returncode == 0


def stop_game():
    if WINDOWS:
        subprocess.run(['taskkill', '/F', '/IM', 'tlg-probe.exe'], capture_output=True)
    else:
        subprocess.run(['pkill', '-TERM', '-x', 'tlg-probe'], capture_output=True)


def newest_log(before):
    logs = sorted(LOGS.glob('run-*.log'), key=lambda p: p.stat().st_mtime)
    return logs[-1] if logs and logs[-1].name != before else None


def summarize(log):
    windows, levels, lists = [], {}, [0, 0]
    for line in log.read_text(errors='replace').splitlines():
        m = STATS.search(line)
        if m:
            windows.append((float(m.group(1)), float(m.group(2)), int(m.group(3))))
        m = LEVELS.search(line)
        if m:
            lists = [max(lists[0], int(m.group(2))), max(lists[1], int(m.group(3)))]
            for pair in m.group(4).split():
                level, count = pair.split(':')
                levels[int(level)] = levels.get(int(level), 0) + int(count)
    play = [w for w in windows if w[2] > 1000]
    if not play:
        return {'windows': len(windows), 'gameplay_windows': 0}
    fps = sorted(w[0] for w in play)
    return {
        'windows': len(windows), 'gameplay_windows': len(play),
        'fps_mean': round(sum(fps) / len(fps), 1), 'fps_min': fps[0], 'fps_p10': fps[len(fps) // 10],
        'worst_ms_mean': round(sum(w[1] for w in play) / len(play)),
        'draws_mean': round(sum(w[2] for w in play) / len(play)),
        'levels': dict(sorted(levels.items())), 'draw_lists_max_kb': lists,
    }


def main(argv):
    if not argv:
        sys.exit(__doc__)
    label, extra = argv[0], dict(a.split('=', 1) for a in argv[1:] if '=' in a)
    for path, what in ((SAVE / 'savedata', 'save snapshot'), (ROUTE, 'route'), (GAME, 'game folder')):
        if not path.exists():
            sys.exit(f'missing {what}: {path}')
    if running():
        sys.exit('a tlg-probe is already running: not starting a second game')
    shutil.rmtree(USER / 'savedata', ignore_errors=True)
    shutil.copytree(SAVE / 'savedata', USER / 'savedata')
    OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update({
        'ISS_USER_DIR': str(USER), 'ISS_GAME_DIR': str(GAME), 'ISS_FRAME_STATS': '1',
        'ISS_PAD_REPLAY': str(ROUTE), 'ISS_PAD_REPLAY_AUTO': '1',
        'ISS_LAGGED_READS': '0x1717c0', 'ISS_NOWAIT_WRITES': '0x43e1fb',
    })
    if WINDOWS:   # a source tree: the DLLs of the build environment
        env['PATH'] = os.environ.get('MSYS2_ROOT', 'C:\\msys64') + '\\clang64\\bin;' + env['PATH']
    env.update(extra)
    LOGS.mkdir(parents=True, exist_ok=True)
    before = max((p.name for p in LOGS.glob('run-*.log')), default='')
    started = time.time()
    runner = subprocess.Popen([sys.executable, str(PORT / 'launcher' / 'infamous_launcher.py'), '--play', '0'],
                              env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log, finished = None, False
    limit = float(os.environ.get('ISS_ROUTE_WAIT', 900))
    while time.time() - started < limit:
        time.sleep(2)
        log = log or newest_log(before)
        if log and 'pad replay finished' in log.read_text(errors='replace'):
            finished = True
            break
        if runner.poll() is not None:
            break
    time.sleep(3)
    stop_game()
    try:
        runner.wait(timeout=20)
    except subprocess.TimeoutExpired:
        runner.kill()
    if log is None:
        sys.exit(f'{label}: no log')
    result = {'label': label, 'env': extra, 'log': log.name, 'finished': finished,
              'seconds': round(time.time() - started)}
    result.update(summarize(log))
    kept = OUT / f'{label}-{log.name}'
    shutil.copy2(log, kept)
    with open(OUT / 'route_results.jsonl', 'a') as f:
        f.write(json.dumps(result) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main(sys.argv[1:])
