#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# wait_game.sh [max seconds, default 300]: waits until no game process (tlg-probe, tlggpu-host) runs.
# Exit 0 when none runs, 1 on timeout. Matches the process name exactly (pgrep -x): `pgrep -f` also
# matches the shell running this loop and never returns (an agent lost 8 hours to that).
# Never kills anything: a Rosetta process must not be killed (AGENTS.md).
max=${1:-300}
for ((t = 0; t < max; t += 5)); do
    pgrep -x tlg-probe >/dev/null || pgrep -x tlggpu-host >/dev/null || pgrep -x tlggpu-host-macos27 >/dev/null || exit 0
    sleep 5
done
echo "wait_game.sh: a game process still runs after ${max} s" >&2
exit 1
